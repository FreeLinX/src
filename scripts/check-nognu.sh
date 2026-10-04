#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 FreeLinX OS Project.
#
# check-nognu.sh - report GNU and GCC contamination in a staged rootfs.
#
# The rule this enforces: nothing in a FreeLinX image may be built by GCC or
# linked against glibc.  The userland is musl and clang/LLD throughout, so a
# single GCC object or glibc DT_NEEDED is a defect, not a style preference.
#
# A file is a violation when any of these hold:
#   * its interpreter is not musl
#   * it needs a glibc or GCC runtime library (libc.so.6, libstdc++.so.6,
#     libgcc_s.so.1 and friends)
#   * it carries version symbols from those runtimes (GLIBC_, GLIBCXX_, GCC_)
#   * it was compiled by GCC
#
# A file is a warning when it carries both a GCC and a clang marker: a
# prebuilt GCC archive was linked into an otherwise clang link.  Worth seeing,
# but not a hard failure, so -s is needed to make it one.
#
# Findings are one line each, "V<TAB>path<TAB>reason; reason", so the file can
# be grepped, counted and sorted without any awk.

set -eu

PROG=${0##*/}

usage() {
	cat <<EOF
usage: $PROG [-s] [-v] ROOTFS

  -s   fail on warnings as well as violations
  -v   list every ELF file that was checked, not just the bad ones
EOF
}

STRICT=0
VERBOSE=0
ROOT=

while [ $# -gt 0 ]; do
	case $1 in
	-s) STRICT=1 ;;
	-v) VERBOSE=1 ;;
	-h) usage; exit 0 ;;
	-*) usage >&2; exit 1 ;;
	*)  ROOT=$1 ;;
	esac
	shift
done

[ -n "$ROOT" ] || { usage >&2; exit 1; }
[ -d "$ROOT" ] || { echo "$PROG: $ROOT is not a directory" >&2; exit 1; }

RED= GREEN= CYAN= YELLOW= BOLD= NC=
if [ -t 1 ]; then
	RED=$(printf '\033[31m'); GREEN=$(printf '\033[32m')
	CYAN=$(printf '\033[36m'); YELLOW=$(printf '\033[33m')
	BOLD=$(printf '\033[1m'); NC=$(printf '\033[0m')
fi

READELF=${READELF:-llvm-readelf}
command -v "$READELF" >/dev/null 2>&1 || READELF=readelf
if ! command -v "$READELF" >/dev/null 2>&1; then
	echo "$PROG: no readelf; set READELF=/path/to/llvm-readelf" >&2
	exit 1
fi

# Shared libraries that mean a glibc or GCC build.  musl exposes libc.so and
# ld-musl-*.so.1, neither of which is here.
# GNU code linked in statically leaves no DT_NEEDED and, built by clang, no
# GCC marker; it is found by its own strings (ncurses' NCURSES_NO_PADDING,
# "GNU Readline", a gnu.org help address, ...).
GNU_SIGS='NCURSES_NO_PADDING|ncurses 6\.[0-9]|GNU Readline|readline-[0-9]\.[0-9]|GNU gettext|GNU libiconv|Libgcrypt [0-9]|libgpg-error [0-9]|GNU Wget|GNU bash, version|GNU coreutils|GNU Make [0-9]|GNU MP |GNU MPFR|GnuTLS [0-9]|GNU libunistring|GNU nano [0-9]|GNU tar [0-9]|GNU findutils|GNU diffutils|GNU Awk|gnu\.org/gethelp|home page: <https?://www\.gnu\.org/software/'
GNU_LIB_RE='Shared library: \[(libc\.so\.6|libm\.so\.6|libpthread\.so\.0|libdl\.so\.2|librt\.so\.1|libutil\.so\.1|libresolv\.so\.2|libcrypt\.so\.1|libnsl\.so\.1|libstdc\+\+\.so\.6|libgcc_s\.so\.1|libatomic\.so\.1|libanl\.so\.1|libBrokenLocale\.so\.1)\]'

list=$(mktemp)
findings=$(mktemp)
dump=$(mktemp)
trap 'rm -f "$list" "$findings" "$dump"' EXIT

# One name per line.  This used to be find -print0 read back with read -d '',
# which is bash: under sh the read failed at once, the loop never ran, and the
# check reported "no GNU contamination in 0 ELF files" for any tree at all.
find "$ROOT" -type f >"$list" 2>/dev/null || :

n_elf=0

