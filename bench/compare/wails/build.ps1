# Builds the Wails v3 reference app the way "wails3 build" does for Windows production (tags production, -trimpath,
# -ldflags "-w -s -H windowsgui", icon/manifest .syso). Output: bench\compare\wails\bin\wails_compare.exe
#   powershell -ExecutionPolicy Bypass -File bench\compare\wails\build.ps1
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
foreach ($name in @("GOMODCACHE", "GOCACHE")) {
    $value = (& go env $name).Trim()
    if ($value -and -not (Test-Path (Split-Path -Qualifier $value))) {
        $fallback = if ($name -eq "GOMODCACHE") { Join-Path $env:USERPROFILE "go\pkg\mod" } else { Join-Path $env:LOCALAPPDATA "go-build" }
        Write-Host "[wails] $name=$value is unavailable, using $fallback"
        Set-Item -Path "env:$name" -Value $fallback
    }
}
$shared = Join-Path $PSScriptRoot "..\shared"
$front = Join-Path $PSScriptRoot "frontend"
if (Test-Path $front) { Remove-Item -Recurse -Force $front }
New-Item -ItemType Directory -Force -Path $front | Out-Null
Copy-Item (Join-Path $shared "index.html"), (Join-Path $shared "app.js"), (Join-Path $shared "style.css") $front
Copy-Item adapter.js $front

& go mod tidy
if ($LASTEXITCODE -ne 0) { throw "go mod tidy failed" }
Push-Location build
& wails3 generate syso -arch amd64 -icon ../../icon.ico -manifest windows/wails.exe.manifest -info windows/info.json -out ../wails_windows_amd64.syso
$code = $LASTEXITCODE
Pop-Location
if ($code -ne 0) { throw "wails3 generate syso failed" }
$env:CGO_ENABLED = "0"
& go build -tags production -trimpath -buildvcs=false -ldflags="-w -s -H windowsgui" -o bin\wails_compare.exe .
$code = $LASTEXITCODE
Remove-Item *.syso -ErrorAction SilentlyContinue
if ($code -ne 0) { throw "go build failed" }
Write-Host "[wails] built $(Resolve-Path bin\wails_compare.exe) ($((Get-Item bin\wails_compare.exe).Length) bytes)"
