#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 FreeLinX OS Project.
#
# test-flxconsole.sh - one console, one login, for as long as the console lasts.
#
#   sh scripts/test-flxconsole.sh
#
# flxconsole is run under runit, so it has to stay up: runsvdir restarts a
# service that exits, and the machine has no prompt at all in between.  At the
# same time a login is not supposed to last for ever - somebody types Ctrl-D, or
# the ssh session they were on drops - and the console has to ask again.  Those
# two pull against each other and neither can be checked by reading the script,
# because the interesting part is what happens after the first login exits, which
# is a wait and a loop.
#
# So this drives the real script and waits.  A regular file stands in for a
# console: the script tests for it with -c, which a regular file fails, so that
# one -c is the only thing that is different.  /etc/flx-shell, /etc/flx-installed
# and /etc/motd are named by path, /dev is not, so they are pointed at a scratch
# directory the same way.  getty, login and the shell are stubs.
#
# The stubs do not end on their own.  A login lasts until this test releases it,
# because how long a session lasts decides what the script under test does: with
# stubs that return in a moment, a supervisor that opens a login every second
# looks exactly like one that waits for the login to end.  A test that cannot
# tell those two apart is not testing anything.  So the release is explicit, and
# the "is the login still there" question is answered with kill -0 on the pid the
# stub wrote down, because a process killed with TERM does not get to clean up
# after itself.
#
#   sh test-flxconsole.sh             check the shipped rootfs
#   ROOTFS=DIR sh test-flxconsole.sh  check another one

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOTFS=${ROOTFS:-$HERE/../rootfs}
CONSOLE=$ROOTFS/sbin/flxconsole

