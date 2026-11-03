#!/bin/sh
# Native audio on the Quest 1 (no Android): speakers, 3.5 mm jacks and microphones through
# ALSA -> PipeWire (pipewire-pulse for SteamVR's vrcompositor and Steam), plus the hardware
# volume buttons. See docs/audio.md.
#
# Hardware path: ALSA PCM -> ADSP (q6 ASM/ADM/AFE over APR) -> primary MI2S -> C-Media CM7104
# DSP codec (cm710x, I2C 8-002c) -> speaker amp / headphone driver; mics come back the same way.
# The sound card (msm8998-cm710x_pvt_revB-snd_card) only registers once the ADSP runs: its
# ION/voice platforms are APR child devices, created ~100 ms after the ADSP is up. Android boots
# the ADSP in late-fs (init.monterey.rc: write /sys/kernel/boot_adsp/boot 1); the codec then
# downloads its own DSP firmware (CM710X.bin) over SPI. Both images come from /run/fw
# (holo-android-blobs).
#
# Usage (on the headset, as root):
#   holo-audio.sh install   packages, ALSA UCM profile, holo-audio.service (NOT enabled),
#                           PipeWire user units, volume-button daemon (enabled)
#   holo-audio start        boot the ADSP once, wait for the card, apply the static mixer setup
#   holo-audio enable       boot the ADSP at every boot (only once `start` was verified by hand)
#   holo-audio status
#   holo-audio ucm          (re)write only the UCM profile
#   holo-audio volkeys      (re)install only the volume-button daemon
# Never write 0 to /sys/kernel/boot_adsp/boot: that shuts the running ADSP down (subsystem_put).
set -e
L=/run/holo-audio
UCMDIR=/usr/share/alsa/ucm2/Quest1

card_index() { sed -n 's/^ *\([0-9]\+\) \[.*\]: msm8998-cm710x.*/\1/p' /proc/asound/cards | head -1; }

adsp_subsys() {
	for s in /sys/bus/msm_subsys/devices/*; do
		if [ "$(cat "$s/name")" = adsp ]; then echo "$s"; return; fi
	done
}

start() {
	mkdir -p $L
	if [ -z "$(card_index)" ]; then
		s=$(adsp_subsys)
		[ -n "$s" ] || { echo "holo-audio: no adsp subsystem"; exit 1; }
		for f in adsp.mdt adsp.b00 adsp.b11 CM710X.bin; do
			[ -r /run/fw/$f ] || { echo "holo-audio: /run/fw/$f missing (holo-android-blobs?)"; exit 1; }
		done
		# An ADSP crash must restart only the ADSP (SSR "RELATED"); at "SYSTEM" level any ADSP
		# error fatal panics the kernel and the headset reboots into Android. Never touch the modem.
		echo RELATED > "$s/restart_level"
		[ "$(cat "$s/restart_level")" = RELATED ] || { echo "holo-audio: cannot set adsp restart_level"; exit 1; }
		# once per boot (the loader itself is also a no-op once the ADSP is loaded)
		if [ "$(cat "$s/state")" != ONLINE ] && [ ! -e $L/adsp-booted ]; then
			touch $L/adsp-booted
			echo "holo-audio: booting the ADSP"
			echo 1 > /sys/kernel/boot_adsp/boot
		fi
		# PIL load + "error ready" handshake (the kernel gives up after 10 s), then the card
		i=0
		while [ "$(cat "$s/state")" != ONLINE ] && [ $i -lt 75 ]; do sleep 0.2; i=$((i + 1)); done
		echo "holo-audio: adsp $(cat "$s/state") after $((i / 5)) s"
		i=0
		while [ -z "$(card_index)" ] && [ $i -lt 75 ]; do sleep 0.2; i=$((i + 1)); done
	fi
	c=$(card_index)
	[ -n "$c" ] || { echo "holo-audio: no sound card after ADSP boot (see journalctl -k)"; exit 1; }
	# UCM lookup key = the card's driver field; follow it if it ever differs from the installed one
	d=$(sed -n "s/^ *$c \[.*\]: \([^ ]*\) - .*/\1/p" /proc/asound/cards)
	if [ -n "$d" ] && [ -e $UCMDIR/Quest1.conf ] && [ ! -e "/usr/share/alsa/ucm2/conf.d/$d/$d.conf" ]; then
		mkdir -p "/usr/share/alsa/ucm2/conf.d/$d"
		ln -sfn ../../Quest1/Quest1.conf "/usr/share/alsa/ucm2/conf.d/$d/$d.conf"
	fi
	# static setup = the "initial mixer settings" of Android's mixer_paths_cm710x_pvt_revB.xml:
	# primary MI2S at 48 kHz, 24-bit, stereo; Meta's speaker EQ for the CM7104 DSP.
	# Routing and the speaker/headphone switches are done per device by the UCM profile.
	a() { amixer -q -c "$c" cset "name=$1" "$2" || echo "holo-audio: cannot set '$1'"; }
	a 'PRIM_MI2S_RX Format' S24_3LE
	a 'PRIM_MI2S_TX Format' S24_3LE
	a 'PRIM_MI2S_RX Channels' Two
	a 'PRIM_MI2S_TX Channels' Two
	a 'EQ Pre Gain' 10
	b=1
	for g in 25 25 25 24 22 21 18 17 13 14 20 24 24 22 25 26 27 28 28; do
		a "EQ Band $b" $g; b=$((b + 1))
	done
	# routing: WirePlumber does not drive the card (install_pipewire_nodes), so the UCM verb and
	# devices are applied here; the mixer keeps them after alsaucm exits
	output speaker
	echo "holo-audio: card $c ready"
	# the user's PipeWire may have started before the card existed: restart it to pick it up
	systemctl --user -M steamos@ try-restart pipewire.service pipewire-pulse.service wireplumber.service 2>/dev/null ||
		systemctl --user -M steamos@ start pipewire.socket pipewire-pulse.socket 2>/dev/null || true
}

