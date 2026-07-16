// SPDX-License-Identifier: GPL-2.0

#include <linux/mdev.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/vfio.h>

#include "priv.h"

static const struct class nvmet_mdev_parent_class = {
	.name = "nvmet-mdev-pci",
};

static const struct vfio_device_ops nvmet_mdev_device_ops;

static int nvmet_mdev_init_dev(struct vfio_device *vdev)
{
	struct nvmet_mdev_ctrl *ctrl =
		container_of(vdev, struct nvmet_mdev_ctrl, vdev);
	struct mdev_device *mdev = to_mdev_device(vdev->dev);
	struct nvmet_mdev_port *mport = dev_get_drvdata(mdev->dev.parent);
	int ret;

	if (!mport)
		return -ENODEV;

	ctrl->mdev = mdev;
	ctrl->mport = mport;
	ctrl->stats = alloc_percpu(struct nvmet_mdev_stats);
	if (!ctrl->stats)
		return -ENOMEM;
	mutex_init(&ctrl->lock);
	INIT_LIST_HEAD(&ctrl->mappings);
	nvmet_mdev_iova_init(ctrl);
	nvmet_mdev_irq_init(ctrl);
	mutex_lock(&ctrl->lock);
	ret = nvmet_mdev_pci_init(ctrl);
	mutex_unlock(&ctrl->lock);
	if (ret) {
		nvmet_mdev_iova_cleanup(ctrl);
		free_percpu(ctrl->stats);
		ctrl->stats = NULL;
		return ret;
	}

	ret = nvmet_mdev_ctrl_init(ctrl);
	if (ret) {
		nvmet_mdev_pci_cleanup(ctrl);
		nvmet_mdev_iova_cleanup(ctrl);
		free_percpu(ctrl->stats);
		ctrl->stats = NULL;
		return ret;
	}

	ret = nvmet_mdev_queue_init(ctrl);
	if (ret) {
		nvmet_mdev_ctrl_cleanup(ctrl);
		nvmet_mdev_pci_cleanup(ctrl);
		nvmet_mdev_iova_cleanup(ctrl);
		free_percpu(ctrl->stats);
		ctrl->stats = NULL;
	}
	return ret;
}

static void nvmet_mdev_release_dev(struct vfio_device *vdev)
{
	struct nvmet_mdev_ctrl *ctrl =
		container_of(vdev, struct nvmet_mdev_ctrl, vdev);

	nvmet_mdev_queue_cleanup(ctrl);
	nvmet_mdev_ctrl_cleanup(ctrl);
	WARN_ON(!list_empty(&ctrl->mappings));
	WARN_ON(!list_empty(&ctrl->payloads));
	WARN_ON(!xa_empty(&ctrl->pin_cache));
	nvmet_mdev_irq_cleanup(ctrl);
	nvmet_mdev_pci_cleanup(ctrl);
	nvmet_mdev_iova_cleanup(ctrl);
	free_percpu(ctrl->stats);
	ctrl->stats = NULL;
}

static int nvmet_mdev_probe(struct mdev_device *mdev)
{
	struct nvmet_mdev_ctrl *ctrl;
	int ret;

	ctrl = vfio_alloc_device(nvmet_mdev_ctrl, vdev, &mdev->dev,
				 &nvmet_mdev_device_ops);
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	ret = vfio_register_emulated_iommu_dev(&ctrl->vdev);
	if (ret) {
		vfio_put_device(&ctrl->vdev);
		return ret;
	}

	dev_set_drvdata(&mdev->dev, ctrl);
	return 0;
}

static void nvmet_mdev_remove(struct mdev_device *mdev)
{
	struct nvmet_mdev_ctrl *ctrl = dev_get_drvdata(&mdev->dev);

	vfio_unregister_group_dev(&ctrl->vdev);
	vfio_put_device(&ctrl->vdev);
}

