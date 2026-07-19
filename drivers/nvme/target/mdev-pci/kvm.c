// SPDX-License-Identifier: GPL-2.0

#include <linux/kvm_host.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/unaligned.h>

#include "priv.h"

#define NVMET_MDEV_KVM_TRACK_RETRY_MS	20
#define NVMET_MDEV_KVM_TRACK_RETRIES	50

static bool kvm_doorbell_tracking = true;
module_param_named(kvm_doorbell_tracking, kvm_doorbell_tracking, bool, 0644);
MODULE_PARM_DESC(kvm_doorbell_tracking,
		 "Use KVM tracking for sparse BAR0 doorbells (default: true)");

bool nvmet_mdev_kvm_tracking_requested(void)
{
	return READ_ONCE(kvm_doorbell_tracking);
}

static bool nvmet_mdev_kvm_bar0_gfn(struct nvmet_mdev_ctrl *ctrl, gfn_t *gfn)
{
	u64 bar0;
	u32 low, high;
	u16 command;

	mutex_lock(&ctrl->lock);
	command = get_unaligned_le16(ctrl->config + PCI_COMMAND);
	low = get_unaligned_le32(ctrl->config + PCI_BASE_ADDRESS_0);
	high = get_unaligned_le32(ctrl->config + PCI_BASE_ADDRESS_1);
	mutex_unlock(&ctrl->lock);

	if (!(command & PCI_COMMAND_MEMORY))
		return false;
	bar0 = ((u64)high << 32) | (low & PCI_BASE_ADDRESS_MEM_MASK);
	if (!bar0 || !IS_ALIGNED(bar0, NVMET_MDEV_PCI_BAR0_SIZE))
		return false;

	*gfn = gpa_to_gfn(bar0 + NVME_REG_DBS);
	return true;
}

static bool nvmet_mdev_kvm_memslot_matches(struct kvm *kvm, gfn_t gfn,
					   unsigned long hva)
{
	struct kvm_memory_slot *slot;
	int idx;
	bool matches = false;

	idx = srcu_read_lock(&kvm->srcu);
	slot = __gfn_to_memslot(kvm_memslots(kvm), gfn);
	if (slot)
		matches = __gfn_to_hva_memslot(slot, gfn) == hva;
	srcu_read_unlock(&kvm->srcu, idx);
	return matches;
}

static void nvmet_mdev_kvm_retry(struct nvmet_mdev_ctrl *ctrl)
{
	struct nvmet_mdev_kvm_tracker *tracker = &ctrl->kvm_tracker;

	if (++tracker->retries >= NVMET_MDEV_KVM_TRACK_RETRIES) {
		nvmet_mdev_stat_inc(ctrl, kvm_track_failures);
		dev_warn(ctrl->vdev.dev,
			 "KVM doorbell write tracking unavailable; retaining polling\n");
		return;
	}
	nvmet_mdev_stat_inc(ctrl, kvm_track_retries);
	mod_delayed_work(system_unbound_wq, &tracker->activate_work,
			 msecs_to_jiffies(NVMET_MDEV_KVM_TRACK_RETRY_MS));
}

