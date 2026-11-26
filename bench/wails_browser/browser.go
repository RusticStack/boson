package main

import (
	"fmt"
	"net/url"
	"strings"
	"sync"
	"sync/atomic"

	"github.com/wailsapp/wails/v3/pkg/application"
	"github.com/wailsapp/wails/v3/pkg/events"
)

// Tab is one content WebView: a frameless, owned top-level window kept over the main window's content area.
type Tab struct {
	ID        int
	URL       string
	Title     string
	win       *application.WebviewWindow
	discarded bool // "asleep": the window was closed and is recreated on activation
}

type TabInfo struct {
	ID       int    `json:"id"`
	Title    string `json:"title"`
	URL      string `json:"url"`
	Sleeping bool   `json:"sleeping"`
}

type State struct {
	Tabs   []TabInfo `json:"tabs"`
	Active int       `json:"active"`
}

// Browser mirrors examples/browser (Boson): a frameless main window with a 40-DIP header shell and one WebView
// per tab. Wails v3 has one WebView per window, so tabs are owned top-level windows repositioned from the main
// window's move/resize hooks.
type Browser struct {
	app        *application.App
	main       *application.WebviewWindow
	mu         sync.Mutex
	tabs       []*Tab
	active     int
	nextID     int
	windowGen  int
	activeHwnd atomic.Uintptr
	onNav      func(tab *Tab) // benchmark hook, called from the window-event goroutine
}

func normalizeURL(input string) string {
	input = strings.TrimSpace(input)
	switch {
	case input == "":
		return "/newtab.html"
	case strings.HasPrefix(input, "/"), strings.Contains(input, "://"), strings.HasPrefix(input, "about:"), strings.HasPrefix(input, "data:"):
		return input
	case !strings.Contains(input, " ") && (strings.Contains(input, ".") || strings.HasPrefix(input, "localhost")):
		return "https://" + input
	default:
		return "https://duckduckgo.com/?q=" + url.QueryEscape(input)
	}
}

func (b *Browser) mainHwnd() uintptr { return uintptr(b.main.NativeWindow()) }

// syncActive keeps the visible tab glued to the content area. Runs from the main window's WindowDidMove /
// WindowDidResize hooks (synchronous hooks, no listener goroutine), directly with SetWindowPos in physical pixels.
func (b *Browser) syncActive() {
	hwnd := b.activeHwnd.Load()
	if hwnd == 0 {
		return
	}
	x, y, w, h := contentRect(b.mainHwnd())
	placeWindow(hwnd, x, y, w, h)
}

func (b *Browser) createWindow(t *Tab) *application.WebviewWindow {
	b.mu.Lock()
	b.windowGen++
	name := fmt.Sprintf("tab-%d-%d", t.ID, b.windowGen)
	b.mu.Unlock()
	win := b.app.Window.NewWithOptions(application.WebviewWindowOptions{
		Name:             name,
		Title:            fmt.Sprintf("Tab %d", t.ID),
		Frameless:        true,
		Hidden:           true,
		DisableResize:    true,
		URL:              t.URL,
		BackgroundColour: application.NewRGB(32, 33, 36),
		Windows: application.WindowsWindow{
			HiddenOnTaskbar:                   true,
			DisableFramelessWindowDecorations: true,
		},
	})
	win.RegisterHook(events.Windows.WebViewNavigationCompleted, func(*application.WindowEvent) {
		if b.onNav != nil {
			b.onNav(t)
		}
	})
	if hwnd := uintptr(win.NativeWindow()); hwnd != 0 {
		setOwner(hwnd, b.mainHwnd())
	}
	return win
}

// OpenTab creates a tab (and its window) and optionally activates it.
func (b *Browser) OpenTab(rawURL string, activate bool) *Tab {
	b.mu.Lock()
	b.nextID++
	t := &Tab{ID: b.nextID, URL: normalizeURL(rawURL)}
	b.mu.Unlock()
	t.win = b.createWindow(t)
	b.mu.Lock()
	b.tabs = append(b.tabs, t)
	first := b.active == 0
	b.mu.Unlock()
	if activate || first {
		b.Activate(t.ID)
	} else {
		b.publish()
	}
	return t
}

func (b *Browser) find(id int) *Tab {
	b.mu.Lock()
	defer b.mu.Unlock()
	for _, t := range b.tabs {
		if t.ID == id {
			return t
		}
	}
	return nil
}

