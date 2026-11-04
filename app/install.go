package main

import (
	"bufio"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"
	"unsafe"
)

// room needed on the PC: the headset's scratch disk (downloads, the image being built: ~20 GB at most, given
// back as the build goes) and the sparse image that comes back (~9-10 GB)
const needBytes = 30e9

// Install: the whole sequence once the user accepted the wipe. Steps (one progress bar each on the page):
// files, boot, scratch, dl:* (prepare.py on the headset), build:* (build_image.py), download, flash, cleanup.
// The headset's storage is only written by the final flash: its scratch space is a file here (nbd.go).
func (a *App) Install() error {
	serial := a.serial
	if serial == "" {
		return errors.New("run the checks first")
	}
	step := func(id, status string) { a.emit(map[string]any{"type": "step", "step": id, "status": status}) }

	// our boot and installer images, from the repository's prebuilt branch
	step("files", "start")
	var man struct {
		Commit string
		Files  map[string]struct {
			Size   int64
			Sha256 string
		}
	}
	if err := getJSON(prebuiltBase+"manifest.json", &man); err != nil {
		return fmt.Errorf("prebuilt files: %w", err)
	}
	os.MkdirAll(a.big, 0o755)
	files := []string{"installer.img", "boot-native-holo.img"}
	var total, done int64
	for _, f := range files {
		total += man.Files[f].Size
	}
	for _, f := range files {
		m := man.Files[f]
		if err := download(prebuiltBase+f, filepath.Join(a.big, f), m.Sha256, func(n int64) {
			a.progress("files", done+n, total)
		}); err != nil {
			return fmt.Errorf("%s: %w", f, err)
		}
		done += m.Size
	}
	step("files", "done")

	// the micro Linux: loaded in RAM by the bootloader, nothing written
	step("boot", "start")
	if a.dev.AdbDevices()[serial] == "device" {
		a.dev.Adb(serial, "reboot", "bootloader")
	}
	if err := a.dev.WaitFastboot(serial, 90*time.Second); err != nil {
		return err
	}
	if out, err := a.dev.Fastboot(serial, "boot", filepath.Join(a.big, "installer.img")); err != nil {
		return fmt.Errorf("fastboot boot: %s", out)
	}
	scratch := filepath.Join(a.big, "scratch.img")
	ag := &Agent{base: agentURL, scratch: scratch}
	if err := ag.Wait(3 * time.Minute); err != nil {
		return errors.New("the installer did not come up on the USB network (is the USB NCM driver installed?)")
	}
	step("boot", "done")

	// the headset's way out (downloads) and its scratch disk: connections we open to it
	for i := 0; i < 4; i++ {
		go ag.Tunnel()
	}
	step("scratch", "start")
	if err := ag.Post("/scratch", map[string]any{}, nil); err != nil {
		return err
	}
	step("scratch", "done")
	if err := a.follow(ag, "/prepare", "dl:"); err != nil {
		return err
	}
	if err := a.follow(ag, "/build", "build:"); err != nil {
		return err
	}

	step("download", "start")
	img := filepath.Join(a.big, "userdata.simg")
	if err := ag.Image(img, func(n, t int64) { a.progress("download", n, t) }); err != nil {
		return err
	}
	step("download", "done")

	// flash: back to the bootloader (the agent unmounts and detaches the scratch disk first), the scratch
	// file goes, then only these partitions are written
	step("flash", "start")
	ag.Post("/reboot", map[string]any{"target": "bootloader"}, nil)
	if err := a.dev.WaitFastboot(serial, 2*time.Minute); err != nil {
		return err
	}
	os.Remove(scratch)
	boot := filepath.Join(a.big, "boot-native-holo.img")
	cmds := [][]string{
		{"flash", "boot_a", boot}, {"flash", "boot_b", boot},
		{"flash", "userdata", img},
		{"erase", "system_a"}, {"erase", "system_b"},
		{"set_active", "a"},
	}
	for i, c := range cmds {
		a.log("fastboot %s", strings.Join(c, " "))
		if out, err := a.dev.Fastboot(serial, c...); err != nil {
			return fmt.Errorf("fastboot %s: %s", strings.Join(c, " "), out)
		}
		a.progress("flash", int64(i+1), int64(len(cmds)))
	}
	a.dev.Fastboot(serial, "reboot")
	step("flash", "done")

	step("cleanup", "start")
	os.RemoveAll(a.big)
	step("cleanup", "done")
	a.emit(map[string]any{"type": "done"})
	return nil
}

