# One-command comparison: Boson vs Electron vs Tauri v2 vs Wails v3 (bench/compare). From the repository root:
#
#   powershell -ExecutionPolicy Bypass -File bench\run_compare.ps1 [-Runs 5] [-Warmup 1] [-IdleSeconds 30] [-SkipBuild]
#
# 1. Builds the same reference app four times (Boson Release+LTO into build\rel, Electron packaged with @electron/packager,
#    Tauri v2 release via cargo, Wails v3 production build via go). The frontend is bench\compare\shared, identical bytes.
# 2. Runs every app -Warmup + -Runs times, interleaved (the starting framework rotates each round), 3 s cooldown between runs.
#    Each run is one process tree: cold start, 30 s idle, memory at 1 and 2 windows, the IPC tests, shutdown.
# 3. Writes <Out>\results.json (every raw run), <Out>\comparison.md, <Out>\sizes.md.
#
# The page drives itself (no polling, no timers while idle); the harness only watches marks the backends write to
# COMPARE_DIR and samples the process tree. See docs/BENCHMARKS.md ("Boson vs Electron vs Tauri v2 vs Wails v3").
# Close other heavy programs first: the apps' windows open (and activate) on screen.
param(
    [int]$Runs = 5,
    [int]$Warmup = 1,
    [int]$IdleSeconds = 30,
    [int]$SettleSeconds = 3,
    [string[]]$Frameworks = @("boson", "electron", "tauri", "wails"),
    [string]$BuildDir = "build\rel",
    [string]$Out = "build\bench-results\compare",
    [string[]]$CMakeArgs = @(),
    [int]$CooldownSeconds = 3,
    [switch]$SkipBuild,
    [switch]$SkipSizes,
    [switch]$FailOnError    # exit 1 when a measured run failed (used by the ctest smoke test)
)
$ErrorActionPreference = "Stop"
$Frameworks = @($Frameworks | ForEach-Object { $_ -split "," })
$CMakeArgs = @($CMakeArgs | ForEach-Object { $_ -split "," } | Where-Object { $_ })   # also accepts -Frameworks a,b through "powershell -File"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$compare = Join-Path $root "bench\compare"
$inv = [Globalization.CultureInfo]::InvariantCulture

# ---- Build -------------------------------------------------------------------------------------------------------
function Find-VcVars {
    $pf86 = [Environment]::GetEnvironmentVariable("ProgramFiles(x86)")
    $vswhere = Join-Path $pf86 "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($path) {
            $candidate = Join-Path $path "VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $candidate) { return $candidate }
        }
    }
    $found = Get-ChildItem "$env:ProgramFiles\Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars64.bat" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { return $found.FullName }
    throw "vcvars64.bat not found (install the MSVC x64 build tools)"
}

function Invoke-CMakeBuild([string]$dir, [string[]]$targets) {
    $vcvars = Find-VcVars
    $configure = @("-S", ".", "-B", $dir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release") + $CMakeArgs
    $quoted = ($configure | ForEach-Object { '"' + $_ + '"' }) -join " "
    $bat = Join-Path ([IO.Path]::GetTempPath()) ("boson_compare_build_" + [Guid]::NewGuid() + ".bat")
    @(
        "@echo off",
        "call ""$vcvars"" >nul || exit /b 1",
        "cmake $quoted || exit /b 1",
        "cmake --build ""$dir"" --target $($targets -join ' ') || exit /b 1"
    ) | Set-Content -Encoding ascii $bat
    & cmd.exe /d /c $bat
    $code = $LASTEXITCODE
    Remove-Item $bat -ErrorAction SilentlyContinue
    if ($code -ne 0) { throw "CMake build of $dir failed" }
}

function Invoke-ChildScript([string]$script) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $script
    if ($LASTEXITCODE -ne 0) { throw "$script failed" }
}

$apps = [ordered]@{
    boson    = @{ Exe = (Join-Path $(if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $root $BuildDir }) "boson_compare_app.exe"); Dist = $null }
    electron = @{ Exe = (Join-Path $compare "electron\dist\electron-compare-win32-x64\electron-compare.exe"); Dist = (Join-Path $compare "electron\dist\electron-compare-win32-x64") }
    tauri    = @{ Exe = (Join-Path $compare "tauri\src-tauri\target\release\tauri-compare.exe"); Dist = $null }
    wails    = @{ Exe = (Join-Path $compare "wails\bin\wails_compare.exe"); Dist = $null }
}

