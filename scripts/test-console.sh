#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 FreeLinX OS Project.
#
# test-console.sh - what reaches the screen while the system boots.
#
#   sh scripts/test-console.sh
#
# Everything a runit service or /init writes to stdout or stderr goes to
# /dev/console, and /dev/console is the terminal the operator is typing on.  A
# service that logs there does not add to the boot: it interleaves with the
# keystrokes.  ntpd -d and dbus --print-address both did exactly that, and the
# fix is only worth anything if it holds, so it is checked here rather than by
# reading the run script and believing it.
#
# The service run scripts are executed, not inspected.  Each one is run in a
# private mount namespace with its own /sbin over the real one, holding a stub
# that writes one line to stdout and one to stderr.  Anything that appears on
# the harness's own stdout is console pollution; anything in the log file is
# where it was supposed to go.  A grep could tell you the run script has a
# redirection in it; only running it tells you the redirection is on the right
# stream.
#
#   sh test-console.sh          check the shipped rootfs
#   ROOTFS=DIR sh test-console.sh   check another one (the base image stages one)

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOTFS=${ROOTFS:-$HERE/../rootfs}
[ -d "$ROOTFS" ] || { printf 'no rootfs at %s\n' "$ROOTFS" >&2; exit 2; }

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

# have WHAT WANT - is the file's text containing WANT?
have() {
	if grep -q -- "$2" "$1"; then ok "$3"; else no "$3"; fi
}

hasnt() {
	if grep -q -- "$2" "$1"; then no "$3"; else ok "$3"; fi
}

