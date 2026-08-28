#!/bin/sh
# common.sh - shared helpers for the FreeLinX build/check scripts.
#
# This file is source-oriented, not a build artifact of its own. It is
# intended to be sourced by the other scripts in this directory:
#
#     . "$(dirname "$0")/common.sh"
#
# All scripts in this repository are POSIX /bin/sh. No bash-only syntax.
#
# Environment variables (FREELINX_*) always override configuration files,
# and command-line flags override environment variables.

# ---------------------------------------------------------------------------
# Repository root.
#
# $0 is the invoking script, which lives next to common.sh, so the
# repository root is one level above this directory.
# ---------------------------------------------------------------------------
if [ -z "${FREELINX_ROOT:-}" ]; then
    FREELINX_ROOT=$(CDPATH='' cd -P "$(dirname "$0")/.." && pwd)
fi

# Architecture default. The config file re-applies this with the
# ${VAR:-default} idiom so that a pre-set environment variable wins.
FREELINX_ARCH=${FREELINX_ARCH:-x86_64}

# Host tools required by the build scripts themselves. These defaults apply
# before any configuration is loaded; the config file may override them.
FREELINX_REQUIRED_TOOLS=${FREELINX_REQUIRED_TOOLS:-"mkdir cp rm date find grep sed"}
FREELINX_OPTIONAL_TOOLS=${FREELINX_OPTIONAL_TOOLS:-"make clang ld.lld tar bmake"}

# ---------------------------------------------------------------------------
# Messaging helpers.
# ---------------------------------------------------------------------------
freelinx_info() { printf '[FreeLinX] %s\n' "$*"; }
freelinx_warn() { printf '[FreeLinX][warning] %s\n' "$*" >&2; }
freelinx_die() {
    printf '[FreeLinX][error] %s\n' "$*" >&2
    exit 1
}

# ---------------------------------------------------------------------------
# Misc helpers.
# ---------------------------------------------------------------------------
have_cmd() { command -v "$1" >/dev/null 2>&1; }

freelinx_usage() {
    cat <<EOF
FreeLinX build framework: $(basename "$0")

Usage: $(basename "$0") [-a ARCH] [-c CONFIG] [-h]

  -a ARCH    target architecture (default: \$FREELINX_ARCH or x86_64)
  -c CONFIG  path to configuration file
             (default: \$FREELINX_CONFIG or config/<ARCH>/default.conf)
  -h         show this help

All settings can also be provided via environment variables (FREELINX_*);
environment variables take precedence over the configuration file.
EOF
}

# ---------------------------------------------------------------------------
# Configuration loading.
#
# FREELINX_CONFIG resolves to config/<ARCH>/default.conf unless it was set
# explicitly (via -c or a FREELINX_CONFIG environment variable). The
# configuration files are plain POSIX shell fragments that (re)apply
# defaults using ${VAR:-default}, so any FREELINX_* environment variable set
# before sourcing survives.
# ---------------------------------------------------------------------------
freelinx_resolve_config() {
    FREELINX_CONFIG=${FREELINX_CONFIG:-"${FREELINX_ROOT}/config/${FREELINX_ARCH}/default.conf"}
}

freelinx_load_config() {
    freelinx_resolve_config
    if [ ! -f "$FREELINX_CONFIG" ]; then
        freelinx_die "configuration not found: $FREELINX_CONFIG (set -a/-c or FREELINX_CONFIG)"
    fi
    # shellcheck disable=SC1090   # config files are dynamic shell fragments
    . "$FREELINX_CONFIG"
}

# ---------------------------------------------------------------------------
# Host environment validation.
#
# A list of command names is checked with command -v. Required commands are
# needed for the scripts themselves; missing ones abort with a clear error.
# Optional commands are reported but do not fail the build.
# ---------------------------------------------------------------------------
freelinx_host_check() {
    _missing=0
    for cmd in $FREELINX_REQUIRED_TOOLS; do
        if have_cmd "$cmd"; then
            freelinx_info "host tool: ${cmd} - ok"
        else
            freelinx_warn "host tool: ${cmd} - MISSING (required)"
            _missing=1
        fi
    done
    for cmd in $FREELINX_OPTIONAL_TOOLS; do
        if have_cmd "$cmd"; then
            freelinx_info "host tool: ${cmd} - ok (optional)"
        else
            freelinx_info "host tool: ${cmd} - not present (optional)"
        fi
    done
    return "$_missing"
}

# ---------------------------------------------------------------------------
# Toolchain detection.
#
# Returns 0 and prints the path when a usable FreeLinX toolchain is found.
# The layout is configurable; bin/ is the toolchain repo output directory.
# ---------------------------------------------------------------------------
freelinx_find_clang() {
    if [ -n "${FREELINX_TOOLCHAIN_BIN:-}" ] &&
       [ -x "${FREELINX_TOOLCHAIN_BIN}/${FREELINX_TRIPLE}-clang" ]; then
        printf '%s\n' "${FREELINX_TOOLCHAIN_BIN}/${FREELINX_TRIPLE}-clang"
        return 0
    fi
    if [ -n "${FREELINX_TOOLCHAIN_BIN:-}" ] &&
       [ -x "${FREELINX_TOOLCHAIN_BIN}/clang" ]; then
        printf '%s\n' "${FREELINX_TOOLCHAIN_BIN}/clang"
        return 0
    fi
    return 1
}

freelinx_find_ld() {
    if [ -n "${FREELINX_TOOLCHAIN_BIN:-}" ] &&
       [ -x "${FREELINX_TOOLCHAIN_BIN}/ld.lld" ]; then
        printf '%s\n' "${FREELINX_TOOLCHAIN_BIN}/ld.lld"
        return 0
    fi
    return 1
}

# Validate that the configured triple matches the configured architecture.
freelinx_validate_arch() {
    case "$FREELINX_TRIPLE" in
        "$FREELINX_ARCH"*)
            freelinx_info "arch/triple: ${FREELINX_ARCH} / ${FREELINX_TRIPLE} - match"
            return 0
            ;;
        *)
            freelinx_warn "triple '${FREELINX_TRIPLE}' does not start with arch '${FREELINX_ARCH}'"
            return 1
            ;;
    esac
}