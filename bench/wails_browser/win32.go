package main

// Thin user32/kernel32 helpers: owned tab windows, physical-pixel placement and QueryPerformanceCounter stamps
// (the same clock boson_bench and Boson use).

import (
	"unsafe"

	"golang.org/x/sys/windows"
)

var (
	user32                = windows.NewLazySystemDLL("user32.dll")
	kernel32              = windows.NewLazySystemDLL("kernel32.dll")
	procSetWindowLongPtrW = user32.NewProc("SetWindowLongPtrW")
	procSetWindowPos      = user32.NewProc("SetWindowPos")
	procGetClientRect     = user32.NewProc("GetClientRect")
	procClientToScreen    = user32.NewProc("ClientToScreen")
	procGetDpiForWindow   = user32.NewProc("GetDpiForWindow")
	procQPC               = kernel32.NewProc("QueryPerformanceCounter")
)

type rect struct{ Left, Top, Right, Bottom int32 }
type point struct{ X, Y int32 }

const (
	swpNoZOrder      = 0x0004
	swpNoActivate    = 0x0010
	swpNoOwnerZOrder = 0x0200
	headerDIP        = 40
)

// gwlpHwndParent is GWLP_HWNDPARENT (-8): for a top-level window it sets the owner.
var gwlpHwndParent = ^uintptr(7)

func qpcNow() int64 {
	var v int64
	procQPC.Call(uintptr(unsafe.Pointer(&v)))
	return v
}

// setOwner makes hwnd an owned window of owner: it stays above it, minimises with it and has no taskbar button.
func setOwner(hwnd, owner uintptr) {
	procSetWindowLongPtrW.Call(hwnd, gwlpHwndParent, owner)
}

// contentRect is the main window's client area below the 40-DIP header, in physical screen pixels.
func contentRect(main uintptr) (x, y, w, h int32) {
	var rc rect
	procGetClientRect.Call(main, uintptr(unsafe.Pointer(&rc)))
	var origin point
	procClientToScreen.Call(main, uintptr(unsafe.Pointer(&origin)))
	dpi, _, _ := procGetDpiForWindow.Call(main)
	if dpi == 0 {
		dpi = 96
	}
	header := int32((headerDIP*int64(dpi) + 48) / 96)
	return origin.X, origin.Y + header, rc.Right - rc.Left, rc.Bottom - rc.Top - header
}

func placeWindow(hwnd uintptr, x, y, w, h int32) {
	procSetWindowPos.Call(hwnd, 0, uintptr(x), uintptr(y), uintptr(w), uintptr(h), swpNoZOrder|swpNoActivate|swpNoOwnerZOrder)
}
