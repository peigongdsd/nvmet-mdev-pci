/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NVMET_MDEV_PCI_PRIV_H
#define _NVMET_MDEV_PCI_PRIV_H

#include <linux/device.h>
#include <linux/eventfd.h>
#include <linux/hrtimer.h>
#include <linux/list.h>
#include <linux/mdev.h>
#include <linux/mempool.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/scatterlist.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/vfio.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

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
	unsigned int nr_runs;
	unsigned int nr_entries;
	bool active;
	bool cached;
};

struct nvmet_mdev_runtime_config {
	bool pinned_io;
	bool inline_data;
	bool pin_cache;
	bool direct_submit;
	bool direct_complete;
	bool lockless_io;
	bool budget_poll;
	bool fast_doorbell;
	bool msix_scan_suppress;
	bool cq_head_suppress;
	unsigned int pin_cache_pages;
	unsigned int pin_cache_max_segs;
	unsigned int poll_budget;
};

struct nvmet_mdev_stats {
	atomic64_t commands;
	atomic64_t pinned_io_bytes;
	atomic64_t completions;
	atomic64_t interrupts;
	atomic64_t doorbell_kicks;
	atomic64_t poll_runs;
	atomic64_t poll_queue_checks;
	atomic64_t prp_heap_allocs;
	atomic64_t payload_sg_heap_allocs;
	atomic64_t pin_calls;
	atomic64_t unpin_calls;
	atomic64_t pin_cache_hits;
	atomic64_t pin_cache_misses;
	atomic64_t pin_cache_evictions;
	atomic64_t pin_cache_permission_fallbacks;
	atomic64_t payload_dma_lock_contentions;
	atomic64_t submit_work_hops;
	atomic64_t response_work_hops;
	atomic64_t sq_work_runs;
	atomic64_t cq_work_runs;
	atomic64_t sq_batches;
	atomic64_t cq_batches;
	atomic64_t poll_wakeups;
	atomic64_t poll_sleeps;
	atomic64_t fast_doorbell_writes;
	atomic64_t cq_head_wakeups;
};

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

struct nvmet_mdev_sq {
	struct nvmet_mdev_ctrl *ctrl;
	struct nvmet_mdev_cq *cq;
	struct nvmet_sq nvme_sq;
	struct nvmet_mdev_mapping *mapping;
	struct work_struct work;
	struct workqueue_struct *iod_wq;
	u8 *entries;
	u16 qid;
	u16 depth;
	u16 head;
	bool live;
};

struct nvmet_mdev_cq {
	struct nvmet_mdev_ctrl *ctrl;
	struct nvmet_cq nvme_cq;
	struct nvmet_mdev_mapping *mapping;
	struct work_struct work;
	/* Shared with block-completion softirq; always acquire with irqsave. */
	spinlock_t lock;
	struct list_head completions;
	atomic_t work_queued;
	/* Protected by lock. */
	bool blocked;
	u8 *entries;
	u16 qid;
	u16 depth;
	u16 head;
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
	/* Lock order for nested acquisition: state_lock -> lock -> dma_lock. */
	/* Serializes controller enable, disable and queue teardown. */
	struct mutex state_lock;
	/* Protects PCI config, BAR0 and interrupt eventfd state. */
	struct mutex lock;
	/* Protects payload pins, the pin cache and DMA invalidation state. */
	struct mutex dma_lock;
	u8 *config;
	u8 *bar0;
	struct eventfd_ctx *irq_ctx[NVMET_MDEV_PCI_MSIX_VECTORS];
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
	unsigned long poll_busy_until;
	unsigned int poll_next_qid;
	struct nvmet_mdev_stats stats;
	struct nvmet_mdev_runtime_config runtime;
	mempool_t iod_pool;
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
