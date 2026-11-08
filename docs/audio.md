# Audio on native Holo (speakers, 3.5 mm jacks, microphones)

Install: `device/holo-audio.sh install` on the headset (as root, idempotent). It installs
PipeWire (pipewire, pipewire-pulse, pipewire-alsa, wireplumber) and alsa-utils, an ALSA UCM2
profile for the Quest's sound card, `/usr/local/sbin/holo-audio`, `holo-audio.service` (written
but **not enabled**: see "Booting the ADSP"), the volume-button daemon (`quest1-volkeys.service`,
enabled), and enables the PipeWire user units for every user (`systemctl --global`).
`holo-audio status` prints the card, the ADSP state and the key mixer controls.

## Hardware path

```
app -> PulseAudio API -> pipewire-pulse -> PipeWire -> ALSA hw:0,0 (FE "MultiMedia1")
    -> ADSP (q6 ASM -> ADM/COPP -> AFE, over APR/glink "lpass")
    -> primary MI2S (PRI_MI2S_RX / PRI_MI2S_TX, 48 kHz, S24_3LE, 2 ch, SoC is master, 19.2 MHz MCLK)
    -> C-Media CM7104 DSP codec (driver cm710x, I2C 8-002c + SPI) -> speaker amp / headphone driver
```

- No WCD/Tasha/Tavil codec and no WSA amplifiers are used on this board (`vs1`), even though the
  kernel builds them. The DT card is `soc/sound-vs1`, compatible `qcom,msm8998-asoc-snd-cm710x`,
  model `msm8998-cm710x_pvt_revB-snd_card`; machine driver `sound/soc/msm/msm8998.c`
  (`CONFIG_VS1_BOARD`: the PRI_MI2S back ends use the cm710x codec instead of the stub codec).
- Two 3.5 mm jacks (left and right side of the headset), each with a detect GPIO
  (`cm710x,left/right-jack-detect-gpio`); both feed one ALSA jack control, **`Headphone Jack`**
  (plugged = either jack). There is no headset-mic detection: the mic is always the built-in array.
- The CM7104 is a small DSP with its own firmware, `CM710X.bin` (`/system/vendor/firmware`), which
  the codec driver downloads over SPI (`cm710x_spi`) when the card is instantiated (`request_firmware`, via
  `/run/fw`). It also holds Meta's speaker EQ (20 bands + pre-gain).

## Why there was no sound card

At boot the machine driver probe defers forever (`ASoC: platform (null) not registered`): the
`msm-pcm-voice`, `msm-voip-dsp` and `msm-compr-dsp` platforms need `msm_audio_ion`, and these are
child devices of the APR node that `apr.c` only creates (`of_platform_populate`, 100 ms after
`apr_adsp_up`) once the ADSP runs. Android boots the ADSP from `init.monterey.rc`
(`on late-fs: write /sys/kernel/boot_adsp/boot 1`); nothing does that on native Holo. So the only
missing step is:

```
echo 1 > /sys/kernel/boot_adsp/boot     # adsp-loader -> subsystem_get("adsp") -> PIL (TZ) loads /run/fw/adsp.*
```

after which the deferred probes complete, the card registers (`/proc/asound/cards`) and the codec
loads its firmware. `holo-audio start` does it once per boot, after `holo-android-blobs`
(firmware path) and `android-modem` (pd-mapper/qrtr-ns/tftp_server, which serve the ADSP's
protection-domain list and remote file system), and sets the ADSP's `restart_level` to `RELATED`
first so an ADSP crash only restarts the ADSP. It never touches the modem subsystem, and nothing
may ever write `0` to that file (`adsp_loader_unload` -> `subsystem_put` shuts the ADSP down).

ACDB calibration (`/vendor/etc/acdbdata/MTP/*.acdb`, sent by Android's audio HAL through
libacdbloader) is **not** used: without it the ADSP runs its default (pass-through) topologies,
and Meta's speaker tuning lives in the CM7104 EQ anyway, which `holo-audio start` programs with
the values from Android's mixer paths.

## Mixer setup (from `/vendor/etc/mixer_paths_cm710x_pvt_revB.xml`)

Initial settings (applied by `holo-audio start`):

