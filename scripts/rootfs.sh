#!/bin/sh
# shellcheck source=common.sh
# shellcheck disable=SC2034   # FREELINX_ARCH/FREELINX_CONFIG via config load
# rootfs.sh - prepare the FreeLinX root filesystem.
#
# Produces a staging root filesystem under the build directory that can later
# be consumed by the ISO repository. The version-controlled tree in
# <repo>/rootfs is the source template; nothing generated is ever written
# back into it, keeping the repository source-oriented.
#
# The staging tree is rebuilt from scratch on every run so output is
# deterministic.
#
# No fake binaries are installed. Directories and minimal configuration
# (os-release, bootsrap /init) are staged; real userland binaries are added
# later by the ports system.

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

freelinx_resolve_config
freelinx_info "Loading configuration: $FREELINX_CONFIG"
freelinx_load_config

# ---------------------------------------------------------------------------
# 1. Ensure the source template layout exists.
# ---------------------------------------------------------------------------
freelinx_info "Preparing root filesystem (template: $FREELINX_ROOTFS_DIR)"

# Every directory we expect the final system to have. lib/ and usr/libexec/
# are included because the FHS places dynamic loaders and libexec tools there.
_dirs="bin sbin etc dev proc sys tmp lib
       usr/bin usr/sbin usr/lib usr/libexec usr/share var"

for _d in $_dirs; do
    _full="${FREELINX_ROOTFS_DIR}/${_d}"
    if [ ! -d "$_full" ]; then
        freelinx_info "creating template directory: ${_d}/"
        mkdir -p "$_full"
    fi
    # Keep empty directories trackable in git.
    if [ ! -e "$_full/.gitkeep" ]; then
        : > "$_full/.gitkeep"
    fi
done

for _f in init etc/os-release; do
    if [ ! -f "${FREELINX_ROOTFS_DIR}/${_f}" ]; then
        freelinx_warn "template file missing: ${_f} (add it to rootfs/)"
    fi
done

# ---------------------------------------------------------------------------
# 2. Build a fresh staging copy.
# ---------------------------------------------------------------------------
freelinx_info "Staging rootfs at: $FREELINX_STAGE_ROOTFS"
rm -rf "$FREELINX_STAGE_ROOTFS"
mkdir -p "$FREELINX_STAGE_ROOTFS"

# cp -a preserves modes/timestamps, keeping the staging tree identical to the
# template. Squash-Down: .gitkeep markers are only git bookkeeping.
cp -a "$FREELINX_ROOTFS_DIR"/. "$FREELINX_STAGE_ROOTFS"/ 2>/dev/null || {
    freelinx_warn "cp -a failed; falling back to cp -R"
    cp -R "$FREELINX_ROOTFS_DIR"/. "$FREELINX_STAGE_ROOTFS"/
}

# Remove git bookkeeping files from the staged tree.
find "$FREELINX_STAGE_ROOTFS" -name .gitkeep -delete

# The bootstrap /init must be executable in the staged root.
if [ -f "$FREELINX_STAGE_ROOTFS/init" ]; then
    chmod 755 "$FREELINX_STAGE_ROOTFS/init"
fi

# ---------------------------------------------------------------------------
# 2b. Restore the modes git cannot carry.
#
# Git tracks exactly one mode bit - the executable one - so every file in the
# template arrives as 0644 (or 0755 for anything with a shebang). That is
# correct for ordinary data and wrong for a handful of files whose whole
# purpose is to be private. /etc/shadow readable by every user on the system
# is a standing credential-disclosure hole: it holds a hash per account, and
# hashes are exactly the thing the mode is there to keep off other accounts.
#
# So the modes those files need are applied here, where the staged tree is
# built, rather than in the template, where there is nowhere to put them. A
# file listed here and missing from the template is reported rather than
# skipped, because a private file that silently did not get created is the
# outcome this step exists to prevent.
#
# format: <mode> <path relative to the staged root>
#
# Read a line at a time rather than word-splitting the whole table: the paths
# are single tokens only by accident, and a path with a space in it would
# silently become a mode.
while read -r _mode _rel _rest; do
    case $_mode in ''|'#'*) continue ;; esac
    if [ -n "${_rest:-}" ]; then
        freelinx_die "malformed entry in the sensitive-file table: '$_mode $_rel $_rest'"
    fi
    if [ -e "$FREELINX_STAGE_ROOTFS/$_rel" ]; then
        chmod "$_mode" "$FREELINX_STAGE_ROOTFS/$_rel"
    else
        freelinx_warn "sensitive file absent from template: $_rel (wanted mode $_mode)"
    fi
done <<EOF
0600 etc/shadow
0600 etc/gshadow
0600 etc/sudoers
0640 etc/doas.conf
EOF

# ---------------------------------------------------------------------------
# 3. Verify the staging tree.
# ---------------------------------------------------------------------------
for _d in $_dirs; do
    if [ ! -d "$FREELINX_STAGE_ROOTFS/$_d" ]; then
        freelinx_warn "staged rootfs missing directory: ${_d}/"
    fi
done

freelinx_info "Root filesystem stage complete: $FREELINX_STAGE_ROOTFS"