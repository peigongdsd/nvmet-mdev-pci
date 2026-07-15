# nvmet-mdev-pci

`nvmet-mdev-pci` exposes a Linux NVMe target subsystem as a mediated VFIO PCI
device. A VM sees an ordinary NVMe PCI controller while the host keeps nvmet's
namespace and backend implementation.

- [Usage and VM assignment](USAGE.md)
- [Development and test status](DEVELOPING.md)
- [Performance design and acceptance gates](PERFORMANCE.md)
- [Benchmark procedure](tools/testing/nvmet-mdev-pci/BENCHMARK.md)

The current development target is Linux 7.2-rc3. Ordinary guest memory is
supported; SPDK and hugetlbfs are not required.
