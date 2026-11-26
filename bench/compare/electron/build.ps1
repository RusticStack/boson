# Builds the Electron reference app: stages the shared frontend + this folder's main/preload/adapter, then packages it
# with @electron/packager (asar, win32-x64). Output: bench\compare\electron\dist\electron-compare-win32-x64\electron-compare.exe
#   powershell -ExecutionPolicy Bypass -File bench\compare\electron\build.ps1
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
$shared = Join-Path $PSScriptRoot "..\shared"
$stage = Join-Path $PSScriptRoot "stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item main.js, preload.js, adapter.js $stage
Copy-Item (Join-Path $shared "index.html"), (Join-Path $shared "app.js"), (Join-Path $shared "style.css") $stage
@{ name = "electron-compare"; version = "1.0.0"; main = "main.js"; description = "Boson comparison reference app" } | ConvertTo-Json | Set-Content -Encoding ascii (Join-Path $stage "package.json")

if (-not (Test-Path node_modules\.bin\electron-packager.cmd)) {
    & npm install --no-audit --no-fund --loglevel=error
    if ($LASTEXITCODE -ne 0) { throw "npm install failed" }
}
$previous = Join-Path $PSScriptRoot "dist\electron-compare-win32-x64"
for ($i = 0; $i -lt 5 -and (Test-Path $previous); $i++) { Remove-Item -Recurse -Force $previous -ErrorAction SilentlyContinue; if (Test-Path $previous) { Start-Sleep -Seconds 2 } }
$electronVersion = (Get-Content (Join-Path $PSScriptRoot "electron-version.txt") -Raw).Trim()
& npx --no-install electron-packager $stage electron-compare --platform=win32 --arch=x64 --out=dist --overwrite --asar --electron-version=$electronVersion --no-prune --quiet
if ($LASTEXITCODE -ne 0) { throw "electron-packager failed" }
$exe = Join-Path $PSScriptRoot "dist\electron-compare-win32-x64\electron-compare.exe"
if (-not (Test-Path $exe)) { throw "missing $exe" }
Write-Host "[electron] built $exe"
