#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 FreeLinX OS Project.
#
# scripts/initramfs.sh - build a FreeLinX initramfs image.
#
# The two base images need two different initramfs images, and this is the only
# thing in the tree that builds either of them.  It used to be a checked-in
# 31 MB binary in iso/, built once by hand, which is why the shipped image
# disagreed with the source tree: iso/initramfs.img.gz carried a placeholder
# /init and an empty /var/service while src/rootfs/init had been a real runit
# init for weeks.  A build step nobody runs is a file that only ever goes
# stale, so it is a script now.
#
#   normal   the full rootfs, booting through the runit /init in
#            src/rootfs.  This is what base.iso uses: a real system, in RAM,
#            from which the installer is run.
#   rescue   a small static set - shell, fileutils, vim, runit, terminfo - that
#            boots to a rescue shell.  This is what 'base (boot only).iso'
#            uses, and a rescue shell is the correct thing for it to do.
#
# The two differ in what they contain, not in how they are built, which is why
# one script with a profile argument builds both.
#
# Non-GNU: the archive is written by the rootfs's own bsdtar (base/libarchive)
# and compressed by the rootfs's own gzip (archivers/gzip).  Both are musl
# binaries in this tree and are run through the toolchain's musl loader, so no
# GNU cpio and no host gzip ever touch a shipped artifact.  The host's
# cpio(1) is GNU cpio and is deliberately not used.
#
# Reproducible: entries are sorted, ownership is forced to 0:0 root:root, the
# mtime is pinned, and xattrs are dropped, so the same rootfs produces a
# byte-identical image.  That is what makes it possible to tell "the image
# changed" from "the build is not reproducible".
#
# usage: initramfs.sh [-o OUT] [-r ROOTFS] [normal|rescue]

set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/.." && pwd)
ROOTFS=${ROOTFS:-$SRC/rootfs}
TOOLCHAIN=${FREELINX_TOOLCHAIN_DIR:-$SRC/../TestForBase/toolchain}
OUTDIR=$SRC/../iso

PROFILE=
OUT=

die() { printf 'initramfs.sh: %s\n' "$*" >&2; exit 1; }

usage() {
	cat <<EOF
usage: ${0##*/} [-o OUT] [-r ROOTFS] normal|rescue

  normal   full rootfs, runit /init          -> base.iso
  rescue   small rescue set, rescue shell   -> 'base (boot only).iso'

  -o OUT   output path (default $OUTDIR/initramfs.img.gz,
            or initramfs-rescue.img.gz for the rescue profile)
  -r DIR   rootfs to package (default $ROOTFS)
EOF
}

while [ $# -gt 0 ]; do
	case $1 in
	-o) OUT=$2; shift ;;
	-r) ROOTFS=$2; shift ;;
	-h) usage; exit 0 ;;
	-*) die "unknown option: $1" ;;
	normal|rescue) PROFILE=$1 ;;
	*) die "unknown profile: $1 (want normal or rescue)" ;;
	esac
	shift
done
[ -n "$PROFILE" ] || { usage >&2; die 'no profile given'; }

[ -d "$ROOTFS" ] || die "rootfs not found: $ROOTFS"
[ -f "$ROOTFS/init" ] || die "no /init in $ROOTFS"

# The musl loader, so the rootfs's own binaries run on the build host.  The
# toolchain ships it; without it there is no non-GNU way to build this.
LOADER=$TOOLCHAIN/x86_64-linux-musl/lib/ld-musl-x86_64.so.1
[ -f "$LOADER" ] || die "musl loader not found: $LOADER"

# musl_prefix BINARY - print the loader needed to start BINARY here, or nothing.
#
# The rootfs binaries are a mix: /bin/sh, /bin/gzip and /sbin/runsvdir are
# static musl and start directly, while /bin/tar is a dynamic musl PIE that
# wants /lib/ld-musl-x86_64.so.1, which the glibc build host does not have.  So
# a runner that always went through the loader failed on every static binary
# with "Not a valid dynamic program", and one that never did failed on tar.
# Both cases have to work, hence this.
musl_prefix() {
	if file -b "$1" 2>/dev/null | grep -q 'dynamically linked'; then
		printf '%s' "$LOADER"
	fi
}

