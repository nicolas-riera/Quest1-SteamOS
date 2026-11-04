package main

import (
	"embed"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"syscall"
	"time"
)

// Google's platform-tools for Windows (adb, fastboot and their DLLs), fetched by build.sh, never committed.
//
//go:embed platform-tools
var platformTools embed.FS

type Device struct {
	adb, fastboot string
}

// NewDevice unpacks the bundled platform-tools: the app never uses an adb/fastboot of the user's.
func NewDevice(dir string) (*Device, error) {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, err
	}
	err := fs.WalkDir(platformTools, "platform-tools", func(p string, d fs.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return err
		}
		b, err := platformTools.ReadFile(p)
		if err != nil {
			return err
		}
		out := filepath.Join(dir, filepath.Base(p))
		if st, e := os.Stat(out); e == nil && st.Size() == int64(len(b)) {
			return nil
		}
		return os.WriteFile(out, b, 0o755)
	})
	if err != nil {
		return nil, err
	}
	return &Device{adb: filepath.Join(dir, "adb.exe"), fastboot: filepath.Join(dir, "fastboot.exe")}, nil
}

func run(bin string, args ...string) (string, error) {
	c := exec.Command(bin, args...)
	c.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: 0x08000000} // CREATE_NO_WINDOW
	out, err := c.CombinedOutput()
	return strings.TrimSpace(strings.ReplaceAll(string(out), "\r", "")), err
}

func (d *Device) Adb(serial string, args ...string) (string, error) {
	if serial != "" {
		args = append([]string{"-s", serial}, args...)
	}
	return run(d.adb, args...)
}

func (d *Device) Fastboot(serial string, args ...string) (string, error) {
	if serial != "" {
		args = append([]string{"-s", serial}, args...)
	}
	return run(d.fastboot, args...)
}

// AdbDevices: serial -> state ("device", "unauthorized", "offline"...).
func (d *Device) AdbDevices() map[string]string {
	out, _ := d.Adb("", "devices")
	m := map[string]string{}
	for _, l := range strings.Split(out, "\n")[1:] {
		f := strings.Fields(l)
		if len(f) >= 2 {
			m[f[0]] = f[1]
		}
	}
	return m
}

func (d *Device) FastbootDevices() []string {
	out, _ := d.Fastboot("", "devices")
	var s []string
	for _, l := range strings.Split(out, "\n") {
		if f := strings.Fields(l); len(f) >= 2 && f[1] == "fastboot" {
			s = append(s, f[0])
		}
	}
	return s
}

func (d *Device) WaitFastboot(serial string, timeout time.Duration) error {
	for end := time.Now().Add(timeout); time.Now().Before(end); time.Sleep(time.Second) {
		for _, s := range d.FastbootDevices() {
			if s == serial {
				return nil
			}
		}
	}
	return errors.New("the headset did not show up in fastboot mode")
}

func (d *Device) WaitAdb(serial string, timeout time.Duration) error {
	for end := time.Now().Add(timeout); time.Now().Before(end); time.Sleep(2 * time.Second) {
		if d.AdbDevices()[serial] == "device" {
			return nil
		}
	}
	return errors.New("the headset did not come back in Android")
}

// Getvar returns a bootloader variable ("unlocked", "partition-size:userdata"...).
func (d *Device) Getvar(serial, name string) string {
	out, _ := d.Fastboot(serial, "getvar", name)
	m := regexp.MustCompile(`(?m)^` + regexp.QuoteMeta(name) + `:\s*(.*)$`).FindStringSubmatch(out)
	if m == nil {
		return ""
	}
	return strings.TrimSpace(m[1])
}

// --- the checks page --------------------------------------------------------------------------------------

func (a *App) check(id, state, detail string) {
	a.emit(map[string]any{"type": "check", "id": id, "state": state, "detail": detail})
}