static void nvmet_mdev_kvm_activate_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct nvmet_mdev_kvm_tracker *tracker;
	struct nvmet_mdev_ctrl *ctrl;
	struct kvm *kvm;
	unsigned long hva;
	gfn_t gfn, old_gfn = 0;
	bool remove_old = false;
	int ret;

	tracker = container_of(dwork, struct nvmet_mdev_kvm_tracker,
			       activate_work);
	ctrl = container_of(tracker, struct nvmet_mdev_ctrl, kvm_tracker);
	kvm = READ_ONCE(ctrl->vdev.kvm);

	mutex_lock(&tracker->lock);
	if (tracker->closing || !tracker->registered || !kvm) {
		mutex_unlock(&tracker->lock);
		return;
	}
	hva = tracker->doorbell_hva;
	if (!hva || !nvmet_mdev_kvm_bar0_gfn(ctrl, &gfn)) {
		mutex_unlock(&tracker->lock);
		return;
	}
	if (tracker->active && tracker->doorbell_gfn == gfn &&
	    nvmet_mdev_kvm_memslot_matches(kvm, gfn, hva)) {
		mutex_unlock(&tracker->lock);
		return;
	}
	if (tracker->active) {
		old_gfn = tracker->doorbell_gfn;
		WRITE_ONCE(tracker->active, false);
		remove_old = true;
	}
	mutex_unlock(&tracker->lock);

	if (remove_old) {
		kvm_write_track_remove_gfn(kvm, old_gfn);
		wake_up_interruptible(&ctrl->poll_wait);
	}

	if (!nvmet_mdev_kvm_memslot_matches(kvm, gfn, hva)) {
		mutex_lock(&tracker->lock);
		if (!tracker->closing && tracker->registered)
			nvmet_mdev_kvm_retry(ctrl);
		mutex_unlock(&tracker->lock);
		return;
	}

	ret = kvm_write_track_add_gfn(kvm, gfn);
	if (ret) {
		mutex_lock(&tracker->lock);
		if (!tracker->closing && tracker->registered)
			nvmet_mdev_kvm_retry(ctrl);
		mutex_unlock(&tracker->lock);
		return;
	}

	/* Reject a stale BAR-to-memslot match that changed during activation. */
	if (!nvmet_mdev_kvm_memslot_matches(kvm, gfn, hva)) {
		kvm_write_track_remove_gfn(kvm, gfn);
		mutex_lock(&tracker->lock);
		if (!tracker->closing && tracker->registered)
			nvmet_mdev_kvm_retry(ctrl);
		mutex_unlock(&tracker->lock);
		return;
	}

	mutex_lock(&tracker->lock);
	if (tracker->closing || !tracker->registered ||
	    tracker->doorbell_hva != hva) {
		mutex_unlock(&tracker->lock);
		kvm_write_track_remove_gfn(kvm, gfn);
		return;
	}
	WRITE_ONCE(tracker->doorbell_gfn, gfn);
	/* Publish the tracked GFN before callbacks and the poller observe active. */
	smp_store_release(&tracker->active, true);
	tracker->retries = 0;
	mutex_unlock(&tracker->lock);

	nvmet_mdev_stat_inc(ctrl, kvm_track_activations);
	/* Catch a direct write that completed before KVM installed protection. */
	nvmet_mdev_rescan_doorbells(ctrl);
	/* A parked poller must observe the newly authoritative tracking path. */
	wake_up_interruptible(&ctrl->poll_wait);
}

static void
nvmet_mdev_kvm_track_write(gpa_t gpa, const u8 *value, int bytes,
			   struct kvm_page_track_notifier_node *node)
{
	struct nvmet_mdev_kvm_tracker *tracker;
	struct nvmet_mdev_ctrl *ctrl;
	gpa_t page_base, end;
	unsigned int first, last, db;

	(void)value;
	if (bytes <= 0 || check_add_overflow(gpa, bytes, &end))
		return;
	tracker = container_of(node, struct nvmet_mdev_kvm_tracker, notifier);
	/* Pairs with activation's publication of doorbell_gfn. */
	if (!smp_load_acquire(&tracker->active) ||
	    gpa_to_gfn(gpa) != READ_ONCE(tracker->doorbell_gfn))
		return;
	ctrl = container_of(tracker, struct nvmet_mdev_ctrl, kvm_tracker);

	mutex_lock(&tracker->lock);
	if (!tracker->active || tracker->closing ||
	    gpa_to_gfn(gpa) != tracker->doorbell_gfn) {
		mutex_unlock(&tracker->lock);
		return;
	}
	page_base = gfn_to_gpa(tracker->doorbell_gfn);
	if (gpa < page_base || gpa >= page_base + SZ_4K) {
		mutex_unlock(&tracker->lock);
		return;
	}
	end = min_t(gpa_t, end, page_base + SZ_4K);
	first = (gpa - page_base) / sizeof(u32);
	last = (end - 1 - page_base) / sizeof(u32);
	nvmet_mdev_stat_inc(ctrl, kvm_track_writes);
	for (db = first; db <= last; db++)
		nvmet_mdev_schedule_doorbell(ctrl, db / 2, db & 1);
	mutex_unlock(&tracker->lock);
}

static void
nvmet_mdev_kvm_track_remove_region(gfn_t gfn, unsigned long nr_pages,
				   struct kvm_page_track_notifier_node *node)
{
	struct nvmet_mdev_kvm_tracker *tracker;
	struct nvmet_mdev_ctrl *ctrl;
	bool removed = false;

	tracker = container_of(node, struct nvmet_mdev_kvm_tracker, notifier);
	ctrl = container_of(tracker, struct nvmet_mdev_ctrl, kvm_tracker);
	mutex_lock(&tracker->lock);
	if (tracker->active && tracker->doorbell_gfn >= gfn &&
	    tracker->doorbell_gfn - gfn < nr_pages) {
		WRITE_ONCE(tracker->active, false);
		WRITE_ONCE(tracker->doorbell_gfn, 0);
		removed = true;
	}
	if (removed && !tracker->closing && tracker->registered) {
		tracker->retries = 0;
		mod_delayed_work(system_unbound_wq, &tracker->activate_work,
				 msecs_to_jiffies(NVMET_MDEV_KVM_TRACK_RETRY_MS));
	}
	mutex_unlock(&tracker->lock);
	if (removed)
		wake_up_interruptible(&ctrl->poll_wait);
}

