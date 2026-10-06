#!/bin/sh
# Install driver_quest1 on the headset from the PC (Git Bash): the built .so (build.sh, in WSL)
# and the driver's resources (input profile and bindings, settings).
# Usage: sh install.sh [root@192.168.77.1]   then restart SteamVR (/root/vrstart.sh on the headset)
set -e
H=${1:-root@192.168.77.1}
cd "$(dirname "$0")/quest1"
D=/opt/quest1-steamvr/quest1
ssh "$H" "mkdir -p $D/bin/linuxarm64 $D/resources/input $D/resources/settings"
scp -q bin/linuxarm64/driver_quest1.so "$H:$D/bin/linuxarm64/driver_quest1.so.new"
scp -q driver.vrdrivermanifest "$H:$D/"
scp -q resources/input/*.json "$H:$D/resources/input/"
scp -q resources/settings/default.vrsettings "$H:$D/resources/settings/"
ssh "$H" "mv $D/bin/linuxarm64/driver_quest1.so.new $D/bin/linuxarm64/driver_quest1.so && ls -la $D/bin/linuxarm64"
