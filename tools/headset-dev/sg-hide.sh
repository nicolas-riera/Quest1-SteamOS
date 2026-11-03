#!/bin/sh
# hide.sh NODE-TYPE: remove that scene graph node from the room setup (live, until the step is restarted)
python3 /root/cdp.py "$(sed "s/__TYPE__/$1/" /root/sg-hide.js)" | grep RESULT
