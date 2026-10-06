# One-time setup inside the Holo chroot (run via enter.sh).
set -e
# Android paranoid network: sockets need these supplementary gids.
getent group aid_inet >/dev/null || groupadd -g 3003 aid_inet
getent group aid_net_raw >/dev/null || groupadd -g 3004 aid_net_raw
getent group aid_net_admin >/dev/null || groupadd -g 3005 aid_net_admin
usermod -aG aid_inet,aid_net_raw alpm
# No Landlock on 4.4: pacman's download sandbox can't work.
grep -q '^DisableSandbox' /etc/pacman.conf || sed -i 's/^\[options\]/[options]\nDisableSandbox/' /etc/pacman.conf
pacman-key --init >/dev/null 2>&1 || true
echo "holo-quest" > /etc/hostname
curl -sI https://holo-packages.steamos.cloud | head -1
pacman -Sy 2>&1 | tail -3
pacman -Q | wc -l