# output speaker|headphones: route MultiMedia1 to the speakers or the 3.5 mm jack (mic always on)
output() {
	c=$(card_index)
	[ -n "$c" ] || { echo "holo-audio: no sound card"; exit 1; }
	case "$1" in
	speaker) on=Speaker off=Headphones ;;
	headphones) on=Headphones off=Speaker ;;
	*) echo "usage: holo-audio output speaker|headphones"; exit 1 ;;
	esac
	alsaucm -c "hw:$c" set _verb HiFi set _disdev "$off" set _enadev "$on" set _enadev Mic ||
		echo "holo-audio: alsaucm failed"
}

status() {
	cat /proc/asound/cards
	s=$(adsp_subsys)
	[ -z "$s" ] || echo "adsp: $(cat "$s/state"), restart_level $(cat "$s/restart_level"), crashes $(cat "$s/crash_count")"
	c=$(card_index); [ -n "$c" ] || exit 0
	for n in 'PRI_MI2S_RX Audio Mixer MultiMedia1' 'MultiMedia1 Mixer PRI_MI2S_TX' 'Speaker Switch' \
		'Headphone Switch' 'Mic Switch' 'Headphone Volume' 'Speaker Volume' 'MIC Volume' 'Headphone Jack'; do
		printf '%s: ' "$n"; amixer -c "$c" cget "name=$n" 2>/dev/null | sed -n 's/^ *: values=//p'
	done
}