func (a *App) progress(step string, done, total int64) {
	a.emit(map[string]any{"type": "progress", "step": step, "done": done, "total": total})
}

// follow starts a job on the headset and turns its "@step"/"@progress" lines into the page's bars.
func (a *App) follow(ag *Agent, path, prefix string) error {
	var ev struct {
		Next, Total int
		Lines       []string
		State       string
	}
	if err := ag.Get("/events?from=0", &ev); err != nil {
		return err
	}
	n := ev.Total
	if err := ag.Post(path, map[string]any{}, nil); err != nil {
		return err
	}
	for {
		if err := ag.Get("/events?from="+strconv.Itoa(n), &ev); err != nil {
			return err
		}
		for _, l := range ev.Lines {
			kind, js, _ := strings.Cut(l, " ")
			var m map[string]any
			if (kind == "@step" || kind == "@progress") && json.Unmarshal([]byte(js), &m) == nil {
				m["type"] = kind[1:]
				m["step"] = prefix + fmt.Sprint(m["step"])
				a.emit(m)
			} else {
				a.emit(map[string]any{"type": "log", "line": l})
			}
		}
		n = ev.Next
		if (ev.State == "done" || ev.State == "failed") && len(ev.Lines) == 0 {
			if ev.State == "failed" {
				return fmt.Errorf("%s failed on the headset (see the log)", path[1:])
			}
			return nil
		}
		if len(ev.Lines) == 0 {
			time.Sleep(time.Second)
		}
	}
}

// --- the agent (installer/agent.py) --------------------------------------------------------------------

type Agent struct{ base, scratch string }

var client = &http.Client{Timeout: 10 * time.Minute}

func (g *Agent) Get(path string, v any) error {
	r, err := client.Get(g.base + path)
	if err != nil {
		return err
	}
	defer r.Body.Close()
	if r.StatusCode >= 300 {
		b, _ := io.ReadAll(r.Body)
		return fmt.Errorf("agent %s: %s %s", path, r.Status, b)
	}
	return json.NewDecoder(r.Body).Decode(v)
}

func (g *Agent) Post(path string, body, v any) error {
	b, _ := json.Marshal(body)
	r, err := client.Post(g.base+path, "application/json", bytes.NewReader(b))
	if err != nil {
		return err
	}
	defer r.Body.Close()
	if r.StatusCode >= 300 {
		e, _ := io.ReadAll(r.Body)
		return fmt.Errorf("agent %s: %s %s", path, r.Status, e)
	}
	if v != nil {
		return json.NewDecoder(r.Body).Decode(v)
	}
	return nil
}

func (g *Agent) Wait(timeout time.Duration) error {
	var info map[string]any
	c := &http.Client{Timeout: 3 * time.Second}
	for end := time.Now().Add(timeout); time.Now().Before(end); time.Sleep(2 * time.Second) {
		if r, err := c.Get(g.base + "/info"); err == nil {
			json.NewDecoder(r.Body).Decode(&info)
			r.Body.Close()
			return nil
		}
	}
	return errors.New("no agent")
}

func (g *Agent) Image(out string, prog func(n, total int64)) error {
	r, err := (&http.Client{}).Get(g.base + "/image")
	if err != nil {
		return err
	}
	defer r.Body.Close()
	if r.StatusCode != 200 {
		return fmt.Errorf("image: %s", r.Status)
	}
	f, err := os.Create(out + ".part")
	if err != nil {
		return err
	}
	n, err := io.Copy(f, &counter{r: r.Body, f: func(n int64) { prog(n, r.ContentLength) }})
	f.Close()
	if err != nil || n != r.ContentLength {
		return fmt.Errorf("image download incomplete: %v", err)
	}
	return os.Rename(out+".part", out)
}

// Tunnel lends the headset one outgoing connection at a time (installer/agent.py GET /tunnel): the headset
// sends "host:port\n", we connect there, answer "OK\n" and copy bytes both ways. Only outgoing connections
// from the PC, so no firewall prompt.
func (g *Agent) Tunnel() {
	host := strings.TrimPrefix(g.base, "http://")
	for {
		if err := g.tunnelOnce(host); err != nil {
			time.Sleep(time.Second)
		}
	}
}