if (-not $SkipBuild) {
    foreach ($fw in $Frameworks) {
        Write-Host "== build $fw"
        switch ($fw) {
            "boson"    { Invoke-CMakeBuild $BuildDir @("boson_compare_app") }
            "electron" { Invoke-ChildScript (Join-Path $compare "electron\build.ps1") }
            "tauri"    { Invoke-ChildScript (Join-Path $compare "tauri\build.ps1") }
            "wails"    { Invoke-ChildScript (Join-Path $compare "wails\build.ps1") }
            default    { throw "unknown framework $fw" }
        }
    }
}
foreach ($fw in $Frameworks) {
    if (-not (Test-Path $apps[$fw].Exe)) { throw "missing $($apps[$fw].Exe) (run without -SkipBuild)" }
}

# ---- Harness primitives ------------------------------------------------------------------------------------------
Add-Type -Path (Join-Path $compare "harness\ProcTree.cs")
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $root $Out }
$Out = [IO.Path]::GetFullPath($Out)   # apps get absolute paths: their working directory is their own
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$runsRoot = Join-Path $Out "runs"
if (Test-Path $runsRoot) { Remove-Item -Recurse -Force $runsRoot }
New-Item -ItemType Directory -Force -Path $runsRoot | Out-Null

function Wait-Until([double]$epochMs) {
    while ($true) {
        $left = $epochMs - [Cmp.Tree]::NowMs()
        if ($left -le 0) { return }
        Start-Sleep -Milliseconds ([Math]::Min(50, [Math]::Max(1, [int]$left)))
    }
}

# Returns the page's wall-clock timestamp (ms) from <dir>\<name>.mark, or $null on timeout.
function Wait-Mark([string]$dir, [string]$name, [int]$timeoutMs, $tree = $null) {
    $path = Join-Path $dir "$name.mark"
    $deadline = [Cmp.Tree]::NowMs() + $timeoutMs
    $nextRaise = 0.0
    while ([Cmp.Tree]::NowMs() -lt $deadline) {
        if ($null -ne $tree -and [Cmp.Tree]::NowMs() -ge $nextRaise) {   # keep the app windows un-occluded (see RaiseWindows)
            $tree.Refresh()
            $tree.RaiseWindows()
            $nextRaise = [Cmp.Tree]::NowMs() + 500
        }
        if (Test-Path -LiteralPath $path) {
            try {
                $text = [IO.File]::ReadAllText($path).Trim()
                $value = 0.0
                if ([double]::TryParse($text, [Globalization.NumberStyles]::Float, $inv, [ref]$value)) { return $value }
            } catch { }
        }
        Start-Sleep -Milliseconds 5
    }
    return $null
}

function Get-Median([double[]]$values) {
    if ($values.Count -eq 0) { return $null }
    $s = @($values | Sort-Object)
    $n = $s.Count
    if ($n % 2 -eq 1) { return [double]$s[($n - 1) / 2] }
    return ([double]$s[$n / 2 - 1] + [double]$s[$n / 2]) / 2.0
}

# Median of $count samples of the tree, $gapMs apart.
function Get-MemorySample($tree, [int]$count, [int]$gapMs) {
    $samples = @()
    for ($i = 0; $i -lt $count; $i++) {
        $samples += $tree.Sample()
        if ($i -lt $count - 1) { Start-Sleep -Milliseconds $gapMs }
    }
    $med = { param($prop) Get-Median ([double[]]($samples | ForEach-Object { $_.$prop })) }
    return [ordered]@{
        host_private_bytes_mb = & $med "HostCommitMb"
        host_private_ws_mb    = & $med "HostPrivateWsMb"
        tree_private_ws_mb    = & $med "TreePrivateWsMb"
        tree_commit_mb        = & $med "TreeCommitMb"
        tree_ws_mb            = & $med "TreeWsMb"
        processes             = & $med "Alive"
        handles               = & $med "Handles"
        threads               = & $med "Threads"
    }
}

