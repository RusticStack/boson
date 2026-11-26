// wails_compare: the Wails v3 reference app of the comparison suite (bench/compare). Same commands and timeline as
// the Boson, Electron and Tauri apps; the frontend is bench/compare/shared plus adapter.js.
package main

import (
	"embed"
	"io/fs"
	"log"
	"os"
	"path/filepath"
	"regexp"
	"strings"

	"github.com/wailsapp/wails/v3/pkg/application"
)

//go:embed all:frontend
var assets embed.FS

// Item is one element of the 10k-item reply.
type Item struct {
	ID     int     `json:"id"`
	Name   string  `json:"name"`
	Value  float64 `json:"value"`
	Active bool    `json:"active"`
}

var markName = regexp.MustCompile(`^[a-z0-9_]{1,32}$`)

// CompareService holds the commands the page calls (Call.ByName("main.CompareService.<Method>")).
type CompareService struct {
	app *application.App
}

func writeAtomically(name, text string) {
	dir := os.Getenv("COMPARE_DIR")
	if dir == "" {
		return
	}
	target := filepath.Join(dir, name)
	tmp := target + ".tmp"
	if err := os.WriteFile(tmp, []byte(text), 0o644); err == nil {
		_ = os.Rename(tmp, target)
	}
}

func (s *CompareService) Config() string {
	if c := os.Getenv("COMPARE_CONFIG"); c != "" {
		return c
	}
	return "{}"
}

func (s *CompareService) Ping(seq int) int        { return seq }
func (s *CompareService) Echo(text string) string { return text }

func (s *CompareService) Items() []Item {
	items := make([]Item, 10000)
	for i := range items {
		items[i] = Item{ID: i, Name: "item-" + itoa(i), Value: float64(i) * 0.5, Active: i%2 == 0}
	}
	return items
}

// Burst emits n "burst" events. Wails v3 has no per-window emit: the event reaches every window.
func (s *CompareService) Burst(n int) {
	for i := 0; i < n; i++ {
		s.app.Event.Emit("burst", i)
	}
}

func (s *CompareService) OpenWindow() {
	s.app.Window.NewWithOptions(windowOptions("w2", "Comparison app (2)", "/?w=2"))
}

func (s *CompareService) Mark(text string) {
	name, value, ok := strings.Cut(text, " ")
	if ok && markName.MatchString(name) {
		writeAtomically(name+".mark", value)
	}
}

func (s *CompareService) Report(result string) {
	writeAtomically("result.json", result)
	writeAtomically("done.mark", "1")
}

func itoa(i int) string {
	var buf [20]byte
	pos := len(buf)
	if i == 0 {
		return "0"
	}
	for i > 0 {
		pos--
		buf[pos] = byte('0' + i%10)
		i /= 10
	}
	return string(buf[pos:])
}

func windowOptions(name, title, url string) application.WebviewWindowOptions {
	return application.WebviewWindowOptions{
		Name:             name,
		Title:            title,
		Width:            900,
		Height:           640,
		URL:              url,
		BackgroundColour: application.NewRGB(27, 27, 31),
	}
}

func main() {
	frontend, err := fs.Sub(assets, "frontend")
	if err != nil {
		log.Fatal(err)
	}
	service := &CompareService{}
	app := application.New(application.Options{
		Name:     "wails_compare",
		Services: []application.Service{application.NewService(service)},
		Assets:   application.AssetOptions{Handler: application.AssetFileServerFS(frontend)},
	})
	service.app = app
	app.Window.NewWithOptions(windowOptions("main", "Comparison app", "/"))
	if err := app.Run(); err != nil {
		log.Fatal(err)
	}
}
