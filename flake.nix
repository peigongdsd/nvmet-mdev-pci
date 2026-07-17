{
  description = "Development environment for the nvmet mdev PCI target";

  inputs.nixpkgs.url = "nixpkgs";

  outputs =
    { self, nixpkgs, ... }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      mkNvmetKernel =
        kernelPkgs:
        let
          base = kernelPkgs.linux_latest;
          version = "7.1.3-nvmet-mdev-pci";
        in
        base.override {
          argsOverride = {
            pname = "linux-nvmet-mdev-pci";
            inherit version;
            modDirVersion = "7.1.3";
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
        linux_nvmet_mdev_pci = mkNvmetKernel pkgs;
        default = linux_nvmet_mdev_pci;
      };

      legacyPackages.${system}.linuxPackages_nvmet_mdev_pci =
        pkgs.linuxPackagesFor
          self.packages.${system}.linux_nvmet_mdev_pci;

      overlays.default = final: _previous: {
        linux_nvmet_mdev_pci = mkNvmetKernel final;
        linuxPackages_nvmet_mdev_pci = final.linuxPackagesFor final.linux_nvmet_mdev_pci;
      };

      nixosModules.default =
        { pkgs, ... }:
        {
          nixpkgs.overlays = [ self.overlays.default ];
          boot.kernelPackages = pkgs.linuxPackages_nvmet_mdev_pci;
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
          export CCACHE_BASEDIR="''${CCACHE_BASEDIR:-$PWD}"
          export CCACHE_COMPILERCHECK="''${CCACHE_COMPILERCHECK:-content}"
          export CCACHE_NOHASHDIR="''${CCACHE_NOHASHDIR:-true}"
          export CC="''${KBUILD_CC:-ccache gcc}"
          export HOSTCC="''${KBUILD_HOSTCC:-ccache gcc}"
          mkdir -p "$KBUILD_OUTPUT" "$CCACHE_DIR"

          echo "nvmet-mdev-pci kernel development shell"
          echo "  source:  $PWD"
          echo "  output:  $KBUILD_OUTPUT"
          echo "  ccache:  $CCACHE_DIR"
          echo "  prepare: make defconfig"
          echo "  target:  make -j$(nproc) drivers/nvme/target/nvmet-mdev-pci.o"
        '';
      };

      formatter.${system} = pkgs.nixfmt;
    };
}
