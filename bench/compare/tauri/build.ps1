# Builds the Tauri v2 reference app with cargo (release, custom-protocol = assets embedded, like "tauri build" without
# bundling). Output: bench\compare\tauri\src-tauri\target\release\tauri-compare.exe
#   powershell -ExecutionPolicy Bypass -File bench\compare\tauri\build.ps1
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
$shared = Join-Path $PSScriptRoot "..\shared"
$front = Join-Path $PSScriptRoot "frontend"
if (Test-Path $front) { Remove-Item -Recurse -Force $front }
New-Item -ItemType Directory -Force -Path $front | Out-Null
Copy-Item (Join-Path $shared "index.html"), (Join-Path $shared "app.js"), (Join-Path $shared "style.css") $front
Copy-Item adapter.js $front
New-Item -ItemType Directory -Force -Path (Join-Path $PSScriptRoot "src-tauri\icons") | Out-Null
Copy-Item (Join-Path $PSScriptRoot "..\icon.ico") (Join-Path $PSScriptRoot "src-tauri\icons\icon.ico") -Force
Push-Location src-tauri
& cargo build --release --features custom-protocol
$code = $LASTEXITCODE
Pop-Location
if ($code -ne 0) { throw "cargo build failed" }
$exe = Join-Path $PSScriptRoot "src-tauri\target\release\tauri-compare.exe"
if (-not (Test-Path $exe)) { throw "missing $exe" }
Write-Host "[tauri] built $exe"