# The WebView2 browser process can outlive its host by tens of seconds; killing it leaves a dirty profile that makes the
# next start of the same app slow, so the wait is long and a kill is the exception.
$shutdownCapMs = 60000
function Invoke-Run([string]$fw, [int]$index, [bool]$isWarmup, $config) {
    $app = $apps[$fw]
    $dir = Join-Path $runsRoot ("{0}-{1}" -f $fw, $index)
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $run = [ordered]@{ framework = $fw; run = $index; warmup = $isWarmup; error = $null }
    $envVars = New-Object "System.Collections.Generic.Dictionary[string,string]"
    $envVars["COMPARE_DIR"] = $dir
    $envVars["COMPARE_CONFIG"] = ($config | ConvertTo-Json -Compress)
    $tree = [Cmp.Tree]::Start($app.Exe, "", (Split-Path -Parent $app.Exe), $envVars)
    try {
        $launch = $tree.LaunchMs
        $first = $tree.WaitFirstWindow(60000)
        if ($first -lt 0) { throw "no window within 60 s" }
        $run.first_window_ms = $first - $launch
        $paint = Wait-Mark $dir "paint" 60000 $tree
        if ($null -eq $paint) { throw "no first paint mark within 60 s" }
        $run.paint_ms = $paint - $launch
        $ready = Wait-Mark $dir "ready" 30000 $tree
        if ($null -eq $ready) { throw "page never became ready (first IPC round trip)" }
        $run.ready_ms = $ready - $launch

        # ---- idle, one window -------------------------------------------------------------------------------------
        $idleStart = $ready + $SettleSeconds * 1000
        $idleEnd = $idleStart + $IdleSeconds * 1000
        Wait-Until $idleStart
        $a = $tree.Sample()
        $ctxA = $tree.ContextSwitches()
        $sysA = [Cmp.Tree]::SystemTimes()
        Wait-Until $idleEnd
        $b = $tree.Sample()
        $ctxB = $tree.ContextSwitches()
        $sysB = [Cmp.Tree]::SystemTimes()
        $seconds = ($b.AtMs - $a.AtMs) / 1000.0
        $ctxDelta = 0.0
        foreach ($p in $ctxA.Keys) { if ($ctxB.ContainsKey($p)) { $ctxDelta += $ctxB[$p] - $ctxA[$p] } }
        $sysTotal = ($sysB[1] - $sysA[1]) + ($sysB[2] - $sysA[2])
        $run.idle = [ordered]@{
            window_s           = $seconds
            cpu_pct_of_core    = 100.0 * ($b.CpuMs - $a.CpuMs) / 1000.0 / $seconds
            mcycles_per_s      = ($b.CycleMillions - $a.CycleMillions) / $seconds
            ctx_switches_per_s = $ctxDelta / $seconds
            system_busy_pct    = $(if ($sysTotal -gt 0) { 100.0 * ($sysTotal - ($sysB[0] - $sysA[0])) / $sysTotal } else { 0.0 })
        }
        $run.mem_1window = Get-MemorySample $tree 5 400
        $run.mem_1window.visible_windows = $tree.VisibleWindowCount()

        # ---- second window ----------------------------------------------------------------------------------------
        $req = Wait-Mark $dir "win2_requested" 30000
        $ready2 = Wait-Mark $dir "ready2" 20000 $tree
        if ($null -eq $req -or $null -eq $ready2) { throw "second window did not become ready within 20 s" }
        $run.window2_open_to_ready_ms = $ready2 - $req
        Wait-Until ($ready2 + 2000)
        $run.mem_2windows = Get-MemorySample $tree 5 400
        $run.mem_2windows.visible_windows = $tree.VisibleWindowCount()
        $ipcStart = Wait-Mark $dir "ipc_start" 20000 $tree
        if ($null -ne $ipcStart -and [Cmp.Tree]::NowMs() -gt $ipcStart + 50) { $run.note = "2-window memory sampling overlapped the IPC test" }

        # ---- IPC ----------------------------------------------------------------------------------------------------
        $done = Wait-Mark $dir "done" 240000 $tree
        if ($null -eq $done) { throw "IPC tests did not finish within 240 s" }
        $result = Get-Content -Raw -LiteralPath (Join-Path $dir "result.json") | ConvertFrom-Json
        if (-not $result.ok) { throw "IPC tests failed: $($result.error)" }
        $run.ipc = $result

        # ---- shutdown -----------------------------------------------------------------------------------------------
        Start-Sleep -Milliseconds 500
        $tree.Refresh()
        $closeAt = [Cmp.Tree]::NowMs()
        $tree.CloseWindows()
        $rootGone = $tree.WaitRootExited($shutdownCapMs)
        if ($rootGone -ge 0) { $run.shutdown_host_ms = $rootGone - $closeAt }
        $gone = $tree.WaitAllExited([Math]::Max(1, [int]($shutdownCapMs - ([Cmp.Tree]::NowMs() - $closeAt))))
        if ($gone -lt 0) {
            $run.shutdown_killed = $true
            $run.shutdown_survivors = $tree.DescribeAlive()
            $run.shutdown_ms = [double]$shutdownCapMs
        } else {
            $run.shutdown_ms = $gone - $closeAt
        }
    } catch {
        $run.error = $_.Exception.Message
    } finally {
        try { $tree.KillAll() } catch { }
        try { $tree.WaitAllExited(10000) | Out-Null } catch { }
        $tree.Dispose()
    }
    return $run
}