TAR=$ROOTFS/bin/tar
GZIP=$ROOTFS/bin/gzip
[ -f "$TAR" ] || die "no bsdtar at $TAR (the base/libarchive port)"
[ -x "$TAR" ] || die "$TAR is not executable"
[ -f "$GZIP" ] || die "no gzip at $GZIP (the archivers/gzip port)"
[ -x "$GZIP" ] || die "$GZIP is not executable"
command -v file >/dev/null 2>&1 ||
	die 'need file(1) to tell static musl binaries from dynamic ones'

# bsdtar has no gzip filter module in this build ("Unknown module name: gzip"),
# so the archive is written uncompressed and piped through the rootfs's own
# gzip.  That gzip is NetBSD's, from the archivers/gzip port, and it is static
# musl, so it runs on the build host directly with no loader.
#
# -9 pins the level so the bytes do not move with a zlib default, and -n omits
# the name and mtime from the gzip header, which would otherwise record when
# the build ran and make the image irreproducible.
TAR_RUN="${LOADER} $TAR"
GZIP_RUN="${GZIP_RUN:-$GZIP -9 -n}"

# Pinned so two builds of one rootfs are byte-identical.  2000-01-01T00:00:00Z
# is an arbitrary fixed point; any constant works, a moving one does not.
MTIME=${SOURCE_DATE_EPOCH:-946684800}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/flx-initramfs.XXXXXX")
trap 'rm -rf "$WORK"' EXIT INT TERM
STAGE=$WORK/stage
mkdir -p "$STAGE"

if [ -z "$OUT" ]; then
	if [ "$PROFILE" = rescue ]; then
		OUT=$OUTDIR/initramfs-rescue.img.gz
	else
		OUT=$OUTDIR/initramfs.img.gz
	fi
fi
mkdir -p "$(dirname "$OUT")"

# --- what goes in ----------------------------------------------------------
#
# rescue: the smallest tree that gives a shell, the tools to repair a
# filesystem with, vim, and runit.  Listed explicitly rather than globbed so
# that a tool vanishing from the rootfs is a build failure naming the tool,
# not a silently smaller image.
#
# normal: the whole rootfs.  It is what gets installed to disk, so it has to be
# the real system - a trimmed one would install a trimmed one.

# binutils (ar, nm, objcopy, ranlib, readelf, size, strings, strip) is not
# listed: the rescue image carries a shell and filesystem tools, and a stripped
# rescue image that no longer matches the rootfs is worse than one that never
# pretended to have them.  Anything added to this list has to exist in the
# rootfs or the build fails naming it, which is the point.
RESCUE_BIN='base64 basename cal cat chgrp chmod chown cksum cmp comm
compress cp cut date dc diff dirname dmesg du echo env expand fastfetch find
fold git grep gzip head hexdump hostname iconv id join jot kill ln logger ls
mkdir mktemp mount mv nc ninja nohup od paste printenv
printf renice rev rm rmdir sed seq sftp sh sleep sort split
ssh sshd ssh-keygen stat sync tail tee test time timeout touch tr
tsort umount uname unexpand uniq vim vis wc who xargs yes'

RESCUE_SBIN='runsv runsvdir'
RESCURE_USRBIN='chpst clear last patch pfetch sv'

