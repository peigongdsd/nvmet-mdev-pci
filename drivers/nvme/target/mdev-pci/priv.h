/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NVMET_MDEV_PCI_PRIV_H
#define _NVMET_MDEV_PCI_PRIV_H

#include <asm/local64.h>

#include <linux/device.h>
#include <linux/eventfd.h>
#include <linux/hrtimer.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/llist.h>
#include <linux/mdev.h>
#include <linux/mempool.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/preempt.h>
#include <linux/refcount.h>
#include <linux/rwsem.h>
#include <linux/scatterlist.h>
#include <linux/seqlock.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/vfio.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

#if defined(CONFIG_X86) && IS_ENABLED(CONFIG_KVM_EXTERNAL_WRITE_TRACKING)
#include <asm/kvm_page_track.h>
#endif

#include "../nvmet.h"

#define NVMET_MDEV_PCI_CONFIG_SIZE	SZ_4K
#define NVMET_MDEV_PCI_BAR0_SIZE	SZ_16K
#define NVMET_MDEV_PCI_MSIX_VECTORS	16
#define NVMET_MDEV_PCI_MSIX_CAP		0x40
#define NVMET_MDEV_PCI_MSIX_TABLE	0x2000
#define NVMET_MDEV_PCI_MSIX_PBA		0x3000
#define NVMET_MDEV_INLINE_SEGS		32

#define NVMET_MDEV_VFIO_OFFSET_SHIFT	40
#define NVMET_MDEV_VFIO_OFFSET_MASK	\
	((1ULL << NVMET_MDEV_VFIO_OFFSET_SHIFT) - 1)
#define NVMET_MDEV_VFIO_OFFSET_TO_INDEX(offset) \
	((offset) >> NVMET_MDEV_VFIO_OFFSET_SHIFT)
#define NVMET_MDEV_VFIO_INDEX_TO_OFFSET(index) \
	((u64)(index) << NVMET_MDEV_VFIO_OFFSET_SHIFT)

struct nvmet_mdev_port {
	struct nvmet_port *port;
	struct device dev;
	struct mdev_parent parent;
	struct mdev_type type;
	struct mdev_type *typep;
};

struct nvmet_mdev_mapping {
	struct list_head entry;
	u64 iova;
	u64 length;
	u64 data_iova;
	size_t data_length;
	unsigned int npages;
	struct page **pages;
	void *vaddr;
};

struct nvmet_mdev_iova_segment {
	u64 iova;
	size_t length;
};

struct nvmet_mdev_pin_run {
	u64 iova;
	unsigned int npages;
};

struct nvmet_mdev_pin_cache_entry {
	struct list_head lru;
	struct page *page;
	refcount_t refs;
	u64 iova;
};

struct nvmet_mdev_payload_backing {
	struct sg_table sgt;
	struct nvmet_mdev_pin_run *runs;
	struct page **pages;
	struct nvmet_mdev_pin_cache_entry **cache_entries;
	unsigned int capacity;
	unsigned int runs_capacity;
	unsigned int cache_entries_capacity;
};

struct nvmet_mdev_payload {
	struct list_head entry;
	struct sg_table sgt;
	struct nvmet_mdev_pin_run *runs;
	struct page **pages;
	struct nvmet_mdev_pin_cache_entry **cache_entries;
	struct scatterlist inline_sg[NVMET_MDEV_INLINE_SEGS];
	struct nvmet_mdev_pin_run inline_runs[NVMET_MDEV_INLINE_SEGS];
	struct page *inline_pages[NVMET_MDEV_INLINE_SEGS];
	struct nvmet_mdev_pin_cache_entry *inline_cache_entries[NVMET_MDEV_INLINE_SEGS];
	struct nvmet_mdev_payload_backing backing;
	unsigned int nr_runs;
	unsigned int nr_entries;
	bool active;
	bool cached;
};

