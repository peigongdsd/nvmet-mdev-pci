// SPDX-License-Identifier: GPL-2.0

#include <kunit/test.h>

#include "nvmet.h"
#include "pci-common.h"

static void nvmet_pci_cq_full_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, nvmet_pci_cq_full(4, 3, 8));
	KUNIT_EXPECT_TRUE(test, nvmet_pci_cq_full(0, 7, 8));
	KUNIT_EXPECT_FALSE(test, nvmet_pci_cq_full(3, 3, 8));
	KUNIT_EXPECT_FALSE(test, nvmet_pci_cq_full(7, 3, 8));
}

static void nvmet_pci_prp_helpers_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, nvmet_pci_prp2_valid(0x2000, SZ_4K));
	KUNIT_EXPECT_FALSE(test, nvmet_pci_prp2_valid(0x2100, SZ_4K));
	KUNIT_EXPECT_TRUE(test, nvmet_pci_prp2_valid(0x2100, SZ_8K));
	KUNIT_EXPECT_FALSE(test, nvmet_pci_prp2_valid(0x2104, SZ_8K));
	KUNIT_EXPECT_EQ(test, nvmet_pci_prp_list_bytes(SZ_8K, 512),
			(size_t)(2 * sizeof(__le64)));
	KUNIT_EXPECT_EQ(test, nvmet_pci_prp_list_bytes(SZ_4M, 512),
			(size_t)SZ_4K);
}

static void nvmet_pci_dbbuf_event_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, nvmet_pci_dbbuf_need_event(10, 11, 10));
	KUNIT_EXPECT_FALSE(test, nvmet_pci_dbbuf_need_event(9, 11, 10));
	KUNIT_EXPECT_TRUE(test, nvmet_pci_dbbuf_need_event(0xffff, 0, 0xffff));
	KUNIT_EXPECT_FALSE(test, nvmet_pci_dbbuf_need_event(0xfffe, 0, 0xffff));
}

static void nvmet_pci_advance_sq_head_test(struct kunit *test)
{
	u16 head = 3;

	nvmet_pci_advance_sq_head(&head, 8);
	KUNIT_EXPECT_EQ(test, head, (u16)4);

	head = 7;
	nvmet_pci_advance_sq_head(&head, 8);
	KUNIT_EXPECT_EQ(test, head, (u16)0);
}

static void nvmet_pci_advance_cq_tail_test(struct kunit *test)
{
	u16 phase = 1;
	u16 tail = 3;

	nvmet_pci_advance_cq_tail(&tail, &phase, 8);
	KUNIT_EXPECT_EQ(test, tail, (u16)4);
	KUNIT_EXPECT_EQ(test, phase, (u16)1);

	tail = 7;
	nvmet_pci_advance_cq_tail(&tail, &phase, 8);
	KUNIT_EXPECT_EQ(test, tail, (u16)0);
	KUNIT_EXPECT_EQ(test, phase, (u16)0);
}

static void nvmet_pci_prepare_cqe_test(struct kunit *test)
{
	struct nvme_completion cqe = {};
	__le16 command_id = cpu_to_le16(0x1234);

	nvmet_pci_prepare_cqe(&cqe, 7, 3, command_id, 0x42, 1);

	KUNIT_EXPECT_EQ(test, le16_to_cpu(cqe.sq_head), (u16)7);
	KUNIT_EXPECT_EQ(test, le16_to_cpu(cqe.sq_id), (u16)3);
	KUNIT_EXPECT_EQ(test, cqe.command_id, command_id);
	KUNIT_EXPECT_EQ(test, le16_to_cpu(cqe.status), (u16)0x85);
}