| control | value |
|---|---|
| `PRIM_MI2S_RX Format` / `PRIM_MI2S_TX Format` | `S24_3LE` |
| `PRIM_MI2S_RX Channels` / `PRIM_MI2S_TX Channels` | `Two` |
| `EQ Pre Gain` | 10 |
| `EQ Band 1..19` | 25 25 25 24 22 21 18 17 13 14 20 24 24 22 25 26 27 28 28 |

Routes and device paths (applied by the UCM profile, i.e. by PipeWire when it opens the card):

| Android path | controls |
|---|---|
| `deep-buffer-playback` | `PRI_MI2S_RX Audio Mixer MultiMedia1` = 1 (PCM 0 playback) |
| `audio-record` | `MultiMedia1 Mixer PRI_MI2S_TX` = 1 (PCM 0 capture) |
| `speaker` | `EQ Switch` 1, `Headphone Switch` 0 0, `Headphone Volume` 86 86, `Speaker Switch` 1 1 |
| `headphones` | `EQ Switch` 0, `Headphone Switch` 1 1, `Speaker Switch` 0 0 |
| `headset-mic` / `handset-mic` | `Mic Switch` 1 (`handset-mic-aec` adds `Mic Array Switch` 1) |

Other codec controls: `Headphone Volume` (DAC2 digital, 0..87 = -65.25..0 dB), `Speaker Volume`
and `MIC Volume` (0..31 = -17.25..+6 dB), `Playback From` (AIF1 / AMIC loopback), `EQ Band 0`.
Android never changes these at runtime: its volume is applied in software by AudioFlinger.

## UCM profile and PipeWire

`/usr/share/alsa/ucm2/Quest1/{Quest1,HiFi}.conf`, found by alsa-lib through
`conf.d/msm8998-cm710x_/msm8998-cm710x_.conf` (the card's driver field is the DT model truncated to
15 characters; `holo-audio start` adds a link if the driver string ever differs). One verb `HiFi`:

- `Speaker` and `Headphones` (conflicting) share PCM `hw:<card>,0`, so WirePlumber builds one
  sink with two ports; `Headphones` has `JackControl "Headphone Jack"` and a higher priority, so
  plugging either 3.5 mm jack switches to it and unplugging returns to the speakers.
- `Mic`: one source on the same PCM 0 (capture).

No hardware volume is declared, so PipeWire uses software volume on a fixed hardware level (the
same as Android); `pactl`/`wpctl set-volume`, Steam and SteamVR all control it.

Steam and SteamVR run as `steamos` with `XDG_RUNTIME_DIR=/run/user/1000`; `user@1000` lingers, so
`pipewire-pulse.socket` provides `/run/user/1000/pulse/native` at boot. The only card's sink and
source become the defaults. vrcompositor ("AUDIO: Initializing Pulse Audio") connects at start:
SteamVR must be restarted once after PipeWire first comes up.

## Verification

```
holo-audio status
sudo -u steamos XDG_RUNTIME_DIR=/run/user/1000 wpctl status
sudo -u steamos XDG_RUNTIME_DIR=/run/user/1000 wpctl set-volume @DEFAULT_AUDIO_SINK@ 0.2
sudo -u steamos XDG_RUNTIME_DIR=/run/user/1000 pw-play /tmp/tone.wav     # low-amplitude test tone
sudo -u steamos XDG_RUNTIME_DIR=/run/user/1000 pw-record --channels 2 /tmp/mic.wav   # then RMS, delete
```

## Volume buttons

`quest1-volkeys.service` runs `/usr/local/bin/quest1-volkeys` (Python, ~6 MB RSS) as `steamos`
(group `input`, `XDG_RUNTIME_DIR=/run/user/1000`). It finds the two key devices by name
(`gpio-keys` = KEY_VOLUMEUP, `qpnp_pon` = KEY_POWER + KEY_VOLUMEDOWN), reads them **without**
`EVIOCGRAB`, ignores every key except the two volume keys, and runs
`wpctl set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ 5%+/-` (volume up also unmutes). Holding a key
repeats after 0.4 s every 0.12 s. Step size: `QUEST1_VOLUME_STEP` (default `5%`).

The power key is left alone: systemd-logind handles it (both devices carry the `power-switch`
udev tag) and its default `HandlePowerKey=poweroff` is still in effect, i.e. a short press of
the power button currently powers the headset off. Sleep/power-menu handling belongs to the
power-button task (`logind.conf.d` `HandlePowerKey=`), not to this daemon.