struct nvmet_mdev_runtime_config {
	/* Capacity and scheduling policy, snapshotted at mdev creation. */
	unsigned int pin_cache_pages;
	unsigned int pin_cache_max_segs;
	unsigned int poll_budget;
	unsigned int response_workers;
	u8 irq_coalesce_threshold;
	u8 irq_coalesce_time;
	bool lock_irq_coalescing;
	/* Resolved transport capabilities, not independent feature switches. */
	bool mmap_doorbells;
	bool kvm_doorbell_tracking;
};

struct nvmet_mdev_stats {
	local64_t commands;
	local64_t pinned_io_bytes;
	local64_t completions;
	local64_t interrupts;
	local64_t doorbell_kicks;
	local64_t poll_runs;
	local64_t poll_queue_checks;
	local64_t sq_tail_changes;
	local64_t prp_heap_allocs;
	local64_t payload_sg_heap_allocs;
	local64_t pin_calls;
	local64_t unpin_calls;
	local64_t pin_cache_hits;
	local64_t pin_cache_misses;
	local64_t pin_cache_evictions;
	local64_t pin_cache_permission_fallbacks;
	local64_t payload_dma_lock_contentions;
	local64_t response_work_runs;
	local64_t response_batches;
	local64_t response_items;
	local64_t iod_cache_hits;
	local64_t iod_cache_misses;
	local64_t sq_work_runs;
	local64_t cq_work_runs;
	local64_t poll_wakeups;
	local64_t poll_sleeps;
	local64_t fast_doorbell_writes;
	local64_t doorbell_mmaps;
	local64_t kvm_track_writes;
	local64_t kvm_track_activations;
	local64_t kvm_track_retries;
	local64_t kvm_track_failures;
	local64_t cq_head_wakeups;
	local64_t interrupt_suppressed;
	local64_t interrupt_resignals;
	local64_t sq_runner_requeues;
	local64_t cq_publisher_requeues;
};

#if defined(CONFIG_X86) && IS_ENABLED(CONFIG_KVM_EXTERNAL_WRITE_TRACKING)
struct nvmet_mdev_kvm_tracker {
	struct kvm_page_track_notifier_node notifier;
	struct delayed_work activate_work;
	/* Serializes notifier, BAR mapping and activation lifecycle state. */
	struct mutex lock;
	unsigned long doorbell_hva;
	gfn_t doorbell_gfn;
	unsigned int retries;
	bool registered;
	bool active;
	bool closing;
};
#endif

#define nvmet_mdev_stat_add(ctrl, member, value) do { \
	struct nvmet_mdev_stats *__stats; \
	preempt_disable(); \
	__stats = this_cpu_ptr((ctrl)->stats); \
	local64_add((value), &__stats->member); \
	preempt_enable(); \
} while (0)

#define nvmet_mdev_stat_inc(ctrl, member) \
	nvmet_mdev_stat_add(ctrl, member, 1)

#define nvmet_mdev_stat_read(ctrl, member) ({ \
	u64 __total = 0; \
	int __cpu; \
	for_each_possible_cpu(__cpu) \
		__total += local64_read( \
			&per_cpu_ptr((ctrl)->stats, __cpu)->member); \
	__total; \
})

struct nvmet_mdev_ctrl;

struct nvmet_mdev_irq_vector {
	struct nvmet_mdev_ctrl *ctrl;
	struct hrtimer timer;
	struct work_struct work;
	/* Protects pending completion count and timer decisions. */
	spinlock_t lock;
	unsigned int vector;
	unsigned int pending;
	bool coalescing_disabled;
};

struct nvmet_mdev_cq;
struct nvmet_mdev_sq;

struct nvmet_mdev_response_lane {
	struct nvmet_mdev_sq *sq;
	struct work_struct work;
	/* Protects responses and work_queued. */
	spinlock_t lock;
	struct list_head responses;
	atomic_t work_queued;
};