static ssize_t nvmet_mdev_read(struct vfio_device *vdev, char __user *buf,
			       size_t count, loff_t *ppos)
{
	struct nvmet_mdev_ctrl *ctrl =
		container_of(vdev, struct nvmet_mdev_ctrl, vdev);

	return nvmet_mdev_pci_read(ctrl, buf, count, ppos);
}

static ssize_t nvmet_mdev_write(struct vfio_device *vdev,
				const char __user *buf, size_t count,
				loff_t *ppos)
{
	struct nvmet_mdev_ctrl *ctrl =
		container_of(vdev, struct nvmet_mdev_ctrl, vdev);

	return nvmet_mdev_pci_write(ctrl, buf, count, ppos);
}

static int nvmet_mdev_region_info(struct vfio_device *vdev,
				  struct vfio_region_info *info,
				  struct vfio_info_cap *caps)
{
	if (info->index >= VFIO_PCI_NUM_REGIONS)
		return -EINVAL;

	info->offset = NVMET_MDEV_VFIO_INDEX_TO_OFFSET(info->index);
	info->size = 0;
	info->flags = 0;

	switch (info->index) {
	case VFIO_PCI_BAR0_REGION_INDEX:
		info->size = NVMET_MDEV_PCI_BAR0_SIZE;
		info->flags = VFIO_REGION_INFO_FLAG_READ |
			      VFIO_REGION_INFO_FLAG_WRITE;
		break;
	case VFIO_PCI_CONFIG_REGION_INDEX:
		info->size = NVMET_MDEV_PCI_CONFIG_SIZE;
		info->flags = VFIO_REGION_INFO_FLAG_READ |
			      VFIO_REGION_INFO_FLAG_WRITE;
		break;
	default:
		break;
	}

	return 0;
}

static long nvmet_mdev_ioctl(struct vfio_device *vdev, unsigned int cmd,
			     unsigned long arg)
{
	struct nvmet_mdev_ctrl *ctrl =
		container_of(vdev, struct nvmet_mdev_ctrl, vdev);
	unsigned long minsz;

	switch (cmd) {
	case VFIO_DEVICE_GET_INFO: {
		struct vfio_device_info info;

		minsz = offsetofend(struct vfio_device_info, num_irqs);
		if (copy_from_user(&info, (void __user *)arg, minsz))
			return -EFAULT;
		if (info.argsz < minsz)
			return -EINVAL;

		info.flags = VFIO_DEVICE_FLAGS_PCI | VFIO_DEVICE_FLAGS_RESET;
		info.num_regions = VFIO_PCI_NUM_REGIONS;
		info.num_irqs = VFIO_PCI_NUM_IRQS;
		if (copy_to_user((void __user *)arg, &info, minsz))
			return -EFAULT;
		return 0;
	}
	case VFIO_DEVICE_GET_IRQ_INFO: {
		struct vfio_irq_info info;
		int ret;

		minsz = offsetofend(struct vfio_irq_info, count);
		if (copy_from_user(&info, (void __user *)arg, minsz))
			return -EFAULT;
		if (info.argsz < minsz)
			return -EINVAL;

		ret = nvmet_mdev_irq_info(&info);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &info, minsz))
			return -EFAULT;
		return 0;
	}
	case VFIO_DEVICE_SET_IRQS: {
		struct vfio_irq_set hdr;
		size_t data_size = 0;
		void *data = NULL;
		int ret;

		minsz = offsetofend(struct vfio_irq_set, count);
		if (copy_from_user(&hdr, (void __user *)arg, minsz))
			return -EFAULT;
		ret = vfio_set_irqs_validate_and_prepare(&hdr,
							 NVMET_MDEV_PCI_MSIX_VECTORS,
							 VFIO_PCI_NUM_IRQS,
							 &data_size);
		if (ret)
			return ret;

		if (data_size) {
			data = memdup_user((void __user *)(arg + minsz), data_size);
			if (IS_ERR(data))
				return PTR_ERR(data);
		}
		ret = nvmet_mdev_set_irqs(ctrl, &hdr, data);
		kfree(data);
		return ret;
	}
	case VFIO_DEVICE_RESET:
		nvmet_mdev_disable_ctrl(ctrl, 0);
		mutex_lock(&ctrl->lock);
		nvmet_mdev_pci_reset(ctrl);
		mutex_unlock(&ctrl->lock);
		return 0;
	default:
		return -ENOTTY;
	}
}

