// SPDX-License-Identifier: GPL-2.0

#include <asm/cpuid/api.h>
#include <asm/cpufeature.h>
#include <asm/mwait.h>
#include <asm/processor.h>
#include <asm/tsc.h>

#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/stringify.h>

#include "priv.h"

#define NVMET_MDEV_UMONITOR_LINE_DWORDS		\
	(NVMET_MDEV_UMONITOR_LINE_SIZE / sizeof(u32))
#define NVMET_MDEV_UMONITOR_WATCHDOG_US		100
#define NVMET_MDEV_DEFAULT_UMONITOR_DOORBELLS	true
#define NVMET_MDEV_DEFAULT_UMONITOR_WORKERS	1

static bool umonitor_doorbells = NVMET_MDEV_DEFAULT_UMONITOR_DOORBELLS;
module_param_named(umonitor_doorbells, umonitor_doorbells, bool, 0644);
MODULE_PARM_DESC(umonitor_doorbells,
		 "Use UMONITOR/UMWAIT for sparse BAR0 doorbells (default: "
		 __stringify(NVMET_MDEV_DEFAULT_UMONITOR_DOORBELLS) ")");

static uint umonitor_workers = NVMET_MDEV_DEFAULT_UMONITOR_WORKERS;
module_param_named(umonitor_workers, umonitor_workers, uint, 0644);
MODULE_PARM_DESC(umonitor_workers,
		 "Doorbell cache lines monitored per controller (default: "
		 __stringify(NVMET_MDEV_DEFAULT_UMONITOR_WORKERS) ")");

bool nvmet_mdev_umonitor_requested(void)
{
	return READ_ONCE(umonitor_doorbells);
}

unsigned int nvmet_mdev_umonitor_requested_workers(void)
{
	return READ_ONCE(umonitor_workers);
}

static __always_inline void nvmet_mdev_umonitor(const void *addr)
{
	/* UMONITOR %rax */
	asm volatile(".byte 0xf3, 0x0f, 0xae, 0xf0"
		     : : "a" (addr) : "memory");
}

static __always_inline void nvmet_mdev_umwait_c02(u64 deadline)
{
	/* UMWAIT %ecx; ECX=0 requests C0.2. */
	asm volatile(".byte 0xf2, 0x0f, 0xae, 0xf1"
		     : : "a" (lower_32_bits(deadline)),
			 "d" (upper_32_bits(deadline)),
			 "c" (TPAUSE_C02_STATE) : "memory");
}

static bool nvmet_mdev_umonitor_supported(void)
{
	u32 eax, ebx, ecx, edx;

	if (!boot_cpu_has(X86_FEATURE_WAITPKG) ||
	    boot_cpu_data.cpuid_level < 5 || !READ_ONCE(tsc_khz))
		return false;
	cpuid(5, &eax, &ebx, &ecx, &edx);
	return (eax & 0xffff) == NVMET_MDEV_UMONITOR_LINE_SIZE &&
	       (ebx & 0xffff) == NVMET_MDEV_UMONITOR_LINE_SIZE;
}

static bool
nvmet_mdev_umonitor_worker_required(struct nvmet_mdev_umonitor_worker *worker)
{
	struct nvmet_mdev_ctrl *ctrl = worker->ctrl;

	return nvmet_mdev_umonitor_active(ctrl) &&
	       READ_ONCE(ctrl->enabled) &&
	       /* Paired with DBBUF publication in nvmet_mdev_set_dbbuf(). */
	       !smp_load_acquire(&ctrl->dbbuf_dbs);
}

static void
nvmet_mdev_umonitor_snapshot(struct nvmet_mdev_umonitor_worker *worker,
			     u32 snapshot[NVMET_MDEV_UMONITOR_LINE_DWORDS])
{
	__le32 *line = (__le32 *)(worker->ctrl->bar0 + NVME_REG_DBS +
		worker->line * NVMET_MDEV_UMONITOR_LINE_SIZE);
	unsigned int i;

	for (i = 0; i < NVMET_MDEV_UMONITOR_LINE_DWORDS; i++)
		snapshot[i] = (__force u32)READ_ONCE(line[i]);
}

