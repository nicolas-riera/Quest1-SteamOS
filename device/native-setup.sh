#!/bin/bash
# Run inside the Holo chroot (from Android, via enter.sh): prepares holo.img to boot
# natively as PID 1 (initrd/init in "holo" mode). Idempotent.
set -e
S=/etc/systemd/system

# --- debug access: USB NCM network + serial console on the gadget's ACM port ----
mkdir -p /etc/systemd/network
cat > /etc/systemd/network/10-usb0.network <<'E'
[Match]
Name=usb0

[Network]
Address=192.168.77.1/24
DHCPServer=yes
ConfigureWithoutCarrier=yes

[DHCPServer]
PoolOffset=2
PoolSize=1
EmitDNS=no
EmitRouter=no
E
mkdir -p $S/serial-getty@ttyGS0.service.d
cat > $S/serial-getty@ttyGS0.service.d/autologin.conf <<'E'
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin root --keep-baud 115200,57600,38400,9600 - $TERM
# the gadget serial port's agetty ignores SIGTERM: without this it holds power off for 15 s
TimeoutStopSec=2
E

# the saved random seed counts as entropy: kernel 4.4 has no other early source, getrandom() would block boot
mkdir -p $S/systemd-random-seed.service.d
cat > $S/systemd-random-seed.service.d/credit.conf <<'E'
[Service]
Environment=SYSTEMD_RANDOM_SEED_CREDIT=yes
E
# kernel 4.4: networkd must not manage foreign routes/next hops (netlink calls it lacks)
mkdir -p /etc/systemd/networkd.conf.d
cat > /etc/systemd/networkd.conf.d/holo-4.4.conf <<'E'
[Network]
ManageForeignNextHops=no
ManageForeignRoutes=no
ManageForeignRoutingPolicyRules=no
E
# journal in RAM, also on the kernel log (readable from the serial console and pstore)
mkdir -p /etc/systemd/journald.conf.d
cat > /etc/systemd/journald.conf.d/kmsg.conf <<'E'
[Journal]
ForwardToKMsg=yes
MaxLevelKMsg=info
Storage=volatile
RuntimeMaxUse=64M
E
# a hung Android daemon must not hold power off for 90 s
mkdir -p /etc/systemd/system.conf.d
cat > /etc/systemd/system.conf.d/holo-timeouts.conf <<'E'
[Manager]
DefaultTimeoutStopSec=15s
E

# --- services ----------------------------------------------------------------
systemctl set-default multi-user.target
systemctl enable systemd-networkd.service systemd-networkd.socket serial-getty@ttyGS0.service
systemctl enable sshd.service 2>/dev/null || echo "sshd not installed (yet)"
# resolved can't bind on 4.4 (missing socket option); use a static resolv.conf
systemctl mask systemd-resolved.service systemd-resolved-varlink.socket systemd-resolved-monitor.socket
rm -f /etc/resolv.conf; printf 'nameserver 1.1.1.1\nnameserver 8.8.8.8\n' > /etc/resolv.conf
# machine id (first-boot wizard has no console here)
[ -s /etc/machine-id ] && [ "$(cat /etc/machine-id)" != uninitialized ] || systemd-machine-id-setup
systemctl mask systemd-firstboot.service
# Every group lookup also asks systemd's user databases through nss-systemd ("group: files [SUCCESS=merge]
# systemd"). nsresourced and userdbd serve them with worker processes; on 4.4 both stop answering after a while (the
# main process alone left, waiting: no worker started again), and id, su, setpriv, ls -l and anything calling
# getgrouplist hang forever. Nothing here needs them (users are in /etc/passwd; nsresourced needs BPF LSM anyway):
# masked, clients get "connection refused" at once and go on.
systemctl mask systemd-nsresourced.socket systemd-nsresourced.service systemd-userdbd.socket systemd-userdbd.service
# don't let NetworkManager (if any) fight networkd over usb0
systemctl disable NetworkManager.service 2>/dev/null || true

if [ "${QUEST1_LAYOUT:-}" = full ]; then
# full install (the PC installer): the root is userdata itself; the 4 GB swap file is made at the first boot
# (writing it into the image would add 4 GB to flash)
cat > /usr/local/sbin/quest1-swapfile <<'E'
#!/bin/sh
F=/var/swap/swapfile
[ -f $F ] && exit 0
mkdir -p /var/swap && chmod 700 /var/swap
dd if=/dev/zero of=$F.new bs=1M count=4096 status=none && chmod 600 $F.new && mkswap $F.new >/dev/null && mv $F.new $F
E
chmod 755 /usr/local/sbin/quest1-swapfile
cat > $S/quest1-swapfile.service <<'E'
[Unit]
Description=Create the swap file (first boot)
DefaultDependencies=no
After=local-fs.target
Before=var-swap-swapfile.swap
ConditionPathExists=!/var/swap/swapfile

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/quest1-swapfile

[Install]
RequiredBy=var-swap-swapfile.swap
E
cat > $S/var-swap-swapfile.swap <<'E'
[Unit]
Description=Swap file
After=quest1-swapfile.service

[Swap]
What=/var/swap/swapfile
Priority=5

[Install]
WantedBy=swap.target
E
systemctl enable quest1-swapfile.service var-swap-swapfile.swap
else
# Android userdata, moved here by the initramfs
mkdir -p /userdata
# swap file lives in userdata (unencrypted)
cat > $S/userdata-steamos-swap.swap <<'E'
[Unit]
Description=Swap file in Android userdata

[Swap]
What=/userdata/steamos/swap
Priority=5

[Install]
WantedBy=swap.target
E
systemctl enable userdata-steamos-swap.swap
fi

# root over SSH with the PC's key (if provided)
if [ -f /root/pc.pub ]; then
  mkdir -p /root/.ssh && chmod 700 /root/.ssh
  grep -qF "$(cat /root/pc.pub)" /root/.ssh/authorized_keys 2>/dev/null || cat /root/pc.pub >> /root/.ssh/authorized_keys
  chmod 600 /root/.ssh/authorized_keys
fi
echo "native setup done"
