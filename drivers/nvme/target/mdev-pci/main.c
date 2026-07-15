// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>

#include "priv.h"

static int __init nvmet_mdev_init(void)
{
	int ret;

	ret = nvmet_mdev_vfio_init();
	if (ret)
		return ret;

	ret = nvmet_register_transport(&nvmet_mdev_fabrics_ops);
	if (ret)
		nvmet_mdev_vfio_exit();

	return ret;
}

static void __exit nvmet_mdev_exit(void)
{
	nvmet_unregister_transport(&nvmet_mdev_fabrics_ops);
	nvmet_mdev_vfio_exit();
}

module_init(nvmet_mdev_init);
module_exit(nvmet_mdev_exit);

MODULE_DESCRIPTION("NVMe mediated VFIO PCI target driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("nvmet-transport-mdev-pci");
