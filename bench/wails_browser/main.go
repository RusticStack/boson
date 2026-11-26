// wails_browser: the Wails v3 equivalent of examples/browser (Boson) for the multi-tab browser benchmark.
//
//	wails_browser.exe [--move-debounce=MS] [--bench] [url ...]
//
// Frameless main window with a 40-DIP header shell (frontend/index.html) and one WebView per tab. Wails v3
// hosts one WebView per window, so each tab is a frameless top-level window owned by the main window and moved
// with it from the WindowDidMove / WindowDidResize hooks (Windows debounces WindowDidMove, 50 ms by default).
// "Sleeping" a tab closes its window (the public API has no TrySuspend); activation recreates it.
package main

import (
	"embed"
	"io/fs"
	"log"
	"os"
	"strconv"
	"strings"
	"unsafe"

	"github.com/wailsapp/wails/v3/pkg/application"
	"github.com/wailsapp/wails/v3/pkg/events"
)

//go:embed all:frontend
var assets embed.FS

func uintptrOf(p *int64) uintptr { return uintptr(unsafe.Pointer(p)) }

func main() {
	moveDebounce := uint16(0) // 0 = Wails default (50 ms)
	benchMode := false
	var browserArgs []string
	var urls []string
	for _, arg := range os.Args[1:] {
		switch {
		case strings.HasPrefix(arg, "--move-debounce="):
			if v, err := strconv.Atoi(strings.TrimPrefix(arg, "--move-debounce=")); err == nil && v > 0 && v < 65536 {
				moveDebounce = uint16(v)
			}
		case arg == "--bench":
			benchMode = true
		case strings.HasPrefix(arg, "--remote-debugging-port="): // diagnostics: attach DevTools / CDP
			browserArgs = append(browserArgs, arg)
		case !strings.HasPrefix(arg, "--"):
			urls = append(urls, arg)
		}
	}
	benchDir := os.Getenv("BOSON_BENCH_DIR")
	if benchMode && benchDir == "" {
		log.Println("--bench needs BOSON_BENCH_DIR (run it through boson_bench --scenario browser)")
		benchMode = false
	}

	frontend, err := fs.Sub(assets, "frontend")
	if err != nil {
		log.Fatal(err)
	}
	br := &Browser{}
	benchService := &BenchService{}
	app := application.New(application.Options{
		Name:        "wails_browser",
		Description: "Wails v3 multi-tab browser (Boson benchmark counterpart)",
		Services: []application.Service{
			application.NewService(&BrowserService{b: br}),
			application.NewService(benchService),
		},
		Assets:  application.AssetOptions{Handler: application.AssetFileServerFS(frontend)},
		Windows: application.WindowsOptions{AdditionalBrowserArgs: browserArgs},
	})
	br.app = app
	benchService.app = app

	br.main = app.Window.NewWithOptions(application.WebviewWindowOptions{
		Name:             "main",
		Title:            "Wails Browser",
		Width:            1280,
		Height:           860,
		MinWidth:         640,
		MinHeight:        420,
		Frameless:        true,
		URL:              "/", // frontend/index.html (the Wails asset server requires an index.html)
		BackgroundType:   application.BackgroundTypeTranslucent,
		BackgroundColour: application.NewRGBA(0, 0, 0, 0),
		Windows: application.WindowsWindow{
			BackdropType:            application.Mica,
			Theme:                   application.Dark,
			WindowDidMoveDebounceMS: moveDebounce,
		},
	})
	br.main.RegisterHook(events.Windows.WindowDidMove, func(*application.WindowEvent) { br.syncActive() })
	br.main.RegisterHook(events.Windows.WindowDidResize, func(*application.WindowEvent) { br.syncActive() })
	br.main.RegisterHook(events.Common.WindowClosing, func(*application.WindowEvent) { go app.Quit() })

	var bench *Bench
	if benchMode {
		bench = newBench(benchDir, br)
		benchService.d = bench
		br.onNav = bench.onNav
	}

	app.Event.OnApplicationEvent(events.Common.ApplicationStarted, func(*application.ApplicationEvent) {
		go func() {
			if bench != nil {
				br.OpenTab(bench.nextTabURL(), true)
				bench.run()
				return
			}
			if len(urls) == 0 {
				urls = []string{""}
			}
			for _, u := range urls {
				br.OpenTab(u, true)
			}
		}()
	})

	if err := app.Run(); err != nil {
		log.Fatal(err)
	}
}