# --- the scripts parse --------------------------------------------------------
echo '== every service run script and /init parses =='
for f in "$ROOTFS/init" "$ROOTFS"/var/service/*/run; do
	[ -f "$f" ] || continue
	if sh -n "$f" 2>/dev/null; then
		ok "${f#"$ROOTFS"/} parses"
	else
		no "${f#"$ROOTFS"/} does not parse"
	fi
done

# --- /dev/console is not where a service writes ------------------------------
echo '== a service does not write to the console it inherits =='
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM

# The services are run in a chroot holding a shell and its libraries, a stub for
# each one, and nothing else.  The stub writes one line to stdout and one to
# stderr and exits; stdout is inherited from this script, which is the same
# position /dev/console is in on a real boot.  So a run script with no
# redirection hands both lines back here, and one that redirects hands back
# neither.  A grep could only tell you a redirection is present; it cannot tell
# you which stream it is on, or that the exec is the one carrying it.
#
# unshare -r gives the chroot a uid 0 in a private user namespace, so no bind
# mounts and nothing on this host is reachable from inside.
CROOT=$TMP/croot
build_croot() {
	mkdir -p "$CROOT"/bin "$CROOT"/usr/bin "$CROOT"/sbin "$CROOT"/lib64 \
		"$CROOT"/etc "$CROOT"/run "$CROOT"/tmp "$CROOT"/var/log \
		"$CROOT"/var/lib/dbus "$CROOT"/service || return 1
	for b in sh sleep mkdir cp cat; do
		p=$(command -v "$b") || return 1
		cp "$p" "$CROOT/usr/bin/$b" || return 1
		ldd "$p" 2>/dev/null | awk '{ print $3 }' | grep '^/' |
			while read -r l; do
				mkdir -p "$CROOT$(dirname "$l")" &&
					cp -Ln "$l" "$CROOT$l"
			done
	done
	# bash links its interpreter by absolute path and so does every library.
	ldd "$CROOT/usr/bin/sh" 2>/dev/null | awk '/ld-linux/ { print $1 }' |
		while read -r l; do
			mkdir -p "$CROOT$(dirname "$l")" && cp -Ln "$l" "$CROOT$l"
		done
	# The stubs carry #!/bin/sh, and so do the real run scripts.
	ln -sf ../usr/bin/sh "$CROOT/bin/sh" || return 1
	return 0
}

# stub NAME SCRIPT FILE - a stand-in for a program, writing MARKER to whichever
# streams the real one writes to under the flags it was given.  Modelling the
# flag is the point: the defect being looked for is a flag that makes a daemon
# talk on a stream the run script forgot to redirect, so a stub that spoke
# unconditionally would report a leak whether or not there was one.
#
# A marker on the console is a leak.  A marker that appears nowhere at all means
# the stub never ran, which is a failure of this harness and not a clean service.
#
#   dbus-daemon --print-address=1   writes the bus address to stdout
#   ntpd -d                         stays in the foreground and logs to stderr
#   dbus-uuidgen                    writes the machine id to stdout, and the
#                                   run script redirects that into /var/lib
stub() {
	{
		printf '%s\n' '#!/bin/sh' "m=\"marker from $1\""
		# Say that it ran at all, separately from what it says.  A stub
		# that only speaks when the flag is there cannot tell "clean" and
		# "never started" apart, and this harness would report the second
		# as the first.
		printf '%s\n' 'echo ran >>/tmp/ran'
		case $2 in
		*always*)
			printf '%s\n' 'echo "$m on stdout"'
			;;
		*)
			printf '%s\n' 'for a in "$@"; do' '	case $a in'
			case $2 in
			*print*) printf '%s\n' \
				'	--print-address*) echo "$m on stdout"; break ;;' ;;
			esac
			case $2 in
			*d*) printf '%s\n' \
				'	-d) echo "$m on stderr" >&2; break ;;' ;;
			esac
			printf '%s\n' '	esac' 'done'
			;;
		esac
		printf '%s\n' 'exit 0'
	} >"$3"
	chmod 755 "$3"
}

if ! unshare -r true 2>/dev/null; then
	printf '  (no unshare -r: cannot run the services, skipping)\n'
elif ! build_croot; then
	printf '  (could not build the chroot, skipping)\n'
else
	for svc in ntpd dbus; do
		run=$ROOTFS/var/service/$svc/run
		[ -f "$run" ] || continue
		case $svc in
		ntpd) at=$CROOT/sbin/ntpd;          flags=d ;;
		dbus) at=$CROOT/usr/bin/dbus-daemon; flags=print ;;
		esac
		rm -f "$at"
		stub "$svc" "$flags" "$at"
		[ "$svc" = dbus ] && stub dbus-uuidgen always "$CROOT/usr/bin/dbus-uuidgen"
		cp "$run" "$CROOT/service/run"
		rm -f "$CROOT"/var/log/*.log "$CROOT/tmp/ran"
		out=$(unshare -r chroot "$CROOT" /usr/bin/sh /service/run 2>&1)

		# Did the stub run at all, and did anything it said get here?
		if [ -s "$CROOT/tmp/ran" ]; then ran=yes; else ran=no; fi
		case $out in
		*"marker from $svc"*)
			no "$svc wrote to the console it inherited"
			printf '       got: %s\n' "$out"
			;;
		*)
			case $ran in
			yes) ok "$svc writes nothing to the console it inherited" ;;
			*) no "$svc could not be run, so nothing was checked"
			   printf '       got: %s\n' "$out" ;;
			esac
			;;
		esac
		if [ "$svc" = ntpd ] && [ "$ran" = yes ]; then
			if grep -q 'marker from ntpd' "$CROOT/var/log/ntpd.log" 2>/dev/null
			then
				ok 'ntpd logs to /var/log/ntpd.log'
			else
				no 'ntpd logs nowhere: /var/log/ntpd.log has none of its output'
			fi
		fi
	done
fi

# flxconsole is a shell script, so there is nothing to stub: the run script is
# `exec /sbin/flxconsole` and the program it execs is the one in this tree, so
# running it runs the whole of it.  Its stdout is this script's stdout, which is
# where /dev/console is on a real boot: anything it prints there lands on the
# screen the operator is looking at.
#
# The one thing substituted is the list of terminal names.  A chroot has no
# /dev/tty1, and left alone the script spends fifty seconds waiting for one to
# appear.  /dev/null is named instead: it is a character device, it is always
# there, and writing a banner to it discards the banner, which is what the check
# does not care about.  Everything else - open_console, the redirect, the shell -
# is the code as shipped.
echo '== flxconsole writes nothing to the console =='
mkdir -p "$CROOT/dev" "$CROOT/sbin"
sed "s#^TTYS=.*#TTYS='/dev/null'#" "$ROOTFS/sbin/flxconsole" \
	>"$CROOT/sbin/flxconsole"
if unshare -r -m true 2>/dev/null && [ -f "$CROOT/sbin/flxconsole" ]
then
	# The host's /dev is bound in so that /dev/null is a real character
	# device inside.  Only /dev/null is opened: TTYS names nothing else.
	# --rbind and not --bind: /dev carries submounts, and a non-recursive
	# bind of a tree with submounts is refused.
	unshare -r -m sh -c "mount --rbind /dev '$CROOT/dev' &&
		exec chroot '$CROOT' /usr/bin/sh /sbin/flxconsole" \
		>"$TMP/flx.out" 2>"$TMP/flx.err"
	rc=$?
	if [ "$rc" -ne 0 ]; then
		no "flxconsole could not be run, so nothing was checked"
		printf '       rc=%s, stderr: %s\n' "$rc" "$(cat "$TMP/flx.err")"
	elif [ -s "$TMP/flx.out" ]; then
		no 'flxconsole printed to stdout, which is the screen'
		sed 's/^/       /' "$TMP/flx.out"
	else
		ok 'flxconsole puts its banner on the terminal and nothing on stdout'
	fi
	# The shell it starts has to have gone, or the check above would have
	# passed simply because the script was still running.
	if [ -s "$TMP/flx.err" ]; then
		no 'flxconsole wrote to stderr on a healthy boot'
		sed 's/^/       /' "$TMP/flx.err"
	else
		ok 'flxconsole says nothing on stderr either'
	fi
else
	printf '  (no unshare -r -m: cannot run flxconsole, skipping)\n'
fi
# Nothing else on this system shows /etc/motd: there is no getty and no login in
# base, so a banner flxconsole does not write is a banner nobody sees.  The tty
# above is /dev/null, so what it received cannot be read back; which file is
# sent there is all that can be asked here.
#
# The name is not what is looked for.  /etc/motd is named in the comments above
# the code that uses it, so grepping for the string proves nothing - an earlier
# version of this check passed with the line deleted.  What is asked for is the
# redirect: the banner goes to the terminal being opened.
if grep -q 'cat /etc/motd >"\$1"' "$ROOTFS/sbin/flxconsole"; then
	ok 'flxconsole writes /etc/motd to the console it is opening'
else
	no 'flxconsole does not write /etc/motd to the console'
fi
if [ -f "$ROOTFS/etc/motd" ]; then
	ok '/etc/motd exists to be shown'
else
	no '/etc/motd is missing, so there is no banner at the prompt'
fi
# The banner lands on a cleared screen.  ESC[2J on its own erases from the
# cursor down, and the cursor is wherever the boot log finished, so the log would
# stay and push the banner off the bottom of the screen.  ESC[H has to come
# first, or the whole thing is in the wrong place.
_flx_body=$(sed -n '/^open_console() {/,/^}/p' "$ROOTFS/sbin/flxconsole")
if printf '%s\n' "$_flx_body" | grep -q '033\[H.*033\[2J'; then
	ok 'flxconsole clears the screen before the banner, home cursor first'
else
	no 'flxconsole does not clear the screen before the banner'
fi
if [ "$(printf '%s\n' "$_flx_body" | grep -c '033\[H')" = 1 ]; then
	ok 'the banner is cleared once, not on every redraw'
else
	no 'the clear is written more than once in open_console'
fi
# And the clear has to come before the banner, not after, or the banner is what
# gets wiped.
_cl=$(printf '%s\n' "$_flx_body" | grep -n '033\[H' | cut -d: -f1)
_ba=$(printf '%s\n' "$_flx_body" | grep -n 'cat /etc/motd' | cut -d: -f1)
if [ -n "$_cl" ] && [ -n "$_ba" ] && [ "$_cl" -lt "$_ba" ]; then
	ok 'the clear comes before the banner, so the banner survives'
else
	no 'the clear does not come before the banner'
fi

# A shell in a runit service has no controlling terminal.  It says so on every
# start, and under those two lines Ctrl-C reaches nothing, because there is no
# terminal to raise SIGINT from and no foreground job for it to interrupt.  Both
# services flxconsole replaced ran `setsid -c` for exactly this, so dropping it
# was a regression rather than a simplification.
if printf '%s\n' "$_flx_body" | grep -q '\$SETSID -c "\$LOGIN_SHELL" -l'; then
	ok 'flxconsole gives the shell a controlling terminal with setsid -c'
else
	no 'flxconsole starts the shell with no controlling terminal'
fi
# /usr/bin/setsid is in the released image and not in this tree, so a check that
# only knows about that path would pass on a system that cannot run it.
if grep -q "SETSID='/bin/toybox setsid'" "$ROOTFS/sbin/flxconsole"; then
	ok 'it finds setsid through toybox where there is no /usr/bin/setsid'
else
	no 'it only knows about /usr/bin/setsid, which this tree does not have'
fi
if printf '%s\n' "$_flx_body" | grep -q 'cd /root 2>/dev/null || cd /'; then
	ok 'flxconsole puts the shell in /root, not the service directory'
else
	no 'the shell opens in the service directory'
fi

# An installed system has a root password, so its consoles have to ask for it.
# A console that opens a shell regardless makes the password decorative.
if grep -q '/etc/flx-installed' "$ROOTFS/sbin/flxconsole"; then
	ok 'flxconsole can tell an installed system from the live medium'
else
	no 'flxconsole cannot tell an installed system from the live medium'
fi
if sed -n '/^open_console() {/,/^}/p' "$ROOTFS/sbin/flxconsole" |
	grep -q 'GETTY -l "\$LOGINPROG"'
then
	ok 'an installed system gets a getty asking for a login'
else
	no 'an installed system gets no login prompt'
fi
# The check that matters: the shell must be on the far side of that test, so a
# missing getty cannot quietly fall through to it.
_live=$(sed -n '/^open_console() {/,/^}/p' "$ROOTFS/sbin/flxconsole" |
	grep -n 'if \[ -e /etc/flx-installed \]' | cut -d: -f1)
# Two lines start the shell - the normal one and the no-setsid fallback - and
# the first is the one that has to sit behind the installed-system test.
_shell=$(sed -n '/^open_console() {/,/^}/p' "$ROOTFS/sbin/flxconsole" |
	grep -n 'LOGIN_SHELL" -l' | head -1 | cut -d: -f1)
if [ -n "$_live" ] && [ -n "$_shell" ] && [ "$_live" -lt "$_shell" ]; then
	ok 'the installed-system test comes before the shell is started'
else
	no 'the shell can be started without the installed-system test'
fi
if grep -q "GETTY='/bin/toybox getty'" "$ROOTFS/sbin/flxconsole"; then
	ok 'it finds getty through toybox, since /usr/bin/getty is not in this tree'
else
	no 'it only knows about /usr/bin/getty, which this tree does not have'
fi

# --- /var/log exists, and exists before any service could want it ------------
echo '== /var/log =='
if [ -d "$ROOTFS/var/log" ]; then
	ok 'the rootfs ships /var/log'
else
	no 'the rootfs has no /var/log, so every service has to log to the console'
fi
have "$ROOTFS/init" '/var/log' '/init makes /var/log before starting anything'
have "$ROOTFS/init" 'exec /sbin/runsvdir' '/init hands over to runsvdir'

# --- one mdevd, not two ------------------------------------------------------
# Two processes, one uevent netlink socket: one binds it, one fails, and runit
# restarts the loser once a second for the life of the system.
echo '== mdevd runs once =='
if [ -d "$ROOTFS/var/service/mdevd" ]; then
	no 'var/service/mdevd exists and so does the one in /init'
else
	ok 'there is no second mdevd under /var/service'
fi
have "$ROOTFS/init" '/sbin/mdevd -O 4 -f /etc/mdev.conf' '/init is the only thing that starts mdevd'

# --- the console a person is looking at is the one that exists ---------------
# console= is last-one-wins for /dev/console.  tty0 last means the screen, which
# always exists; a serial line last means a device that may not be wired to
# anything, and every write to it fails.
echo '== every generated kernel command line =='
for f in "$ROOTFS/sbin/flxinstall" "$HERE/../../base/build-base.sh" \
	"$HERE/../../base/xsetup.d/setup-disk.sh"; do
	[ -f "$f" ] || continue
	name=$(basename "$f")

	# One report per cmdline line, because one wrong line is one broken boot.
	n=0
	bad_last=0
	quiet=0
	while IFS= read -r line; do
		n=$((n + 1))
		# The last console= on the line, as written: console=ttyS0,115200
		# ends at a space or the end of the line.
		last=$(printf '%s\n' "$line" | sed -n 's/.*\(console=[^ ]*\).*$/\1/p')
		[ "$last" = 'console=tty0' ] || bad_last=$((bad_last + 1))
		case $line in *quiet*) quiet=$((quiet + 1)) ;; esac
	done <<EOL
$(grep 'cmdline:' "$f")
EOL
	if [ "$n" -eq 0 ]; then
		no "$name writes no kernel command line at all"
		continue
	fi
	if [ "$bad_last" -eq 0 ]; then
		ok "$name leaves tty0 last on all $n command lines"
	else
		no "$name has $bad_last of $n command lines whose last console= is not tty0"
	fi
	if [ "$quiet" -eq 0 ]; then
		ok "$name passes no quiet on any command line"
	else
		no "$name passes quiet on $quiet of $n command lines, so the boot prints nothing"
	fi
done

# --- the boot menu hands over a console on BIOS too --------------------------
# Limine hands over a framebuffer unless told not to, and vgacon's first act on
# a framebuffer is to refuse it.  textmode: yes is the BIOS half of the console
# story; the framebuffer console is the UEFI half.
echo '== the bootloader =='
for f in "$ROOTFS/sbin/flxinstall" "$HERE/../../base/build-base.sh" \
	"$HERE/../../base/xsetup.d/setup-disk.sh"; do
	[ -f "$f" ] || continue
	name=$(basename "$f")
	have "$f" 'textmode: yes' "$name asks Limine for a text screen on BIOS"
done

printf '\n%s passed, %s failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]