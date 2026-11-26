# Performance

Findings of the optimisation pass, one self-contained section per area. Measurements are on Windows 11 Pro 10.0.26300,
Intel Core i7-13700KF (24 logical CPUs), MSVC 19.52 (VS 18), WebView2 runtime 158.0.4370.33. Other agents built and ran
UI tests on the same machine the whole time, so wall-clock numbers carry heavy noise; sizes (bytes), section/object
contributions and process counts are exact and are the primary evidence in this section.

## Build configuration, binary size and codegen

### Result in one paragraph

Boson executables were already small (the linker drops every unreferenced module of `boson_core`). The one codegen
switch that wins clearly is **RTTI off in Release (`/GR-`, now the default)**: -11 to -17 % executable size, no
behavioural change (nothing uses `dynamic_cast`/`typeid`; 22/22 ctest suites pass in that configuration).
`/Gw`, `/Zc:inline`, `/Gy`, `/arch:AVX2` and PGO produced **no measurable wall-clock, CPU or memory difference** in
the Boson host process, so they are not defaults (PGO and AVX2 stay as opt-ins; PGO does shrink the file by 8-10 %).
A one-file **single executable** build (`-DBOSON_SINGLE_EXE=ON`) imports only Windows system DLLs: 618 KB for a hello
app, 1.46 MB for the demo.

### Baseline (before any change)

Release + LTO, `/MD`, `/O2 /Ob3`, `/OPT:REF,ICF`, `/DEBUG:NONE` (already in the tree). Sizes in bytes:

| Exe | Baseline |
|---|---:|
| `boson_demo.exe` | 1,370,624 |
| `boson_browser.exe` | 1,290,752 |
| `boson_bench_app.exe` | 765,952 |
| `boson_hello.exe` (new minimal app, `examples/hello`) | 485,376 |

### Where the bytes go (`boson_demo.exe`, `link /MAP`, LTCG)

| Section | Size |
|---|---:|
| .text | 900 KB (0xDC000) |
| .rdata | 324 KB (0x51000) |
| .data | 104 KB (0x1A000; 83 KB of it is `.data$rs`, glaze/zmij/fast_float tables) |
| .pdata + .xdata (unwind) | 30 KB + 62 KB |
| .reloc + .rsrc | 8 KB + 4 KB |

Largest contributors by object (publics, size to the next symbol): `main.cpp.obj` 578 KB (63 % of .text: the demo's
controllers, `Router` dispatch lambdas, glaze reader/writer instantiations and the two embedded assets), then
`boson_core` objects, largest first: `webview_events` 79 KB, `updater` 74 KB, `environment` 62 KB, `window_state` 59 KB,
`crash_reporter` 40 KB, `store` 39 KB; the static WebView2 loader is 23 KB; `msvcprt` (iostream/locale/filesystem pieces)
is about 26 KB. Single largest data objects: glaze `itoa_40kb` digit table 40 KB, embedded demo JS 17.6 KB, fast_float
powers 10.4 KB, zmij constants 10 KB + 5 KB. Nothing is individually worth removing: the demo's 63 % is the price of
reflecting 20+ controllers, and per-controller glaze instantiations are not duplicated (each controller's `read`/`write`
specialisations exist once; ICF merges the identical thunks).

### Modules the app does not use are dropped

`boson_core` is one static library, but the linker only pulls the objects an executable references, and
`/OPT:REF,ICF` removes the rest. Measured: `boson_hello` (window + WebView only) is 402 KB against 1.21 MB for
`boson_demo`, while both link the same library. No split into per-module libraries was needed (and none was done):
there is nothing left to drop. `boson_gpu` remains a separate library because it adds d3d/dwrite imports.

### Experiments (one change at a time, Release + LTO, `/MD`, bytes)