// Activate hides every other tab and shows id (recreating its window first when it was discarded).
func (b *Browser) Activate(id int) {
	target := b.find(id)
	if target == nil {
		return
	}
	b.mu.Lock()
	others := make([]*application.WebviewWindow, 0, len(b.tabs))
	for _, t := range b.tabs {
		if t.ID != id && t.win != nil {
			others = append(others, t.win)
		}
	}
	b.mu.Unlock()
	for _, w := range others {
		w.Hide()
	}
	if target.discarded || target.win == nil {
		target.win = b.createWindow(target)
		target.discarded = false
	}
	hwnd := uintptr(target.win.NativeWindow())
	b.activeHwnd.Store(hwnd)
	x, y, w, h := contentRect(b.mainHwnd())
	placeWindow(hwnd, x, y, w, h)
	target.win.Show()
	b.mu.Lock()
	b.active = id
	b.mu.Unlock()
	b.publish()
}

// CloseTab destroys a tab's window.
func (b *Browser) CloseTab(id int) {
	b.mu.Lock()
	index := -1
	for i, t := range b.tabs {
		if t.ID == id {
			index = i
		}
	}
	if index < 0 {
		b.mu.Unlock()
		return
	}
	t := b.tabs[index]
	b.tabs = append(b.tabs[:index], b.tabs[index+1:]...)
	wasActive := b.active == id
	var next int
	if len(b.tabs) > 0 {
		next = b.tabs[min(index, len(b.tabs)-1)].ID
	}
	if wasActive {
		b.active = 0
		b.activeHwnd.Store(0)
	}
	b.mu.Unlock()
	if t.win != nil {
		t.win.Close()
	}
	switch {
	case next == 0:
		b.OpenTab("", true)
	case wasActive:
		b.Activate(next)
	default:
		b.publish()
	}
}

// Discard closes a hidden tab's window to release its renderer ("sleeping" without TrySuspend, which the
// Wails v3 public API does not expose). The tab is recreated on activation.
func (b *Browser) Discard(id int) bool {
	t := b.find(id)
	b.mu.Lock()
	isActive := b.active == id
	b.mu.Unlock()
	if t == nil || isActive || t.win == nil {
		return false
	}
	w := t.win
	t.win = nil
	t.discarded = true
	w.Close()
	return true
}

func (b *Browser) TabIDs() []int {
	b.mu.Lock()
	defer b.mu.Unlock()
	ids := make([]int, 0, len(b.tabs))
	for _, t := range b.tabs {
		ids = append(ids, t.ID)
	}
	return ids
}

func (b *Browser) Active() int {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.active
}

func (b *Browser) state() State {
	b.mu.Lock()
	defer b.mu.Unlock()
	s := State{Active: b.active, Tabs: make([]TabInfo, 0, len(b.tabs))}
	for _, t := range b.tabs {
		s.Tabs = append(s.Tabs, TabInfo{ID: t.ID, Title: fmt.Sprintf("Tab %d", t.ID), URL: t.URL, Sleeping: t.discarded})
	}
	return s
}

func (b *Browser) publish() { b.app.Event.Emit("tabs:updated", b.state()) }

// BrowserService is the shell's RPC surface (Browser.* in the Boson example).
type BrowserService struct{ b *Browser }

func (s *BrowserService) GetState() State            { return s.b.state() }
func (s *BrowserService) NewTab(u string)            { go s.b.OpenTab(u, true) }
func (s *BrowserService) CloseTab(id int)            { go s.b.CloseTab(id) }
func (s *BrowserService) ActivateTab(id int)         { go s.b.Activate(id) }
func (s *BrowserService) Minimise()                  { s.b.main.Minimise() }
func (s *BrowserService) ToggleMaximise()            { s.b.main.ToggleMaximise() }
func (s *BrowserService) Close()                     { s.b.main.Close() }
func (s *BrowserService) Navigate(u string) {
	if t := s.b.find(s.b.Active()); t != nil && t.win != nil {
		t.URL = normalizeURL(u)
		t.win.SetURL(t.URL)
	}
}
func (s *BrowserService) Back() {
	if t := s.b.find(s.b.Active()); t != nil && t.win != nil {
		t.win.ExecJS("history.back()")
	}
}
func (s *BrowserService) Forward() {
	if t := s.b.find(s.b.Active()); t != nil && t.win != nil {
		t.win.ExecJS("history.forward()")
	}
}
func (s *BrowserService) Reload() {
	if t := s.b.find(s.b.Active()); t != nil && t.win != nil {
		t.win.Reload()
	}
}
