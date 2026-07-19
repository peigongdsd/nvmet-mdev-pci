# SPDX-License-Identifier: GPL-2.0

ccflags-y				+= -I$(src)

obj-$(CONFIG_NVME_TARGET)		+= nvmet.o
obj-$(CONFIG_NVME_TARGET_LOOP)		+= nvme-loop.o
obj-$(CONFIG_NVME_TARGET_RDMA)		+= nvmet-rdma.o
obj-$(CONFIG_NVME_TARGET_FC)		+= nvmet-fc.o
obj-$(CONFIG_NVME_TARGET_FCLOOP)	+= nvme-fcloop.o
obj-$(CONFIG_NVME_TARGET_TCP)		+= nvmet-tcp.o
obj-$(CONFIG_NVME_TARGET_PCI_EPF)	+= nvmet-pci-epf.o
obj-$(CONFIG_NVME_TARGET_MDEV_PCI)	+= nvmet-mdev-pci.o
obj-$(CONFIG_NVMET_PCI_KUNIT_TEST)	+= pci-common-test.o

nvmet-y		+= core.o configfs.o admin-cmd.o fabrics-cmd.o \
			discovery.o io-cmd-file.o io-cmd-bdev.o pr.o pci-common.o
nvmet-$(CONFIG_NVME_TARGET_DEBUGFS)	+= debugfs.o
nvmet-$(CONFIG_NVME_TARGET_PASSTHRU)	+= passthru.o
nvmet-$(CONFIG_BLK_DEV_ZONED)		+= zns.o
nvmet-$(CONFIG_NVME_TARGET_AUTH)	+= fabrics-cmd-auth.o auth.o
nvme-loop-y	+= loop.o
nvmet-rdma-y	+= rdma.o
nvmet-fc-y	+= fc.o
nvme-fcloop-y	+= fcloop.o
nvmet-tcp-y	+= tcp.o
nvmet-pci-epf-y	+= pci-epf.o
nvmet-mdev-pci-y	+= mdev-pci/main.o mdev-pci/target.o \
			   mdev-pci/pci.o mdev-pci/irq.o mdev-pci/ctrl.o \
			   mdev-pci/iova.o mdev-pci/queue.o mdev-pci/vfio.o
nvmet-mdev-pci-$(CONFIG_KVM_EXTERNAL_WRITE_TRACKING) += mdev-pci/kvm.o
nvmet-$(CONFIG_TRACING)	+= trace.o
