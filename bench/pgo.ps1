# Profile-guided optimisation pipeline (MSVC, docs/PERFORMANCE.md "Build configuration"). From the repository root:
#
#   powershell -ExecutionPolicy Bypass -File bench\pgo.ps1 [-BuildDir build\pgo] [-Runs 3] [-CMakeArgs ...]
#
# 1. Configures <BuildDir> with -DBOSON_PGO=GEN and builds the instrumented boson_bench_app, boson_browser and
#    boson_bench (/GENPROFILE, writes <BuildDir>\<target>!N.pgc).
# 2. Training: boson_bench runs boson_bench_app (startup, first frame, IPC load: sequential / concurrent calls,
#    10 MB strings) and the browser scenario of boson_browser (tabs, sleeping, resize, window moves).
# 3. pgomgr /merge folds the .pgc counts into <target>.pgd.
# 4. Re-configures with -DBOSON_PGO=USE and relinks: only the link step is redone (/GL objects are reused).
# The result is <BuildDir>\boson_bench_app.exe / boson_browser.exe with profile-guided code layout and inlining.
param(
    [string]$BuildDir = "build\pgo",
    [int]$Runs = 3,
    [string[]]$CMakeArgs = @()
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

function Find-VcVars {
    $found = Get-ChildItem "$env:ProgramFiles\Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars64.bat" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { return $found.FullName }
    throw "vcvars64.bat not found (install the MSVC x64 build tools)"
}

# The instrumented executables load pgort140.dll, which lives next to cl.exe (not on the default PATH).
function Add-PgoRuntimeToPath {
    $dll = Get-ChildItem "$env:ProgramFiles\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\pgort140.dll" -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $dll) { throw "pgort140.dll not found (needed to run /GENPROFILE executables)" }
    $env:PATH = "$($dll.DirectoryName);$env:PATH"
}

function Invoke-Vc([string[]]$commands) {
    $bat = Join-Path ([IO.Path]::GetTempPath()) ("boson_pgo_" + [Guid]::NewGuid() + ".bat")
    (@("@echo off", "call `"$(Find-VcVars)`" >nul || exit /b 1") + $commands) | Set-Content -Encoding ascii $bat
    & cmd.exe /d /c $bat
    $code = $LASTEXITCODE
    Remove-Item $bat -ErrorAction SilentlyContinue
    if ($code -ne 0) { throw "command failed ($code): $($commands -join ' & ')" }
}

function Build([string]$mode) {
    $cfg = ("-S", ".", "-B", $BuildDir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DBOSON_PGO=$mode") + $CMakeArgs
    $q = ($cfg | ForEach-Object { '"' + $_ + '"' }) -join " "
    Invoke-Vc @("cmake $q", "cmake --build `"$BuildDir`" --target boson_bench_app boson_browser boson_bench")
}

$pgoDir = Join-Path $BuildDir "pgo"
New-Item -ItemType Directory -Force -Path $pgoDir | Out-Null
Remove-Item (Join-Path $pgoDir "*.pgd"), (Join-Path $BuildDir "*.pgc") -ErrorAction SilentlyContinue

Write-Host "== 1/4 instrumented build"
Build "GEN"

Write-Host "== 2/4 training"
Add-PgoRuntimeToPath
$bench = Join-Path $BuildDir "boson_bench.exe"
$train = Join-Path $BuildDir "pgo-train"
& $bench --exe (Join-Path $BuildDir "boson_bench_app.exe") --name boson_bench_app --out $train --runs $Runs --idle 3 --settle 1 --wait-ready --load-signal --cooldown 2 --no-samples
if ($LASTEXITCODE -ne 0) { throw "bench_app training failed" }
& $bench --scenario browser --target "boson=$(Join-Path $BuildDir 'boson_browser.exe')" --runs 1 --warmup 0 --out $train
if ($LASTEXITCODE -ne 0) { throw "browser training failed" }

Write-Host "== 3/4 merge profiles"
foreach ($target in "boson_bench_app", "boson_browser") {
    # The counts land next to the executable ("<exe>!N.pgc"); the .pgd they belong to is in $pgoDir.
    $pgcs = Get-ChildItem (Join-Path $BuildDir "$target!*.pgc") -ErrorAction SilentlyContinue
    if (-not $pgcs) { throw "no .pgc for $target in $BuildDir (did the instrumented app exit cleanly?)" }
    Invoke-Vc @("pgomgr /merge `"$BuildDir\$target!*.pgc`" `"$pgoDir\$target.pgd`"")
    Remove-Item $pgcs.FullName
}

Write-Host "== 4/4 optimised relink"
Build "USE"
Write-Host "Done: $BuildDir\boson_bench_app.exe, $BuildDir\boson_browser.exe (profile data in $pgoDir)"
