{
  description = "Development environment for the nvmet mdev PCI target";

  inputs.nixpkgs.url = "nixpkgs";

  outputs =
    { self, nixpkgs, ... }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      mkNvmetKernel = kernelPkgs:
        let
          base = kernelPkgs.linuxPackages_testing.kernel;
          version = "7.2-rc3-nvmet-mdev-pci";
        in
        base.override {
          argsOverride = {
            pname = "linux-nvmet-mdev-pci";
            inherit version;
            modDirVersion = "7.2.0-rc3";
            src = self;
            structuredExtraConfig = with kernelPkgs.lib.kernel; {
              BLK_DEV_NVME = module;
              CONFIGFS_FS = yes;
              NVME_TARGET = module;
              NVME_TARGET_MDEV_PCI = module;
              VFIO = module;
              VFIO_MDEV = module;
            };
          };
        };
    in
    {
      packages.${system} = rec {
        kernel = mkNvmetKernel pkgs;
        default = kernel;
      };

      legacyPackages.${system}.kernelPackages =
        pkgs.linuxPackagesFor self.packages.${system}.kernel;

      nixosModules.default =
        { lib, pkgs, ... }:
        let
          kernel = mkNvmetKernel pkgs;
        in
        {
          boot.kernelPackages = lib.mkForce (pkgs.linuxPackagesFor kernel);
          boot.kernelModules = [ "nvmet-mdev-pci" ];
        };

      devShells.${system}.default = pkgs.mkShell {
        name = "nvmet-mdev-pci-kernel-dev";
        hardeningDisable = [ "all" ];

        packages = with pkgs; [
          bashInteractive
          bc
          bear
          binutils
          bison
          ccache
          clang
          clang-tools
          coccinelle
          cpio
          elfutils
          flex
          gcc
          gdb
          git
          gnumake
          kmod
          libcap
          lld
          llvm
          ncurses
          nixfmt
          openssl
          pahole
          perl
          pkg-config
          python3
          qemu
          rsync
          sparse
          util-linux
          zlib
        ];

        shellHook = ''
          export KBUILD_OUTPUT="''${KBUILD_OUTPUT:-$PWD/.build}"
          export CCACHE_DIR="''${CCACHE_DIR:-$PWD/.cache/ccache}"
          mkdir -p "$KBUILD_OUTPUT" "$CCACHE_DIR"

          echo "nvmet-mdev-pci kernel development shell"
          echo "  source:  $PWD"
          echo "  output:  $KBUILD_OUTPUT"
          echo "  prepare: make defconfig"
          echo "  target:  make -j$(nproc) drivers/nvme/target/"
        '';
      };

      formatter.${system} = pkgs.nixfmt;
    };
}
