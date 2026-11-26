# Builds bench/wails_browser the way "wails3 build" does for Windows (production tags, -trimpath, -w -s,
# windowsgui, icon/manifest/version .syso), after copying the shared benchmark assets from bench/shared/.
#   powershell -ExecutionPolicy Bypass -File bench\wails_browser\build.ps1
param([string]$Out = "bin\wails_browser.exe")
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

# A GOMODCACHE/GOCACHE on a drive that does not exist (e.g. a removed dev drive) breaks every go command.
foreach ($name in @("GOMODCACHE", "GOCACHE")) {
    $value = (& go env $name).Trim()
    if ($value -and -not (Test-Path (Split-Path -Qualifier $value))) {
        $fallback = if ($name -eq "GOMODCACHE") { Join-Path $env:USERPROFILE "go\pkg\mod" } else { Join-Path $env:LOCALAPPDATA "go-build" }
        Write-Host "[wails_browser] $name=$value is unavailable, using $fallback"
        Set-Item -Path "env:$name" -Value $fallback
    }
}

Copy-Item ..\shared\bench_shell.js frontend\bench_shell.js -Force
Copy-Item ..\shared\bench_tab.html frontend\bench_tab.html -Force

& go mod tidy
if ($LASTEXITCODE -ne 0) { throw "go mod tidy failed" }
Push-Location build
& wails3 generate syso -arch amd64 -icon windows/icon.ico -manifest windows/wails.exe.manifest -info windows/info.json -out ../wails_windows_amd64.syso
$code = $LASTEXITCODE
Pop-Location
if ($code -ne 0) { throw "wails3 generate syso failed" }
$env:CGO_ENABLED = "0"
& go build -tags production -trimpath -buildvcs=false -ldflags="-w -s -H windowsgui" -o $Out .
$code = $LASTEXITCODE
Remove-Item *.syso -ErrorAction SilentlyContinue
if ($code -ne 0) { throw "go build failed" }
Write-Host "[wails_browser] built $(Resolve-Path $Out) ($((Get-Item $Out).Length) bytes)"