static void nvmet_mdev_close(struct vfio_device *vdev)
{
	struct nvmet_mdev_ctrl *ctrl =
		container_of(vdev, struct nvmet_mdev_ctrl, vdev);

	nvmet_mdev_disable_ctrl(ctrl, 0);
	mutex_lock(&ctrl->lock);
	nvmet_mdev_unmap_all(ctrl);
	mutex_unlock(&ctrl->lock);
	nvmet_mdev_iova_reset(ctrl);
	nvmet_mdev_irq_cleanup(ctrl);
}

static void nvmet_mdev_vfio_dma_unmap(struct vfio_device *vdev, u64 iova,
				      u64 length)
{
	struct nvmet_mdev_ctrl *ctrl =
		container_of(vdev, struct nvmet_mdev_ctrl, vdev);

	nvmet_mdev_dma_unmap(ctrl, iova, length);
}

static const struct vfio_device_ops nvmet_mdev_device_ops = {
	.name = "nvmet-mdev-pci",
	.init = nvmet_mdev_init_dev,
	.release = nvmet_mdev_release_dev,
	.read = nvmet_mdev_read,
	.write = nvmet_mdev_write,
	.ioctl = nvmet_mdev_ioctl,
	.get_region_info_caps = nvmet_mdev_region_info,
	.close_device = nvmet_mdev_close,
	.dma_unmap = nvmet_mdev_vfio_dma_unmap,
	.bind_iommufd = vfio_iommufd_emulated_bind,
	.unbind_iommufd = vfio_iommufd_emulated_unbind,
	.attach_ioas = vfio_iommufd_emulated_attach_ioas,
	.detach_ioas = vfio_iommufd_emulated_detach_ioas,
};

static ssize_t nvmet_mdev_show_description(struct mdev_type *mtype, char *buf)
{
	return sysfs_emit(buf, "NVMe PCI controller backed by nvmet\n");
}