# ---- Run ------------------------------------------------------------------------------------------------------------
$config = [ordered]@{
    idle_total_ms     = ($SettleSeconds + $IdleSeconds) * 1000 + 6000   # settle + idle + 5 memory samples + slack
    two_windows_ms    = 12000                                           # open window 2 -> IPC tests start
    ping_calls        = 2000
    concurrent_calls  = 1000
    echo_1k_calls     = 500
    echo_64k_calls    = 200
    echo_1m_calls     = 30
    items_calls       = 30
    burst_events      = 10000
    burst_runs        = 5
}
$results = New-Object System.Collections.ArrayList
$total = $Warmup + $Runs
for ($r = 0; $r -lt $total; $r++) {
    $isWarm = $r -lt $Warmup
    for ($k = 0; $k -lt $Frameworks.Count; $k++) {
        $fw = $Frameworks[($k + $r) % $Frameworks.Count]   # rotate the starting framework each round
        Write-Host ("[{0}/{1}] {2}{3}" -f ($r + 1), $total, $fw, $(if ($isWarm) { " (warm-up)" } else { "" }))
        $run = Invoke-Run $fw ($r + 1) $isWarm $config
        if ($run.error) { Write-Host "    FAILED: $($run.error)" -ForegroundColor Yellow }
        else { Write-Host ([string]::Format($inv, "    window {0:N0} ms, paint {1:N0} ms, ready {2:N0} ms, idle {3:N2} % core, 1-window tree {4:N1} MB, shutdown {5:N0} ms", $run.first_window_ms, $run.paint_ms, $run.ready_ms, $run.idle.cpu_pct_of_core, $run.mem_1window.tree_private_ws_mb, $run.shutdown_ms)) }
        [void]$results.Add($run)
        Start-Sleep -Seconds $CooldownSeconds
        if ($run.error -or $run.shutdown_killed) { Write-Host "    (run ended abnormally: waiting 30 s for the profile to settle)"; Start-Sleep -Seconds 30 }
    }
}

# ---- Sizes ----------------------------------------------------------------------------------------------------------
$sizes = [ordered]@{}
if (-not $SkipSizes) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    foreach ($fw in $Frameworks) {
        $app = $apps[$fw]
        $stage = Join-Path ([IO.Path]::GetTempPath()) ("compare_zip_" + [Guid]::NewGuid())
        New-Item -ItemType Directory -Force -Path $stage | Out-Null
        if ($app.Dist) { Copy-Item -Recurse (Join-Path $app.Dist "*") $stage } else { Copy-Item $app.Exe $stage }
        $zip = $stage + ".zip"
        [IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)
        $files = @(Get-ChildItem -Recurse -File $stage)
        $sizes[$fw] = [ordered]@{
            exe_bytes  = (Get-Item $app.Exe).Length
            dist_bytes = ($files | Measure-Object Length -Sum).Sum
            dist_files = $files.Count
            zip_bytes  = (Get-Item $zip).Length
        }
        Remove-Item -Recurse -Force $stage, $zip
    }
}

