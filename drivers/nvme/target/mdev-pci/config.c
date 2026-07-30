// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/stringify.h>

#include "priv.h"

#define NVMET_MDEV_DEFAULT_PIN_CACHE_PAGES	65536
#define NVMET_MDEV_DEFAULT_PIN_CACHE_MAX_SEGS	64
#define NVMET_MDEV_DEFAULT_POLL_BUDGET		128
#define NVMET_MDEV_DEFAULT_RESPONSE_WORKERS	0
#define NVMET_MDEV_DEFAULT_IRQ_COALESCE_THR	0
#define NVMET_MDEV_DEFAULT_IRQ_COALESCE_TIME	0
#define NVMET_MDEV_DEFAULT_LOCK_IRQ_COALESCING	false
#define NVMET_MDEV_DEFAULT_MMAP_DOORBELLS	true

static uint pin_cache_pages = NVMET_MDEV_DEFAULT_PIN_CACHE_PAGES;
module_param_named(pin_cache_pages, pin_cache_pages, uint, 0644);
MODULE_PARM_DESC(pin_cache_pages,
		 "Maximum cached guest pages per controller (default: "
		 __stringify(NVMET_MDEV_DEFAULT_PIN_CACHE_PAGES) ")");

static uint pin_cache_max_segs = NVMET_MDEV_DEFAULT_PIN_CACHE_MAX_SEGS;
module_param_named(pin_cache_max_segs, pin_cache_max_segs, uint, 0644);
MODULE_PARM_DESC(pin_cache_max_segs,
		 "Maximum PRP segments admitted to the pin cache (default: "
		 __stringify(NVMET_MDEV_DEFAULT_PIN_CACHE_MAX_SEGS) ")");

static uint poll_budget = NVMET_MDEV_DEFAULT_POLL_BUDGET;
module_param_named(poll_budget, poll_budget, uint, 0644);
MODULE_PARM_DESC(poll_budget,
		 "Maximum queue pairs examined by one doorbell scan (default: "
		 __stringify(NVMET_MDEV_DEFAULT_POLL_BUDGET) ")");

static uint response_workers = NVMET_MDEV_DEFAULT_RESPONSE_WORKERS;
module_param_named(response_workers, response_workers, uint, 0644);
MODULE_PARM_DESC(response_workers,
		 "Response cleanup workers per I/O SQ (default: "
		 __stringify(NVMET_MDEV_DEFAULT_RESPONSE_WORKERS) ")");

static u8 irq_coalesce_threshold = NVMET_MDEV_DEFAULT_IRQ_COALESCE_THR;
module_param_named(irq_coalesce_threshold, irq_coalesce_threshold, byte, 0644);
MODULE_PARM_DESC(irq_coalesce_threshold,
		 "Initial NVMe Feature 08 THR value (default: "
		 __stringify(NVMET_MDEV_DEFAULT_IRQ_COALESCE_THR) ")");

static u8 irq_coalesce_time = NVMET_MDEV_DEFAULT_IRQ_COALESCE_TIME;
module_param_named(irq_coalesce_time, irq_coalesce_time, byte, 0644);
MODULE_PARM_DESC(irq_coalesce_time,
		 "Initial NVMe Feature 08 TIME in 100 us units (default: "
		 __stringify(NVMET_MDEV_DEFAULT_IRQ_COALESCE_TIME) ")");

static bool lock_irq_coalescing = NVMET_MDEV_DEFAULT_LOCK_IRQ_COALESCING;
module_param_named(lock_irq_coalescing, lock_irq_coalescing, bool, 0644);
MODULE_PARM_DESC(lock_irq_coalescing,
		 "Reject guest changes to NVMe Features 08 and 09 (default: "
		 __stringify(NVMET_MDEV_DEFAULT_LOCK_IRQ_COALESCING) ")");

static bool mmap_doorbells = NVMET_MDEV_DEFAULT_MMAP_DOORBELLS;
module_param_named(mmap_doorbells, mmap_doorbells, bool, 0644);
MODULE_PARM_DESC(mmap_doorbells,
		 "Use VFIO sparse mmap for the 4 KiB BAR0 doorbell page (default: true)");

void nvmet_mdev_snapshot_runtime_config(struct nvmet_mdev_ctrl *ctrl)
{
	struct nvmet_mdev_runtime_config *cfg = &ctrl->runtime;

	cfg->pin_cache_pages = READ_ONCE(pin_cache_pages);
	cfg->pin_cache_max_segs = READ_ONCE(pin_cache_max_segs);
	cfg->poll_budget = max_t(unsigned int, READ_ONCE(poll_budget), 1);
	cfg->response_workers = READ_ONCE(response_workers);
	cfg->irq_coalesce_threshold = READ_ONCE(irq_coalesce_threshold);
	cfg->irq_coalesce_time = READ_ONCE(irq_coalesce_time);
	cfg->lock_irq_coalescing = READ_ONCE(lock_irq_coalescing);

	/* Sparse mmap exposes exactly one 4 KiB NVMe doorbell page. */
	cfg->mmap_doorbells = READ_ONCE(mmap_doorbells) && PAGE_SIZE == SZ_4K;
	/* KVM tracking is an optional wakeup for that shared page, not a mode. */
	cfg->kvm_doorbell_tracking = cfg->mmap_doorbells &&
		nvmet_mdev_kvm_tracking_requested();
}
