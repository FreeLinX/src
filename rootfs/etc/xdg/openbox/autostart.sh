#!/bin/sh
# FreeLinX Retro Desktop Autostart

# Set retro dark charcoal desktop background
if command -v xsetroot >/dev/null 2>&1; then
    xsetroot -solid "#2d2d2d" 2>/dev/null
fi

# Launch default interactive terminal
(cd "$HOME" && /usr/bin/uxterm -g 84x26+40+40 -e /bin/sh -l) &
