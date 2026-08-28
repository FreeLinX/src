#!/bin/sh
# shellcheck source=common.sh
# shellcheck disable=SC2034   # FREELINX_ARCH/FREELINX_CONFIG via config load
# clean.sh - remove FreeLinX build artifacts.
#
# Only generated output under the build directory is removed. The source
# template in <repo>/rootfs and the configuration are never touched.

set -eu

. "$(dirname "$0")/common.sh"

while [ "$#" -gt 0 ]; do
    case "$1" in
        -a) FREELINX_ARCH=$2; shift 2 ;;
        -c) FREELINX_CONFIG=$2; shift 2 ;;
        -n|--dry-run) _dry_run=1; shift ;;
        -h|--help) freelinx_usage; exit 0 ;;
        *) freelinx_die "unknown option: $1 (see -h)" ;;
    esac
done

freelinx_load_config

if [ ! -d "$FREELINX_BUILD_DIR" ]; then
    freelinx_info "Nothing to clean (build directory does not exist): $FREELINX_BUILD_DIR"
    exit 0
fi

if [ "${_dry_run:-0}" -eq 1 ]; then
    freelinx_info "Would remove: $FREELINX_BUILD_DIR"
else
    freelinx_info "Removing build directory: $FREELINX_BUILD_DIR"
    rm -rf "$FREELINX_BUILD_DIR"
fi

freelinx_info "Clean complete."