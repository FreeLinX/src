#!/bin/sh
# shellcheck source=common.sh
# shellcheck disable=SC2154   # repo dirs retrieved via eval (indirect reference)
# check.sh - validate the host environment and the FreeLinX configuration.
#
# Non-destructive: reports the state of the host, configuration, external
# repositories and the root filesystem template. Exits non-zero only when a
# hard requirement (host tools, configuration, arch/triple match) is broken;
# missing optional components such as kernel/userland/ports are expected at
# this stage and only reported.

set -eu

. "$(dirname "$0")/common.sh"

while [ "$#" -gt 0 ]; do
    case "$1" in
        -a) FREELINX_ARCH=$2; shift 2 ;;
        -c) FREELINX_CONFIG=$2; shift 2 ;;
        -h|--help) freelinx_usage; exit 0 ;;
        *) freelinx_die "unknown option: $1 (see -h)" ;;
    esac
done

_status=0

freelinx_info "Checking environment..."
if ! freelinx_host_check; then
    _status=1
fi

freelinx_resolve_config
freelinx_info "Loading configuration: $FREELINX_CONFIG"
if [ ! -f "$FREELINX_CONFIG" ]; then
    freelinx_warn "configuration not found: $FREELINX_CONFIG"
    exit 1
fi
freelinx_load_config

freelinx_info "Checking configuration..."
freelinx_info "  arch:   $FREELINX_ARCH"
freelinx_info "  triple: $FREELINX_TRIPLE"
freelinx_info "  build:  $FREELINX_BUILD_DIR"

if ! freelinx_validate_arch; then
    _status=1
fi

freelinx_info "Checking toolchain..."
if clang_path=$(freelinx_find_clang); then
    freelinx_info "  compiler: $clang_path"
else
    freelinx_info "  compiler: not found (bootstrap toolchain expected later)"
fi
if ld_path=$(freelinx_find_ld); then
    freelinx_info "  linker:   $ld_path"
else
    freelinx_info "  linker:   not found (bootstrap toolchain expected later)"
fi
if [ -n "${FREELINX_SYSROOT:-}" ] && [ -d "$FREELINX_SYSROOT" ]; then
    freelinx_info "  sysroot:  $FREELINX_SYSROOT"
else
    freelinx_info "  sysroot:  not found ($FREELINX_SYSROOT)"
fi

freelinx_info "Checking external repositories..."
for _pair in \
    "FREELINX_KERNEL_DIR=FreeLinX/kernel" \
    "FREELINX_TOOLCHAIN_DIR=FreeLinX/toolchain" \
    "FREELINX_PORTS_DIR=FreeLinX/ports" \
    "FREELINX_ISO_DIR=FreeLinX/iso"
do
    _reponame=${_pair#*=}
    _revar=${_pair%%=*}
    eval "_dir=\$$_revar"
    if [ -d "$_dir" ]; then
        freelinx_info "  $_reponame: present ($_dir)"
    else
        freelinx_info "  $_reponame: not present yet"
    fi
done

freelinx_info "Checking rootfs template..."
freelinx_rootfs_ok=1
if [ -d "$FREELINX_ROOTFS_DIR" ]; then
    freelinx_info "  template dir: $FREELINX_ROOTFS_DIR"
    for _f in init etc/os-release; do
        if [ -f "$FREELINX_ROOTFS_DIR/$_f" ]; then
            freelinx_info "  template: $_f"
        else
            freelinx_warn "  template file missing: $_f"
            freelinx_rootfs_ok=0
        fi
    done
else
    freelinx_warn "  rootfs template directory missing: $FREELINX_ROOTFS_DIR"
    freelinx_rootfs_ok=0
fi
[ "$freelinx_rootfs_ok" -eq 0 ] && _status=1

if [ -d "$FREELINX_STAGE_ROOTFS" ]; then
    freelinx_info "Staged rootfs present: $FREELINX_STAGE_ROOTFS"
else
    freelinx_info "Staged rootfs not built yet (run: make rootfs)"
fi

if [ "$_status" -eq 0 ]; then
    freelinx_info "Environment and configuration OK."
else
    freelinx_die "Environment or configuration check FAILED."
fi