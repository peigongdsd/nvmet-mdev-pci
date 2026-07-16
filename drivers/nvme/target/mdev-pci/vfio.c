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
	mutex_init(&ctrl->lock);
	INIT_LIST_HEAD(&ctrl->mappings);
	nvmet_mdev_iova_init(ctrl);
	nvmet_mdev_irq_init(ctrl);
	mutex_lock(&ctrl->lock);
	ret = nvmet_mdev_pci_init(ctrl);
	mutex_unlock(&ctrl->lock);
	if (ret) {
		nvmet_mdev_iova_cleanup(ctrl);
		return ret;
	}

	ret = nvmet_mdev_ctrl_init(ctrl);
	if (ret) {
		nvmet_mdev_pci_cleanup(ctrl);
		nvmet_mdev_iova_cleanup(ctrl);
		return ret;
	}

	ret = nvmet_mdev_queue_init(ctrl);
	if (ret) {
		nvmet_mdev_ctrl_cleanup(ctrl);
		nvmet_mdev_pci_cleanup(ctrl);
		nvmet_mdev_iova_cleanup(ctrl);
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
		"commands %lld\npinned_io_bytes %lld\ncompletions %lld\n"
		"interrupts %lld\ndoorbell_kicks %lld\npoll_runs %lld\n"
		"poll_queue_checks %lld\nprp_heap_allocs %lld\n"
		"payload_sg_heap_allocs %lld\n"
		"pin_calls %lld\nunpin_calls %lld\n"
		"pin_cache_hits %lld\npin_cache_misses %lld\n"
		"pin_cache_evictions %lld\npin_cache_permission_fallbacks %lld\n"
		"payload_dma_lock_contentions %lld\nsubmit_work_hops %lld\n"
		"response_work_hops %lld\nsq_work_runs %lld\ncq_work_runs %lld\n"
		"sq_batches %lld\ncq_batches %lld\npoll_wakeups %lld\n"
		"poll_sleeps %lld\npin_cache_pages_current %u\n",
		atomic64_read(&ctrl->stats.commands),
		atomic64_read(&ctrl->stats.pinned_io_bytes),
		atomic64_read(&ctrl->stats.completions),
		atomic64_read(&ctrl->stats.interrupts),
		atomic64_read(&ctrl->stats.doorbell_kicks),
		atomic64_read(&ctrl->stats.poll_runs),
		atomic64_read(&ctrl->stats.poll_queue_checks),
		atomic64_read(&ctrl->stats.prp_heap_allocs),
		atomic64_read(&ctrl->stats.payload_sg_heap_allocs),
		atomic64_read(&ctrl->stats.pin_calls),
		atomic64_read(&ctrl->stats.unpin_calls),
		atomic64_read(&ctrl->stats.pin_cache_hits),
		atomic64_read(&ctrl->stats.pin_cache_misses),
		atomic64_read(&ctrl->stats.pin_cache_evictions),
		atomic64_read(&ctrl->stats.pin_cache_permission_fallbacks),
		atomic64_read(&ctrl->stats.payload_dma_lock_contentions),
		atomic64_read(&ctrl->stats.submit_work_hops),
		atomic64_read(&ctrl->stats.response_work_hops),
		atomic64_read(&ctrl->stats.sq_work_runs),
		atomic64_read(&ctrl->stats.cq_work_runs),
		atomic64_read(&ctrl->stats.sq_batches),
		atomic64_read(&ctrl->stats.cq_batches),
		atomic64_read(&ctrl->stats.poll_wakeups),
		atomic64_read(&ctrl->stats.poll_sleeps),
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
		"pinned_io %u\ninline_data %u\npin_cache %u\n"
		"direct_submit %u\ndirect_complete %u\nlockless_io %u\n"
		"budget_poll %u\npin_cache_pages %u\n"
		"pin_cache_max_segs %u\npoll_budget %u\n",
		cfg->pinned_io, cfg->inline_data, cfg->pin_cache,
		cfg->direct_submit, cfg->direct_complete, cfg->lockless_io,
		cfg->budget_poll, cfg->pin_cache_pages, cfg->pin_cache_max_segs,
		cfg->poll_budget);
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
