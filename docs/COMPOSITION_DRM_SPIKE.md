# Spike: Composition Hosting vs. Protected Content (issues #10, #15)

## Status

Composition hosting (`HostingMode::Composition`) is implemented and covered by `boson_integration_tests`:

- DirectComposition visual tree;
- opacity and rounded clips;
- mouse, wheel and cursor forwarding;
- CSS app-region mapped to `WM_NCHITTEST`;
- in-place `Environment::recreate`.

Issue #15 ran the protected-content procedure on one machine with a discrete GPU. The results there:

- **PlayReady SL3000 (hardware DRM) plays in windowed and in composition hosting.** It also plays:
  - after hide/show and after suspend/resume;
  - under a translucent sibling WebView;
  - with rounded corners and opacity;
  - in element fullscreen over the whole monitor.
- **No capture method tried could capture it.** Two protected sessions at once mostly do not work.
- **Background playback:** SL3000 video is **paused** whenever the controller is hidden. That happens when the window
  is minimized (`WebViewHost` hides controllers) and when `set_visible(false)` is called. It keeps playing when the
  window is only covered or unfocused. `WebView::set_keep_visible_when_minimized(true)` (added for this issue) keeps
  it playing while minimized.
- **Screen sharing (Discord, Teams, OBS):** PlayReady video is black in every capture API. Widevine video is black in
  everything except WebView2 `CapturePreview`. With `--disable-direct-composition`:
  - PlayReady becomes unavailable, so sites fall back to Widevine;
  - Widevine video is fully visible to DXGI duplication, `PrintWindow` and Windows.Graphics.Capture (window and
    monitor);
  - GPU decoding stays on.

**No iGPU machine and no Windows 10 machine was tested.** Windowed hosting therefore stays the default for apps that
play protected media.

## Test rig

| | |
|---|---|
| OS | Windows 11 Pro, build 26300.9974 |
| GPU (dGPU row) | AMD Radeon RX 9070 XT, driver 32.0.32015.7029, one display, local console (not remote desktop) |
| GPU (iGPU row) | **not tested**: no iGPU machine was available |
| WebView2 | runtime 158.0.4370.33, SDK 1.0.4370.26 |
| Player | Shaka Player 4.16.58 (jsDelivr), on a `boson://` page served from memory |
| SL3000 stream | Axinom `v7-MultiDRM-SingleKey` DASH (cenc, 1080p avc1), Axinom PlayReady licence server, key system `com.microsoft.playready.recommendation.3000`, robustness `3000` |
| SL2000 stream | Microsoft PlayReady test server, Tears of Steel 4K DASH, licence `sl:150` (or `sl:3000` for the negative case), key system `com.microsoft.playready.recommendation`, robustness `2000` |
| Widevine stream | Axinom `v7-MultiDRM-SingleKey`, Axinom Widevine licence, `SW_SECURE_DECODE` |

The tool is `tools/drm_spike/main.cpp` (`boson_drm_spike` target). It is a manual tool: it needs the network, a GPU and
a display, so it is not registered with CTest. Run it like this:

```
boson_drm_spike.exe --out <dir> [--seconds 6] [--rounds 2] [--modes windowed,composition]
                    [--skip-matrix] [--skip-user] [--skip-flags] [--user fullscreen,minimized,hidden,covered,SL3000,Widevine]
                    [--fs-debug]
```

It writes `results.md`, `results.json` and a PNG for each capture. For each case it:

- creates a fresh Environment with its own user data folder (`%LOCALAPPDATA%\BosonDrmSpike-<pid>`, deleted afterwards);
- clears the HTTP disk cache, then navigates freshly to the player;
- waits for playback, then measures for N seconds:
  - `currentTime` advance;
  - `getVideoPlaybackQuality()` decoded and dropped frames;
  - `requestVideoFrameCallback` (rVFC) presented frames;
  - licence requests and responses;
  - audio (`webkitAudioDecodedByteCount` and `IsDocumentPlayingAudio`);
- measures how much of the centre of the video is non-black in five captures:
  - DXGI Desktop Duplication, which is what screen recorders see;
  - `PrintWindow(PW_RENDERFULLCONTENT)`;
  - WebView2 `CapturePreview`;
  - Windows.Graphics.Capture (WGC) of the window, which is Discord's and Teams' "share a window";
  - WGC of the monitor, which is "share your screen".