install_ucm() {
	# ALSA UCM2 profile (what PipeWire/WirePlumber use to build the sink/source and its ports).
	# alsa-lib looks it up as conf.d/<card driver>/<card driver>.conf; the driver field is the
	# DT card name truncated to 15 characters.
	mkdir -p $UCMDIR /usr/share/alsa/ucm2/conf.d/msm8998-cm710x_
	cat > $UCMDIR/Quest1.conf <<'EOF'
# Oculus Quest 1 (MSM8998 ADSP + C-Media CM7104 on primary MI2S) - installed by holo-audio.sh
Syntax 4

SectionUseCase."HiFi" {
	File "/Quest1/HiFi.conf"
	Comment "Speakers, 3.5 mm jacks and microphones"
}
EOF
	cat > $UCMDIR/HiFi.conf <<'EOF'
# Routing from Android's mixer_paths_cm710x_pvt_revB.xml:
#   deep-buffer-playback = PCM 0 (MultiMedia1) -> PRI_MI2S_RX, audio-record = PRI_MI2S_TX -> PCM 0
#   "speaker" / "headphones" / "headset-mic" device paths -> CM7104 switches
SectionVerb {
	EnableSequence [
		cset "name='PRIM_MI2S_RX Format' S24_3LE"
		cset "name='PRIM_MI2S_TX Format' S24_3LE"
		cset "name='PRIM_MI2S_RX Channels' Two"
		cset "name='PRIM_MI2S_TX Channels' Two"
		cset "name='PRI_MI2S_RX Audio Mixer MultiMedia1' 1"
		cset "name='MultiMedia1 Mixer PRI_MI2S_TX' 1"
	]
	DisableSequence [
		cset "name='PRI_MI2S_RX Audio Mixer MultiMedia1' 0"
		cset "name='MultiMedia1 Mixer PRI_MI2S_TX' 0"
	]
	Value {
		TQ "HiFi"
	}
}

SectionDevice."Speaker" {
	Comment "Speakers"
	ConflictingDevice [
		"Headphones"
	]
	EnableSequence [
		cset "name='EQ Switch' 1"
		cset "name='Headphone Switch' 0,0"
		cset "name='Headphone Volume' 86,86"
		cset "name='Speaker Switch' 1,1"
	]
	DisableSequence [
		cset "name='Speaker Switch' 0,0"
	]
	Value {
		PlaybackPriority 100
		PlaybackPCM "hw:${CardId},0"
		PlaybackChannels 2
	}
}

SectionDevice."Headphones" {
	Comment "Headphones (3.5 mm)"
	ConflictingDevice [
		"Speaker"
	]
	EnableSequence [
		cset "name='EQ Switch' 0"
		cset "name='Speaker Switch' 0,0"
		cset "name='Headphone Switch' 1,1"
	]
	DisableSequence [
		cset "name='Headphone Switch' 0,0"
	]
	Value {
		PlaybackPriority 200
		PlaybackPCM "hw:${CardId},0"
		PlaybackChannels 2
		JackControl "Headphone Jack"
	}
}

SectionDevice."Mic" {
	Comment "Microphones"
	EnableSequence [
		cset "name='Mic Switch' 1,1"
	]
	DisableSequence [
		cset "name='Mic Switch' 0,0"
	]
	Value {
		CapturePriority 100
		CapturePCM "hw:${CardId},0"
		CaptureChannels 2
	}
}
EOF
	ln -sfn ../../Quest1/Quest1.conf /usr/share/alsa/ucm2/conf.d/msm8998-cm710x_/msm8998-cm710x_.conf
}