# Redirection from a file rather than a pipe keeps this loop in the current
# shell, so the counters survive.  Piping into while would run it in a
# subshell and silently zero everything.
while IFS= read -r f; do
	# One readelf pass per file covers every check: -h to prove it is ELF at
	# all, -d for DT_NEEDED, -l for the interpreter, -p .comment for the
	# compiler markers.
	"$READELF" -h -d -l -p .comment "$f" >"$dump" 2>/dev/null || continue

	# Test the ELF header, not the presence of a dynamic section.  A
	# statically linked binary has no DT_NEEDED and no interpreter, so
	# keying off those would skip exactly the files most likely to be a
	# hand-built GCC blob.
	grep -q 'ELF Header:' "$dump" || continue

	rel=${f#"$ROOT"/}
	n_elf=$((n_elf + 1))
	if [ "$VERBOSE" -eq 1 ]; then
		printf '  %s\n' "$rel"
	fi

	reasons=

	interp=$(sed -n 's/.*interpreter: \(.*\)\]/\1/p' "$dump" | head -1)
	if [ -n "$interp" ] && ! printf '%s' "$interp" | grep -q 'ld-musl'; then
		reasons="$reasons; interpreter is $interp, not musl"
	fi

	# -E because the alternation in GNU_LIB_RE is an ERE, not a BRE.
	badlibs=$(grep -oE "$GNU_LIB_RE" "$dump" | sed 's/.*\[//; s/\]//' \
		| sort -u | tr '\n' ' ')
	if [ -n "$badlibs" ]; then
		reasons="$reasons; needs $badlibs (a GNU runtime library)"
	fi

	badvers=$(grep -o 'GLIBC_[0-9.]*\|GLIBCXX_[0-9.]*\|GCC_[0-9.]*\|CXXABI_[0-9.]*' \
		"$dump" | sort -u | tr '\n' ' ')
	if [ -n "$badvers" ]; then
		reasons="$reasons; carries GNU version symbols: $badvers"
	fi

	ngcc=$(grep -c 'GCC: (GNU)' "$dump" || true)
	nclang=$(grep -c 'clang version' "$dump" || true)
	if [ "$ngcc" -gt 0 ]; then
		ver=$(grep -o 'GCC: (GNU) [0-9.]*' "$dump" | head -1)
		if [ "$nclang" -gt 0 ]; then
			# Both markers in one binary: a GCC object linked into an
			# otherwise clang/LLD link.
			reasons="$reasons; mixed build: $ver alongside clang"
		else
			reasons="$reasons; built by $ver"
		fi
	fi

	gnulibs=$(grep -oE 'Shared library: \[lib(ncurses|tinfo|form|menu|panel)w?\.so[^]]*\]|Shared library: \[lib(readline|history|intl|iconv|gmp|mpfr|gcrypt|gpg-error|gnutls|unistring|idn2?)\.so[^]]*\]' "$dump" |
		sed 's/.*\[//; s/\]//' | sort -u | tr '\n' ' ')
	if [ -n "$gnulibs" ]; then
		reasons="$reasons; needs $gnulibs (a GNU library)"
	fi
	if LC_ALL=C grep -aqE "$GNU_SIGS" "$f" 2>/dev/null; then
		reasons="$reasons; GNU code linked in ($(LC_ALL=C grep -aoE "$GNU_SIGS" "$f" | head -1))"
	fi

	[ -n "$reasons" ] || continue

	# Leading "; " is trimmed so the column lines up.
	printf 'V\t%s\t%s\n' "$rel" "${reasons#; }" >>"$findings"
done <"$list"

n_violation=$(grep -c '^V' "$findings" || true)
n_warning=0

printf '%s%s: audited %s%s\n' "$BOLD" "$PROG" "$ROOT" "$NC"

if [ "$n_violation" -gt 0 ]; then
	printf '\n%sGNU violations (%s):%s\n' "$RED" "$n_violation" "$NC"
	grep '^V' "$findings" | while IFS=$(printf '\t') read -r _tag path why; do
		printf '%s%s%s\n' "$RED" "$path" "$NC"
		printf '    %s\n' "$why"
	done
fi

if [ "$n_violation" -eq 0 ]; then
	printf '\n%s%self: no GNU contamination in %d ELF files%s\n' \
		"$GREEN" "$BOLD" "$n_elf" "$NC"
else
	printf '\n%s%d violations, %d ELF files checked%s\n' \
		"$BOLD" "$n_violation" "$n_elf" "$NC"
fi

[ "$n_violation" -eq 0 ] || exit 1
[ "$STRICT" -eq 0 ] || [ "$n_warning" -eq 0 ] || exit 1
exit 0