if [ "$PROFILE" = rescue ]; then
	for b in $RESCUE_BIN; do
		[ -e "$ROOTFS/bin/$b" ] || die "rescue: no /bin/$b in the rootfs"
		mkdir -p "$STAGE/bin"
		cp -P "$ROOTFS/bin/$b" "$STAGE/bin/$b"
	done
	mkdir -p "$STAGE/sbin"
	for b in $RESCUE_SBIN; do
		[ -e "$ROOTFS/sbin/$b" ] || die "rescue: no /sbin/$b in the rootfs"
		cp -P "$ROOTFS/sbin/$b" "$STAGE/sbin/$b"
	done
	mkdir -p "$STAGE/usr/bin"
	for b in $RESCURE_USRBIN; do
		[ -e "$ROOTFS/usr/bin/$b" ] || die "rescue: no /usr/bin/$b in the rootfs"
		cp -P "$ROOTFS/usr/bin/$b" "$STAGE/usr/bin/$b"
	done

	# vim's runtime and the terminfo database, or vim starts and cannot draw a
	# screen and the shell is unusable in a way that looks like a broken kernel.
	for d in usr/share/vim usr/share/terminfo; do
		[ -d "$ROOTFS/$d" ] || die "rescue: no /$d in the rootfs"
		mkdir -p "$STAGE/$(dirname "$d")"
		cp -RP "$ROOTFS/$d" "$STAGE/$d"
	done

	mkdir -p "$STAGE/etc"
	cp -P "$ROOTFS/etc/os-release" "$STAGE/etc/os-release"
	# /etc/profile is sourced by the interactive shell (export ENV in the
	# normal /init) and by setup-user, so the rescue shell has to have it too.
	[ -f "$ROOTFS/etc/profile" ] && cp -P "$ROOTFS/etc/profile" "$STAGE/etc/profile"

	# A rescue shell is the point of this profile, so it gets a /init that says
	# so plainly instead of the runit one, which would find no services to
	# supervise and sit there.
	cat >"$STAGE/init" <<'RESCUE_INIT'
#!/bin/sh
# FreeLinX /init - rescue profile.
#
# This image is the RAM-only rescue environment: it exists to give you a shell
# and a few filesystem tools on a system that will not boot, not to run
# services.  So it mounts the pseudo filesystems, brings up the loopback
# interface and hands you a shell.
set -u
PATH=/sbin:/bin:/usr/sbin:/usr/bin
export PATH
HOME=/root
export HOME
TERM=${TERM:-linux}
export TERM
umask 022

mount -t proc     proc     /proc 2>/dev/null || :
mount -t sysfs    sysfs    /sys  2>/dev/null || :
mount -t devtmpfs devtmpfs /dev  2>/dev/null || :
mkdir -p /dev/pts /dev/shm /run
mount -t devpts -o gid=5,mode=0620 devpts /dev/pts 2>/dev/null || :
mount -t tmpfs tmpfs /run 2>/dev/null || :

ifconfig lo 127.0.0.1 up 2>/dev/null || flxifconfig lo up 2>/dev/null || :

echo "FreeLinX rescue shell.  This is the RAM-only rescue image; nothing here"
echo "is written to disk.  The full installer is on the 'base' image."
exec /bin/sh
RESCUE_INIT
	chmod 755 "$STAGE/init"
else
	# The normal profile is the rootfs, staged so nothing is copied that
	# /init or runit does not need, and so the build is a staging of the
	# source tree rather than a second, drifting copy of it.
	cp -a "$ROOTFS/." "$STAGE/"

	# A console shell, so the normal image comes up to something you can type
	# at.  The rootfs's own var/service/shell waits for /dev/ttyS1, which is
	# the installed system's serial console; Limine hands this image
	# console=ttyS0,115200, so that service would sit there failing and the
	# system would come up supervised by runit with no way in - which looks
	# exactly like a hang.
	#
	# It is added here rather than to src/rootfs/var/service so the installed
	# system's service set is unchanged.
	mkdir -p "$STAGE/var/service/flx-console"
	cat >"$STAGE/var/service/flx-console/run" <<'CONSOLE_RUN'
#!/bin/sh
# A login shell on whatever console this system actually has.
#
# Limine passes console=ttyS0,115200, so the serial line is the one to use; a
# virtual console is started too when the kernel made one, because a graphical
# terminal with no serial line is a normal way to run this image.
export TERM=${TERM:-linux}
export HOME=/root
export USER=root
export LOGNAME=root

