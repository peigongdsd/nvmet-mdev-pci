// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/slab.h>

#include "priv.h"

static void nvmet_mdev_port_release(struct device *dev)
{
	struct nvmet_mdev_port *mport =
		container_of(dev, struct nvmet_mdev_port, dev);

	kfree(mport);
}

static int nvmet_mdev_add_port(struct nvmet_port *port)
{
	struct nvmet_mdev_port *mport;
	int ret;

	mport = kzalloc_obj(*mport);
	if (!mport)
		return -ENOMEM;

	mport->port = port;
	mport->type.sysfs_name = "nvme";
	mport->type.pretty_name = "NVMe PCI controller";
	mport->typep = &mport->type;

	device_initialize(&mport->dev);
	mport->dev.class = nvmet_mdev_class();
	mport->dev.release = nvmet_mdev_port_release;
	dev_set_drvdata(&mport->dev, mport);

	ret = dev_set_name(&mport->dev, "nvmet-mdev-pci-%s",
			   config_item_name(&port->group.cg_item));
	if (ret)
		goto out_put_device;

	ret = device_add(&mport->dev);
	if (ret)
		goto out_put_device;

	ret = mdev_register_parent(&mport->parent, &mport->dev,
				   nvmet_mdev_driver(), &mport->typep, 1);
	if (ret)
		goto out_unregister_device;

	port->priv = mport;

	return 0;

out_unregister_device:
	device_unregister(&mport->dev);
	return ret;
out_put_device:
	put_device(&mport->dev);
	return ret;
}

static void nvmet_mdev_remove_port(struct nvmet_port *port)
{
	struct nvmet_mdev_port *mport = port->priv;

	if (!mport)
		return;

	port->priv = NULL;
	mdev_unregister_parent(&mport->parent);
	device_unregister(&mport->dev);
}

const struct nvmet_fabrics_ops nvmet_mdev_fabrics_ops = {
	.owner		= THIS_MODULE,
	.name		= "mdev-pci",
	.type		= NVMF_TRTYPE_PCI,
	.flags		= NVMF_NO_SGLS,
	.add_port	= nvmet_mdev_add_port,
	.remove_port	= nvmet_mdev_remove_port,
	.delete_ctrl	= nvmet_mdev_delete_ctrl,
	.queue_response = nvmet_mdev_queue_response,
	.get_mdts	= nvmet_mdev_get_mdts,
	.get_max_queue_size = nvmet_mdev_get_max_queue_size,
	.create_sq	= nvmet_mdev_create_sq,
	.delete_sq	= nvmet_mdev_delete_sq,
	.create_cq	= nvmet_mdev_create_cq,
	.delete_cq	= nvmet_mdev_delete_cq,
	.get_feature	= nvmet_mdev_get_feature,
	.set_feature	= nvmet_mdev_set_feature,
};
