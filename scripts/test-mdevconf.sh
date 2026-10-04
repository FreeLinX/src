#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 FreeLinX OS Project.
#
# test-mdevconf.sh - who can do what with the nodes in /dev.
#
#   sh scripts/test-mdevconf.sh
#
# mdevd's rules are not decoration.  It walks them in order, takes the first one
# whose regex matches the whole device name, and then chmods and chowns the node
# to what that rule says - including when devtmpfs already made the node and the
# permissions the kernel chose were right.  A device that matches no rule gets
# mdevd's built-in default, 0660 root:root.
#
# So "the rule is missing" does not mean "the kernel's mode stands".  It means
# root:root 0660, which is how /dev/null became unusable for a normal user and
# took /etc/profile down with it: the first "cmd >/dev/null" failed and the
# login never finished.
#
# Reading the file cannot catch that.  This resolves it the way mdevd does - the
# first full-name regex match wins, in file order - over the device names the
# kernel always creates, and compares the mode and owner that come out.
#
#   sh test-mdevconf.sh                 check the shipped rootfs
#   ROOTFS=DIR sh test-mdevconf.sh     check another one (base stages one)

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOTFS=${ROOTFS:-$HERE/../rootfs}
CONF=${MDEV_CONF:-$ROOTFS/etc/mdev.conf}

[ -f "$CONF" ] || { printf 'no %s\n' "$CONF" >&2; exit 2; }

pass=0
fail=0

ok() {
	pass=$((pass + 1))
	printf '  ok   %s\n' "$1"
}

no() {
	fail=$((fail + 1))
	printf '  FAIL %s\n' "$1"
}

# resolves DEV - print "uid:gid mode" the way mdevd would apply it
#
# mdevd anchors a device regex to the whole name: "tty" matches the controlling
# terminal and "tty[0-9]*" does not, and ".*" matches everything.  "^("$1")$" is
# that anchoring; "^...$" is also the only thing keeping a rule for one device
# from silently catching the one after it.
resolves() {
	awk -v dev="$1" '
		/^[[:space:]]*($|#)/ { next }
		{
			re = $1
			own = $2
			mode = $3
			if (match(dev, "^(" re ")$")) {
				print own " " mode
				exit
			}
		}
	' "$CONF"
}

# want DEV UID:GID MODE - does the first matching rule say what it should?
want() {
	_got=$(resolves "$1")
	if [ "$_got" = "$2 $3" ]; then
		ok "$1 is $2 $3"
	else
		no "$1 is ${_got:-<no rule>} - expected $2 $3"
	fi
}

echo '== /etc/mdev.conf is a file mdevd can read =='
if awk '/^[[:space:]]*($|#)/ { next } NF < 3 { print NR ": " $0; bad = 1 }
	END { exit bad ? 1 : 0 }' "$CONF"; then
	ok 'every rule has a regex, an owner and a mode'
else
	no 'every rule has a regex, an owner and a mode'
fi

echo '== the nodes every boot needs =='
want null 0:0 0666
want zero 0:0 0666
want full 0:0 0666
want random 0:0 0666
want urandom 0:0 0666
want shm 0:0 0666
want tty 0:0 0666

echo '== consoles =='
want console 0:5 0600
want kmsg 0:5 0660
want tty0 0:5 0660
want tty1 0:5 0660
want ttyS0 0:5 0660
want ptmx 0:5 0666
want pts/0 0:5 0620

echo '== storage and media =='
want vda 0:6 0660
want vda3 0:6 0660
want sda 0:6 0660
want mmcblk0p1 0:6 0660
want sr0 0:6 0660

echo '== hardware =='
want fb0 0:44 0660
want input/event0 0:97 0660

echo '== hardware nobody has a rule for yet =='
want brandnew0 0:0 0660

echo '== nothing is below the catch-all =='
# First match wins, so a rule after ".*" can never be reached.  One there is not
# a rule that is too restrictive, it is a rule that is not there at all.
_dead=$(awk '/^[[:space:]]*($|#)/ { next }
	seen && $1 != ".*" { print $1 }
	$1 == ".*" { seen = 1 }' "$CONF")
if [ -z "$_dead" ]; then
	ok 'the catch-all is the last rule'
else
	no "rules after the catch-all can never match: $_dead"
fi

printf '\n%s passed, %s failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