# ---- Report ---------------------------------------------------------------------------------------------------------
function Get-WebView2Version {
    foreach ($key in @("HKLM:\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}", "HKLM:\SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}")) {
        $v = (Get-ItemProperty -Path $key -ErrorAction SilentlyContinue).pv
        if ($v) { return $v }
    }
    return "unknown"
}
function Get-ToolVersions {
    $v = [ordered]@{}
    $v.boson = (& git rev-parse --short HEAD 2>$null)
    $v.electron = (Get-Content (Join-Path $compare "electron\electron-version.txt") -Raw).Trim()
    $lock = Join-Path $compare "tauri\src-tauri\Cargo.lock"
    $v.tauri = "unknown"
    if (Test-Path $lock) {
        $m = [regex]::Match((Get-Content -Raw $lock), 'name = "tauri"\r?\nversion = "([^"]+)"')
        if ($m.Success) { $v.tauri = $m.Groups[1].Value }
    }
    $mod = Join-Path $compare "wails\go.mod"
    $v.wails = "unknown"
    if (Test-Path $mod) {
        $m = [regex]::Match((Get-Content -Raw $mod), 'wailsapp/wails/v3 (v[0-9.]+)')
        if ($m.Success) { $v.wails = $m.Groups[1].Value }
    }
    return $v
}

