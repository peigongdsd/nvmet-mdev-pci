# NixOS packaging

This driver cannot be built safely with `boot.extraModulePackages`. It changes
nvmet core transport registration and adds symbols consumed by both
`nvmet-pci-epf` and `nvmet-mdev-pci`, so the nvmet core and the mediated driver
must come from the same kernel build.

The flake exports project-specific package names:

- `packages.x86_64-linux.linux_nvmet_mdev_pci`: the patched 7.2-rc kernel;
- `legacyPackages.x86_64-linux.linuxPackages_nvmet_mdev_pci`: its package set;
- `nixosModules.default`: selects that package set and loads
  `nvmet-mdev-pci` during boot.

## Add it to the system flake

Add an input to `/persistent/nixos/flake.nix`:

```nix
inputs.nvmet-mdev-pci = {
  url = "path:/home/krusl/code/nvmet-mdev-pci";
  inputs.nixpkgs.follows = "nixpkgs";
};
```

Accept `nvmet-mdev-pci` in the outputs argument and add its NixOS module:

```nix
outputs = { self, nixpkgs, nvmet-mdev-pci, ... }@attrs: {
  nixosConfigurations.KruslPC = nixpkgs.lib.nixosSystem {
    # ...
    modules = [
      nvmet-mdev-pci.nixosModules.default
      # existing modules
    ];
  };
};
```

Remove or comment out the existing `boot.kernelPackages` assignment in
`hardware-configuration.nix`; the imported module supplies it with
`lib.mkForce`.

The module builds `linux_nvmet_mdev_pci` from this repository and exposes its
package set as `linuxPackages_nvmet_mdev_pci`. The pinned nixpkgs kernel is used
only as a configuration/build baseline; it is not the public identity of the
custom kernel. This revision is based on upstream commit `af5e34a41cd6` in the
Linux 7.2 release-candidate cycle plus the nvmet mdev patch series.

## Build and activate

Build the complete machine closure without changing the running generation:

```sh
nix build \
  /persistent/nixos#nixosConfigurations.KruslPC.config.system.build.toplevel \
  --no-link
```

This is the only full Nix build needed for a runtime checkpoint. During normal
development, use the persistent `.build` Kbuild tree described in
`DEVELOPING.md`; a clean Nix derivation cannot reuse individual kernel objects.

Then install the already-built closure as the next boot generation:

```sh
run0 nixos-rebuild boot --flake /persistent/nixos#KruslPC
sudo reboot
```

After reboot, verify the kernel and module:

```sh
uname -r
modinfo nvmet-mdev-pci
lsmod | grep nvmet_mdev_pci
```

Loading the module only registers the nvmet transport and class. An mdev
parent appears after an nvmet port with `addr_trtype=mdev-pci` is configured,
linked to exactly one subsystem, and enabled.
