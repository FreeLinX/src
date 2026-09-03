# FreeLinX/src

Root filesystem and integration repo for FreeLinX.

`rootfs/` is the layout template. The scripts under `scripts/` stage it into
a build tree and, later, install userland into it. The build validates the
host, loads a config file, and reports what it can and cannot do.

## What each piece is

- `scripts/rootfs.sh` — copies `rootfs/` into `build/<arch>/rootfs`. The
  staging tree is rebuilt from scratch every run.
- `scripts/build.sh` — the full build: host check, config, toolchain
  detection, arch validation, rootfs staging, then userland install once
  `FreeLinX/ports` exists, and finally a manifest for the ISO repo.
- `scripts/check.sh` — read-only diagnostics: host tools, config,
  toolchain, sibling repos, rootfs template.
- `scripts/clean.sh` — deletes `build/`.
- `rootfs/sbin/` — BSD-style networking + service-supervision tools:
  `/sbin/runsvdir -P /var/service` if runit is present, falling back to a
  rescue shell otherwise. **Verified booting in QEMU (2026-08-30)**: kernel
  → `/init` → `runsvdir` → `runsv` supervising a test service, restarting
  it on exit as designed. The runit binaries (`runsvdir`, `runsv`, `sv`,
  `chpst`) come from `FreeLinX/ports` (`sysutils/runit`); `/var/service/*`
  entries are added per-service as FreeLinX gains real supervised daemons.
- `config/x86_64/default.conf` — the only config file for now (x86_64).

`lib/` in the rootfs now carries the kernel build output needed for real
hardware:

- `lib/modules/6.6.21/` — the stripped WiFi driver modules (plus `depmod`
  metadata): `iwlwifi`, `ath9k`/`ath9k_htc`, `ath10k_pci`, `brcmfmac`
  (+ `brcmfmac-cyw`/`-wcc`/`-bca`), `brcmsmac`, plus deps (`ath`, `bcma`,
  `cordic`, `brcmutil`). Built in the `kernel` repo, installed with
  `INSTALL_MOD_STRIP=1 modules_install INSTALL_MOD_PATH=src/rootfs`.
- `lib/firmware/` — redistributable device blobs (from
  `ports/firmware/linux-firmware`) for those chips. **Not committed** (1.2G);
  it ships as `firmware-6.6.21.tar.xz` in the release. To populate it locally:

      tar xJf /path/to/firmware-6.6.21.tar.xz -C rootfs/lib              # -> rootfs/lib/firmware


### Networking & wireless
`rootfs/sbin/` holds the boot-relevant BSD-flavoured tools (all static musl,
zero GNU):

    flx-ifconfig      BSD-style ifconfig (netlink via libnl)
    flx-route         BSD-style route  (netlink via libnl)
    flx-wifi          wrapper: wpa_supplicant + wpa_cli + dhcpcd
    wpa_supplicant    WPA/WPA2/WPA3 supplicant
    wpa_cli           supplicant control CLI
    wpa_passphrase    WPA PSK generator
    dhcpcd            DHCP client

Bring up wired (verified) or wireless (needs real hardware):

    flx-ifconfig eth0 up
    flx-ifconfig eth0 inet 10.0.2.15/24
    flx-route add default 10.0.2.2
    ping 10.0.2.2

    # wireless (physical NIC + matching firmware):
    modprobe iwlwifi
    flx-wifi scan
    flx-wifi connect "MyNetwork" "password"
    dhcpcd wlan0

### Interactive shell image
For quick manual testing, a self-contained QEMU initramfs boots straight to
a root shell with all of the above present:

    qemu-system-x86_64 \
        -kernel <kernel>/linux-6.6.21/arch/x86/boot/bzImage \
        -initrd build/x86_64/initramfs-shell.cpio.gz \
        -append "console=ttyS0 rdinit=/sbin/flx-shell-init quiet" \
        -nographic -monitor none -m 512M

You are dropped to a `#` prompt with `lo` up and the wifi driver modules
loaded; the rest is up to you. Exit with `Ctrl-A X`.


The build writes nothing back into `rootfs/`; that tree stays source.
Generated stuff goes to:

    build/<arch>/rootfs/              staged root filesystem
    build/<arch>/build-manifest.txt   what was built / what was skipped

## Build

    make build     # or: ./scripts/build.sh
    make check     # or: ./scripts/check.sh
    make rootfs    # or: ./scripts/rootfs.sh
    make clean     # or: ./scripts/clean.sh

As of the toolchain/kernel/ports bring-up, `make check` reports the
toolchain, kernel, and ports repos as present, and `make rootfs` stages a
bootable root filesystem including `netbsd-sh`, runit, and the ported base
utilities. Run `make check` for the current, exact status.

## Configuration

Defaults: arch `x86_64`, triple `x86_64-linux-musl`, sibling repos at
`../kernel`, `../toolchain`, `../ports`, `../iso`. Config files are shell
fragments full of `VAR=${VAR:-default}`, so any `FREELINX_*` environment
variable overrides the file. `-a` and `-c` override both.

    FREELINX_TOOLCHAIN_DIR=/opt/freelinx-tc ./scripts/build.sh
    ./scripts/build.sh -a x86_64 -c /tmp/my.conf   (see -h)

Host tools the scripts need: `sh`, `mkdir`, `cp`, `rm`, `date`, `find`,
`grep`, `sed`. Everything else is optional during bootstrap.

## Repository relationships

    toolchain  builds clang/LLD + musl        -> ../toolchain
    kernel     Linux 6.6.21                   -> ../kernel
    ports      NetBSD-derived userland + runit -> ../ports
    iso        final ISO, consumes build/<arch>/ and the manifest
    freelinxf  project docs (separate)

`src` integrates `toolchain`+`kernel`+`ports` output into one staging tree;
`iso` turns that into a bootable image. If a repo is missing, the build
reports it and continues with what it has.

Details and the future boot flow are in `docs/architecture.md`.