started=
for tty in /dev/ttyS0 /dev/tty1; do
	[ -c "$tty" ] || continue
	started=yes
	/bin/sh -l <"$tty" >"$tty" 2>&1 &
done

# Nothing to attach to yet: the kernel may not have probed the serial port yet,
# and a service that exits immediately is restarted forever by runsvdir, which
# fills the log and never gives up.  Wait for a console instead.
if [ -z "$started" ]; then
	i=0
	while [ "$i" -lt 50 ]; do
		for tty in /dev/ttyS0 /dev/tty1; do
			[ -c "$tty" ] || continue
			exec /bin/sh -l <"$tty" >"$tty" 2>&1
		done
		i=$((i + 1))
		sleep 1
	done
	echo 'flx-console: no console found (/dev/ttyS0, /dev/tty1)' >&2
	exit 1
fi

wait
CONSOLE_RUN
	chmod 755 "$STAGE/var/service/flx-console/run"
fi

# --- the mount points the kernel expects ------------------------------------
#
# The kernel mounts these itself when it unpacks the archive, but a directory
# has to exist in the archive for the mount to land somewhere, and an initramfs
# whose /proc does not exist is a kernel panic on the first mount.
for d in proc sys dev tmp run var root etc; do
	mkdir -p "$STAGE/$d"
done

# --- sanity: the required pieces are actually in the stage ------------------

[ -f "$STAGE/init" ] || die "$PROFILE: staging produced no /init"
[ -x "$STAGE/bin/sh" ] || die "$PROFILE: staging produced no /bin/sh"
[ -x "$STAGE/bin/mount" ] || die "$PROFILE: staging produced no /bin/mount"

if [ "$PROFILE" = normal ]; then
	[ -x "$STAGE/sbin/runsvdir" ] ||
		die 'normal: no /sbin/runsvdir, so /init would fall back to a rescue
     shell.  Build the sysutils/runit port first.'
	[ -d "$STAGE/var/service" ] ||
		die 'normal: no /var/service, so runsvdir would supervise nothing and
     the system would come up with no way in.  The service run scripts live
     in src/rootfs/var/service.'
	# runsvdir needs runsv on PATH, not just runsvdir: runsvdir execs it to
	# supervise each service directory, and a missing runsv makes every
	# service fail to start with nothing on the console.
	[ -x "$STAGE/sbin/runsv" ] ||
		die 'normal: no /sbin/runsv.  runsvdir execs it to supervise each
     service; without it nothing starts even though runsvdir is present.'
	# A service that gives the operator a prompt.  Checked by name so that a
	# rootfs whose only services are dbus/xorg - which both exit when there is
	# no session - is reported here rather than as a system that boots to a
	# blank screen.
	if [ ! -d "$STAGE/var/service/flx-console" ] &&
	   ! -d "$STAGE/var/service/shell"; then
		die 'normal: /var/service has neither flx-console nor shell, so the
     system would come up with no console to type at.'
	fi
fi

# --- the archive ------------------------------------------------------------
#
# newc is the format the kernel's initramfs unpacker wants.  Sorted, with
# ownership and mtime pinned and xattrs dropped, so the output depends only on
# the contents and not on when or by whom it was built.

printf 'packing %s profile from %s\n' "$PROFILE" "$ROOTFS"
printf '  entries  %s\n' "$(find "$STAGE" | wc -l | tr -d ' ')"

