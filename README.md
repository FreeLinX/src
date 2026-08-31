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
- `rootfs/init` — `/init`. Mounts proc/sys/dev, then execs
  `/sbin/runsvdir -P /var/service` if runit is present, falling back to a
  rescue shell otherwise. **Verified booting in QEMU (2026-08-30)**: kernel
  → `/init` → `runsvdir` → `runsv` supervising a test service, restarting
  it on exit as designed. The runit binaries (`runsvdir`, `runsv`, `sv`,
  `chpst`) come from `FreeLinX/ports` (`sysutils/runit`); `/var/service/*`
  entries are added per-service as FreeLinX gains real supervised daemons.
- `config/x86_64/default.conf` — the only config file for now (x86_64).

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
