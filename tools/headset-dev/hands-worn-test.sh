#!/bin/sh
# The normal switch is the SteamVR setting Settings > Oculus Quest > Hand tracking; this script does the same from a shell.
# Worn test of Meta's hand tracking (docs/hand-tracking.md). Run on the headset as root BEFORE putting it on:
#   sh hands-worn-test.sh on      hands on (auto: hands when no controller is in use, controllers as soon as you pick one up)
#   sh hands-worn-test.sh log     what the daemon and the driver say (Ctrl-C to stop)
#   sh hands-worn-test.sh off     back to controllers only (removes /etc/quest1-hands, hands off)
# What to try, headset on:
#   1. Put both controllers down (on a table, not in the hands). After ~3 s the cameras switch (1-2 s): you get hands
#      shown as a thin blue joint skeleton on each hand ("hand tracked" in the log). Pick a controller up: it switches back.
#   2. Open the dashboard (Oculus gesture below), point with the hand: the laser comes from the pinch point.
#   3. Palm toward your face: the small Oculus mark (right hand) / Menu bars (left) appears next to the palm.
#      Pinch thumb and index (mark turns yellow): the SteamVR system button; with the palm turned away it is a trigger
#      click, only while the dashboard is open.
#   4. VRChat: Settings > Tracking & IK > hand tracking / "SteamVR skeletal input"; fingers should follow yours.
#      If fingers or the wrist are twisted, tell which hand and how (the retargeting is untested with a real hand).
# Not working at all: sh hands-worn-test.sh log, send the output.
A="sh /root/andrun.sh /system/bin"
case "$1" in
on)
	echo auto > /tmp/quest1-hands
	echo "hands: auto. android-q1handsd must be running: systemctl is-active android-q1handsd" ;;
off)
	echo off > /tmp/quest1-hands; rm -f /etc/quest1-hands
	sleep 2
	$A/oculuspreferences --setc hand_tracking_enabled false | tail -1
	echo "hands: off" ;;
log)
	( journalctl -f -u android-q1handsd -n 20 --no-pager &
	  tail -F /home/steamos/.local/share/Steam/logs/vrserver.txt 2>/dev/null | grep --line-buffered -a "quest1: .*hand" ) ;;
*) echo "usage: $0 on|off|log" >&2; exit 2 ;;
esac
