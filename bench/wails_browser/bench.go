package main

// Benchmark mode (--bench with BOSON_BENCH_DIR set, driven by boson_bench --scenario browser). Same scenario,
// step order and file protocol as examples/browser/bench_driver.hpp; see docs/BENCHMARKS.md.

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/wailsapp/wails/v3/pkg/application"
)

type AppResult struct {
	Framework         string          `json:"framework"`
	SleepMode         string          `json:"sleep_mode"`
	IPC               json.RawMessage `json:"ipc"`
	NewTabNavMs       []float64       `json:"new_tab_nav_ms"`
	TabSwitchMs       []float64       `json:"tab_switch_ms"`
	WakeMs            []float64       `json:"wake_ms"`
	RecreateNavMs     []float64       `json:"recreate_nav_ms"`
	RecreateVisibleMs []float64       `json:"recreate_visible_ms"`
	SleptAt5          int             `json:"slept_at_5"`
	SleptAt10         int             `json:"slept_at_10"`
	Errors            []string        `json:"errors"`
}

type Bench struct {
	dir     string
	br      *Browser
	freq    int64
	painted chan struct{}
	navCh   chan int
	markCh  chan int
	ipcCh   chan string
	once    sync.Once
	navMu   sync.Mutex
	firstNav int64
	tabNo   int
	cpSeq   int
	markSeq int
	result  AppResult
}

const (
	switches  = 20
	recreates = 5
)

func newBench(dir string, br *Browser) *Bench {
	d := &Bench{
		dir: dir, br: br,
		painted: make(chan struct{}, 1), navCh: make(chan int, 64), markCh: make(chan int, 64), ipcCh: make(chan string, 1),
		result: AppResult{Framework: "wails-v3", SleepMode: "discard (window closed, recreated on activation)",
			IPC: json.RawMessage("null"), NewTabNavMs: []float64{}, TabSwitchMs: []float64{}, WakeMs: []float64{},
			RecreateNavMs: []float64{}, RecreateVisibleMs: []float64{}, Errors: []string{}},
	}
	var f int64
	procQPF := kernel32.NewProc("QueryPerformanceFrequency")
	procQPF.Call(uintptrOf(&f))
	d.freq = f
	return d
}

func (d *Bench) ms(t0 int64) float64 { return float64(qpcNow()-t0) * 1000.0 / float64(d.freq) }

func (d *Bench) errorf(format string, args ...any) {
	d.result.Errors = append(d.result.Errors, fmt.Sprintf(format, args...))
	d.logf("error: "+format, args...)
}

// logf appends a line to BOSON_BENCH_DIR/app.log (the harness prints its tail when a run fails).
func (d *Bench) logf(format string, args ...any) {
	f, err := os.OpenFile(filepath.Join(d.dir, "app.log"), os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o644)
	if err != nil {
		return
	}
	defer f.Close()
	fmt.Fprintf(f, "%s "+format+"\n", append([]any{time.Now().Format("15:04:05.000")}, args...)...)
}

func (d *Bench) write(name, text string) {
	target := filepath.Join(d.dir, name)
	tmp := target + ".tmp"
	if err := os.WriteFile(tmp, []byte(text), 0o644); err == nil {
		_ = os.Rename(tmp, target)
	}
}

func (d *Bench) nextTabURL() string {
	d.tabNo++
	return fmt.Sprintf("/bench_tab.html?tab=%d", d.tabNo)
}

// ---- inputs ----------------------------------------------------------------------------------------------

func (d *Bench) onPainted() {
	d.once.Do(func() {
		stamp := strconv.FormatInt(qpcNow(), 10)
		d.write("shell_paint", stamp)
		d.write("ready", stamp)
		d.painted <- struct{}{}
	})
}

