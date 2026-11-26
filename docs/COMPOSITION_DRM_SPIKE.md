# Spike: Composition Hosting vs. Protected Content (issue #10)

## Status

Composition hosting (`HostingMode::Composition`) is implemented and covered by `boson_integration_tests`:

- DirectComposition visual tree;
- opacity and rounded clips;
- mouse, wheel and cursor forwarding;
- CSS app-region mapped to `WM_NCHITTEST`;
- in-place `Environment::recreate`.

`boson_browser --composition` shows a translucent settings modal blended over live page content.

**Not verified here: hardware-DRM (PlayReady SL3000 / Widevine L1) playback under composition hosting.** The development machine used for this spike had no test stream or licence server suitable for SL3000, so this document records the open questions and a test procedure rather than a result. **Windowed hosting stays the default.**

## Why it is a risk

- Hardware-protected video is decoded into protected surfaces and usually shown through a hardware overlay or MPO plane. With windowed hosting, Chromium owns the HWND and its own DirectComposition tree. With visual hosting, the WebView's root visual is grafted into *our* tree. Whether protected swap chains survive that path (and our clips, opacity and transforms) depends on the runtime and the driver.
- Public reports show black video with working audio for PlayReady hardware key systems on some Windows 11 24H2 machines, even in ordinary WebView2 (WebView2Feedback #4935). In those reports, HDCP, remote desktop and screen capture changed the outcome.
- In another browser engine, rounded-corner clips applied to a DRM video layer blacked out the video while audio kept playing (Mozilla bug 1896948). `set_corner_radius` and `set_opacity` are therefore suspects.

## Test procedure

Run each step on a physical machine with an HDCP 2.2 display, outside remote desktop.

1. Launch `boson_browser.exe` (windowed) and open a PlayReady SL3000 test stream and a Widevine test stream, for example the DRM page linked from the new-tab page. Record whether video, audio and resolution are correct. Open `edge://media-internals` in a tab and check the CDM and key system in use.
2. Repeat with `boson_browser.exe --composition`.
3. In composition mode, open the settings modal over the playing video. This is a translucent sibling visual above the protected content.
4. In composition mode, call `set_corner_radius(8)` and `set_opacity(0.95f)` on the content tab, then re-check playback.
5. Toggle **screenshare mode** (which disables `HardwareSecureDecryption` through an in-place `Environment::recreate`). Confirm the stream falls back to software DRM and can be screen-captured.
6. Repeat 1–5 on Windows 10 22H2 and Windows 11 24H2 or later, on one Intel iGPU and one discrete GPU.

Record each run as: OS build, GPU and driver, WebView2 runtime version, key system, security level, video OK / black / blocked, and audio OK.

## Decision until results exist

- Keep `HostingMode::Windowed` as the default for any app that plays protected media.
- Use composition hosting for app UI (overlapping panels, translucent overlays, animated transitions). Do not use it for third-party media pages.
- Offer the screenshare switch (`disable_features = {L"HardwareSecureDecryption"}` through `Environment::recreate` or `app::relaunch`) when users need to capture protected content.