install_volkeys() {
	# Volume buttons -> volume of PipeWire's default sink, as steamos (owner of the PipeWire
	# session that Steam and SteamVR use). Reads /dev/input (group input) without grabbing it.
	cat > /usr/local/bin/quest1-volkeys <<'EOF'
#!/usr/bin/python3
# Quest 1 hardware volume buttons -> PipeWire default sink volume (wpctl).
# VOLUMEUP comes from gpio-keys, VOLUMEDOWN from qpnp_pon (which also carries KEY_POWER).
# The devices are only read, never grabbed (no EVIOCGRAB): the power key keeps reaching
# logind and anyone else; this daemon ignores every key but the two volume keys.
# Holding a key repeats the step after REPEAT_DELAY, every REPEAT_RATE.
import os
import select
import struct
import subprocess
import sys
import time

KEY_VOLUMEDOWN, KEY_VOLUMEUP = 114, 115
EV_KEY = 1
EVENT = struct.Struct("llHHi")  # struct input_event on 64-bit: timeval, type, code, value
STEP = os.environ.get("QUEST1_VOLUME_STEP", "5%")
REPEAT_DELAY, REPEAT_RATE = 0.4, 0.12
NAMES = ("gpio-keys", "qpnp_pon")


def find_devices():
    devs = []
    for ev in sorted(os.listdir("/sys/class/input")):
        if not ev.startswith("event"):
            continue
        try:
            with open(f"/sys/class/input/{ev}/device/name") as f:
                name = f.read().strip()
        except OSError:
            continue
        if name in NAMES:
            devs.append(f"/dev/input/{ev}")
    return devs


def step(key):
    sign = "+" if key == KEY_VOLUMEUP else "-"
    cmd = ["wpctl", "set-volume", "-l", "1.0", "@DEFAULT_AUDIO_SINK@", STEP + sign]
    try:
        if key == KEY_VOLUMEUP:
            subprocess.run(["wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "0"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=3)
        subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=3)
    except (OSError, subprocess.SubprocessError) as e:
        print(f"quest1-volkeys: wpctl failed: {e}", file=sys.stderr, flush=True)


def main():
    paths = find_devices()
    if not paths:
        sys.exit("quest1-volkeys: no gpio-keys/qpnp_pon input device")
    fds = [os.open(p, os.O_RDONLY | os.O_NONBLOCK) for p in paths]
    print(f"quest1-volkeys: watching {' '.join(paths)}", flush=True)
    held, next_repeat = None, 0.0
    while True:
        timeout = max(0.0, next_repeat - time.monotonic()) if held else None
        ready, _, _ = select.select(fds, [], [], timeout)
        for fd in ready:
            try:
                data = os.read(fd, EVENT.size * 64)
            except BlockingIOError:
                continue
            for off in range(0, len(data) - EVENT.size + 1, EVENT.size):
                _, _, etype, code, value = EVENT.unpack_from(data, off)
                if etype != EV_KEY or code not in (KEY_VOLUMEUP, KEY_VOLUMEDOWN):
                    continue
                if value == 1:  # press (kernel autorepeat, value 2, is ignored)
                    held, next_repeat = code, time.monotonic() + REPEAT_DELAY
                    step(code)
                elif value == 0 and code == held:
                    held = None
        if held and time.monotonic() >= next_repeat:
            step(held)
            next_repeat = time.monotonic() + REPEAT_RATE


if __name__ == "__main__":
    main()
EOF
	chmod 755 /usr/local/bin/quest1-volkeys
	cat > /etc/systemd/system/quest1-volkeys.service <<'EOF'
[Unit]
Description=Quest 1 volume buttons -> PipeWire volume
Wants=user@1000.service
After=user@1000.service

[Service]
User=steamos
SupplementaryGroups=input
Environment=XDG_RUNTIME_DIR=/run/user/1000
ExecStart=/usr/local/bin/quest1-volkeys
Restart=always
RestartSec=2
Nice=-5

[Install]
WantedBy=multi-user.target
EOF
	systemctl daemon-reload
	systemctl enable --now quest1-volkeys.service
}

# WirePlumber's ALSA monitor (ACP) builds a "Pro Audio" profile for every card by opening each of
# its PCMs: on this card VoIP, SLIMBUS hostless, AFE proxy and other DSP front ends, which the ADSP
# refuses; the first boot ended in an oops in msm_afe_hw_params. PipeWire 1.4 has no switch to skip
# that probe, so WirePlumber ignores the card and PipeWire gets fixed nodes on the only PCM the
# hardware path uses (MultiMedia1, hw:0,0, routed by the UCM HiFi verb that `start` applies).
# No automatic speaker/headphone switching this way (`holo-audio output speaker|headphones`).
install_pipewire_nodes() {
	mkdir -p /etc/wireplumber/wireplumber.conf.d /etc/pipewire/pipewire.conf.d
	cat > /etc/wireplumber/wireplumber.conf.d/50-quest1-audio.conf <<'EOF'
# Quest 1 (holo-audio.sh): the msm8998 card is not probed (see /etc/pipewire/pipewire.conf.d/
# 50-quest1-audio.conf for its nodes)
monitor.alsa.rules = [
  {
    matches = [ { device.name = "~alsa_card.*" } ]
    actions = { update-props = { device.disabled = true } }
  }
]
EOF
	cat > /etc/pipewire/pipewire.conf.d/50-quest1-audio.conf <<'EOF'
# Quest 1 (holo-audio.sh): speaker/headphone output and microphone on MultiMedia1 (hw:0,0)
context.objects = [
  { factory = adapter
    flags = [ nofail ]
    args = {
      factory.name = api.alsa.pcm.sink
      node.name = "quest1-output"
      node.description = "Quest 1"
      media.class = "Audio/Sink"
      api.alsa.path = "hw:0,0"
      # the msm PCM only moves its pointer at period boundaries: PipeWire's timer-based
      # scheduling then underruns all the time (audible stutter), so use period interrupts
      api.alsa.disable-tsched = true
      api.alsa.period-size = 1024
      api.alsa.period-num = 4
      audio.format = "S16LE"
      audio.rate = 48000
      audio.channels = 2
      audio.position = [ FL FR ]
      priority.session = 2000
      priority.driver = 2000
    }
  }
  { factory = adapter
    flags = [ nofail ]
    args = {
      factory.name = api.alsa.pcm.source
      node.name = "quest1-mic"
      node.description = "Quest 1 microphone"
      media.class = "Audio/Source"
      api.alsa.path = "hw:0,0"
      api.alsa.disable-tsched = true
      api.alsa.period-size = 1024
      api.alsa.period-num = 4
      audio.format = "S16LE"
      audio.rate = 48000
      audio.channels = 1
      audio.position = [ MONO ]
      # above the output's monitor (2000), else WirePlumber makes that the default source and
      # Steam finds no microphone
      priority.session = 2100
    }
  }
]
EOF
	# no rtkit on Holo: let the user's PipeWire raise its own data threads to real time
	# (module-rt), or vrcompositor's CPU load makes it underrun. Applies from the next login.
	mkdir -p /etc/systemd/system/user@.service.d
	cat > /etc/systemd/system/user@.service.d/50-quest1-rt.conf <<'EOF'
[Service]
LimitRTPRIO=95
LimitNICE=-11
EOF
	systemctl daemon-reload
}

enable_boot() {
	systemctl enable holo-audio.service
	echo "holo-audio: the ADSP now boots at every boot (holo-audio.service)"
}

do_install() {
	pacman -S --needed --noconfirm pipewire pipewire-pulse pipewire-alsa wireplumber alsa-utils
	cp -f "$0" /usr/local/sbin/holo-audio
	chmod 755 /usr/local/sbin/holo-audio
	install_ucm
	install_pipewire_nodes
	# Not enabled here: booting the ADSP is enabled with `holo-audio enable` once a manual
	# `holo-audio start` worked. WirePlumber picks the card up whenever it appears (udev).
	cat > /etc/systemd/system/holo-audio.service <<'EOF'
[Unit]
Description=Quest 1 audio: boot the ADSP, sound card setup (CM7104 codec)
Requires=holo-android-blobs.service
After=holo-android-blobs.service android-modem.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/holo-audio start

[Install]
WantedBy=multi-user.target
EOF
	# PipeWire for every user session (pipewire-pulse serves SteamVR's and Steam's PulseAudio clients)
	systemctl --global enable pipewire.socket pipewire-pulse.socket wireplumber.service
	usermod -aG audio,input steamos 2>/dev/null || true
	install_volkeys
	echo "holo-audio: installed; next: holo-audio start (boots the ADSP), then holo-audio enable"
}

case "$1" in
start | status) "$1" ;;
enable) enable_boot ;;
output) output "$2" ;;
install) do_install ;;
ucm) install_ucm; install_pipewire_nodes ;;
volkeys) install_volkeys ;;
*) echo "usage: $0 install | start | enable | status | output speaker|headphones | ucm | volkeys"; exit 1 ;;
esac