// Check: a Quest 1 in Android with USB debugging, on Meta's last build, charged, bootloader unlocked, room on
// the PC. The bootloader is only looked at (getvar), then the headset goes back to Android.
func (a *App) Check() error {
	ok := true
	fail := func(id, detail string) { a.check(id, "fail", detail); ok = false }
	defer func() { a.emit(map[string]any{"type": "checks", "ok": ok}) }()

	a.check("adb", "run", "")
	var serial string
	for s, st := range a.dev.AdbDevices() {
		switch st {
		case "device":
			if serial != "" {
				fail("adb", "More than one Android device is connected: keep only the headset plugged in.")
				return nil
			}
			serial = s
		case "unauthorized":
			fail("adb", "Put the headset on and allow USB debugging for this computer, then check again.")
			return nil
		}
	}
	if serial == "" {
		fail("adb", "No headset found. Plug it in with a data USB cable, with developer mode and USB debugging on.")
		return nil
	}
	a.serial = serial
	a.check("adb", "ok", "Headset found.")

	a.check("model", "run", "")
	if dev, _ := a.dev.Adb(serial, "shell", "getprop", "ro.product.device"); dev != "monterey" {
		fail("model", fmt.Sprintf("This is not an Oculus Quest 1 (device %q).", dev))
		return nil
	}
	a.check("model", "ok", "Oculus Quest 1.")

	a.check("build", "run", "")
	if fp, _ := a.dev.Adb(serial, "shell", "getprop", "ro.build.fingerprint"); !strings.Contains(fp, metaBuild) {
		fail("build", "Update the headset to Meta's last software (build "+metaBuild+") first: "+
			"the installer takes the tracking software from that version.")
	} else {
		a.check("build", "ok", "Meta's last build.")
	}

	a.check("battery", "run", "")
	out, _ := a.dev.Adb(serial, "shell", "dumpsys", "battery")
	lvl := -1
	if m := regexp.MustCompile(`level:\s*(\d+)`).FindStringSubmatch(out); m != nil {
		lvl, _ = strconv.Atoi(m[1])
	}
	if lvl < minBattery {
		fail("battery", fmt.Sprintf("Battery at %d %%: charge it to %d %% or more.", lvl, minBattery))
	} else {
		a.check("battery", "ok", fmt.Sprintf("Battery at %d %%.", lvl))
	}

	a.check("space", "run", "")
	free, err := freeSpace(a.work)
	if need := uint64(imageBytes * 115 / 100); err != nil || free < need {
		fail("space", fmt.Sprintf("Not enough free space on the drive of %s: %.1f GB needed, %.1f GB free.",
			a.work, float64(need)/1e9, float64(free)/1e9))
	} else {
		a.check("space", "ok", fmt.Sprintf("%.0f GB free on the PC.", float64(free)/1e9))
	}

	// the bootloader: only read (getvar), then back to Android before it times out
	a.check("bootloader", "run", "Restarting the headset into its bootloader...")
	if _, err := a.dev.Adb(serial, "reboot", "bootloader"); err != nil {
		fail("bootloader", "Could not restart the headset into its bootloader.")
		return nil
	}
	if err := a.dev.WaitFastboot(serial, 60*time.Second); err != nil {
		fail("bootloader", "The headset did not show up in fastboot mode (check the USB driver).")
		return nil
	}
	unlocked := a.dev.Getvar(serial, "unlocked")
	size, _ := strconv.ParseUint(strings.TrimPrefix(a.dev.Getvar(serial, "partition-size:userdata"), "0x"), 16, 64)
	a.check("bootloader", "run", "Going back to Android...")
	a.dev.Fastboot(serial, "reboot")
	switch {
	case unlocked != "yes":
		fail("bootloader", "The bootloader is locked: unlock it first (see the welcome page).")
	case size < 40e9:
		fail("bootloader", fmt.Sprintf("Unexpected partition table (userdata is %d bytes).", size))
	default:
		a.check("bootloader", "ok", "Bootloader unlocked.")
	}
	if a.dev.WaitAdb(serial, 120*time.Second) != nil {
		a.log("the headset is not back in Android yet; it will be found again before the install")
	}
	return nil
}