struct nvmet_mdev_sq {
	struct nvmet_mdev_ctrl *ctrl;
	struct nvmet_mdev_cq *cq;
	struct nvmet_sq nvme_sq;
	struct nvmet_mdev_mapping *mapping;
	struct work_struct work;
	struct workqueue_struct *iod_wq;
	struct nvmet_mdev_response_lane *response_lanes;
	unsigned int nr_response_lanes;
	u8 *entries;
	u16 qid;
	u16 depth;
	u16 head;
	/* Written only by the doorbell poller while this SQ is live. */
	u32 polled_tail;
	/* runner_active owns SQ head; kick_pending closes its release race. */
	atomic_t runner_active;
	atomic_t kick_pending;
	bool live;
};

struct nvmet_mdev_cq {
	struct nvmet_mdev_ctrl *ctrl;
	struct nvmet_cq nvme_cq;
	struct nvmet_mdev_mapping *mapping;
	struct work_struct work;
	struct llist_head completions;
	/* FIFO snapshot owned exclusively by the CQ publisher. */
	struct llist_node *pending_completions;
	/* One publisher owns tail/phase; kick_pending closes its release race. */
	atomic_t publisher_active;
	atomic_t kick_pending;
	/* Cleared by an observed CQ-head acknowledgement. */
	atomic_t irq_outstanding;
	/* Set only while pending completions cannot fit in the guest CQ. */
	atomic_t blocked;
	u8 *entries;
	u16 qid;
	u16 depth;
	atomic_t head;
	u16 tail;
	u16 phase;
	u16 vector;
	bool live;
	bool irq_enabled;
};

struct nvmet_mdev_ctrl {
	struct vfio_device vdev;
	struct mdev_device *mdev;
	struct nvmet_mdev_port *mport;
	/* Lock order: state_lock -> dma_pin_lock -> lock -> dma_lock. */
	/* Serializes controller enable, disable and queue teardown. */
	struct mutex state_lock;
	/* Protects controller lifecycle registers and non-MSI-X PCI state. */
	struct mutex lock;
	/* Serializes MSI-X configuration and eventfd replacement. */
	spinlock_t irq_state_lock;
	/* Lets signalers retry concurrent configuration or eventfd changes. */
	seqcount_t irq_state_seq;
	/* Excludes VFIO invalidation from request pin admission. */
	struct rw_semaphore dma_pin_lock;
	/* Protects active payload metadata, the pin cache and blocked state. */
	struct mutex dma_lock;
	u8 *config;
	u8 *bar0;
	struct eventfd_ctx __rcu *irq_ctx[NVMET_MDEV_PCI_MSIX_VECTORS];
	struct nvmet_mdev_irq_vector irq_vectors[NVMET_MDEV_PCI_MSIX_VECTORS];
	u8 irq_coalesce_threshold;
	u8 irq_coalesce_time;
	struct nvmet_ctrl *tctrl;
	struct list_head mappings;
	struct list_head payloads;
	struct xarray pin_cache;
	struct list_head pin_cache_lru;
	unsigned int pin_cache_nr_pages;
	struct nvmet_mdev_sq *sqs;
	struct nvmet_mdev_cq *cqs;
	struct nvmet_mdev_mapping *dbbuf_dbs_mapping;
	struct nvmet_mdev_mapping *dbbuf_eis_mapping;
	__le32 *dbbuf_dbs;
	__le32 *dbbuf_eis;
	struct delayed_work poll_work;
	struct task_struct *poll_thread;
	wait_queue_head_t poll_wait;
	atomic_t poll_kick;
	unsigned long *poll_qids;
	unsigned int poll_next_qid;
	struct nvmet_mdev_stats __percpu *stats;
	struct nvmet_mdev_runtime_config runtime;
#if defined(CONFIG_X86) && IS_ENABLED(CONFIG_KVM_EXTERNAL_WRITE_TRACKING)
	struct nvmet_mdev_kvm_tracker kvm_tracker;
#endif
	mempool_t iod_pool;
	struct llist_head iod_free;
	atomic_t iod_free_count;
	unsigned int iod_free_limit;
	u16 nr_queues;
	bool iod_pool_ready;
	bool enabled;
	bool dma_blocked;
};

