# FreeLinX architecture

## The final system

Linux 6.1 on x86_64, musl libc, clang/LLD toolchain, NetBSD-derived
userland, runit for service supervision. The userspace must not depend on
glibc or GNU coreutils once it exists; GNU tools are only acceptable on the
host during bootstrap.

## Boot flow

    Linux kernel mounts the staging rootfs, runs /init
      |
      v
    /init (bootstrap placeholder): mount proc/sys/dev, then runsvdir
      |
      v
    runit runs /var/service services (NetBSD-derived userland)
      |
      v
    final FreeLinX system

Today `/init` is a placeholder (`rootfs/init`): it mounts the pseudo
filesystems and either execs `/sbin/runsvdir` (once it exists) or drops to a
rescue shell. It is staged, never executed by the build.

## Build flow

    toolchain repo -> clang/ld.lld + musl sysroot
    kernel repo    -> Linux 6.1 image
    src (this)     -> rootfs layout, system config, orchestration
    ports repo     -> statically linked NetBSD-derived userland
    iso repo       -> bootable image from src's build/<arch>/ output

`src` is the integration point: it collects everything into one staging
rootfs and hands it, plus a manifest, to the ISO repo.

## How the build works

1. Host environment check (the tools the scripts themselves need).
2. Load `config/<arch>/default.conf` as a shell fragment. Every value is
   written `VAR=${VAR:-default}`, so a `FREELINX_*` environment variable
   wins over the file; `-a`/`-c` flags win over both.
3. Toolchain detection: clang + ld.lld in `$FREELINX_TOOLCHAIN_BIN` and a
   sysroot directory. Missing is fine for now; it is reported.
4. Arch validation: the triple must start with the arch name.
5. Rootfs staging: copy `rootfs/` into `build/<arch>/rootfs`, strip
   `.gitkeep`, make `init` executable. Fresh tree every run.
6. Userland: not built yet. `ports` will install into
   `$FREELINX_STAGE_ROOTFS`; the reserved `FREELINX_PORTS_BUILD` hook is
   where that gets wired in.
7. System config: the staged `/init` and `/etc/os-release` are checked.
8. Artifact: `build/<arch>/build-manifest.txt` for the ISO repo.

All paths default relative to the repo root or to sibling repos. No
machine-specific absolute paths are hard-coded.

## Config variables that matter

    FREELINX_ARCH          x86_64
    FREELINX_TRIPLE        x86_64-linux-musl
    FREELINX_KERNEL_DIR    ../kernel
    FREELINX_TOOLCHAIN_DIR ../toolchain
    FREELINX_TOOLCHAIN_BIN $TOOLCHAIN_DIR/bin
    FREELINX_SYSROOT       $TOOLCHAIN_DIR/x86_64-linux-musl
    FREELINX_PORTS_DIR     ../ports
    FREELINX_ISO_DIR       ../iso
    FREELINX_ROOTFS_DIR    <root>/rootfs
    FREELINX_BUILD_DIR     <root>/build
    FREELINX_ARTIFACT_DIR  $BUILD_DIR/<arch>
    FREELINX_STAGE_ROOTFS  $ARTIFACT_DIR/rootfs

All of them can be overridden on the command line of any script.

## What is missing

- Userland binaries (`ports` repo) and the toolchain `bin/`/sysroot output.
- Kernel image handling (owned by `iso`).
- runit service trees under `var/service` and the real init.
- Dynamic loaders and static binaries under `bin/`, `sbin/`, `usr/bin`,
  `usr/sbin`, `usr/lib`.

Until those exist, `scripts/build.sh` completes with a manifest that says
exactly what was available and what was skipped.