func (g *Agent) tunnelOnce(host string) error {
	c, err := net.DialTimeout("tcp", host, 10*time.Second)
	if err != nil {
		return err
	}
	fmt.Fprintf(c, "GET /tunnel HTTP/1.1\r\nHost: headset\r\nUpgrade: q1tunnel\r\nConnection: Upgrade\r\n\r\n")
	br := bufio.NewReader(c)
	status, err := br.ReadString('\n')
	if err != nil || !strings.Contains(status, " 101 ") {
		c.Close()
		return errors.New("tunnel refused")
	}
	for {
		l, err := br.ReadString('\n')
		if err != nil {
			c.Close()
			return err
		}
		if l == "\r\n" {
			break
		}
	}
	target, err := br.ReadString('\n')
	if err != nil {
		c.Close()
		return err
	}
	if strings.TrimSpace(target) == "nbd:scratch" { // the scratch disk, served from here
		c.Write([]byte("OK\n"))
		go serveNBD(c, br, g.scratch)
		return nil
	}
	t, err := net.DialTimeout("tcp", strings.TrimSpace(target), 30*time.Second)
	if err != nil {
		c.Write([]byte("NO\n"))
		c.Close()
		return nil
	}
	c.Write([]byte("OK\n"))
	go func() {
		io.Copy(t, br) // what the headset sends (buffered bytes first)
		t.(*net.TCPConn).CloseWrite()
	}()
	go func() {
		io.Copy(c, t)
		c.(*net.TCPConn).CloseWrite()
		c.Close()
		t.Close()
	}()
	return nil
}

// --- downloads and system helpers ---------------------------------------------------------------------------

type counter struct {
	r io.Reader
	n int64
	f func(int64)
	t time.Time
}

func (c *counter) Read(p []byte) (int, error) {
	n, err := c.r.Read(p)
	c.n += int64(n)
	if time.Since(c.t) > 300*time.Millisecond || err == io.EOF {
		c.t = time.Now()
		c.f(c.n)
	}
	return n, err
}

func getJSON(url string, v any) error {
	r, err := client.Get(url)
	if err != nil {
		return err
	}
	defer r.Body.Close()
	if r.StatusCode != 200 {
		return fmt.Errorf("%s: %s", url, r.Status)
	}
	return json.NewDecoder(r.Body).Decode(v)
}

// download fetches url to out unless out already has the wanted sha256.
func download(url, out, want string, prog func(int64)) error {
	if sum, err := fileSha256(out); err == nil && sum == want {
		return nil
	}
	r, err := client.Get(url)
	if err != nil {
		return err
	}
	defer r.Body.Close()
	if r.StatusCode != 200 {
		return fmt.Errorf("%s: %s", url, r.Status)
	}
	f, err := os.Create(out + ".part")
	if err != nil {
		return err
	}
	h := sha256.New()
	_, err = io.Copy(io.MultiWriter(f, h), &counter{r: r.Body, f: prog})
	f.Close()
	if err != nil {
		return err
	}
	if got := hex.EncodeToString(h.Sum(nil)); got != want {
		os.Remove(out + ".part")
		return fmt.Errorf("sha256 %s, expected %s", got, want)
	}
	return os.Rename(out+".part", out)
}

func fileSha256(p string) (string, error) {
	f, err := os.Open(p)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

var (
	kernel32      = syscall.NewLazyDLL("kernel32.dll")
	getDiskFreeEx = kernel32.NewProc("GetDiskFreeSpaceExW")
	user32        = syscall.NewLazyDLL("user32.dll")
	messageBoxW   = user32.NewProc("MessageBoxW")
)

func freeSpace(dir string) (uint64, error) {
	var free uint64
	p, _ := syscall.UTF16PtrFromString(dir)
	if r, _, err := getDiskFreeEx.Call(uintptr(unsafe.Pointer(p)), uintptr(unsafe.Pointer(&free)), 0, 0); r == 0 {
		return 0, err
	}
	return free, nil
}

func msgBox(title, text string) {
	t, _ := syscall.UTF16PtrFromString(title)
	m, _ := syscall.UTF16PtrFromString(text)
	messageBoxW.Call(0, uintptr(unsafe.Pointer(m)), uintptr(unsafe.Pointer(t)), 0x10)
}
