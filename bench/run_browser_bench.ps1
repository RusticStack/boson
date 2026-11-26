# One-command Boson vs Wails v3 multi-tab browser benchmark (issue #18). From the repository root:
#
#   powershell -ExecutionPolicy Bypass -File bench\run_browser_bench.ps1 [-Runs 5] [-Warmup 1] [-SkipBuild]
#
# 1. Builds Boson Release + LTO (build\rel: boson_browser, boson_demo, boson_bench) and, unless -NoStaticCrt,
#    a static-CRT variant (build\rel-mt) for the size table.
# 2. Builds bench\wails_browser (production flags, like "wails3 build").
# 3. Runs boson_bench --scenario browser against boson_browser, wails_browser and (unless -NoWailsTuned)
#    wails_browser --move-debounce=1, interleaving the runs.
# 4. Writes <Out>\*.scenario.{json,md}, <Out>\comparison.md and <Out>\sizes.md.
# Close other heavy programs first; the apps' windows open without activation but do appear on screen.
param(
    [int]$Runs = 5,
    [int]$Warmup = 1,
    [string]$BuildDir = "build\rel",
    [string]$StaticBuildDir = "build\rel-mt",
    [string]$Out = "build\bench-results\browser",
    [string[]]$CMakeArgs = @(),
    [switch]$SkipBuild,
    [switch]$NoStaticCrt,
    [switch]$NoWailsTuned
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

function Find-VcVars {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
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

function Invoke-CMakeBuild([string]$dir, [string[]]$extra, [string[]]$targets) {
    $vcvars = Find-VcVars
    $configure = @("-S", ".", "-B", $dir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release") + $extra + $CMakeArgs
    $quoted = ($configure | ForEach-Object { '"' + $_ + '"' }) -join " "
    # A temporary .bat avoids PowerShell 5.1's native-argument quoting of the vcvars + cmake chain.
    $bat = Join-Path ([IO.Path]::GetTempPath()) ("boson_bench_build_" + [Guid]::NewGuid() + ".bat")
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

if (-not $SkipBuild) {
    Write-Host "== Boson Release + LTO ($BuildDir)"
    Invoke-CMakeBuild $BuildDir @() @("boson_browser", "boson_demo", "boson_bench")
    if (-not $NoStaticCrt) {
        Write-Host "== Boson Release + LTO + static CRT ($StaticBuildDir)"
        Invoke-CMakeBuild $StaticBuildDir @("-DBOSON_STATIC_CRT=ON") @("boson_browser", "boson_demo")
    }
    Write-Host "== Wails v3 browser"
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "bench\wails_browser\build.ps1")
    if ($LASTEXITCODE -ne 0) { throw "Wails build failed" }
}

$bench = Join-Path $root "$BuildDir\boson_bench.exe"
$boson = Join-Path $root "$BuildDir\boson_browser.exe"
$wails = Join-Path $root "bench\wails_browser\bin\wails_browser.exe"
foreach ($p in @($bench, $boson, $wails)) { if (-not (Test-Path $p)) { throw "missing $p (run without -SkipBuild)" } }
New-Item -ItemType Directory -Force -Path $Out | Out-Null

$benchArgs = @("--scenario", "browser", "--target", "boson=$boson", "--target", "wails=$wails")
if (-not $NoWailsTuned) {
    $benchArgs += @("--target", "wails_debounce1=$wails", "--target-arg", "wails_debounce1=--move-debounce=1")
}
$benchArgs += @("--runs", "$Runs", "--warmup", "$Warmup", "--out", $Out)
Write-Host "== boson_bench $($benchArgs -join ' ')"
& $bench @benchArgs
$benchExit = $LASTEXITCODE

# ---- Binary and archive sizes ------------------------------------------------------------------------------
$rows = @()
$candidates = @(
    @{ Name = "boson_browser.exe (Release, LTO, /MD)"; Path = $boson },
    @{ Name = "boson_demo.exe (Release, LTO, /MD, frontend embedded)"; Path = (Join-Path $root "$BuildDir\boson_demo.exe") },
    @{ Name = "boson_browser.exe (Release, LTO, static CRT /MT)"; Path = (Join-Path $root "$StaticBuildDir\boson_browser.exe") },
    @{ Name = "boson_demo.exe (Release, LTO, static CRT /MT)"; Path = (Join-Path $root "$StaticBuildDir\boson_demo.exe") },
    @{ Name = "wails_browser.exe (production, -s -w)"; Path = $wails }
)
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("boson_sizes_" + [Guid]::NewGuid())
New-Item -ItemType Directory -Path $tmp | Out-Null
foreach ($c in $candidates) {
    if (-not (Test-Path $c.Path)) { continue }
    $zip = Join-Path $tmp ([IO.Path]::GetFileNameWithoutExtension($c.Path) + "_" + $rows.Count + ".zip")
    Compress-Archive -Path $c.Path -DestinationPath $zip -CompressionLevel Optimal
    $rows += "| $($c.Name) | $((Get-Item $c.Path).Length) | $((Get-Item $zip).Length) |"
}
Remove-Item -Recurse -Force $tmp
$sizes = @("| Binary | Bytes | ZIP (Deflate, optimal) bytes |", "|---|---:|---:|") + $rows
$sizes -join "`n" | Set-Content -Encoding utf8 (Join-Path $Out "sizes.md")
Write-Host ""
$sizes | ForEach-Object { Write-Host $_ }
Write-Host ""
Write-Host "Reports: $(Resolve-Path $Out)"
exit $benchExit
