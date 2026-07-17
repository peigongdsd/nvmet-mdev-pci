# nvmet-mdev-pci

`nvmet-mdev-pci` exposes a Linux NVMe target subsystem as a mediated VFIO PCI
device. A VM sees an ordinary NVMe PCI controller while the host keeps nvmet's
namespace and backend implementation.

- [Usage and VM assignment](USAGE.md)
- [Development and test status](DEVELOPING.md)
- [Performance design and acceptance gates](PERFORMANCE.md)
- [Queue, interrupt, and teardown invariants](CONCURRENCY.md)
- [Benchmark procedure](tools/testing/nvmet-mdev-pci/BENCHMARK.md)

The maintained branches are:

- `v7.2-dev`: active development on the upstream Linux 7.2 release-candidate
  line;
- `v7.1`: the maintained stable Linux 7.1.y backport;
- `master`: an unmodified mirror of upstream Linux for future work.

This revision of `v7.2-dev` is based on upstream commit `af5e34a41cd6`, after
Linux 7.2-rc3 and before the 7.2-rc4 tag. Ordinary guest memory is supported;
SPDK and hugetlbfs are not required.