The window is placed at the top-left of the work area and never activated. During the screen captures it is briefly
made topmost, also without activation. DXGI and WGC-monitor values are not counted when another window still covered
one of 9 sample points; the tool names that window. WGC capture was added after the final matrix run below, so its
columns come from the later runs (sections below and `--rounds 1` of the whole matrix, which agreed).

"Plays" means `currentTime` advanced at least 2 s during the measurement window, the licence was granted, and there
was no player error. With SL3000 the frame counters are unusual: the hardware path reports few decoded frames, many
"dropped" frames and almost no rVFC callbacks, although playback advances in real time. The renderer runs out of process (Media
Foundation), so these counters do not mean much on that path.

## Results (dGPU row: AMD RX 9070 XT, Windows 11 26300, WebView2 158.0.4370.33)

From three rounds of the whole matrix (one `--rounds 2` run, then one `--rounds 1` run with WGC), both hosting
modes, `--seconds 6`. Each cell shows the rounds in which the case played. Captures show the largest non-black share
of the video centre over all rounds (DXGI / PrintWindow / CapturePreview). In the WGC run, both WGC captures were
0 % for every PlayReady case.

| # | Case | Windowed | Composition | Captures |
|---|---|---|---|---|
| 1 | Baseline: SL3000, 1920×1080 | plays 3/3 | plays 3/3 | 0 / 0 / 0 % (black) |
| 2 | Hidden 3 s (`set_visible(false)`), then shown | plays 2/3 ¹ | plays 3/3 | 0 / 0 / 0 % |
| 3 | Hidden + `TrySuspend` (S_OK, `IsSuspended` true), then `Resume` + shown | plays 3/3 | plays 3/3 | 0 / 0 / 0 % |
| 4a | Two SL3000 tabs side by side: first tab | plays 3/3 | plays 3/3 | 0 / 0 / 0 % |
| 4b | Two SL3000 tabs side by side: second tab | **fails 0/3** ² | **1/3** ² | – |
| 5 | Translucent sibling WebView over the lower part of the video | plays 3/3 | plays 3/3 | video 0 / 0 / 0 %; overlay itself 100 % visible |
| 6 | `set_corner_radius(8)` + `set_opacity(0.95)` | n/a | plays 3/3 | 0 / 0 / 0 % |
| 7 | SL2000 key system, `sl:150` licence | plays 3/3 | plays 3/3 | 0 / 0 / 0 % (black even in CapturePreview) |
| 8 | SL2000 key system, `sl:3000` licence | refused 0/3 (HTTP 500) ³ | refused 0/3 ³ | – |
| 1b | SL3000 key system, Microsoft `sl:3000` licence (Tears of Steel) | fails 0/3 ⁴ | fails 0/3 ⁴ | – |
| 9a | `Environment::recreate` with `disable_features={HardwareSecureDecryption}`: SL3000 | **still plays 3/3** | **still plays 2/3** ⁵ | 0 / 0 / 0 % |
| 9b | recreate with `enable_features={msDisablePlayReadyHardwareDrm}`: SL3000 | unavailable 0/3 (Shaka 6012) | unavailable 0/3 | – |
| 9c | the same, SL2000 `sl:150` | plays 3/3 | plays 3/3 | 0 / 0 / 0 % |
| 10 | recreate back to default flags: SL3000 | plays 3/3 | plays 3/3 | 0 / 0 / 0 % |
| 11 | Widevine (software CDM) | plays 3/3 | – | 0 / 0 / **100 %** |
| 12 | `--disable-gpu`: SL3000 / SL2000 / Widevine | – / – / plays 2/2 | – | Widevine **100 / 100 / 100 %** |
| | **iGPU machine** | **not tested** | **not tested** | |
| | **Windows 10** | **not tested** | **not tested** | |

1. "Plays" here means playback after the WebView was shown again. While it is hidden, SL3000 video is paused (case
   16 below), and it resumes on show. In one windowed round the player was in Shaka's error state when it was checked
   while hidden. The player error text was not captured in that round.
2. The second concurrent SL3000 session gets its licence (1/1). Then it fails with Shaka 3014 (a media decode error)
   on its first segments, or, once in composition mode, starts late. The first tab keeps playing. In Edge 158, with
   the same page in two tabs, the foreground tab played and the background tab stalled. So one active hardware-DRM
   session at a time looks like a platform or driver limit on this machine, not a hosting-mode problem.