extern const struct nvmet_fabrics_ops nvmet_mdev_fabrics_ops;

int nvmet_mdev_vfio_init(void);
void nvmet_mdev_vfio_exit(void);
const struct class *nvmet_mdev_class(void);
struct mdev_driver *nvmet_mdev_driver(void);

void nvmet_mdev_snapshot_runtime_config(struct nvmet_mdev_ctrl *ctrl);

int nvmet_mdev_pci_init(struct nvmet_mdev_ctrl *ctrl);
void nvmet_mdev_pci_cleanup(struct nvmet_mdev_ctrl *ctrl);
void nvmet_mdev_pci_reset(struct nvmet_mdev_ctrl *ctrl);
void nvmet_mdev_pci_bind_ctrl(struct nvmet_mdev_ctrl *ctrl);
void nvmet_mdev_pci_set_fatal(struct nvmet_mdev_ctrl *ctrl);
ssize_t nvmet_mdev_pci_read(struct nvmet_mdev_ctrl *ctrl, char __user *buf,
			    size_t count, loff_t *ppos);
ssize_t nvmet_mdev_pci_write(struct nvmet_mdev_ctrl *ctrl,
			     const char __user *buf, size_t count,
			     loff_t *ppos);
int nvmet_mdev_pci_mmap(struct nvmet_mdev_ctrl *ctrl,
			struct vm_area_struct *vma);

void nvmet_mdev_irq_cleanup(struct nvmet_mdev_ctrl *ctrl);
void nvmet_mdev_irq_init(struct nvmet_mdev_ctrl *ctrl);
void nvmet_mdev_irq_quiesce(struct nvmet_mdev_ctrl *ctrl);
int nvmet_mdev_irq_info(struct vfio_irq_info *info);
int nvmet_mdev_set_irqs(struct nvmet_mdev_ctrl *ctrl,
			struct vfio_irq_set *hdr, void *data);
void nvmet_mdev_signal_irq(struct nvmet_mdev_ctrl *ctrl, unsigned int vector);
void nvmet_mdev_notify_irq(struct nvmet_mdev_ctrl *ctrl, unsigned int vector,
			   unsigned int completions, bool force);
void nvmet_mdev_update_pending_irqs(struct nvmet_mdev_ctrl *ctrl);

int nvmet_mdev_map_guest(struct nvmet_mdev_ctrl *ctrl, u64 iova,
			 size_t length, int prot,
			 struct nvmet_mdev_mapping **mappingp);
void nvmet_mdev_unmap_guest(struct nvmet_mdev_ctrl *ctrl,
			    struct nvmet_mdev_mapping *mapping);
void nvmet_mdev_unmap_all(struct nvmet_mdev_ctrl *ctrl);
void nvmet_mdev_dma_unmap(struct nvmet_mdev_ctrl *ctrl, u64 iova, u64 length);
void *nvmet_mdev_mapping_addr(const struct nvmet_mdev_mapping *mapping);
int nvmet_mdev_pin_payload(struct nvmet_mdev_ctrl *ctrl,
			   const struct nvmet_mdev_iova_segment *segments,
			   unsigned int nr_segments, int prot,
			   struct nvmet_mdev_payload *payload);
void nvmet_mdev_unpin_payload(struct nvmet_mdev_ctrl *ctrl,
			      struct nvmet_mdev_payload *payload);
void nvmet_mdev_payload_destroy(struct nvmet_mdev_payload *payload);
void nvmet_mdev_iova_init(struct nvmet_mdev_ctrl *ctrl);
void nvmet_mdev_iova_reset(struct nvmet_mdev_ctrl *ctrl);
void nvmet_mdev_iova_cleanup(struct nvmet_mdev_ctrl *ctrl);