| Configuration | hello | demo | browser | bench_app | Verdict |
|---|---:|---:|---:|---:|---|
| baseline | 485,376 | 1,370,624 | 1,290,752 | 765,952 | |
| + `/Gw /Zc:inline` | 485,376 | 1,370,624 | 1,290,752 | n/a | byte-identical size: under `/GL` + `/OPT:REF` they change nothing. Not set. (`/Gy` is implied by `/O2`.) |
| + `/GR-` (RTTI off) | 402,432 | 1,209,856 | 1,119,232 | 648,192 | **-17.1 % / -11.7 % / -13.3 % / -15.4 %**. Default now. |
| + `/Os` instead of `/O2` | 413,696 | 1,125,376 | 1,115,648 | 662,528 | demo -17.9 % but slower code is likely; not default (`-DBOSON_RELEASE_CODEGEN_FLAGS=-Os`) |
| + `/arch:AVX2` | 486,912 | 1,370,112 | 1,290,752 | 763,904 | no size change, no measurable speed change; opt-in `-DBOSON_AVX2=ON` (default stays baseline x64) |
| `/GR-` + PGO (`bench/pgo.ps1`) | n/a | n/a | 1,003,008 | 594,432 | -10.4 % / -8.3 % vs `/GR-` alone, speed not measurably different. Opt-in `-DBOSON_PGO=GEN/USE` |

`/favor:` was not evaluated: it only retunes instruction selection for a specific micro-architecture and the AVX2 and PGO
runs already showed no measurable difference in the host process. clang-cl was not tried (not installed with this
VS component set; optional in the task).

### Startup, IPC and host cost: A/B, 6 interleaved runs each

`boson_bench --exe <app> --wait-ready --load-signal` on `boson_bench_app`, medians with [min-max]. Host-process numbers are
the app process itself (not WebView2) after the idle phase.

| Variant | First window (ms) | Ready / first frame (ms) | Sequential IPC mean (ms) | Host CPU after startup+idle (ms) | Host private WS (MB) | Host commit (MB) |
|---|---:|---:|---:|---:|---:|---:|
| baseline (`/MD`, RTTI) | 42.6 [23-85] | 439 [406-2800] | 0.18 [0.07-0.37] | 62.5 [15.6-78.1] | 3.2 | 4.5 |
| `/GR-` (new default) | 39.4 [26-46] | 446 [391-568] | 0.12 [0.09-0.15] | 46.9 [31.3-62.5] | 3.2 | 4.5 |
| `/GR-` + PGO | 50.9 [30-58] | 495 [358-539] | 0.21 [0.07-0.34] | 78.1 [31-172] | 3.1 | 4.4 |
| `/Os` | 60.0 [34-101] | 560 [404-933] | 0.17 [0.12-0.27] | 78.1 [47-125] | 3.1 | 4.5 |
| `/arch:AVX2` | 45.4 [33-63] | 541 [413-956] | 0.28 [0.12-0.38] | 78.1 [31-94] | 3.1 | 4.5 |

Reading: every spread overlaps. The time to a first web frame (~440 ms) and the IPC round trip (~0.1-0.2 ms) are dominated
by WebView2's browser/renderer processes and the page-to-host marshalling, not by Boson's code generation; CPU time is
quantised to the 15.6 ms scheduler tick. Handles (248) and process count did not change. Conclusion: none of these switches
earns a place in the default configuration on speed; only size (RTTI) does.

### Not done, with the reason

- **Asset compression (brotli/gzip `Content-Encoding`)**: the embedded frontend of the demo is 25.7 KB in total (17.6 KB JS,
  8.1 KB CSS); gzip would save roughly 18 KB of a 1.2 MB executable (1.5 %) and add an inflate call on every request, and
  whether a `WebResourceResponse` with `Content-Encoding` is decoded by WebView2 was not verified. Assets come from memory, so
  there is no network to save. Not worth it.
- **Embedded injected scripts**: nothing is injected by default (`Bridge::attach` adds no document-created script).
  Opt-in scripts: `BinaryTransport` page runtime 6.2 KB, `FileDrop` 0.9 KB, guest runtime 1.4 KB, sync-IPC boot values
  (tens of bytes). Leading indentation is 11 % of the largest; a constexpr minifier would save about 0.9 KB of JS source per
  navigation and about 1.7 KB of `.rdata` (the literals are `wchar_t`). Below noise, so not added.