3. Expected. The Microsoft server refuses an SL3000 licence to the SL2000 (software) client. This proves the
   `recommendation` key system is software.
4. The Microsoft server grants the `sl:3000` licence to the SL3000 key system, which proves that the client is
   hardware-backed. Playback then fails with 3014 on the first audio fragment. Edge 158 fails the same way with this
   stream, so this is content-specific. The Axinom stream is the SL3000 evidence.
5. In the third round the player did not reach playback after the recreate (no Shaka error recorded). SL3000 playback
   failed now and then without an error in other cases too (about 1 in 15 SL3000 starts overall). Treat single
   failures as noise and the 0/3 rows as real.

## Fullscreen, background playback and screen sharing

These cases model a streaming app such as Disney+: watch in fullscreen, leave the app running in the background, share
it in Discord. They used the Axinom SL3000 and Widevine streams and `--seconds 6`. The data comes from 4 runs of these
cases (one with the whole matrix) plus 3 fullscreen-only runs. Counts are per run.

| # | Case | SL3000 windowed | SL3000 composition | Widevine (both modes) |
|---|---|---|---|---|
| 13 | Element fullscreen: a click handler calls `video.requestFullscreen()`; `ContainsFullScreenElementChanged` → `Window::set_fullscreen(true)`. The window then covered the whole 2560×1440 monitor, with `fullscreenElement` VIDEO. | plays 2/3 | plays 3/4 | plays 4/4 |
| 13 | after `exitFullscreen()` | plays 2/3 | plays 3/4 | plays 4/4 |
| 14 | Window minimized for 8 s (`WebViewHost` sets `IsVisible = false`) | **stops 4/4** (paused in 3, stalled after 1.5 s in 1), resumes after restore | **paused 3/3**, resumes after restore | keeps playing 7/7, audio decoding |
| 14b | The same with `set_keep_visible_when_minimized(true)` | **keeps playing 3/3** (7.7–8.0 s of 8 s) | **keeps playing 2/2** | keeps playing |
| 16 | WebView hidden for 8 s with `set_visible(false)` (another tab shown) | **paused 3/3**, resumes after show | **paused 2/2** | keeps playing |
| 15 | Window fully covered by another opaque window for 8 s | keeps playing 3/3 | keeps playing 3/3 | keeps playing 6/6 |

Notes:

- **Fullscreen** is granted only when the page has focus (`document.hasFocus()`). A real click gives it focus. A
  click delivered through DevTools does not, and the runtime then rejects with "not granted"; the tool then focuses the
  WebView (this steals focus once) and retries. Apps need nothing special beyond handling `on_fullscreen_changed`, as
  `boson_browser` does. In fullscreen, PlayReady was black in all five captures and Widevine was black in all but
  `CapturePreview`, the same as windowed. The SL3000 failures had no player error. They look like the occasional
  SL3000 start failure described in note 5 above, not a fullscreen problem; windowed fullscreen played in the other
  2 runs.
- **"Paused"** means `HTMLMediaElement.paused` became true and `currentTime` did not advance. The pause comes from the
  runtime, not the page. Widevine on the same page keeps playing, with audio decoding and `IsDocumentPlayingAudio`
  true. The runtime never sees PlayReady audio (see Audio below), so it probably treats SL3000 as video-only and applies
  the background pause. The audio stops with it. That is the "running in the background loses playback" failure, and
  `set_keep_visible_when_minimized(true)` fixes it for the minimized case. `boson_browser` sets it on every tab. Hidden
  tabs stay hidden, because the flag only overrides the minimized state.
- **Covered / unfocused:** playback continued. All cases ran without the window ever being in the foreground, apart from
  the fullscreen retry.
- **Discord-style sharing while in the background:** a WGC window capture of the covered window (what Discord gets
  when you share a window that is behind others) showed the page but 0 % of the video, for both key systems.

### Screen sharing (Discord, Teams, OBS)

Discord and Teams share through Windows.Graphics.Capture, and OBS through WGC or DXGI duplication. Measured on this
machine (visible % of the video in DXGI / PrintWindow / CapturePreview / WGC window / WGC monitor):