[ -f "$CONSOLE" ] || { printf 'no %s\n' "$CONSOLE" >&2; exit 2; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/flxconsole.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT INT TERM HUP

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

# --- the stubs ----------------------------------------------------------------
# getty execs the login program and becomes it - one process, not two - so the
# stub does the same.  Faking it with a fork would hide the thing this checks:
# whether a TERM to flxconsole reaches the console process at all.  A fork would
# leave the real login running as an orphan of a subshell that died, which looks
# like flxconsole leaking a console when flxconsole never had one.
cat > "$WORK/getty" <<'EOF'
#!/bin/sh
echo "getty $*" >> "$FLXLOG"
# -l PROGRAM BAUD TTY TERM
prog=$2
shift 3
exec "$prog" "$1"
EOF

# login and shell are the same program wearing different names, so that "the
# console asked again" is one check and not two copies of it.  $FLXNAME says
# which is which; $FLXTIME says how long the session lasts, in tenths of a
# second, so that a test wanting a login that goes on living and a test wanting
# one that ends by itself can both have it.
cat > "$WORK/session" <<'EOF'
#!/bin/sh
echo "$FLXNAME-start $1" >> "$FLXLOG"
echo "cwd $PWD" >> "$FLXLOG"
echo $$ > "$FLXRUNNING"
# A session ends when the test says so (FLXRELEASE), or after FLXTIME tenths of a
# second if the test gave it a length instead.  Whichever comes first.
#
# FLXTIME=0 means "wait to be released and for nothing else", which is what a
# login with somebody sitting at it looks like.  The zero has to be a case of its
# own rather than a length of nothing: read as a length, FLXTIME=0 is over before
# it starts, and every session ends the instant it opens, which is a console
# reopening itself once a second - the very thing this is meant to be able to
# tell apart from a console that waits.
_t=0
while [ ! -e "$FLXRELEASE" ]; do
	[ "$_t" -ge "${FLXTIME:-0}" ] && [ "${FLXTIME:-0}" -gt 0 ] && break
	sleep 0.1
	_t=$((_t + 1))
done
rm -f "$FLXRELEASE"
echo "$FLXNAME-end $1" >> "$FLXLOG"
EOF

chmod 755 "$WORK/getty" "$WORK/session"

# --- the copy of flxconsole this drives ---------------------------------------
#
# The paths are substituted rather than made configurable, because /dev is named
# by the kernel and not by anything this script can pass in, and a test that only
# works because the script under test grew a knob for it is not testing the
# shipped script.
prepare() {
	mkdir -p "$WORK/etc" "$WORK/dev"
	printf 'FreeLinX test banner\n' > "$WORK/etc/motd"
	: > "$WORK/dev/tty0"
	: > "$WORK/log"
	rm -f "$WORK/running" "$WORK/svc.out" "$WORK/release" "$WORK/etc/flx-installed"
	[ "${INSTALLED:-yes}" = yes ] && : > "$WORK/etc/flx-installed"

	# setsid -c is not used here: it needs a terminal it can ioctl, and a
	# regular file standing in for a console is not one.  That the shipped
	# script asks for a controlling terminal at all is checked by reading it,
	# in test-console.sh, because it cannot be checked this way.
	sed -e "s#^LOGIN_SHELL=.*#LOGIN_SHELL=$WORK/session#" \
	    -e 's#/usr/bin/setsid#/nonexistent/setsid#' \
	    -e 's#/bin/toybox#/nonexistent/toybox#' \
	    -e "s#^for t in /usr/bin/getty /bin/getty; do#for t in $WORK/getty; do#" \
	    -e "s#^LOGINPROG=#LOGINPROG=$WORK/session#" \
	    -e "s#^for l in /usr/bin/login /bin/login; do#for l in $WORK/session; do#" \
	    -e "s#^TTYS=.*#TTYS='$WORK/dev/tty0'#" \
	    -e "s#^WAIT_TRIES=.*#WAIT_TRIES=2#" \
	    -e "s#^SETTLE_TRIES=.*#SETTLE_TRIES=1#" \
	    -e "s#\[ -e /etc/flx-installed \]#[ -e $WORK/etc/flx-installed ]#g" \
	    -e "s#cat /etc/motd#cat $WORK/etc/motd#" \
	    -e "s#cd /root#cd $WORK#" \
	    -e 's#\[ -c "\$_t" \]#[ -e "$_t" ]#' \
	    -e 's#\[ -c "\$tty" \]#[ -e "$tty" ]#' \
	    ${GETTY_GONE:+-e "s#^for t in $WORK/getty; do#for t in $WORK/nothing; do#"} \
	    "$CONSOLE" > "$WORK/run.sh"
	FLXLOG=$WORK/log FLXRUNNING=$WORK/running FLXRELEASE=$WORK/release \
	    FLXNAME=${NAME:-login} FLXTIME=${SESSION:-0} \
	    /bin/sh "$WORK/run.sh" > "$WORK/svc.out" 2>&1 &
	FLXC=$!
}

# release - let the session on the console end, as somebody typing Ctrl-D would
release() {
	: > "$WORK/release"
}

# calls WHAT - how many times something was logged
calls() {
	_n=$(grep -c "^$1" "$WORK/log" 2>/dev/null) || _n=0
	printf '%s' "${_n:-0}"
}

# said WHAT - how many times the service printed something
said() {
	_n=$(grep -c "$1" "$WORK/svc.out" 2>/dev/null) || _n=0
	printf '%s' "${_n:-0}"
}

# wait_for WHAT SECONDS - is WHAT logged at least once?
wait_for() {
	_n=0
	while [ "$_n" -lt "$(( $2 * 4 ))" ]; do
		[ "$(calls "$1")" -gt 0 ] && return 0
		sleep 0.25
		_n=$((_n + 1))
	done
	return 1
}

# wait_count WHAT N SECONDS - is WHAT logged at least N times?
wait_count() {
	_n=0
	while [ "$_n" -lt "$(( $3 * 4 ))" ]; do
		[ "$(calls "$1")" -ge "$2" ] && return 0
		sleep 0.25
		_n=$((_n + 1))
	done
	return 1
}

# console_pid - the pid of the process on the console, if it is still there
console_pid() {
	[ -f "$WORK/running" ] || return 1
	cat "$WORK/running"
}

console_alive() {
	_p=$(console_pid) || return 1
	kill -0 "$_p" 2>/dev/null
}

# stop - TERM the service, and insist.
#
# The wait is bounded, because a service that will not go away on TERM is one of
# the bugs this is looking for, and a test that hangs on it reports nothing.  It
# is killed and then reported, not waited on for ever.
stop() {
	_slewed=no
	kill -TERM "$FLXC" 2>/dev/null || :
	_t=0
	while [ "$_t" -lt 5 ] && kill -0 "$FLXC" 2>/dev/null; do
		sleep 1
		_t=$((_t + 1))
	done
	if kill -0 "$FLXC" 2>/dev/null; then
		kill -KILL "$FLXC" 2>/dev/null || :
		_slewed=yes
	fi
	wait "$FLXC" 2>/dev/null || :
}

# --- one console, one login, and then another one -----------------------------
echo '== an installed console logs one person in, and only one at a time =='
prepare
if wait_for login-start 5; then
	ok 'the console got a getty and a login'
else
	no 'the console got a getty and a login'
fi

if console_alive; then
	ok "the login is still there, waiting to be released (pid $(console_pid))"
else
	no 'the login ended before it was told to'
fi

# A second login while the first is still running is not a busy prompt: it is
# two processes reading the same keystrokes on the same terminal.
sleep 3
if [ "$(calls login-start)" -eq 1 ]; then
	ok 'a console that already has a login is left alone'
else
	no "a console that already has a login is left alone ($(calls login-start) logins)"
fi

echo '== the console asks again when the login ends =='
release
if wait_count login-end 1 5 && wait_count login-start 2 5; then
	ok 'a console whose login ended asked again'
else
	no "a console whose login ended asked again ($(calls login-start) logins, $(calls login-end) ended)"
fi
release

echo '== the banner goes to the console and nowhere else =='
if grep -q 'FreeLinX test banner' "$WORK/dev/tty0"; then
	ok 'the banner went to the console'
else
	no 'the banner went to the console'
fi
if grep -q 'FreeLinX test banner' "$WORK/svc.out"; then
	no 'the banner did not go to stdout, which is the kernel console'
else
	ok 'the banner did not go to stdout'
fi

echo '== runit can stop it, and takes the console with it =='
if console_alive; then ok 'a login is running when the service is stopped'; else no 'a login is running when the service is stopped'; fi
_conpid=$(console_pid)
stop
if [ "$_slewed" = yes ]; then
	no 'TERM stops the service (it had to be killed)'
else
	ok 'TERM stops the service'
fi
if kill -0 "$_conpid" 2>/dev/null; then
	no "TERM took the login with it (pid $_conpid is still there)"
else
	ok 'TERM took the login with it'
fi
_n=$(calls login-start)
sleep 2
if [ "$(calls login-start)" = "$_n" ]; then
	ok 'a stopped service does not keep opening consoles'
else
	no 'a stopped service does not keep opening consoles'
fi

# --- a console that goes away is not reopened ---------------------------------
echo '== a console that has gone is not reopened =='
SESSION=3 prepare
wait_for login-start 5
rm -f "$WORK/dev/tty0"
release
sleep 3
if console_alive; then
	no 'the console process is gone with its console'
else
	ok 'the console process is gone with its console'
fi
_n=$(calls login-start)
sleep 2
if [ "$(calls login-start)" = "$_n" ]; then
	ok 'nothing is opened on a console that is not there'
else
	no "nothing is opened on a console that is not there (now $(calls login-start) logins)"
fi
stop

# --- a live medium opens a shell, because there is nothing to log in to -------
echo '== a medium with nothing installed opens a shell, not a login =='
# SESSION is in tenths of a second, and this one is deliberately longer than the
# "left alone" window below: a shell that ends inside that window would be
# reopened by a correct supervisor and the check would pass for the wrong reason.
INSTALLED=no NAME=shell SESSION=25 prepare
if wait_for shell-start 5; then
	ok 'the medium opened a shell'
else
	no 'the medium opened a shell'
fi
if [ "$(calls getty)" -eq 0 ]; then
	ok 'no getty on a medium with nothing installed'
else
	no 'no getty on a medium with nothing installed'
fi
if grep -q "^cwd $WORK\$" "$WORK/log"; then
	ok 'the shell opens in /root'
else
	no 'the shell opens in /root'
fi
_n=$(calls shell-start)
sleep 2
if [ "$(calls shell-start)" = "$_n" ]; then
	ok 'the shell is left alone while it is running'
else
	no 'the shell is left alone while it is running'
fi
release
if wait_count shell-end 1 5 && wait_count shell-start 2 5; then
	ok 'a shell that ended is opened again'
else
	no 'a shell that ended is opened again'
fi
stop

# --- an installed system with no getty says so, once, and hands out nothing --
echo '== an installed system with no getty neither hangs nor hands out root =='
INSTALLED=yes GETTY_GONE=1 prepare
sleep 3
if [ "$(calls shell-start)" -eq 0 ] && [ "$(calls getty)" -eq 0 ]; then
	ok 'no shell and no getty: no free root shell'
else
	no 'no shell and no getty: no free root shell'
fi
n=$(said 'has no getty')
if [ "$n" -eq 1 ]; then
	ok 'the console says why, once'
else
	no "the console says why, once (said it $n times)"
fi
stop

printf '\n%s passed, %s failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