$metrics = @(
    @{ g = "Startup"; l = "first visible window (ms)"; d = 0; low = $true; f = { param($r) $r.first_window_ms } },
    @{ g = "Startup"; l = "first paint (ms)"; d = 0; low = $true; f = { param($r) $r.paint_ms } },
    @{ g = "Startup"; l = "page ready: first IPC round trip (ms)"; d = 0; low = $true; f = { param($r) $r.ready_ms } },
    @{ g = "Idle 30 s, 1 window"; l = "tree CPU (% of one core)"; d = 2; low = $true; f = { param($r) $r.idle.cpu_pct_of_core } },
    @{ g = "Idle 30 s, 1 window"; l = "tree CPU (Mcycles/s)"; d = 1; low = $true; f = { param($r) $r.idle.mcycles_per_s } },
    @{ g = "Idle 30 s, 1 window"; l = "tree context switches (/s)"; d = 0; low = $true; f = { param($r) $r.idle.ctx_switches_per_s } },
    @{ g = "Idle 30 s, 1 window"; l = "system-wide CPU busy during the window (%, noise indicator)"; d = 1; low = $null; f = { param($r) $r.idle.system_busy_pct } },
    @{ g = "Memory, 1 window"; l = "host process private bytes (MB)"; d = 1; low = $true; f = { param($r) $r.mem_1window.host_private_bytes_mb } },
    @{ g = "Memory, 1 window"; l = "host process private working set (MB)"; d = 1; low = $true; f = { param($r) $r.mem_1window.host_private_ws_mb } },
    @{ g = "Memory, 1 window"; l = "whole tree private working set (MB)"; d = 1; low = $true; f = { param($r) $r.mem_1window.tree_private_ws_mb } },
    @{ g = "Memory, 1 window"; l = "whole tree private bytes / commit (MB)"; d = 1; low = $true; f = { param($r) $r.mem_1window.tree_commit_mb } },
    @{ g = "Memory, 1 window"; l = "processes"; d = 0; low = $true; f = { param($r) $r.mem_1window.processes } },
    @{ g = "Memory, 1 window"; l = "threads"; d = 0; low = $true; f = { param($r) $r.mem_1window.threads } },
    @{ g = "Memory, 1 window"; l = "handles"; d = 0; low = $true; f = { param($r) $r.mem_1window.handles } },
    @{ g = "Memory, 2 windows"; l = "second window: request -> its IPC ready (ms)"; d = 0; low = $true; f = { param($r) $r.window2_open_to_ready_ms } },
    @{ g = "Memory, 2 windows"; l = "host process private bytes (MB)"; d = 1; low = $true; f = { param($r) $r.mem_2windows.host_private_bytes_mb } },
    @{ g = "Memory, 2 windows"; l = "whole tree private working set (MB)"; d = 1; low = $true; f = { param($r) $r.mem_2windows.tree_private_ws_mb } },
    @{ g = "Memory, 2 windows"; l = "whole tree private bytes / commit (MB)"; d = 1; low = $true; f = { param($r) $r.mem_2windows.tree_commit_mb } },
    @{ g = "Memory, 2 windows"; l = "processes"; d = 0; low = $true; f = { param($r) $r.mem_2windows.processes } },
    @{ g = "IPC"; l = "ping, sequential: mean (ms)"; d = 3; low = $true; f = { param($r) $r.ipc.ping_sequential.mean_ms } },
    @{ g = "IPC"; l = "ping, sequential: p99 (ms)"; d = 2; low = $true; f = { param($r) $r.ipc.ping_sequential.p99_ms } },
    @{ g = "IPC"; l = "ping, 1000 concurrent (calls/s)"; d = 0; low = $false; f = { param($r) $r.ipc.ping_concurrent.calls_per_s } },
    @{ g = "IPC"; l = "echo 1 KiB: mean (ms)"; d = 3; low = $true; f = { param($r) $r.ipc.echo.'1KiB'.mean_ms } },
    @{ g = "IPC"; l = "echo 1 KiB: p99 (ms)"; d = 2; low = $true; f = { param($r) $r.ipc.echo.'1KiB'.p99_ms } },
    @{ g = "IPC"; l = "echo 64 KiB: mean (ms)"; d = 3; low = $true; f = { param($r) $r.ipc.echo.'64KiB'.mean_ms } },
    @{ g = "IPC"; l = "echo 64 KiB: p99 (ms)"; d = 2; low = $true; f = { param($r) $r.ipc.echo.'64KiB'.p99_ms } },
    @{ g = "IPC"; l = "echo 1 MiB: mean (ms)"; d = 2; low = $true; f = { param($r) $r.ipc.echo.'1MiB'.mean_ms } },
    @{ g = "IPC"; l = "echo 1 MiB: p99 (ms)"; d = 1; low = $true; f = { param($r) $r.ipc.echo.'1MiB'.p99_ms } },
    @{ g = "IPC"; l = "echo 1 MiB: payload throughput (MiB/s, one way)"; d = 1; low = $false; f = { param($r) $r.ipc.echo.'1MiB'.mb_per_s } },
    @{ g = "IPC"; l = "10k-item JSON array: mean (ms)"; d = 2; low = $true; f = { param($r) $r.ipc.items_10k.mean_ms } },
    @{ g = "IPC"; l = "10k-item JSON array: p99 (ms)"; d = 1; low = $true; f = { param($r) $r.ipc.items_10k.p99_ms } },
    @{ g = "Events"; l = "backend -> page burst of 10000 (events/s, median of 5 bursts)"; d = 0; low = $false; f = { param($r) $r.ipc.events.median_events_per_s } },
    @{ g = "Lifecycle"; l = "shutdown: WM_CLOSE -> app process exited (ms)"; d = 0; low = $true; f = { param($r) $r.shutdown_host_ms } },
    @{ g = "Lifecycle"; l = "shutdown: WM_CLOSE -> whole tree gone, incl. WebView2 browser process (ms; capped at 60000)"; d = 0; low = $true; f = { param($r) $r.shutdown_ms } }
)

$measured = @($results | Where-Object { -not $_.warmup -and -not $_.error })
$failed = @($results | Where-Object { -not $_.warmup -and $_.error })
function Format-Cell($values, [int]$decimals) {
    if ($values.Count -eq 0) { return "n/a" }
    $s = @($values | Sort-Object)
    $med = Get-Median ([double[]]$s)
    $fmt = "N$decimals"
    $cell = $med.ToString($fmt, $inv)
    if ($s.Count -gt 1) { $cell += " [" + ([double]$s[0]).ToString($fmt, $inv) + "-" + ([double]$s[-1]).ToString($fmt, $inv) + "]" }
    return $cell
}

