# nvmet-mdev-pci

`nvmet-mdev-pci` exposes a Linux NVMe target subsystem as a mediated VFIO PCI
device. A VM sees an ordinary NVMe PCI controller while the host keeps nvmet's
namespace and backend implementation.

The maintained branches are:

- [`v7.2-dev`](https://github.com/peigongdsd/nvmet-mdev-pci/tree/v7.2-dev):
  active development on the upstream Linux 7.2 release-candidate line;
- [`v7.1`](https://github.com/peigongdsd/nvmet-mdev-pci/tree/v7.1): the
  maintained stable Linux 7.1.y backport;
- `master`: the upstream Linux tree plus this project landing page. It contains
  no nvmet-mdev-pci implementation patches.

Use a versioned branch for implementation, NixOS packaging, usage documentation,
and test procedures.