static void nvmet_pci_single_subsys_test(struct kunit *test)
{
	struct nvmet_subsys_link *link1, *link2;
	struct nvmet_subsys *subsys1, *subsys2;
	struct nvmet_port *port;
	char subsysnqn[NVMF_NQN_SIZE];
	int ret;

	port = kunit_kzalloc(test, sizeof(*port), GFP_KERNEL);
	subsys1 = kunit_kzalloc(test, sizeof(*subsys1), GFP_KERNEL);
	subsys2 = kunit_kzalloc(test, sizeof(*subsys2), GFP_KERNEL);
	link1 = kunit_kzalloc(test, sizeof(*link1), GFP_KERNEL);
	link2 = kunit_kzalloc(test, sizeof(*link2), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, port);
	KUNIT_ASSERT_NOT_NULL(test, subsys1);
	KUNIT_ASSERT_NOT_NULL(test, subsys2);
	KUNIT_ASSERT_NOT_NULL(test, link1);
	KUNIT_ASSERT_NOT_NULL(test, link2);

	subsys1->subsysnqn = "nqn.2014-08.org.nvmexpress:test1";
	subsys2->subsysnqn = "nqn.2014-08.org.nvmexpress:test2";
	link1->subsys = subsys1;
	link2->subsys = subsys2;
	INIT_LIST_HEAD(&port->subsystems);
	ret = nvmet_port_get_single_subsysnqn(port, subsysnqn, sizeof(subsysnqn));
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	list_add_tail(&link1->entry, &port->subsystems);
	ret = nvmet_port_get_single_subsysnqn(port, subsysnqn, sizeof(subsysnqn));
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_STREQ(test, subsysnqn, subsys1->subsysnqn);

	list_add_tail(&link2->entry, &port->subsystems);
	ret = nvmet_port_get_single_subsysnqn(port, subsysnqn, sizeof(subsysnqn));
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static void nvmet_pci_admin_config_test(struct kunit *test)
{
	struct nvmet_pci_admin_config config;
	u32 cc = NVME_CC_ENABLE | NVME_CC_IOSQES | NVME_CC_IOCQES;
	u32 aqa = (31 << 16) | 63;
	u64 cap = 1023;
	int ret;

	ret = nvmet_pci_parse_admin_config(cap, cc, aqa, 0x1000, 0x2000,
					   &config);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, config.asq, (u64)0x1000);
	KUNIT_EXPECT_EQ(test, config.acq, (u64)0x2000);
	KUNIT_EXPECT_EQ(test, config.sq_depth, (u16)64);
	KUNIT_EXPECT_EQ(test, config.cq_depth, (u16)32);
	KUNIT_EXPECT_EQ(test, config.sq_size,
			(size_t)(64 * sizeof(struct nvme_command)));
	KUNIT_EXPECT_EQ(test, config.cq_size,
			(size_t)(32 * sizeof(struct nvme_completion)));
}

static void nvmet_pci_admin_config_invalid_test(struct kunit *test)
{
	struct nvmet_pci_admin_config config;
	u32 cc = NVME_CC_ENABLE | NVME_CC_IOSQES | NVME_CC_IOCQES;
	u32 aqa = (15 << 16) | 15;
	u64 cap = 1023;
	int ret;

	ret = nvmet_pci_parse_admin_config(cap,
					   cc | BIT(NVME_CC_MPS_SHIFT), aqa,
					   0x1000, 0x2000, &config);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = nvmet_pci_parse_admin_config(cap, cc, aqa, 0x1001, 0x2000,
					   &config);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = nvmet_pci_parse_admin_config(7, cc, aqa, 0x1000, 0x2000,
					   &config);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = nvmet_pci_parse_admin_config(cap, cc, 0, 0x1000, 0x2000,
					   &config);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = nvmet_pci_parse_admin_config(cap, cc, aqa | BIT(12),
					   0x1000, 0x2000, &config);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = nvmet_pci_parse_admin_config(cap, cc, aqa, 0x1000, 0x1000,
					   &config);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static struct kunit_case nvmet_pci_common_test_cases[] = {
	KUNIT_CASE(nvmet_pci_cq_full_test),
	KUNIT_CASE(nvmet_pci_prp_helpers_test),
	KUNIT_CASE(nvmet_pci_dbbuf_event_test),
	KUNIT_CASE(nvmet_pci_advance_sq_head_test),
	KUNIT_CASE(nvmet_pci_advance_cq_tail_test),
	KUNIT_CASE(nvmet_pci_prepare_cqe_test),
	KUNIT_CASE(nvmet_pci_single_subsys_test),
	KUNIT_CASE(nvmet_pci_admin_config_test),
	KUNIT_CASE(nvmet_pci_admin_config_invalid_test),
	{}
};

static struct kunit_suite nvmet_pci_common_test_suite = {
	.name = "nvmet-pci-common",
	.test_cases = nvmet_pci_common_test_cases,
};

kunit_test_suite(nvmet_pci_common_test_suite);

MODULE_DESCRIPTION("KUnit tests for NVMe PCI target helpers");
MODULE_LICENSE("GPL");