static bool
nvmet_mdev_umonitor_snapshot_changed(struct nvmet_mdev_umonitor_worker *worker,
	const u32 snapshot[NVMET_MDEV_UMONITOR_LINE_DWORDS])
{
	u32 observed[NVMET_MDEV_UMONITOR_LINE_DWORDS];
	unsigned int i;

	nvmet_mdev_umonitor_snapshot(worker, observed);
	for (i = 0; i < NVMET_MDEV_UMONITOR_LINE_DWORDS; i++)
		if (observed[i] != snapshot[i])
			return true;
	return false;
}

static int nvmet_mdev_umonitor_thread(void *data)
{
	struct nvmet_mdev_umonitor_worker *worker = data;
	struct nvmet_mdev_ctrl *ctrl = worker->ctrl;
	u64 wait_cycles = max_t(u64, READ_ONCE(tsc_khz) / 10, 1);
	u32 snapshot[NVMET_MDEV_UMONITOR_LINE_DWORDS];
	const void *line = ctrl->bar0 + NVME_REG_DBS +
		worker->line * NVMET_MDEV_UMONITOR_LINE_SIZE;

	while (!kthread_should_stop()) {
		wait_event_interruptible(ctrl->poll_wait,
			kthread_should_stop() ||
			nvmet_mdev_umonitor_worker_required(worker));
		if (kthread_should_stop())
			break;

		while (nvmet_mdev_umonitor_worker_required(worker) &&
		       !kthread_should_stop()) {
			bool doorbell_wake;
			u64 deadline;

			nvmet_mdev_umonitor_snapshot(worker, snapshot);
			nvmet_mdev_poll_doorbell_line(ctrl, worker->line);

			deadline = rdtsc_ordered() + wait_cycles;
			/* UMONITOR state belongs to the logical CPU for this wait. */
			migrate_disable();
			nvmet_mdev_umonitor(line);
			/* Order the monitor before the lost-wakeup recheck. */
			smp_mb();
			doorbell_wake =
				nvmet_mdev_umonitor_snapshot_changed(worker, snapshot);
			if (doorbell_wake) {
				migrate_enable();
				nvmet_mdev_stat_inc(ctrl, umonitor_rechecks);
				continue;
			}
			if (need_resched()) {
				migrate_enable();
				cond_resched();
				continue;
			}

			nvmet_mdev_umwait_c02(deadline);
			migrate_enable();
			doorbell_wake =
				nvmet_mdev_umonitor_snapshot_changed(worker, snapshot);
			nvmet_mdev_stat_inc(ctrl, umonitor_waits);
			if (doorbell_wake)
				nvmet_mdev_stat_inc(ctrl, umonitor_wakeups);
			if (need_resched())
				cond_resched();
		}
	}
	return 0;
}

static void nvmet_mdev_umonitor_watchdog_work(struct work_struct *work)
{
	struct nvmet_mdev_umonitor *umonitor =
		container_of(work, struct nvmet_mdev_umonitor, watchdog_work);
	struct nvmet_mdev_ctrl *ctrl =
		container_of(umonitor, struct nvmet_mdev_ctrl, umonitor);
	bool activity = false;

	if (nvmet_mdev_umonitor_active(ctrl) && READ_ONCE(ctrl->enabled)) {
		nvmet_mdev_stat_inc(ctrl, umonitor_watchdog_runs);
		activity = nvmet_mdev_rescan_doorbells(ctrl);
		if (activity)
			nvmet_mdev_stat_inc(ctrl, umonitor_watchdog_activity);
	}
	atomic_set(&umonitor->watchdog_pending, 0);
}

static enum hrtimer_restart
nvmet_mdev_umonitor_watchdog(struct hrtimer *timer)
{
	struct nvmet_mdev_umonitor *umonitor =
		container_of(timer, struct nvmet_mdev_umonitor, watchdog);
	struct nvmet_mdev_ctrl *ctrl =
		container_of(umonitor, struct nvmet_mdev_ctrl, umonitor);

	if (!nvmet_mdev_umonitor_active(ctrl) || !READ_ONCE(ctrl->enabled))
		return HRTIMER_NORESTART;

	hrtimer_forward_now(timer,
			    us_to_ktime(NVMET_MDEV_UMONITOR_WATCHDOG_US));
	if (atomic_cmpxchg(&umonitor->watchdog_pending, 0, 1) == 0 &&
	    !schedule_work(&umonitor->watchdog_work))
		atomic_set(&umonitor->watchdog_pending, 0);
	return HRTIMER_RESTART;
}

