#!/bin/sh
# FreeLinX Desktop Autostart

# 1. Desktop Background (CDE workstation slate gray)
if [ -x /usr/bin/flx-bg ]; then
    /usr/bin/flx-bg "#3a4a58" &
elif command -v xsetroot >/dev/null 2>&1; then
    xsetroot -solid "#3a4a58" 2>/dev/null &
fi

# 2. Desktop Panel (Taskbar, Clock, System Status)
if [ -x /usr/bin/flx-panel ]; then
    /usr/bin/flx-panel &
fi

# 3. Launch Dillo with Welcome Page (Only this application starts on boot)
(sleep 0.4 && /usr/bin/dillo /usr/share/freelinx/welcome.html) &