static ssize_t transport_stats_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct nvmet_mdev_ctrl *ctrl = dev_get_drvdata(dev);

	if (!ctrl)
		return -ENODEV;
	return sysfs_emit(buf,
		"commands %llu\npinned_io_bytes %llu\ncompletions %llu\n"
		"interrupts %llu\ndoorbell_kicks %llu\npoll_runs %llu\n"
		"poll_queue_checks %llu\nprp_heap_allocs %llu\n"
		"payload_sg_heap_allocs %llu\n"
		"pin_calls %llu\nunpin_calls %llu\n"
		"pin_cache_hits %llu\npin_cache_misses %llu\n"
		"pin_cache_evictions %llu\npin_cache_permission_fallbacks %llu\n"
		"payload_dma_lock_contentions %llu\n"
		"response_work_runs %llu\nresponse_batches %llu\n"
		"response_items %llu\niod_cache_hits %llu\niod_cache_misses %llu\n"
		"sq_work_runs %llu\ncq_work_runs %llu\n"
		"sq_batches %llu\ncq_batches %llu\npoll_wakeups %llu\n"
		"poll_sleeps %llu\nfast_doorbell_writes %llu\n"
		"cq_head_wakeups %llu\npin_cache_pages_current %u\n",
		nvmet_mdev_stat_read(ctrl, commands),
		nvmet_mdev_stat_read(ctrl, pinned_io_bytes),
		nvmet_mdev_stat_read(ctrl, completions),
		nvmet_mdev_stat_read(ctrl, interrupts),
		nvmet_mdev_stat_read(ctrl, doorbell_kicks),
		nvmet_mdev_stat_read(ctrl, poll_runs),
		nvmet_mdev_stat_read(ctrl, poll_queue_checks),
		nvmet_mdev_stat_read(ctrl, prp_heap_allocs),
		nvmet_mdev_stat_read(ctrl, payload_sg_heap_allocs),
		nvmet_mdev_stat_read(ctrl, pin_calls),
		nvmet_mdev_stat_read(ctrl, unpin_calls),
		nvmet_mdev_stat_read(ctrl, pin_cache_hits),
		nvmet_mdev_stat_read(ctrl, pin_cache_misses),
		nvmet_mdev_stat_read(ctrl, pin_cache_evictions),
		nvmet_mdev_stat_read(ctrl, pin_cache_permission_fallbacks),
		nvmet_mdev_stat_read(ctrl, payload_dma_lock_contentions),
		nvmet_mdev_stat_read(ctrl, response_work_runs),
		nvmet_mdev_stat_read(ctrl, response_batches),
		nvmet_mdev_stat_read(ctrl, response_items),
		nvmet_mdev_stat_read(ctrl, iod_cache_hits),
		nvmet_mdev_stat_read(ctrl, iod_cache_misses),
		nvmet_mdev_stat_read(ctrl, sq_work_runs),
		nvmet_mdev_stat_read(ctrl, cq_work_runs),
		nvmet_mdev_stat_read(ctrl, sq_batches),
		nvmet_mdev_stat_read(ctrl, cq_batches),
		nvmet_mdev_stat_read(ctrl, poll_wakeups),
		nvmet_mdev_stat_read(ctrl, poll_sleeps),
		nvmet_mdev_stat_read(ctrl, fast_doorbell_writes),
		nvmet_mdev_stat_read(ctrl, cq_head_wakeups),
		READ_ONCE(ctrl->pin_cache_nr_pages));
}
static DEVICE_ATTR_RO(transport_stats);

static ssize_t runtime_config_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct nvmet_mdev_ctrl *ctrl = dev_get_drvdata(dev);
	const struct nvmet_mdev_runtime_config *cfg;

	if (!ctrl)
		return -ENODEV;
	cfg = &ctrl->runtime;
	return sysfs_emit(buf,
		"pin_cache_pages %u\n"
		"pin_cache_max_segs %u\npoll_budget %u\n"
		"response_workers %u\n",
		cfg->pin_cache_pages, cfg->pin_cache_max_segs,
		cfg->poll_budget, cfg->response_workers);
}
static DEVICE_ATTR_RO(runtime_config);

static struct attribute *nvmet_mdev_attrs[] = {
	&dev_attr_transport_stats.attr,
	&dev_attr_runtime_config.attr,
	NULL,
};
ATTRIBUTE_GROUPS(nvmet_mdev);

static struct mdev_driver nvmet_mdev_vfio_driver = {
	.device_api = VFIO_DEVICE_API_PCI_STRING,
	.max_instances = 1,
	.driver = {
		.name = "nvmet-mdev-pci",
		.owner = THIS_MODULE,
		.mod_name = KBUILD_MODNAME,
		.dev_groups = nvmet_mdev_groups,
	},
	.probe = nvmet_mdev_probe,
	.remove = nvmet_mdev_remove,
	.show_description = nvmet_mdev_show_description,
};

const struct class *nvmet_mdev_class(void)
{
	return &nvmet_mdev_parent_class;
}

struct mdev_driver *nvmet_mdev_driver(void)
{
	return &nvmet_mdev_vfio_driver;
}

int nvmet_mdev_vfio_init(void)
{
	int ret;

	ret = class_register(&nvmet_mdev_parent_class);
	if (ret)
		return ret;

	ret = mdev_register_driver(&nvmet_mdev_vfio_driver);
	if (ret)
		class_unregister(&nvmet_mdev_parent_class);

	return ret;
}

void nvmet_mdev_vfio_exit(void)
{
	mdev_unregister_driver(&nvmet_mdev_vfio_driver);
	class_unregister(&nvmet_mdev_parent_class);
}