| Configuration | What a site gets | PlayReady SL3000 / SL2000 | Widevine |
|---|---|---|---|
| default | PlayReady SL3000 | 0 / 0 / 0 / 0 / 0 | 0 / 0 / 100 / 0 / 0 |
| `enable_features={msDisablePlayReadyHardwareDrm}` | PlayReady SL2000 | SL2000: 0 / 0 / 0 / 0 / 0 | 0 / 0 / 100 / 0 / 0 |
| `disable_features={msPlayReadyWin10}` | Widevine L3 | unavailable | 0 / 0 / 100 / 0 / 0 |
| `disable_features={msPlayReadyWin10, DirectCompositionSoftwareOverlays}` | Widevine L3 | unavailable | 0 / 0 / 100 / 0 / 0 |
| **`--disable-direct-composition`** (windowed and composition hosting) | Widevine L3 | unavailable | **100 / 100 / 100 / 100 / 100** |
| `--disable-gpu` | Widevine L3 | unavailable | 100 / 100 / 100 / 100 / 100 |

So PlayReady output is never shareable. Widevine is drawn into a capture-excluded DirectComposition overlay unless
DirectComposition is off. `--disable-direct-composition` is therefore the screen-share mode:

- PlayReady becomes unavailable, so a multi-DRM site falls back to Widevine;
- Widevine video then reaches every capture API, while GPU decoding stays on. Clear Key still offered hev1, which
  `--disable-gpu` removes.

It worked in both hosting modes; composition hosting was checked with one Widevine run. `boson_browser --screenshare`
now applies it through `Environment::recreate`. Whether a given streaming service accepts Widevine L3 or caps its
resolution is up to the service; that was not tested.

### What "black in all captures" means

Protected frames never reached any capture path:

- DXGI Desktop Duplication, which is what OBS, Teams and Snipping Tool use;
- `PrintWindow`;
- WebView2 `CapturePreview`.

The overlay WebView, the page chrome and the Widevine software path were captured normally. So black frames here
mean hardware protection is working, not a rendering failure. Because of this, the agent that ran the spike could not
check by eye that SL3000 frames appear on the physical screen. The evidence for playback is `currentTime` advancing
in real time at 1920×1080, granted licences, `HDCP 2.2` "usable", and no media error.

Software PlayReady (SL2000) is also black in `CapturePreview`, because it uses the same Media Foundation renderer.
Widevine L3 is black in DXGI and `PrintWindow`, but fully visible in `CapturePreview`. With `--disable-gpu`, Widevine
is visible in all three.

### Audio

Widevine audio decodes and `IsDocumentPlayingAudio` is true. For PlayReady (SL2000 and SL3000), neither
`webkitAudioDecodedByteCount` nor `IsDocumentPlayingAudio` ever reports audio, because the Media Foundation renderer
plays audio outside the renderer process. **PlayReady audio output is therefore unverified.**

## Capability probe: `webview::probe_drm`