## Booting the ADSP (needs the user's go-ahead)

### Sequence

Preconditions (all checked by `holo-audio start`, which aborts otherwise): `/run/fw/adsp.*` and
`CM710X.bin` readable, ADSP subsystem `restart_level` reads back `RELATED`. Then:

```
sync                                    # in case the worst case below happens
systemctl start holo-audio.service      # = holo-audio start: RELATED, echo 1 > /sys/kernel/boot_adsp/boot,
                                        #   wait for state ONLINE (<= 15 s) and the card (<= 15 s), mixer setup
holo-audio status
journalctl -k -b --no-pager | grep -E 'lpass|adsp|apr_tal|cm710x|sound-vs1|snd_card'
```

Only once that worked: `holo-audio enable` (the ADSP then boots at every boot from
`holo-audio.service`). The boot itself does not touch Steam, SteamVR or Monado.

### What Android does (bugreport kernel log, stock boot)

```
07:40:23.445 adsp-loader: adsp_loader_do: scheduling work to load ADSP fw      (init, late-fs)
07:40:23.518 subsys-pil-tz 17300000.qcom,lpass: adsp: loading from 0x89a00000 to 0x8b400000
07:40:23.739 subsys-pil-tz 17300000.qcom,lpass: adsp: Brought out of reset
07:40:23.750 ... Subsystem error monitoring/handling services are up / Power/Clock ready interrupt received
07:40:23.750 apr_tal_link_state_cb: edge[lpass] link state[0]
07:40:23.853 msm8998-asoc-snd 1711a000.sound-vs1: msm_init_wsa_dev ...           (APR children populated)
07:40:24.102 cm710x 8-002c: SPI Load Block 0..3 ...                             (CM710X.bin over SPI)
07:40:24.333 msm8998-asoc-snd 1711a000.sound-vs1: Sound card msm8998-cm710x_pvt_revB-snd_card registered
```

Android loads the same files (`adsp.mdt`, `b02`-`b06`, `b08`, `b09`, `b11`) from the same
modem_a partition, at the same reserved address (our kernel log reserves `pil_adsp_region` at
0x89a00000, 26 MiB), with the same kernel driver code (our kernel patch touches no PIL, SSR,
APR or sound file) and the same TrustZone. The native boot loads `venus`, `a540_zap` and the modem
through the same `subsys-pil-tz` / firmware_class path (`firmware_class.path=/run/fw`) already.

Differences on native Holo: the ADSP is booted later (minutes after boot instead of at late-fs),
after the modem, pd-mapper, qrtr-ns and tftp_server are up (on Android they start after it; they
are also up whenever Android restarts the ADSP), and without Android's audio HAL / ACDB (the HAL
only talks to the ADSP once a stream opens; without ACDB the ADSP uses its default topologies).

### What could go wrong, and how likely

Kernel behaviour (`drivers/soc/qcom/subsystem_restart.c`, `adsp-loader.c`, live DT/sysfs):