func (d *Bench) onNav(t *Tab) {
	d.navMu.Lock()
	if d.firstNav == 0 {
		d.firstNav = qpcNow()
		d.write("first_nav", strconv.FormatInt(d.firstNav, 10))
	}
	d.navMu.Unlock()
	select {
	case d.navCh <- t.ID:
	default:
	}
}

func (d *Bench) onMark(seq int) {
	select {
	case d.markCh <- seq:
	default:
	}
}

// ---- primitives ------------------------------------------------------------------------------------------

func (d *Bench) waitNav(id int) bool {
	timeout := time.After(30 * time.Second)
	for {
		select {
		case got := <-d.navCh:
			if got == id {
				return true
			}
		case <-timeout:
			d.errorf("tab %d: no NavigationCompleted", id)
			return false
		}
	}
}

func (d *Bench) mark(id int, t0 int64) float64 {
	d.markSeq++
	seq := d.markSeq
	t := d.br.find(id)
	if t == nil || t.win == nil {
		d.errorf("mark %d: tab %d has no window", seq, id)
		return -1
	}
	t.win.ExecJS(fmt.Sprintf("window.__benchMark(%d)", seq))
	timeout := time.After(15 * time.Second)
	for {
		select {
		case got := <-d.markCh:
			if got == seq {
				return d.ms(t0)
			}
		case <-timeout:
			d.errorf("mark %d timed out", seq)
			return -1
		}
	}
}

