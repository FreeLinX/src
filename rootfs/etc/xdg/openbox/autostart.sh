#!/bin/sh
# FreeLinX Desktop Autostart

# 1. Desktop Background (CDE workstation slate gray)
if [ -x /usr/bin/flxbg ]; then
    /usr/bin/flxbg "#3a4a58" &
elif command -v xsetroot >/dev/null 2>&1; then
    xsetroot -solid "#3a4a58" 2>/dev/null &
fi

# 2. Bottom status bar (i3status -> lemonbar)
if [ -x /usr/bin/flxbar-bottom ]; then
    /usr/bin/flxbar-bottom &
fi

# 3. Start with a clean desktop (Dillo browsers can be launched from the menu)