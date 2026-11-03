#!/bin/sh
# Install: install -m755 q1restart.sh /root/q1restart.sh (development helper; the device/ scripts point to it).
# q1restart.sh <who> <what> <command...>: run a restart of Steam/SteamVR/dashboard/Monado under a shared lock,
# logged in /run/q1-restarts.log, so parallel workers on this headset don't restart things under each other.
# e.g.  sh /root/q1restart.sh audio "steamvr for pulse" sh /root/steamframe.sh
# While /run/q1-user-testing exists the user is wearing the headset: only "main" may restart, others wait
# (without holding the lock).
# While /run/q1-quiet holds "<owner> <epoch-until>" (a measurement window, 15 min max) only main and the owner may
# restart, others wait the same way. Take it with: echo "perf $(( $(date +%s) + 600 ))" > /run/q1-quiet
who=$1; what=$2; shift 2
n=0
while :; do
  exec 9>/run/q1-restart.lock
  flock -w 900 9 || { echo "q1restart: lock busy for 15 min" >&2; exit 1; }
  quiet=
  if [ -e /run/q1-quiet ]; then
    read qo qu < /run/q1-quiet
    now=$(date +%s)
    [ -n "$qu" ] && [ "$qu" -gt "$now" ] && [ $((qu - now)) -le 900 ] && [ "$qo" != "$who" ] && quiet=1
  fi
  [ "$who" = main ] || { [ ! -e /run/q1-user-testing ] && [ -z "$quiet" ]; } && break
  exec 9>&-
  [ $n -eq 0 ] && echo "q1restart: user testing or quiet window, waiting (up to 30 min)" >&2
  n=$((n+1)); [ $n -gt 360 ] && { echo "q1restart: still blocked after 30 min, giving up" >&2; exit 1; }
  sleep 5
done
echo "$(date +%T) $who: $what ($*)" >> /run/q1-restarts.log
"$@" 9>&-  # children (SteamVR, Steam) must not inherit the lock