bool nvmet_mdev_umonitor_init(struct nvmet_mdev_ctrl *ctrl)
{
	struct nvmet_mdev_umonitor *umonitor = &ctrl->umonitor;
	unsigned int maximum, requested, i;

	if (!ctrl->runtime.umonitor_doorbells)
		return false;
	if (!nvmet_mdev_umonitor_supported()) {
		dev_info(ctrl->vdev.dev,
			 "UMONITOR unavailable; using the configured fallback\n");
		return false;
	}

	maximum = DIV_ROUND_UP(ctrl->nr_queues,
			       NVMET_MDEV_UMONITOR_QUEUES_PER_LINE);
	requested = min(ctrl->runtime.umonitor_workers, maximum);
	if (!requested)
		return false;

	umonitor->workers = kcalloc(requested, sizeof(*umonitor->workers),
				    GFP_KERNEL);
	if (!umonitor->workers)
		return false;
	umonitor->nr_workers = requested;
	atomic_set(&umonitor->watchdog_pending, 0);
	INIT_WORK(&umonitor->watchdog_work, nvmet_mdev_umonitor_watchdog_work);
	hrtimer_setup(&umonitor->watchdog, nvmet_mdev_umonitor_watchdog,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	umonitor->initialized = true;

	for (i = 0; i < requested; i++) {
		struct nvmet_mdev_umonitor_worker *worker = &umonitor->workers[i];

		worker->ctrl = ctrl;
		worker->line = i;
		worker->task = kthread_run(nvmet_mdev_umonitor_thread, worker,
					   "nvmet-umon/%u", i);
		if (IS_ERR(worker->task)) {
			dev_warn(ctrl->vdev.dev,
				 "failed to start UMONITOR worker %u; using fallback\n",
				 i);
			worker->task = NULL;
			nvmet_mdev_umonitor_cleanup(ctrl);
			return false;
		}
	}

	ctrl->runtime.umonitor_workers = requested;
	dev_info(ctrl->vdev.dev, "using %u UMONITOR doorbell worker%s\n",
		 requested, requested == 1 ? "" : "s");
	return true;
}

void nvmet_mdev_umonitor_enable(struct nvmet_mdev_ctrl *ctrl)
{
	if (!nvmet_mdev_umonitor_active(ctrl))
		return;
	atomic_set(&ctrl->umonitor.watchdog_pending, 0);
	hrtimer_start(&ctrl->umonitor.watchdog,
		      us_to_ktime(NVMET_MDEV_UMONITOR_WATCHDOG_US),
		      HRTIMER_MODE_REL);
	wake_up_interruptible_all(&ctrl->poll_wait);
}

void nvmet_mdev_umonitor_disable(struct nvmet_mdev_ctrl *ctrl)
{
	if (!ctrl->umonitor.initialized)
		return;
	hrtimer_cancel(&ctrl->umonitor.watchdog);
	cancel_work_sync(&ctrl->umonitor.watchdog_work);
	atomic_set(&ctrl->umonitor.watchdog_pending, 0);
	wake_up_interruptible_all(&ctrl->poll_wait);
}

void nvmet_mdev_umonitor_cleanup(struct nvmet_mdev_ctrl *ctrl)
{
	struct nvmet_mdev_umonitor *umonitor = &ctrl->umonitor;
	unsigned int i;

	if (!umonitor->workers)
		return;
	nvmet_mdev_umonitor_disable(ctrl);
	umonitor->initialized = false;
	wake_up_interruptible_all(&ctrl->poll_wait);
	for (i = 0; i < umonitor->nr_workers; i++) {
		if (umonitor->workers[i].task)
			kthread_stop(umonitor->workers[i].task);
	}
	kfree(umonitor->workers);
	umonitor->workers = NULL;
	umonitor->nr_workers = 0;
}

bool nvmet_mdev_umonitor_active(const struct nvmet_mdev_ctrl *ctrl)
{
	return READ_ONCE(ctrl->runtime.umonitor_doorbells) &&
	       READ_ONCE(ctrl->umonitor.initialized);
}
