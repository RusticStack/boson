# Boson vs Electron vs Tauri v2 vs Wails v3

- Measured: 2026-11-26 15:50, 5 measured run(s) + 1 warm-up per framework, interleaved; cells are **median [min-max]**, the best median of a row is **bold**.
- Machine: Microsoft Windows 11 Pro 10.0.26300, 13th Gen Intel(R) Core(TM) i7-13700KF (24 logical CPUs), 48 GB RAM, WebView2 runtime 158.0.4370.33.
- Versions: Boson 71c3757 (MSVC Release + LTO, /MD), Electron 45.5.0, Tauri 2.12.7 (release, lto, opt-level s), Wails v3.1.0 (production, -w -s).
- Idle window: 3 s settle, then 30 s. Failed measured runs: 0

| Group | Metric | boson | electron | tauri | wails |
|---|---|---:|---:|---:|---:|
| Startup | first visible window (ms) | 29 [26-43] | 107 [104-121] | **29 [28-43]** | 381 [306-641] |
| Startup | first paint (ms) | 312 [273-366] | **202 [197-364]** | 373 [328-400] | 396 [307-658] |
| Startup | page ready: first IPC round trip (ms) | 314 [274-367] | **204 [198-365]** | 378 [333-408] | 403 [312-672] |
| Idle 30 s, 1 window | tree CPU (% of one core) | **0.10 [0.05-2.40]** | 1.77 [0.10-5.31] | 0.21 [0.10-2.14] | 1.67 [0.10-5.63] |
| Idle 30 s, 1 window | tree CPU (Mcycles/s) | **9.1 [7.3-88.9]** | 81.8 [9.9-187.7] | 10.7 [9.3-75.8] | 56.0 [12.6-195.1] |
| Idle 30 s, 1 window | tree context switches (/s) | 11 [4-527] | 1,125 [97-2,371] | **10 [7-458]** | 411 [47-1,551] |
| Idle 30 s, 1 window | system-wide CPU busy during the window (%, noise indicator) | 30.7 [20.3-45.4] | 34.0 [30.3-40.6] | 39.0 [31.4-42.5] | 37.3 [33.0-45.4] |
| Memory, 1 window | host process private bytes (MB) | **4.7 [4.6-4.9]** | 31.4 [31.0-33.2] | 5.6 [5.5-5.7] | 51.6 [51.5-54.9] |
| Memory, 1 window | host process private working set (MB) | **3.4 [3.3-3.5]** | 23.5 [23.2-24.2] | 4.0 [3.9-4.0] | 6.6 [6.4-9.1] |
| Memory, 1 window | whole tree private working set (MB) | 92.3 [87.7-92.4] | **91.2 [88.4-94.3]** | 92.3 [90.9-94.1] | 96.2 [94.4-102.8] |
| Memory, 1 window | whole tree private bytes / commit (MB) | 162.2 [157.8-162.6] | **149.9 [146.9-154.1]** | 163.4 [160.8-166.7] | 212.4 [209.9-218.3] |
| Memory, 1 window | processes | 7 [7-7] | **4 [4-4]** | 7 [7-7] | 7 [7-7] |
| Memory, 1 window | threads | 207 [201-210] | **120 [118-123]** | 209 [204-212] | 195 [192-199] |
| Memory, 1 window | handles | 3,904 [3,877-3,906] | **2,805 [2,796-2,828]** | 3,963 [3,933-3,970] | 4,039 [4,000-4,061] |
| Memory, 2 windows | second window: request -> its IPC ready (ms) | 101 [99-105] | **89 [75-110]** | 132 [129-222] | 150 [141-748] |
| Memory, 2 windows | host process private bytes (MB) | **4.9 [4.9-5.0]** | 33.4 [32.2-34.2] | 6.0 [5.8-6.0] | 52.5 [52.3-55.5] |
| Memory, 2 windows | whole tree private working set (MB) | **107.3 [102.7-107.6]** | 117.1 [115.2-120.7] | 110.0 [109.9-113.4] | 114.9 [111.7-120.2] |
| Memory, 2 windows | whole tree private bytes / commit (MB) | 200.0 [197.8-202.4] | **195.4 [192.6-201.4]** | 212.7 [210.8-214.9] | 255.5 [250.6-259.1] |
| Memory, 2 windows | processes | 8 [8-8] | **5 [5-5]** | 8 [8-8] | 8 [8-8] |
| IPC | ping, sequential: mean (ms) | 0.128 [0.078-0.394] | **0.112 [0.073-0.127]** | 1.507 [0.999-1.817] | 2.272 [1.122-3.041] |
| IPC | ping, sequential: p99 (ms) | **0.30 [0.20-1.50]** | 0.30 [0.20-0.40] | 2.60 [2.00-3.90] | 4.30 [2.20-7.50] |
| IPC | ping, 1000 concurrent (calls/s) | 64,935 [39,841-92,593] | **95,238 [50,761-102,041]** | 3,019 [2,396-4,257] | 1,679 [1,337-2,061] |
| IPC | echo 1 KiB: mean (ms) | 0.143 [0.093-0.210] | **0.129 [0.075-0.155]** | 1.436 [1.150-1.833] | 2.327 [1.366-3.515] |
| IPC | echo 1 KiB: p99 (ms) | **0.30 [0.20-0.40]** | 0.30 [0.20-0.60] | 2.50 [2.00-3.10] | 3.40 [2.60-6.20] |
| IPC | echo 64 KiB: mean (ms) | 1.873 [1.482-2.320] | **0.480 [0.334-0.518]** | 4.376 [3.079-5.211] | 12.524 [7.950-14.678] |
| IPC | echo 64 KiB: p99 (ms) | 2.70 [2.10-3.80] | **1.32 [1.00-2.50]** | 7.37 [4.90-9.14] | 16.60 [10.60-18.40] |
| IPC | echo 1 MiB: mean (ms) | 26.38 [20.48-31.37] | **2.85 [2.54-4.62]** | 36.04 [29.84-41.30] | 138.16 [97.16-156.74] |
| IPC | echo 1 MiB: p99 (ms) | 29.6 [22.2-34.1] | **6.4 [4.3-16.3]** | 40.6 [32.6-48.9] | 156.4 [117.6-361.9] |
| IPC | echo 1 MiB: payload throughput (MiB/s, one way) | 37.9 [31.9-48.8] | **350.9 [216.6-393.7]** | 27.7 [24.2-33.5] | 7.2 [6.4-10.3] |
| IPC | 10k-item JSON array: mean (ms) | 17.49 [13.00-18.82] | **10.11 [9.50-20.34]** | 11.72 [9.21-12.79] | 17.91 [11.08-19.07] |
| IPC | 10k-item JSON array: p99 (ms) | 22.2 [14.6-25.2] | 16.2 [13.6-26.3] | **14.5 [10.9-15.4]** | 22.0 [13.8-22.9] |
| Events | backend -> page burst of 10000 (events/s, median of 5 bursts) | 107,527 [91,408-167,785] | **167,224 [146,843-187,970]** | 23,798 [20,096-33,557] | 14,455 [13,976-25,432] |
| Lifecycle | shutdown: WM_CLOSE -> app process exited (ms) | 308 [273-468] | 80 [79-115] | 60 [49-113] | **57 [32-77]** |
| Lifecycle | shutdown: WM_CLOSE -> whole tree gone, incl. WebView2 browser process (ms; capped at 60000) | 308 [273-468] | **81 [79-115]** | 126 [114-297] | 136 [95-190] |

Per-run values are in results.json. Latencies are measured in the page with performance.now() (100 us resolution in Chromium); the mean is taken from the wall time of the whole loop, p99 from the per-call timings.

