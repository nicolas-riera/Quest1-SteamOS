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
# don't let NetworkManager (if any) fight networkd over usb0
systemctl disable NetworkManager.service 2>/dev/null || true

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

# root over SSH with the PC's key (if provided)
if [ -f /root/pc.pub ]; then
  mkdir -p /root/.ssh && chmod 700 /root/.ssh
  grep -qF "$(cat /root/pc.pub)" /root/.ssh/authorized_keys 2>/dev/null || cat /root/pc.pub >> /root/.ssh/authorized_keys
  chmod 600 /root/.ssh/authorized_keys
fi
echo "native setup done"