# --- normalise the inode numbers --------------------------------------------
#
# bsdtar writes each entry's real inode number into the newc header, and the
# staging directory is a fresh mktemp every run, so those numbers differ on
# every build.  Everything else in the header is already pinned, which is why
# the archive was otherwise byte-identical:
#
#   u1: 070701 0002a6a0 000041ed 00000000 ...
#   u2: 070701 0002b936 000041ed 00000000 ...
#            ^^^^^^ inode, the only field that moved
#
# bsdtar has no option for this ("--help" offers no inode control), so the
# field is rewritten to 0 after the fact.  Nothing reads the inode of an
# initramfs entry: the kernel unpacks the archive into a ramfs/tmpfs and every
# node in it is new, so the number is pure build noise.
#
# The rewrite walks the archive by its own structure.  A newc header is exactly
# 110 bytes of ASCII hex - no NUL bytes, so it survives $(...) intact - laid
# out as:
#
#   magic 6 | ino 8 | mode 8 | uid 8 | gid 8 | nlink 8 | mtime 8 | filesize 8
#   | devmajor 8 | devminor 8 | rdevmajor 8 | rdevminor 8 | namesize 8 | check 8
#
# followed by namesize bytes of name, then the file data, each padded to a
# 4-byte boundary.  The trailer entry has namesize 0.
hexdigits() {
	printf '%s' "$1" | tr -cd '0-9a-fA-F'
}

normalise_inodes() {
	_src=$1
	_dst=$2
	_pos=0
	_size=$(wc -c <"$_src" | tr -d ' ')

	while [ "$_pos" -lt "$_size" ]; do
		# The magic, read as bytes: an all-zero field anywhere else in the
		# header is not a reason to stop, only a bad magic is.
		_m6=$(dd if="$_src" bs=1 skip="$_pos" count=6 2>/dev/null)
		[ "$_m6" = 070701 ] || break

		# Read the two size fields as bytes rather than as text.  cut -c on
		# $( ) loses NULs, and a header whose namesize field is all NULs --
		# which is what an all-zero field looks like -- came back empty and
		# made the arithmetic below fail.  hexdigits turns the 8 hex
		# characters into a value, treating a missing character as 0.
		_fsz=$((0x$(hexdigits "$(dd if="$_src" bs=1 skip=$(( _pos + 54 )) count=8 2>/dev/null)")))
		_nsz=$((0x$(hexdigits "$(dd if="$_src" bs=1 skip=$(( _pos + 94 )) count=8 2>/dev/null)")))

		# The name and the data each start on a 4-byte boundary, counted
		# from the start of the entry -- not from the start of the name.  The
		# header is 110 bytes and 110 % 4 is 2, so the two differ on every
		# entry: with "./d" (nsz 4) the data begins at 110 + 4 = 114 and two
		# bytes of padding follow, so the next header is at 116, not 114.
		# Getting this wrong desynchronises the walk on the second entry and
		# the rewrite emits a 112-byte fragment.
		_nstart=$(( _pos + 110 ))
		_npad=$(( ( 4 - ( ( _nstart + _nsz ) % 4 ) ) % 4 ))
		_dstart=$(( _nstart + _nsz + _npad ))
		_dpad=$(( ( 4 - ( ( _dstart + _fsz ) % 4 ) ) % 4 ))
		_rest=$(( _nsz + _npad + _fsz + _dpad ))

		# The header with the inode field (bytes 7..14) zeroed.  Built by
		# dd rather than printf|cut, because `cut -c` counts characters and
		# the shell's $( ) strips NULs: the trailer's own header contains a
		# NUL in the name-size field, and every header gained or lost bytes
		# that way, so the output was 2 bytes longer per entry than the input
		# and the length check below failed on every build.
		dd if="$_src" bs=1 skip="$_pos" count=6 2>/dev/null >"$_dst.tmp"
		printf '00000000' >>"$_dst.tmp"
		dd if="$_src" bs=1 skip=$(( _pos + 14 )) count=96 2>/dev/null \
			>>"$_dst.tmp"
		# ... then the name and data, verbatim.
		if [ "$_rest" -gt 0 ]; then
			dd if="$_src" bs=1 skip=$(( _pos + 110 )) count="$_rest" \
				2>/dev/null >>"$_dst.tmp"
		fi
		cat "$_dst.tmp" >>"$_dst"
		rm -f "$_dst.tmp"

		_pos=$(( _pos + 110 + _rest ))
	done

	# bsdtar pads the archive out to a 10240-byte block multiple, so there is
	# always a run of NULs after the TRAILER!!! entry.  The kernel ignores
	# anything past the trailer, but dropping those bytes would make this
	# rewrite change the file's length, which is the check below looking for a
	# walk that desynchronised.  Copy the tail verbatim so the rewrite is
	# length-preserving and the check stays meaningful.
	if [ "$_pos" -lt "$_size" ]; then
		dd if="$_src" bs=1 skip="$_pos" 2>/dev/null >>"$_dst"
	fi
}