$md = New-Object System.Text.StringBuilder
$cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name.Trim()
$os = (Get-CimInstance Win32_OperatingSystem)
$tools = Get-ToolVersions
[void]$md.AppendLine("# Boson vs Electron vs Tauri v2 vs Wails v3")
[void]$md.AppendLine()
[void]$md.AppendLine("- Measured: $(Get-Date -Format 'yyyy-MM-dd HH:mm'), $Runs measured run(s) + $Warmup warm-up per framework, interleaved; cells are **median [min-max]**, the best median of a row is **bold**.")
[void]$md.AppendLine("- Machine: $($os.Caption) $($os.Version), $cpu ($([Environment]::ProcessorCount) logical CPUs), $([Math]::Round($os.TotalVisibleMemorySize / 1MB)) GB RAM, WebView2 runtime $(Get-WebView2Version).")
[void]$md.AppendLine("- Versions: Boson $($tools.boson) (MSVC Release + LTO, /MD), Electron $($tools.electron), Tauri $($tools.tauri) (release, lto, opt-level s), Wails $($tools.wails) (production, -w -s).")
$failNote = ""
if ($failed.Count -gt 0) { $failNote = " (" + (($failed | ForEach-Object { "$($_.framework) #$($_.run): $($_.error)" }) -join "; ") + ")" }
[void]$md.AppendLine("- Idle window: $SettleSeconds s settle, then $IdleSeconds s. Failed measured runs: $($failed.Count)$failNote")
[void]$md.AppendLine()
$head = "| Group | Metric |"
$sep = "|---|---|"
foreach ($fw in $Frameworks) { $head += " $fw |"; $sep += "---:|" }
[void]$md.AppendLine($head)
[void]$md.AppendLine($sep)
$summary = [ordered]@{}
foreach ($m in $metrics) {
    $cells = @()
    $medians = @{}
    $key = $m.g + " | " + $m.l
    $summary[$key] = [ordered]@{}
    foreach ($fw in $Frameworks) {
        $vals = @($measured | Where-Object { $_.framework -eq $fw } | ForEach-Object { & $m.f $_ } | Where-Object { $null -ne $_ } | ForEach-Object { [double]$_ })
        $medians[$fw] = $(if ($vals.Count -gt 0) { Get-Median $vals } else { $null })
        $cells += (Format-Cell $vals $m.d)
        $summary[$key][$fw] = $medians[$fw]
    }
    if ($null -ne $m.low) {
        $best = $null
        foreach ($fw in $Frameworks) {
            if ($null -eq $medians[$fw]) { continue }
            if ($null -eq $best -or ($m.low -and $medians[$fw] -lt $medians[$best]) -or (-not $m.low -and $medians[$fw] -gt $medians[$best])) { $best = $fw }
        }
        if ($null -ne $best) { $i = [array]::IndexOf($Frameworks, $best); $cells[$i] = "**" + $cells[$i] + "**" }
    }
    [void]$md.AppendLine("| $($m.g) | $($m.l) | " + ($cells -join " | ") + " |")
}
[void]$md.AppendLine()
[void]$md.AppendLine("Per-run values are in results.json. Latencies are measured in the page with performance.now() (100 us resolution in Chromium); the mean is taken from the wall time of the whole loop, p99 from the per-call timings.")
Set-Content -Encoding UTF8 -Path (Join-Path $Out "comparison.md") -Value $md.ToString()

if (-not $SkipSizes) {
    $sz = New-Object System.Text.StringBuilder
    [void]$sz.AppendLine("| Framework | Main executable (bytes) | Distribution folder (bytes) | Files | ZIP, Deflate optimal (bytes) |")
    [void]$sz.AppendLine("|---|---:|---:|---:|---:|")
    foreach ($fw in $Frameworks) {
        $s = $sizes[$fw]
        [void]$sz.AppendLine("| $fw | $('{0:N0}' -f $s.exe_bytes) | $('{0:N0}' -f $s.dist_bytes) | $($s.dist_files) | $('{0:N0}' -f $s.zip_bytes) |")
    }
    Set-Content -Encoding UTF8 -Path (Join-Path $Out "sizes.md") -Value $sz.ToString()
}

[ordered]@{ config = $config; tools = $tools; sizes = $sizes; summary = $summary; runs = $results } | ConvertTo-Json -Depth 12 | Set-Content -Encoding UTF8 -Path (Join-Path $Out "results.json")
Write-Host ""
Write-Host (Get-Content -Raw (Join-Path $Out "comparison.md"))
Write-Host "Reports: $Out"
if ($FailOnError -and $failed.Count -gt 0) { exit 1 }
