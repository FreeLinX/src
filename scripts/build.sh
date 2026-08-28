#!/bin/sh
# shellcheck source=common.sh
# build.sh - orchestrate the FreeLinX build.
#
# Steps:
#   1. validate the host environment
#   2. load the FreeLinX configuration
#   3. detect the FreeLinX toolchain
#   4. validate the target architecture
#   5. prepare the root filesystem (staging tree)
#   6. build/install userspace components when they become available
#   7. prepare system configuration
#   8. produce the build artifact consumed by the ISO repository
#
# Components that do not exist yet (kernel, userland, ports, runit) are
# detected and reported explicitly; the build is never faked.

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

freelinx_info "FreeLinX build started."

freelinx_resolve_config

# Propagate explicitly chosen arch/config to the rootfs.sh subprocess.
export FREELINX_ARCH FREELINX_CONFIG

# --- 1. Validate the host environment --------------------------------------
freelinx_info "Checking environment..."
if ! freelinx_host_check; then
    freelinx_die "missing required host tools; fix and re-run."
fi

# --- 2. Load the configuration ---------------------------------------------
freelinx_info "Loading configuration: $FREELINX_CONFIG"
freelinx_load_config
freelinx_info "  arch:   $FREELINX_ARCH"
freelinx_info "  triple: $FREELINX_TRIPLE"
freelinx_info "  build:  $FREELINX_BUILD_DIR"

# --- 3. Detect the FreeLinX toolchain --------------------------------------
freelinx_info "Checking toolchain..."
_toolchain_ok=0
if clang_path=$(freelinx_find_clang); then
    freelinx_info "  compiler: $clang_path"
    _toolchain_ok=1
else
    freelinx_info "  compiler: not found ($FREELINX_TOOLCHAIN_BIN)"
fi
if ld_path=$(freelinx_find_ld); then
    freelinx_info "  linker:   $ld_path"
else
    freelinx_info "  linker:   not found ($FREELINX_TOOLCHAIN_BIN)"
    _toolchain_ok=0
fi
if [ -d "$FREELINX_SYSROOT" ]; then
    freelinx_info "  sysroot:  $FREELINX_SYSROOT"
else
    freelinx_info "  sysroot:  not found yet ($FREELINX_SYSROOT)"
    _toolchain_ok=0
fi
[ "$_toolchain_ok" -eq 1 ] && freelinx_info "  toolchain: available"
[ "$_toolchain_ok" -eq 0 ] && freelinx_info "  toolchain: not available yet (bootstrap stage)"

# --- 4. Validate the target architecture ------------------------------------
freelinx_info "Validating target architecture..."
if ! freelinx_validate_arch; then
    freelinx_die "target architecture/triple mismatch."
fi

freelinx_info "Checking kernel..."
if [ -d "$FREELINX_KERNEL_DIR" ]; then
    freelinx_info "  kernel: present ($FREELINX_KERNEL_DIR); image assembly is owned by the ISO repository"
else
    freelinx_info "  kernel: not yet available, skipping (expected at FreeLinX/kernel)"
fi

# --- 5. Prepare the root filesystem -----------------------------------------
# Delegated to rootfs.sh, which owns the staging tree under the build dir.
"$FREELINX_SCRIPT_DIR/rootfs.sh"

# --- 6. Userspace components ------------------------------------------------
freelinx_info "Building userspace components..."
if [ -d "$FREELINX_PORTS_DIR" ]; then
    if [ "$_toolchain_ok" -eq 1 ]; then
        freelinx_info "  ports: $FREELINX_PORTS_DIR"
        if [ -n "${FREELINX_PORTS_BUILD:-}" ] && [ -x "$FREELINX_PORTS_BUILD" ]; then
            freelinx_info "  running ports build hook: $FREELINX_PORTS_BUILD"
            "$FREELINX_PORTS_BUILD"
        else
            freelinx_info "  ports build hook not configured; skipping (set FREELINX_PORTS_BUILD)"
        fi
    else
        freelinx_info "  ports present but toolchain unavailable; skipping"
    fi
else
    freelinx_info "  userland not yet available, skipping (expected at FreeLinX/ports)"
fi

# --- 7. System configuration ------------------------------------------------
freelinx_info "Preparing system configuration..."
if [ -f "$FREELINX_STAGE_ROOTFS/init" ]; then
    freelinx_info "  /init: bootstrap placeholder staged"
else
    freelinx_warn "  /init missing from template; add rootfs/init"
fi
if [ -f "$FREELINX_STAGE_ROOTFS/etc/os-release" ]; then
    freelinx_info "  /etc/os-release: template staged"
else
    freelinx_warn "  /etc/os-release missing from template"
fi

# --- 8. Produce the build artifact ------------------------------------------
freelinx_info "Producing build artifact..."
mkdir -p "$FREELINX_ARTIFACT_DIR"

_manifest="$FREELINX_ARTIFACT_DIR/build-manifest.txt"
{
    printf '# FreeLinX build manifest\n'
    printf 'generated: %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    printf 'arch:      %s\n' "$FREELINX_ARCH"
    printf 'triple:    %s\n' "$FREELINX_TRIPLE"
    printf 'config:    %s\n' "$FREELINX_CONFIG"
    printf 'rootfs:    %s\n' "$FREELINX_STAGE_ROOTFS"
    printf 'components:\n'
    printf '  rootfs:        prepared\n'
    if [ "$_toolchain_ok" -eq 1 ]; then
        printf '  toolchain:     available (%s)\n' "$FREELINX_TOOLCHAIN_BIN"
    else
        printf '  toolchain:     not available\n'
    fi
    if [ -d "$FREELINX_KERNEL_DIR" ]; then
        printf '  kernel:        present (not integrated here)\n'
    else
        printf '  kernel:        not available\n'
    fi
    if [ -d "$FREELINX_PORTS_DIR" ]; then
        printf '  userland:      present (not built)\n'
    else
        printf '  userland:      not available\n'
    fi
    if [ -d "$FREELINX_ISO_DIR" ]; then
        printf '  iso:           present (consumes this artifact)\n'
    else
        printf '  iso:           not available\n'
    fi
    printf '  init:          bootstrap placeholder\n'
} > "$_manifest"

freelinx_info "  artifact: $FREELINX_ARTIFACT_DIR"
freelinx_info "  manifest: $_manifest"

freelinx_info "Build complete. Artifact ready for the ISO repository."