- **Frontend bundles**: already separate entry points. Minified (bun, no tree-shake context): `boson.ts` core 2,988 B,
  `compat/wails` 23.4 KB, `compat/electron` 25.8 KB, `compat/tauri` 13.4 KB, `webview_element` 19.2 KB, `terminal` 9.6 KB,
  `process` 9.4 KB, `native` 7.3 KB, `windows` 6.6 KB. No module imports a compat shim; an app that imports only
  `boson.ts` ships ~3 KB (the demo, which uses the core plus its own UI, is 17.6 KB JS + 8.1 KB CSS).
- **iostream/locale**: only 10 library sources include `<fstream>/<sstream>/<format>`; the whole `msvcprt` contribution is
  about 26 KB (2 % of the demo), so replacing them was not attempted.
- **Static CRT size**: see "Single executable" below (+17 % for hello with `/GR-`, +21 % for demo).

### Recommended release configuration

```bat
cmake -S . -B build\release -G Ninja -DCMAKE_BUILD_TYPE=Release            :: LTO (/GL, /LTCG), /O2 /Ob3, /GR-, /OPT:REF,ICF
cmake -S . -B build\single  -G Ninja -DCMAKE_BUILD_TYPE=Release -DBOSON_SINGLE_EXE=ON   :: + static CRT, import check
bench\pgo.ps1 -BuildDir build\pgo                                             :: optional, -8..-10 % size, speed unchanged
```

Defaults for Release now: LTO, `/GR-` (`-DBOSON_NO_RTTI=OFF` if your own code uses `dynamic_cast`/`typeid`), `/O2 /Ob3`,
`/OPT:REF /OPT:ICF /DEBUG:NONE`. Opt-in knobs: `BOSON_SINGLE_EXE`, `BOSON_STATIC_CRT`, `BOSON_PGO=GEN|USE`, `BOSON_AVX2`,
`BOSON_RELEASE_CODEGEN_FLAGS`. `CMakePresets.json` has `release`, `single-exe`, `single-exe-bootstrapper`, `pgo-gen`, `pgo-use`.

### PGO pipeline

`bench/pgo.ps1` (or `-DBOSON_PGO=GEN`, train, `pgomgr /merge`, `-DBOSON_PGO=USE`): instrumented `boson_bench_app` and
`boson_browser` are trained with `boson_bench` (startup, first frame, IPC load, tabs, resize, window moves), profiles merged to
`<BuildDir>/pgo/<target>.pgd`, then only the link step is redone (the `/GL` objects are reused). Facts found while building it:
`/GENPROFILE:PGD=` writes no `.pgd` unless a PDB is produced, so PGO builds link with `/DEBUG` instead of `/DEBUG:NONE`;
the instrumented exe needs `pgort140.dll` from the compiler's `bin` directory on `PATH`; counters land next to the exe as
`<exe>!N.pgc`. The profile used 3.2 % (bench_app) / 5.5 % (browser) of the reserved counter space, 0 % overflow.

## Single executable

Details, prerequisites, bootstrapper licence notes and limits are in [SHIPPING.md](SHIPPING.md#single-executable).

Sizes (bytes; ZIP = .NET Deflate "Optimal", one file), Release + LTO + `/GR-`:

| Exe | `/MD` (needs VC++ redist) | Single exe (`/MT`) | Single exe, ZIP | + embedded bootstrapper | UPX `--best --lzma` |
|---|---:|---:|---:|---:|---:|
| `boson_hello.exe` | 402,432 | **618,496** | 281,791 | 2,445,824 (ZIP 2,004,244) | 227,328 |
| `boson_demo.exe` | 1,209,856 | **1,463,808** | 642,366 | n/a | 464,896 |
| `boson_browser.exe` | 1,119,232 | **1,355,264** | 591,787 | n/a | 438,784 |

The embedded bootstrapper is Microsoft's 1,826,528-byte `MicrosoftEdgeWebview2Setup.exe` (already compressed, so the ZIP barely
shrinks). For reference `wails_browser.exe` in this repository is 10,578,432 bytes (4,292,581 zipped, see BENCHMARKS.md).
Tauri v2 and Electron single-binary sizes were **not available** (no comparison numbers were produced for them while this
section was written); Electron's runtime alone is far above 100 MB.

