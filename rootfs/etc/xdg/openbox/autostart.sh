#!/bin/sh
# FreeLinX Modern Retro Desktop Autostart

# 1. Set clean flat solid desktop background
if [ -x /usr/bin/flx-bg ]; then
    /usr/bin/flx-bg "#20242c"
elif command -v xsetroot >/dev/null 2>&1; then
    xsetroot -solid "#20242c" 2>/dev/null
fi

# 2. Launch FreeLinX Desktop Panel (Taskbar, RAM monitor, Clock, Start Menu)
if [ -x /usr/bin/flx-panel ]; then
    /usr/bin/flx-panel &
fi

# 3. Launch default interactive terminal on desktop startup
(cd "$HOME" && /usr/bin/uxterm -g 84x26+40+40 -e /bin/sh -l) &