func (d *Bench) checkpoint(name string) {
	d.logf("checkpoint %s", name)
	d.cpSeq++
	d.write("checkpoint", fmt.Sprintf("%d %s", d.cpSeq, name))
	deadline := time.Now().Add(120 * time.Second)
	want := strconv.Itoa(d.cpSeq)
	for time.Now().Before(deadline) {
		if b, err := os.ReadFile(filepath.Join(d.dir, "ack")); err == nil && strings.TrimSpace(string(b)) == want {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	d.errorf("checkpoint %s: no ack from the harness", name)
}

// ---- scenario --------------------------------------------------------------------------------------------

func (d *Bench) run() {
	timeout := time.After(60 * time.Second)
	select {
	case <-d.painted:
	case <-timeout:
		d.errorf("shell never painted")
	}
	for {
		d.navMu.Lock()
		done := d.firstNav != 0
		d.navMu.Unlock()
		if done {
			break
		}
		time.Sleep(5 * time.Millisecond)
	}
	for len(d.navCh) > 0 {
		<-d.navCh
	}
	time.Sleep(500 * time.Millisecond)

	d.checkpoint("tabs_1")
	d.writeHwnds()
	d.checkpoint("window")
	d.runIPC()

	d.openUntil(5)
	d.checkpoint("tabs_5")
	d.result.SleptAt5 = d.sleepHidden()
	d.checkpoint("tabs_5_slept")
	d.wakeHidden()

	d.openUntil(10)
	d.checkpoint("tabs_10")
	d.result.SleptAt10 = d.sleepHidden()
	d.checkpoint("tabs_10_slept")
	d.wakeHidden()

	d.switchTest()
	d.recreateTest()

	out, err := json.Marshal(d.result)
	if err != nil {
		out = []byte(`{"errors":["cannot serialize the result"]}`)
	}
	d.write("result.json", string(out))
	d.write("done", "1")
}

func (d *Bench) writeHwnds() {
	var content uintptr
	if t := d.br.find(d.br.Active()); t != nil && t.win != nil {
		content = uintptr(t.win.NativeWindow())
	}
	d.write("hwnds", fmt.Sprintf("%d %d", d.br.mainHwnd(), content))
}

func (d *Bench) runIPC() {
	d.logf("ipc: start")
	defer d.logf("ipc: done")
	d.br.app.Event.Emit("bench:run-ipc")
	select {
	case report := <-d.ipcCh:
		if json.Valid([]byte(report)) {
			d.result.IPC = json.RawMessage(report)
		} else {
			d.errorf("IPC report is not valid JSON")
		}
	case <-time.After(180 * time.Second):
		d.errorf("IPC test timed out")
	}
}

func (d *Bench) openUntil(count int) {
	for len(d.br.TabIDs()) < count {
		t0 := qpcNow()
		t := d.br.OpenTab(d.nextTabURL(), true)
		if d.waitNav(t.ID) {
			d.result.NewTabNavMs = append(d.result.NewTabNavMs, d.ms(t0))
		}
		time.Sleep(100 * time.Millisecond)
	}
}

func (d *Bench) sleepHidden() int {
	active := d.br.Active()
	slept := 0
	for _, id := range d.br.TabIDs() {
		if id != active && d.br.Discard(id) {
			slept++
		}
	}
	time.Sleep(200 * time.Millisecond)
	return slept
}

// wakeHidden activates every hidden tab once (oldest first), timing activate -> two frames of that tab.
func (d *Bench) wakeHidden() {
	active := d.br.Active()
	ids := []int{}
	for _, id := range d.br.TabIDs() {
		if id != active {
			ids = append(ids, id)
		}
	}
	ids = append(ids, active)
	for i, id := range ids {
		t := d.br.find(id)
		wasDiscarded := t != nil && (t.discarded || t.win == nil)
		t0 := qpcNow()
		d.br.Activate(id)
		if wasDiscarded {
			d.waitNav(id)
		}
		ms := d.mark(id, t0)
		if i+1 < len(ids) {
			d.result.WakeMs = append(d.result.WakeMs, ms)
		}
		time.Sleep(100 * time.Millisecond)
	}
}

func (d *Bench) switchTest() {
	ids := d.br.TabIDs()
	if len(ids) < 2 {
		return
	}
	for i := 0; i < switches; i++ {
		target := ids[i%2]
		t0 := qpcNow()
		d.br.Activate(target)
		d.result.TabSwitchMs = append(d.result.TabSwitchMs, d.mark(target, t0))
		time.Sleep(100 * time.Millisecond)
	}
}

func (d *Bench) recreateTest() {
	for i := 0; i < recreates; i++ {
		active := d.br.Active()
		victim := 0
		for _, id := range d.br.TabIDs() {
			if id != active {
				victim = id
				break
			}
		}
		if victim == 0 {
			return
		}
		t0 := qpcNow()
		d.br.CloseTab(victim)
		t := d.br.OpenTab(d.nextTabURL(), true)
		if d.waitNav(t.ID) {
			d.result.RecreateNavMs = append(d.result.RecreateNavMs, d.ms(t0))
		}
		d.result.RecreateVisibleMs = append(d.result.RecreateVisibleMs, d.mark(t.ID, t0))
		time.Sleep(100 * time.Millisecond)
	}
}

// BenchService is bench_shell.js's and bench_tab.html's RPC surface (Bench.* in the Boson example).
type BenchService struct {
	app *application.App
	d   *Bench
}

func (s *BenchService) Painted() {
	if s.d != nil {
		s.d.onPainted()
	}
}
// Log records a frontend diagnostic line in app.log (benchmark mode only).
func (s *BenchService) Log(line string) {
	if s.d != nil {
		s.d.logf("js: %s", line)
	}
}
func (s *BenchService) Ping(seq int) int        { return seq }
func (s *BenchService) Echo(text string) string { return text }
func (s *BenchService) EmitBurst(n int) {
	if s.d != nil {
		s.d.logf("emit burst %d: start", n)
	}
	for i := 0; i < n; i++ {
		s.app.Event.Emit("bench:tick", i)
	}
	if s.d != nil {
		s.d.logf("emit burst %d: emitted", n)
	}
}
func (s *BenchService) Report(result string) {
	if s.d != nil {
		select {
		case s.d.ipcCh <- result:
		default:
		}
	}
}
func (s *BenchService) Mark(tab int, seq int) {
	if s.d != nil {
		s.d.onMark(seq)
	}
}