- **First boot fails** (firmware missing, TZ authentication refused, no "error ready" within
  10 s): `subsys_start()` returns an error, `adsp-loader` logs `Q6 image loading failed`, the
  ADSP stays off. No panic on this path. Result: no audio, nothing else changes. On the
  err-ready timeout the kernel calls the TZ shutdown of the ADSP (routine for this PAS
  subsystem; this is not the modem's MBA shutdown that hangs).
- **ADSP crashes after it came up** (watchdog bite / error fatal): with `restart_level`
  `RELATED` the SSR work shuts the ADSP down and reloads it. `max_restarts` is 0 (no crash-count
  panic), `enable_ramdumps` 0 (no dump wait). The kernel **panics** only if that shutdown or the
  reload fails, or if the ADSP crashes again during the restart: the DT `lpass` node has no
  `qcom,ignore-ssr-failure`. At `SYSTEM` level any ADSP crash would panic, hence the check.
- **Panic / watchdog reset** = the worst case: the headset reboots, the bootloader starts the
  default slot = stock Android (the native image is only `fastboot boot`ed). Recovery:
  `adb reboot bootloader`, `fastboot boot build/boot-native-holo.img`. The kernel log of the
  crashed boot survives in ramoops (`adb bugreport` -> "LAST KMSG"). Nothing is written to any
  partition by the ADSP boot; the only collateral is an unclean shutdown of holo.img/userdata
  (hence `sync` first; journald is volatile anyway).
- **ADSP up but no card** (e.g. a codec/SPI problem): harmless; the card registration and
  CM710X.bin download can be read in `journalctl -k`.

Likelihood: **low**. Booting the ADSP is something stock Android does on every boot through
exactly this code path, firmware and memory layout, and it takes ~0.3 s there. A reboot needs
the ADSP to crash after booting (no evidence it depends on anything we lack: its QMI/PDR peers
pd-mapper, qrtr-ns, tftp_server and the service locator are running, `audio_notifier` already
registered `PDR_ADSP`) *and* the SSR restart to fail. Unlike the modem, which hung the SoC when
it was restarted, the ADSP restart is the generic TZ path. Estimate: a few percent at most for a
reboot into Android, and no persistent damage.

### Success checks

- `holo-audio status`: `adsp: ONLINE, restart_level RELATED, crashes 0`, card 0
  `msm8998-cm710x_pvt_revB-snd_card`, mixer values readable.
- `journalctl -k`: the Android sequence above, including `cm710x 8-002c: SPI Load Block` and
  `Sound card ... registered`; a new input device `msm8998-cm710x_pvt_revB-snd_car Headphone Jack`.
- `runuser -u steamos -- env XDG_RUNTIME_DIR=/run/user/1000 wpctl status`: a sink and a source on
  the new card (instead of `Dummy Output`), ports Speaker/Headphones; if WirePlumber shows a
  "Pro Audio" / generic profile instead, the UCM profile was not found (check
  `alsaucm -c hw:0 list _verbs`).

### Follow-up once the card is up

1. Playback at low volume first (`wpctl set-volume @DEFAULT_AUDIO_SINK@ 0.2`, short sine via
   `pw-play`), speakers then a 3.5 mm jack (port switches on `Headphone Jack`); check
   `Headphone Volume` (86 = -0.75 dB on the speaker path; the headphone path keeps the codec value).
2. Mic: `pw-record --channels 2`, RMS of the samples, then delete the file.
3. `holo-audio enable`.
4. SteamVR: vrcompositor's `SteamVR Audio` stream follows the new default sink; if not, restart
   SteamVR through `q1restart.sh`. To make SteamVR pick the headset device explicitly (useful once
   other sinks exist), the driver can set `Prop_Audio_DefaultPlaybackDeviceId_String` /
   `Prop_Audio_DefaultRecordingDeviceId_String` (OpenVR 2300/2301) to the PipeWire node names
   shown by `pactl list short sinks/sources`.
5. Volume buttons with the real keys (the daemon is already running against `Dummy Output`).

## Status

Verified on the headset (2026-10-11): packages installed, PipeWire + pipewire-pulse running for
`steamos` (`pactl info`: "PulseAudio (on PipeWire 1.4.9)"), UCM profile parses (only `${CardId}`
unresolved without a card).

Verified 2026-10-30: ADSP subsystem `OFFLINING` (the initial zero state: never booted), `restart_level` `RELATED`,
codec I2C device `8-002c` bound to `cm710x`, firmware links in `/run/fw` resolve. Volume-button
daemon running; events injected into event0/event1 moved the default sink (Dummy Output) by
exactly 5 % per press, held keys repeat, capped at 100 %.

Not yet done (waiting for the go-ahead to boot the ADSP): card registration, CM710X.bin download,
speaker/headphone playback, mic levels, SteamVR/Steam on the real sink, the physical buttons.

Attempted 2026-11-01 (ADSP booted by the main session, `holo-audio start`): ADSP ONLINE (crashes 0), card 0
`msm8998-cm710x_pvt_revB-snd_card` registered, `Headphone Jack` input present, UCM verb `HiFi` found
(`alsaucm -c hw:0 list _verbs`), WirePlumber started its UCM profile (`_ucm0001.hw:msm8998cm710xpv`).
**But audio does not work yet and `holo-audio enable` was NOT run:**

- Within seconds of the card appearing, WirePlumber opened/probed dozens of the card's front-end PCMs
  (VoIP, ULL, SLIMBUS_0 Hostless, AFE proxy, ...). The kernel log fills with `q6asm_audio_client_buf_alloc_contiguous:
  buffer already allocated`, `__q6asm_cmd: DSP returned error[ADSP_EFAILED] opcode 68557`, then
  `afe_cmd_memory_map: config cmd failed [ADSP_ENEEDMORE]`, `fail to map memory to DSP`, `msm-pcm-afe hw params failed: -61`,
  and a kernel oops in `msm_afe_hw_params` (`__memset` on an unmapped address, `snd_pcm_hw_params` ioctl) that killed
  wireplumber (zombie, pid 1441). No ADSP crash/SSR; the kernel did not panic. pipewire/pipewire-pulse keep running
  but have no session manager (`wpctl` hangs).
- After that, a direct `aplay -D hw:0,0` (440 Hz, -28 dBFS) fails: `afe_set_lpass_clock_v2: afe_set_lpass_clk_cfg_v2 failed -61`
  -> `msm_mi2s_set_sclk ... PRI_MI2S_RX startup failed -61` -> `aplay: audio open error: No data available`.
  The AFE is either wedged by the probe flood or needs more than the default ADSP state (no ACDB / AFE calibration).
  To tell the two apart the ADSP needs a clean boot (reboot of the headset) and a first test with only
  `aplay -D hw:0,0` **before** WirePlumber ever touches the card.
- Next steps: (1) reboot, `holo-audio start`, stop wireplumber first (or mask it) and try `aplay`; if that plays,
  the problem is WirePlumber's probing: restrict it to PCM 0 (WirePlumber rule disabling the ACP/pro-audio probing, or a
  fixed `api.alsa.pcm.sink` adapter on `hw:0,0` in a pipewire.conf.d drop-in, with the card's `device.disabled = true`);
  (2) if `aplay` still fails with -61 on the lpass clock, the AFE needs what Android's HAL sends first (ACDB load,
  AFE port config / `AFE_PARAM_ID_LPASS_CLK_CONFIG`, possibly `q6afe` init through `libacdbloader`).
