#!/bin/sh
# usage: sysls.sh <debugfs command...>   (runs on roms/quest1/system.img)
IMG=/mnt/d/Documents/Projets/Quest1-SteamOS/roms/quest1/system.img
debugfs -R "$*" "$IMG" 2>/dev/null
