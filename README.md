# nvmet-mdev-pci

`nvmet-mdev-pci` exposes a Linux NVMe target subsystem as a mediated VFIO PCI
device. A VM sees an ordinary NVMe PCI controller while the host keeps nvmet's
namespace and backend implementation.

- [Usage and VM assignment](USAGE.md)
- [Development and test status](DEVELOPING.md)
- [Performance design and acceptance gates](PERFORMANCE.md)
- [Queue, interrupt, and teardown invariants](CONCURRENCY.md)
- [Benchmark procedure](tools/testing/nvmet-mdev-pci/BENCHMARK.md)

The backport target is the stable Linux 7.1.y series. Ordinary guest memory is
supported; SPDK and hugetlbfs are not required.