- State left: Speaker/Headphone/Mic switches off, ADSP running, `holo-audio.service` not enabled.

### 2026-11-01: WirePlumber no longer drives the card

PipeWire 1.4.9's ACP has no option to skip the Pro Audio probe that oopsed the AFE driver, so
`holo-audio install|ucm` now writes `/etc/wireplumber/wireplumber.conf.d/50-quest1-audio.conf`
(`device.disabled = true` for the ALSA card) and `/etc/pipewire/pipewire.conf.d/50-quest1-audio.conf`
(fixed nodes `quest1-output` and `quest1-mic` on hw:0,0, 48 kHz S16LE). `holo-audio start` applies the
UCM routing itself (`holo-audio output speaker|headphones`). Not yet tested: next boot,
`holo-audio start`, then restart pipewire/wireplumber for steamos, play a short low-level tone, and
only then `holo-audio enable`.


## Notes moved from the to-do list (2026-11-08): Audio

History of measurements, fixes and open points as they were written in docs/todo.md (the to-do list now only
lists what is left to do).

- Audio randomly turns crackly for a while, then recovers by itself, sometimes stays stuck crackly
  (user, 2026-11-02/03). PipeWire sees no xrun at all (pw-top ERR 0, logged every 2 s for 20 min):
  not the graph. Seen with memory pressure (kswapd 50 %, swappiness raised to 100). Capture:
  `q1-audioring` (runtime unit on the headset) keeps the last 90 s of the sink monitor and the ALSA
  status every 20 ms; `systemctl kill -s USR1 q1-audioring` dumps them to /root/audioring-<time>/
  when the user hears it. Clean monitor + crackle = below PipeWire (ADSP, codec).
  Capture 2026-11-03 18:07 in `/root/audioring-180736/` (crackle during and after a SteamVR crash
  loop: load 10-13, kswapd ~46 %, 0.7 GB available; `extra-state.txt` has vmstat and top). Fix later.

- 2026-11-03 22:30: the user recorded the Steam click sound crackling (phone recording, kept on the PC in
  Downloads/Mobile Devices, not in the repo): bursts of short high-frequency spikes on the clicks.
- 2026-11-05: crackling still there, coming and going (user). Memory pressure is constant now (see
  Performance): check the link with it first.
- A short click is heard each time Steam starts, even with the startup movie silenced (user, 2026-11-05:
  "on n'est pas censé entendre ça"). Find its source: the audio device being opened (pop), or a Steam UI sound.