`boson::webview::probe_drm(view, cb)`, and from JS `webview.probeDrm()`, run EME
`requestMediaKeySystemAccess` + `createMediaKeys` + `getStatusForPolicy` for PlayReady SL3000/SL2000, Widevine
`HW_SECURE_ALL`/`SW_SECURE_DECODE` and Clear Key, with avc1/hev1/vp09/av01 and cenc/cbcs. See
[WEBVIEW_FEATURES.md](WEBVIEW_FEATURES.md#drm-capability-probe-bosonwebviewdrmhpp). On this machine, with default flags,
it returned `protection = hardware`, `best = playready_sl3000`:

| Query | Supported | Video codecs | Schemes | createMediaKeys | HDCP 2.2 |
|---|---|---|---|---|---|
| `playready_sl3000` | yes | avc1, hev1, av01 (+hvc1) | cenc | ok | usable |
| `playready_sl2000` | yes | avc1, hev1, vp09, av01 (+dvh1, hvc1) | cenc | ok | usable |
| `widevine_l1` (`HW_SECURE_ALL`) | **no** | – | – | – | – |
| `widevine_l3` (`SW_SECURE_DECODE`) | yes | avc1, vp09, av01 | cenc, cbcs | ok | usable |
| `clearkey` | yes | avc1, hev1, vp09, av01 (+hvc1) | cenc, cbcs | ok | usable |

The codecs in parentheses (dvh1, hvc1) were only probed by the flags matrix below. Widevine L1 does not exist in
WebView2/Edge on Windows; hardware DRM there is PlayReady.

## EnvironmentConfig flags that matter

Each line is a probe after a fresh Environment with only that flag (`enable_features`, `disable_features` or
`additional_browser_arguments`). Feature names were checked against the strings in the 158 runtime's `msedge.dll`.

| Flag | Present in runtime 158? | Effect measured here | Why |
|---|---|---|---|
| `disable_features = {L"HardwareSecureDecryption"}` | yes (Chromium `kHardwareSecureDecryption`) | **None on PlayReady.** SL3000 is still probed *and still plays* (case 9a). | The Chromium feature gates Chromium's own hardware-secure path (MediaFoundation CDM for Widevine/Chromium key systems). Edge's PlayReady integration has its own switches. **It is not a working "screenshare mode" on Edge/WebView2.** |
| `enable_features = {L"msDisablePlayReadyHardwareDrm"}` | yes (Edge) | Removes `playready_sl3000`; SL2000, Widevine L3 and Clear Key remain. SL3000 streams fail with "no supported key system" (6012), SL2000 plays (9b/9c). | Edge's switch for turning off PlayReady hardware DRM. It does **not** help screen sharing: SL2000 output is not capturable either (case 7). |
| `disable_features = {L"msPlayReadyWin10"}` | yes (Edge) | Removes **all** PlayReady (SL3000 and SL2000); Widevine L3 becomes the best option. | Edge's switch for the Windows 10+ PlayReady CDM. |
| `enable_features = {L"msPlayReadyCbcsEncryptionScheme"}` | yes (Edge) | Adds `cbcs` to both PlayReady queries (default: cenc only). | Needed for cbcs (CMAF/HLS-style) PlayReady content. |
| `disable_features = {L"PlatformEncryptedDolbyVision"}` | yes (Chromium `kPlatformEncryptedDolbyVision`) | Removes `dvh1` from `playready_sl2000`. Enabling it explicitly changes nothing (on by default). `dvh1` was never offered on SL3000 on this machine. | Gates encrypted Dolby Vision through the platform (MF) decoder. |
| `HardwareSecureDecryptionExperiment` (enable or disable) | **no** (name not in `msedge.dll`) | None | An unknown feature name is ignored. |
| `disable_features = {L"EdgePlayReadyDrmWin10"}` | **no** | None | The same: the Edge name is `msPlayReadyWin10`. |
| `--disable-direct-composition` (browser argument) | yes (switch string present) | Removes all PlayReady; Widevine L3 and Clear Key remain, including hev1/hvc1. Widevine plays and is **100 % capturable by all five methods**, in windowed and composition hosting. | Without DirectComposition, Chromium cannot use its protected or capture-excluded overlay swap chains, so video is composited into the normal swap chain, and PlayReady (which needs the MF/DComp path) is withdrawn. **This is the screen-share mode.** |
| `disable_features = {L"DirectCompositionSoftwareOverlays"}` | yes (Chromium) | No effect on Widevine capture (still 0 % in DXGI/WGC). | Not the overlay that excludes video from capture. |
| `--disable-gpu` (browser argument) | – | Removes all PlayReady, and removes hev1/hvc1 from Clear Key (no hardware HEVC decoder). Widevine L3 plays and is **100 % capturable by all three methods** (case 12). With `msDisablePlayReadyHardwareDrm` added, the result is the same. | Without the GPU process there is no MF/DXVA protected path. Widevine video is capturable here too, but at the cost of all GPU acceleration. |

Other Edge features present in the 158 runtime, which were not exercised: `msPlayReadyHardwareDrmWorkaround`,
`msPlayReadyDcompWorkaround`, `msDecodeDolbyVisionUsingMF`, and Chromium `HardwareSecureDecryptionAv1`/`Vp9`/
`Fallback`/`RequireServerCert`.

## Caveats found during the spike

- **HTTP disk cache:** SL3000 playback from cached segments failed with Shaka 3014. This reproduced in Edge 158 too.
  The tool clears the disk cache before every case, and with that every repeat passed. Apps that replay protected
  segments from the HTTP cache may hit this. It looks like a runtime or CDM issue; it is not specific to Boson.
- **Concurrent hardware sessions:** only one SL3000 playback at a time worked (case 4b). Apps with several media
  tabs should pause background protected players.
- **Profiles under `%TEMP%`:** a standalone Edge profile under `%TEMP%` failed with
  `MSPR_E_UNABLE_TO_OPEN_DATA_STORE` (0x8004B82E). Profiles under `%LOCALAPPDATA%` work.
- **Autoplay:** `--autoplay-policy=no-user-gesture-required` was set for every environment. Unmuted `play()` was
  still sometimes rejected, so the player falls back to muted playback and unmutes once playback starts.

## Decision

- Keep `HostingMode::Windowed` as the default for apps that play protected media. On this dGPU machine, composition
  hosting behaved the same as windowed hosting for SL3000, including clipping, opacity and an overlapping
  translucent WebView. iGPU and Windows 10 behaviour is still unknown (Intel MPO paths are the classic failure
  case).
- Use composition hosting freely for app UI. Before choosing it for third-party media pages, call
  `probe_drm` and test on the target hardware.
- **Background playback:** call `set_keep_visible_when_minimized(true)` on WebViews that play media. Without it,
  minimizing pauses PlayReady video and its audio. Do not hide a playing WebView with `set_visible(false)`; SL3000
  pauses there too.
- **Fullscreen:** handle `events().on_fullscreen_changed` with `Window::set_fullscreen`, as `boson_browser` does.
  Hardware DRM keeps playing over the whole monitor in both hosting modes.
- **Screenshare mode:** add `--disable-direct-composition` (`additional_browser_arguments`, applied with
  `Environment::recreate` or `app::relaunch`).
  - `disable_features = {L"HardwareSecureDecryption"}` does nothing for PlayReady in WebView2.
  - `enable_features = {L"msDisablePlayReadyHardwareDrm"}` drops SL3000, but SL2000 is not capturable either.
  - `--disable-gpu` also works, but it costs all GPU acceleration.
  - `boson_browser --screenshare` now uses `--disable-direct-composition`.

## Verification

Everything below was measured on the dGPU machine described above (local console, one display) on 2026-11-26.

- The tables above come from these `boson_drm_spike` runs:
  - `--rounds 2 --seconds 6`;
  - `--rounds 1 --seconds 6`, which included the user cases, WGC and the flags;
  - two user-case runs (`--skip-matrix`);
  - three fullscreen-only runs (`--user fullscreen,SL3000`);
  - one composition run with `--disable-direct-composition` (custom Widevine stream).

  Earlier full runs agreed except as noted. They also found the disk-cache problem and the dependence on state left
  by earlier cases, which is why every case now starts from a fresh navigation and a cleared cache.
- WGC capture was checked against itself. The window and monitor captures show the page text and other windows
  correctly, and reach 100 % on capturable video. So 0 % for the video is real exclusion, not a capture bug.
- `boson_integration_tests` has a new step:
  - minimizing hides the controller and the page reports `visibilityState` "hidden";
  - `set_keep_visible_when_minimized(true)` makes it visible again while minimized;
  - restoring keeps it visible.

  It passed 120 of 120 checks.
- In one earlier 3-round run, another program closed the spike's window part-way through, and the run crashed. The
  tool now stops cleanly in that case and writes the partial `results.md` after every case.
- `boson_webview_features_tests` runs `probe_drm` against the real runtime on `boson://app`:
  - the default probe (5 queries, Clear Key supported with keys created, avc1/cenc);
  - a custom probe (Clear Key plus an unknown key system → `clearkey_only`, `NotSupportedError`);
  - the sender overload;
  - `WebView.probe_drm` through Router and Bridge, including an invalid-argument error and replacing (not merging)
    a list.
- Edge 158 (standalone) was used as the reference for the disk-cache, concurrent-session and Microsoft `sl:3000`
  findings.

**Not verified:**

- an iGPU machine;
- Windows 10;
- multiple monitors, MPO on/off or HDR;
- remote desktop;
- a non-HDCP display;
- visual confirmation that SL3000 frames are on the panel, in a window or in fullscreen (captures are black by
  design, and no human checked the screen during the run);
- a real Discord, Teams or OBS session: the capture APIs they use were exercised directly, but not the apps;
- a real streaming service (Disney+, Netflix and so on): which DRM they choose, and whether they accept Widevine L3
  under `--disable-direct-composition`;
- background playback of SL2000 (only SL3000 and Widevine were tested), and minimized playback longer than 8 s;
- PlayReady audio output;
- `boson_browser --composition` with a real streaming site.
