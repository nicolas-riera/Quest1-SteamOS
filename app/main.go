// Quest1-SteamOS installer: installs SteamOS on an Oculus Quest 1 from Windows, from start to finish.
//
// The PC only drives: adb/fastboot (bundled), the installer's micro Linux (fastboot-booted, nothing written)
// downloads everything itself through tunnels this app opens over USB, builds the image, and the app flashes
// it with fastboot. See installer/README.md for the whole design.
package main

import (
	_ "embed"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sync"

	webview2 "github.com/jchv/go-webview2"
)

//go:embed ui.html
var uiHTML string

// Version is set at build time (-ldflags "-X main.Version=0.0.1").
var Version = "dev"

const (
	prebuiltBase = "https://raw.githubusercontent.com/nicolas-riera/Quest1-SteamOS/prebuilt/"
	agentURL     = "http://192.168.77.1:8077"
	metaBuild    = "49845030443200410" // the Meta build our bundled tracking stack comes from
	minBattery   = 50
)

type App struct {
	w      webview2.WebView
	work   string // %TEMP%\quest1-steamos: adb/fastboot, the window's data
	big    string // the scratch disk and images: %TEMP%\quest1-steamos\work, or X:\TEMP-STEAMOS-QUEST1
	dev    *Device
	mu     sync.Mutex
	busy   bool
	serial string
}

// emit sends an event to the page: q1.on({type: ...}).
func (a *App) emit(v map[string]any) {
	b, _ := json.Marshal(v)
	a.w.Dispatch(func() { a.w.Eval("q1.on(" + string(b) + ")") })
}

func (a *App) log(format string, args ...any) {
	a.emit(map[string]any{"type": "log", "line": fmt.Sprintf(format, args...)})
}

// start runs fn once at a time, in the background; a failure is shown on the page.
func (a *App) start(name string, fn func() error) {
	a.mu.Lock()
	if a.busy {
		a.mu.Unlock()
		return
	}
	a.busy = true
	a.mu.Unlock()
	go func() {
		err := fn()
		a.mu.Lock()
		a.busy = false
		a.mu.Unlock()
		if err != nil {
			a.emit(map[string]any{"type": "error", "phase": name, "message": err.Error()})
		}
	}()
}

func main() {
	work := filepath.Join(os.TempDir(), "quest1-steamos")
	os.MkdirAll(work, 0o755)
	w := webview2.NewWithOptions(webview2.WebViewOptions{
		AutoFocus: true,
		DataPath:  filepath.Join(work, "webview"),
		WindowOptions: webview2.WindowOptions{
			Title: "Quest1-SteamOS installer " + Version, Width: 920, Height: 680, Center: true,
		},
	})
	if w == nil {
		msgBox("Quest1-SteamOS installer", "Microsoft Edge WebView2 is missing. Install it from "+
			"https://developer.microsoft.com/microsoft-edge/webview2/ and start the installer again.")
		return
	}
	defer w.Destroy()
	a := &App{w: w, work: work, big: filepath.Join(work, "work")}
	dev, err := NewDevice(filepath.Join(work, "platform-tools"))
	if err != nil {
		msgBox("Quest1-SteamOS installer", "Could not unpack adb/fastboot: "+err.Error())
		return
	}
	a.dev = dev
	w.Bind("q1Version", func() string { return Version })
	w.Bind("q1Check", func() { a.start("check", a.Check) })
	w.Bind("q1Drives", a.Drives)
	w.Bind("q1SetDrive", a.SetDrive)
	w.Bind("q1Install", func() { a.start("install", a.Install) })
	w.Bind("q1Open", func(url string) { exec.Command("rundll32", "url.dll,FileProtocolHandler", url).Start() })
	w.SetHtml(uiHTML)
	w.Run()
}
