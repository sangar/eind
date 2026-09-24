#!/bin/sh
set -e
if command -v systemctl >/dev/null 2>&1; then
    systemctl --global enable eind.service >/dev/null 2>&1 || true
    sysctl --system >/dev/null 2>&1 || true
    echo "eind serve is enabled for every user and starts at their next login."
    echo "Start it for yourself now with: systemctl --user start eind"
else
    echo "Keep the index fresh by starting 'eind serve' from your session startup."
fi