int nvmet_mdev_queue_init(struct nvmet_mdev_ctrl *ctrl);
void nvmet_mdev_queue_cleanup(struct nvmet_mdev_ctrl *ctrl);
int nvmet_mdev_enable_ctrl(struct nvmet_mdev_ctrl *ctrl, u32 cc);
void nvmet_mdev_disable_ctrl(struct nvmet_mdev_ctrl *ctrl, u32 cc);
void nvmet_mdev_disable_ctrl_locked(struct nvmet_mdev_ctrl *ctrl, u32 cc);
void nvmet_mdev_schedule_doorbell(struct nvmet_mdev_ctrl *ctrl, u16 qid,
				  bool cq);
void nvmet_mdev_rescan_doorbells(struct nvmet_mdev_ctrl *ctrl);

#if defined(CONFIG_X86) && IS_ENABLED(CONFIG_KVM_EXTERNAL_WRITE_TRACKING)
bool nvmet_mdev_kvm_tracking_requested(void);
void nvmet_mdev_kvm_tracking_init(struct nvmet_mdev_ctrl *ctrl);
int nvmet_mdev_kvm_tracking_open(struct nvmet_mdev_ctrl *ctrl);
void nvmet_mdev_kvm_tracking_close(struct nvmet_mdev_ctrl *ctrl);
void nvmet_mdev_kvm_tracking_mmap(struct nvmet_mdev_ctrl *ctrl, unsigned long hva);
void nvmet_mdev_kvm_tracking_config_changed(struct nvmet_mdev_ctrl *ctrl);
bool nvmet_mdev_kvm_tracking_active(const struct nvmet_mdev_ctrl *ctrl);
#else
static inline bool nvmet_mdev_kvm_tracking_requested(void)
{
	return false;
}

static inline void nvmet_mdev_kvm_tracking_init(struct nvmet_mdev_ctrl *ctrl)
{
}

static inline int nvmet_mdev_kvm_tracking_open(struct nvmet_mdev_ctrl *ctrl)
{
	return 0;
}

static inline void nvmet_mdev_kvm_tracking_close(struct nvmet_mdev_ctrl *ctrl)
{
}

static inline void nvmet_mdev_kvm_tracking_mmap(struct nvmet_mdev_ctrl *ctrl,
						unsigned long hva)
{
}

static inline void
nvmet_mdev_kvm_tracking_config_changed(struct nvmet_mdev_ctrl *ctrl)
{
}

static inline bool
nvmet_mdev_kvm_tracking_active(const struct nvmet_mdev_ctrl *ctrl)
{
	return false;
}
#endif
void nvmet_mdev_queue_response(struct nvmet_req *req);
u8 nvmet_mdev_get_mdts(const struct nvmet_ctrl *tctrl);
u16 nvmet_mdev_create_sq(struct nvmet_ctrl *tctrl, u16 sqid, u16 cqid,
			 u16 flags, u16 qsize, u64 prp1);
u16 nvmet_mdev_delete_sq(struct nvmet_ctrl *tctrl, u16 sqid);
u16 nvmet_mdev_create_cq(struct nvmet_ctrl *tctrl, u16 cqid, u16 flags,
			 u16 qsize, u64 prp1, u16 irq_vector);
u16 nvmet_mdev_delete_cq(struct nvmet_ctrl *tctrl, u16 cqid);
u16 nvmet_mdev_get_feature(const struct nvmet_ctrl *tctrl, u8 feature,
			   void *data);
u16 nvmet_mdev_set_feature(const struct nvmet_ctrl *tctrl, u8 feature,
			   void *data);
u16 nvmet_mdev_set_dbbuf(struct nvmet_ctrl *tctrl, u64 dbs, u64 eis);

int nvmet_mdev_ctrl_init(struct nvmet_mdev_ctrl *ctrl);
void nvmet_mdev_ctrl_cleanup(struct nvmet_mdev_ctrl *ctrl);
void nvmet_mdev_delete_ctrl(struct nvmet_ctrl *tctrl);
u16 nvmet_mdev_get_max_queue_size(const struct nvmet_ctrl *tctrl);

#endif /* _NVMET_MDEV_PCI_PRIV_H */