UPX experiment (documented only, not a default): files shrink 63-68 %, but the first-window time of the hello app went from
49 ms [30-235] to 172 ms [61-292] (median of 6 interleaved runs) and its private working set grew from 2.2 to 2.7 MB, because
the packed image is unpacked in full at start-up and can no longer be demand-paged. Packed executables are also a frequent
antivirus false positive (not measured here). Do not ship UPX'd builds unless download size matters more than start-up.

Dependency check: `ctest -R single_exe_deps` (`cmake/BosonCheckDependents.cmake`) runs `dumpbin /dependents` on the produced
exe and fails for anything outside the allow-list of Windows system DLLs (and `api-ms-win-*`). `boson_hello` imports 13 DLLs
(ADVAPI32, ole32, SHLWAPI, dwmapi, dcomp, USER32, GDI32, SHELL32, CRYPT32, WINHTTP, WINTRUST, KERNEL32,
api-ms-win-core-synch); the same check run against the `/MD` build fails and names `MSVCP140.dll;MSVCP140_ATOMIC_WAIT.dll;
VCRUNTIME140_1.dll;VCRUNTIME140.dll`.

## Verification

Verified:

- Release + LTO builds of `boson_hello`, `boson_demo`, `boson_browser`, `boson_bench_app`, `boson_bench` in six configurations
  (baseline, `/Gw /Zc:inline`, `/GR-`, `/Os`, AVX2, single exe), zero compiler warnings. The whole tree (365 targets) built in the
  `/GR-` Release configuration.
- `ctest` of that full Release `/GR-` build: 22/22 pass (a first pass had 3 `integration` failures - environment re-creation and
  guest-crash tests, which kill and relaunch WebView2 processes while other agents were using the machine; all three passed on
  an immediate re-run).
- Single exe: `ctest -R single_exe_deps` 3/3 pass (hello, demo, browser) and, as a negative control, fails on the `/MD` hello.
  `boson_hello.exe` (single) starts, creates its window (title "Boson Hello") and a WebView2 browser process tree on this machine.
- Missing-runtime handling: with `WEBVIEW2_BROWSER_EXECUTABLE_FOLDER` pointing at an empty folder (the loader then finds no
  runtime), the plain single exe and the embedded-bootstrapper exe each open a native dialog (window class `#32770`, title
  "Boson Hello") instead of failing silently. `tests/test_shipping.cpp` additionally covers: an embedded bootstrapper is never
  downloaded and unsigned bytes are rejected (`VerificationFailed`) before running, `require_webview2_runtime` returns false
  without dialogs when silent, and true when a runtime is installed.
- PGO: full GEN, train, merge, USE cycle ran through `bench/pgo.ps1`; both instrumented and optimised apps ran their scenarios.

Not verified:

- **Windows Sandbox**: `WindowsSandbox.exe` is not present on this machine (the optional feature is not enabled), so the
  single exe was **not** run on a clean Windows. The dependency check is static (imports), not a clean-machine run.
- The embedded bootstrapper was never actually **executed to install** a runtime (this machine has one; clicking "Yes" was
  avoided so as not to alter it). Its write-to-temp, Authenticode check and launch path is the same code as the downloaded
  bootstrapper, which `boson_shipping_tests` covers up to the signature check.
- Redistribution licence for the bootstrapper: Microsoft's distribution documentation describes shipping the Evergreen
  bootstrapper with an app as the supported way to cover the missing-runtime case, but this pass could not find the licence
  wording programmatically; read the terms at the download page before shipping it inside your exe.
- Wall-clock speed claims: none are made; differences were inside the noise (table above).
- Debug-build ctest (`boson_build.bat`, 383 targets, zero warnings): 20/22 pass first time (`boson_shipping_tests`,
  `boson_integration_tests` etc. included). `boson_security_tests` (314/315) and `boson_process_tests` (397/398) fail one check
  each, `!fs::exists(base)` after "Environment teardown: BrowserProcessExited not observed" (WebView2 teardown timeout leaves the
  profile folder locked on this loaded machine). The security test fails identically with an unrelated, independent build at
  `D:\Projects\Boson\build\wt`, and both passed in the Release `/GR-` run, so they are environmental, not caused by this
  change; `boson_store_tests` failed once under load and passed on re-run.
- clang-cl, `/favor`, asset compression, JS minification: not done (reasons above).