# bsdtar's status is checked on its own, not through a pipe: in a pipeline the
# shell reports the status of the last command, so a bsdtar failure would be
# reported as a successful build of a truncated archive, which then fails much
# later as an unpack error with nothing pointing back here.
(
	cd "$STAGE"
	# -print0 with --null, not -print: a filename with a newline in it would
	# otherwise be read as two paths and tar would fail on the first half with
	# "Cannot stat", which reads as a corrupt rootfs rather than a quoting bug.
	find . -mindepth 1 -print0 | LC_ALL=C sort -z |
		$TAR_RUN \
			--format=newc \
			--null \
			--no-recursion \
			--uid 0 --gid 0 --uname root --gname root \
			--mtime "@$MTIME" \
			--no-xattrs --no-acls --no-fflags \
			-cf - -T -
) >"$WORK/initramfs.raw" ||
	die "bsdtar failed for $PROFILE"

[ -s "$WORK/initramfs.raw" ] || die 'bsdtar produced an empty archive'

[ -n "${FLX_INITRAMFS_KEEP_WORK:-}" ] && cp "$WORK/initramfs.raw" /tmp/flx-initramfs.raw

normalise_inodes "$WORK/initramfs.raw" "$WORK/initramfs.newc"
[ -s "$WORK/initramfs.newc" ] || die 'inode normalisation produced nothing'

# The rewrite has to have consumed the whole archive: if it stopped early the
# image would be a valid-looking prefix of the real one, and the kernel would
# unpack it and then fail to find /init with no clue why.
_n_in=$(wc -c <"$WORK/initramfs.newc" | tr -d ' ')
[ "$_n_in" -eq "$(wc -c <"$WORK/initramfs.raw" | tr -d ' ')" ] ||
	die "inode normalisation changed the size ($_n_in vs $(wc -c <"$WORK/initramfs.raw" | tr -d ' ')); the archive layout is not what this expects"

$GZIP_RUN -c "$WORK/initramfs.newc" >"$WORK/initramfs.newc.gz" ||
	die "gzip failed for $PROFILE"

[ -s "$WORK/initramfs.newc.gz" ] || die "bsdtar produced an empty archive"

# The magic is the only thing that proves this is a newc cpio and not a tar
# that happens to be named .cpio, and the kernel does not say anything useful
# when it gets the wrong one - it just fails to unpack.
# Checked on the uncompressed archive, at offset 0.  Checking the .gz meant
# skipping a 10-byte gzip header, and the 18 that was skipped instead landed in
# the middle of the first filename, so a perfectly good archive was reported as
# malformed.
_magic=$(dd if="$WORK/initramfs.newc" bs=1 count=6 2>/dev/null)
[ "$_magic" = 070701 ] ||
	die "not a newc cpio (magic was '$_magic', wanted 070701)"

gzip -t "$WORK/initramfs.newc.gz" 2>/dev/null ||
	die 'the compressed archive does not pass gzip -t'

cp "$WORK/initramfs.newc.gz" "$OUT"

printf '  output   %s (%s bytes)\n' "$OUT" "$(wc -c <"$OUT" | tr -d ' ')"
printf '  sha256   %s\n' "$(sha256sum "$OUT" | awk '{print $1}')"