void nvmet_mdev_kvm_tracking_init(struct nvmet_mdev_ctrl *ctrl)
{
	struct nvmet_mdev_kvm_tracker *tracker = &ctrl->kvm_tracker;

	mutex_init(&tracker->lock);
	INIT_DELAYED_WORK(&tracker->activate_work,
			  nvmet_mdev_kvm_activate_work);
	tracker->notifier.track_write = nvmet_mdev_kvm_track_write;
	tracker->notifier.track_remove_region =
		nvmet_mdev_kvm_track_remove_region;
}

int nvmet_mdev_kvm_tracking_open(struct nvmet_mdev_ctrl *ctrl)
{
	struct nvmet_mdev_kvm_tracker *tracker = &ctrl->kvm_tracker;
	struct kvm *kvm = READ_ONCE(ctrl->vdev.kvm);
	int ret;

	if (!ctrl->runtime.kvm_doorbell_tracking)
		return 0;
	if (!kvm) {
		nvmet_mdev_stat_inc(ctrl, kvm_track_failures);
		dev_warn(ctrl->vdev.dev,
			 "KVM is unavailable; retaining doorbell polling\n");
		return 0;
	}

	mutex_lock(&tracker->lock);
	tracker->closing = false;
	WRITE_ONCE(tracker->active, false);
	tracker->retries = 0;
	INIT_HLIST_NODE(&tracker->notifier.node);
	mutex_unlock(&tracker->lock);
	ret = kvm_page_track_register_notifier(kvm, &tracker->notifier);
	if (ret) {
		nvmet_mdev_stat_inc(ctrl, kvm_track_failures);
		dev_warn(ctrl->vdev.dev,
			 "KVM doorbell notifier registration failed (%d); retaining polling\n",
			 ret);
		return 0;
	}

	mutex_lock(&tracker->lock);
	tracker->registered = true;
	if (tracker->doorbell_hva)
		mod_delayed_work(system_unbound_wq, &tracker->activate_work, 0);
	mutex_unlock(&tracker->lock);
	return 0;
}

void nvmet_mdev_kvm_tracking_close(struct nvmet_mdev_ctrl *ctrl)
{
	struct nvmet_mdev_kvm_tracker *tracker = &ctrl->kvm_tracker;
	struct kvm *kvm = READ_ONCE(ctrl->vdev.kvm);
	gfn_t gfn = 0;
	bool active, registered;

	mutex_lock(&tracker->lock);
	tracker->closing = true;
	mutex_unlock(&tracker->lock);
	cancel_delayed_work_sync(&tracker->activate_work);

	mutex_lock(&tracker->lock);
	active = tracker->active;
	registered = tracker->registered;
	if (active) {
		gfn = tracker->doorbell_gfn;
		WRITE_ONCE(tracker->active, false);
		WRITE_ONCE(tracker->doorbell_gfn, 0);
	}
	mutex_unlock(&tracker->lock);

	if (active && kvm)
		kvm_write_track_remove_gfn(kvm, gfn);
	if (registered && kvm)
		kvm_page_track_unregister_notifier(kvm, &tracker->notifier);

	mutex_lock(&tracker->lock);
	tracker->registered = false;
	tracker->doorbell_hva = 0;
	tracker->retries = 0;
	mutex_unlock(&tracker->lock);
	wake_up_interruptible(&ctrl->poll_wait);
}

void
nvmet_mdev_kvm_tracking_mmap(struct nvmet_mdev_ctrl *ctrl, unsigned long hva)
{
	struct nvmet_mdev_kvm_tracker *tracker = &ctrl->kvm_tracker;

	if (!ctrl->runtime.kvm_doorbell_tracking)
		return;
	mutex_lock(&tracker->lock);
	tracker->doorbell_hva = hva;
	tracker->retries = 0;
	if (!tracker->closing && tracker->registered)
		mod_delayed_work(system_unbound_wq, &tracker->activate_work, 0);
	mutex_unlock(&tracker->lock);
}

void nvmet_mdev_kvm_tracking_config_changed(struct nvmet_mdev_ctrl *ctrl)
{
	struct nvmet_mdev_kvm_tracker *tracker = &ctrl->kvm_tracker;

	if (!ctrl->runtime.kvm_doorbell_tracking)
		return;
	mutex_lock(&tracker->lock);
	tracker->retries = 0;
	if (!tracker->closing && tracker->registered && tracker->doorbell_hva)
		mod_delayed_work(system_unbound_wq, &tracker->activate_work, 0);
	mutex_unlock(&tracker->lock);
}

bool nvmet_mdev_kvm_tracking_active(const struct nvmet_mdev_ctrl *ctrl)
{
	/* Pairs with activation before parking the mmap poller. */
	return smp_load_acquire(&ctrl->kvm_tracker.active);
}
