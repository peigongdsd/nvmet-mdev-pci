// SPDX-License-Identifier: GPL-2.0

#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <linux/uuid.h>

#include "priv.h"

int nvmet_mdev_ctrl_init(struct nvmet_mdev_ctrl *ctrl)
{
	struct nvmet_alloc_ctrl_args args = {};
	char uuid_string[UUID_STRING_LEN + 1];
	char subsysnqn[NVMF_NQN_SIZE];
	char hostnqn[NVMF_NQN_SIZE];
	struct nvmet_ctrl *tctrl;
	uuid_t hostid;
	int ret;

	ret = nvmet_port_get_single_subsysnqn(ctrl->mport->port, subsysnqn,
					      sizeof(subsysnqn));
	if (ret) {
		dev_err(&ctrl->mdev->dev,
			"exactly one subsystem must be linked to the port\n");
		return ret;
	}

	snprintf(uuid_string, sizeof(uuid_string), "%pUl", &ctrl->mdev->uuid);
	ret = uuid_parse(uuid_string, &hostid);
	if (ret)
		return ret;
	snprintf(hostnqn, sizeof(hostnqn),
		 "nqn.2014-08.org.nvmexpress:uuid:%s", uuid_string);

	args.port = ctrl->mport->port;
	args.subsysnqn = subsysnqn;
	args.hostid = &hostid;
	args.hostnqn = hostnqn;
	args.ops = &nvmet_mdev_fabrics_ops;

	tctrl = nvmet_alloc_ctrl(&args);
	if (!tctrl) {
		dev_err(&ctrl->mdev->dev,
			"failed to allocate nvmet controller, status %#x\n",
			args.status);
		return -ENODEV;
	}

	if (tctrl->pi_support) {
		dev_err(&ctrl->mdev->dev,
			"protection information is not supported\n");
		nvmet_ctrl_put(tctrl);
		return -EOPNOTSUPP;
	}

	mutex_lock(&ctrl->lock);
	ctrl->tctrl = tctrl;
	rcu_assign_pointer(tctrl->drvdata, ctrl);
	nvmet_mdev_pci_bind_ctrl(ctrl);
	mutex_unlock(&ctrl->lock);

	dev_info(&ctrl->mdev->dev, "attached nvmet subsystem %s\n", subsysnqn);
	return 0;
}

void nvmet_mdev_ctrl_cleanup(struct nvmet_mdev_ctrl *ctrl)
{
	struct nvmet_ctrl *tctrl;

	mutex_lock(&ctrl->lock);
	tctrl = ctrl->tctrl;
	ctrl->tctrl = NULL;
	if (tctrl)
		rcu_assign_pointer(tctrl->drvdata, NULL);
	mutex_unlock(&ctrl->lock);

	if (tctrl) {
		synchronize_rcu();
		nvmet_ctrl_put(tctrl);
	}
}

void nvmet_mdev_delete_ctrl(struct nvmet_ctrl *tctrl)
{
	struct nvmet_mdev_ctrl *ctrl;

	rcu_read_lock();
	ctrl = rcu_dereference(tctrl->drvdata);
	if (!ctrl || !get_device(&ctrl->vdev.device)) {
		rcu_read_unlock();
		return;
	}
	rcu_read_unlock();

	mutex_lock(&ctrl->lock);
	if (ctrl->tctrl == tctrl)
		nvmet_mdev_pci_set_fatal(ctrl);
	mutex_unlock(&ctrl->lock);
	put_device(&ctrl->vdev.device);
}

u16 nvmet_mdev_get_max_queue_size(const struct nvmet_ctrl *tctrl)
{
	return tctrl->port->max_queue_size;
}
