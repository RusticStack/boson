# Signing test: creates a throw-away self-signed code-signing certificate in Cert:\CurrentUser\My, signs with
# cmake/BosonSign.cmake by thumbprint and by PFX (+ password env var), packs and signs an MSIX, verifies the
# signatures, and always removes the certificate and every file it created.
param(
    [Parameter(Mandatory = $true)][string]$CMake,
    [Parameter(Mandatory = $true)][string]$SignScript,
    [Parameter(Mandatory = $true)][string]$SignTool,
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$MakeAppx = '',
    [string]$Manifest = '',
    [string]$AssetScript = ''
)
# Native tools write to stderr; with 'Stop' Windows PowerShell would turn that into terminating errors.
$ErrorActionPreference = 'Continue'
# The Cert: drive and New-SelfSignedCertificate come from these modules; autoloading is not guaranteed
# when PSModulePath is inherited from a non-PowerShell parent (ctest, bash).
if (-not (Get-PSDrive -Name Cert -ErrorAction SilentlyContinue)) {
    # An inherited PowerShell 7 PSModulePath makes Windows PowerShell pick incompatible module versions.
    $env:PSModulePath = Join-Path $PSHOME 'Modules'
    Import-Module (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Security') -ErrorAction SilentlyContinue
}
Import-Module (Join-Path $PSHOME 'Modules\PKI')
$failures = 0
$checks = 0
function Check([bool]$cond, [string]$what) {
    $script:checks++
    if (-not $cond) { $script:failures++; Write-Host "[FAIL] $what" } else { Write-Host "[ok]   $what" }
}

$tag = [guid]::NewGuid().ToString('N').Substring(0, 12)
$subject = "CN=Boson Signing Test $tag"
$work = Join-Path ([IO.Path]::GetTempPath()) "boson-signing-$tag"
New-Item -ItemType Directory -Force -Path $work | Out-Null
$cert = $null
try {
    $certArgs = @{
        Type = 'CodeSigningCert'; Subject = $subject; CertStoreLocation = 'Cert:\CurrentUser\My'
        KeyAlgorithm = 'RSA'; KeyLength = 2048; HashAlgorithm = 'SHA256'; KeyExportPolicy = 'Exportable'
        NotAfter = (Get-Date).AddDays(1)
    }
    $cert = New-SelfSignedCertificate @certArgs -ErrorAction Stop
    $thumb = $cert.Thumbprint
    Write-Host "created $subject ($thumb)"

    # 1) By thumbprint, no timestamp (no network in tests).
    $a = Join-Path $work 'by-thumbprint.exe'
    Copy-Item $Exe $a
    & $CMake "-DBOSON_SIGNTOOL=$SignTool" "-DBOSON_SIGN_FILES=$a" "-DBOSON_SIGN_THUMBPRINT=$thumb" "-DBOSON_SIGN_TIMESTAMP_URL=" "-DBOSON_SIGN_DESCRIPTION=Boson test" -P $SignScript | Out-Host
    Check ($LASTEXITCODE -eq 0) 'BosonSign.cmake signs by thumbprint'
    $sig = Get-AuthenticodeSignature $a
    Check ($null -ne $sig.SignerCertificate -and $sig.SignerCertificate.Thumbprint -eq $thumb) 'thumbprint signature carries the test certificate'
    Check ($sig.Status -ne 'NotSigned' -and $sig.Status -ne 'HashMismatch') "signature is intact (status $($sig.Status): the self-signed root is untrusted, as expected)"
    & $SignTool verify /pa /all $a 2>&1 | Out-Null
    Check ($LASTEXITCODE -ne 0) 'signtool verify /pa rejects the untrusted self-signed chain'

    # 2) By PFX with the password in an environment variable.
    $pfx = Join-Path $work 'test.pfx'
    $password = [guid]::NewGuid().ToString()
    Export-PfxCertificate -Cert $cert -FilePath $pfx -Password (ConvertTo-SecureString $password -AsPlainText -Force) | Out-Null
    $b = Join-Path $work 'by-pfx.exe'
    Copy-Item $Exe $b
    $env:BOSON_TEST_PFX_PASSWORD = $password
    & $CMake "-DBOSON_SIGNTOOL=$SignTool" "-DBOSON_SIGN_FILES=$b" "-DBOSON_SIGN_PFX=$pfx" "-DBOSON_SIGN_PFX_PASSWORD_ENV=BOSON_TEST_PFX_PASSWORD" "-DBOSON_SIGN_TIMESTAMP_URL=" -P $SignScript | Out-Host
    Check ($LASTEXITCODE -eq 0) 'BosonSign.cmake signs with a PFX + password env var'
    Check ((Get-AuthenticodeSignature $b).SignerCertificate.Thumbprint -eq $thumb) 'PFX signature carries the test certificate'
    Remove-Item Env:\BOSON_TEST_PFX_PASSWORD

    # 3) Misconfiguration fails loudly.
    $c = Join-Path $work 'unsigned.exe'
    Copy-Item $Exe $c
    & $CMake "-DBOSON_SIGNTOOL=$SignTool" "-DBOSON_SIGN_FILES=$c" -P $SignScript 2>&1 | Out-Null
    Check ($LASTEXITCODE -ne 0) 'no certificate selected -> error'
    & $CMake "-DBOSON_SIGNTOOL=$SignTool" "-DBOSON_SIGN_FILES=$c" "-DBOSON_SIGN_PFX=$pfx" "-DBOSON_SIGN_PFX_PASSWORD_ENV=BOSON_TEST_UNSET_$tag" -P $SignScript 2>&1 | Out-Null
    Check ($LASTEXITCODE -ne 0) 'missing password env var -> error'
    Check ((Get-AuthenticodeSignature $c).Status -eq 'NotSigned') 'failed runs leave the file unsigned'

    # 4) MSIX: pack a layout whose Publisher is the test certificate subject, then sign it.
    if ($MakeAppx -and (Test-Path $MakeAppx) -and $Manifest -and (Test-Path $Manifest)) {
        $layout = Join-Path $work 'layout'
        New-Item -ItemType Directory -Force -Path (Join-Path $layout 'Assets') | Out-Null
        [xml]$xml = Get-Content -Raw $Manifest
        $exeName = $xml.Package.Applications.Application.Executable
        Copy-Item $Exe (Join-Path $layout $exeName)
        $xml.Package.Identity.Publisher = $subject
        $xml.Save((Join-Path $layout 'AppxManifest.xml'))
        & powershell -NoProfile -ExecutionPolicy Bypass -File $AssetScript -OutDir (Join-Path $layout 'Assets') -Name 'Test' | Out-Null
        $msix = Join-Path $work 'test.msix'
        & $MakeAppx pack /o /h SHA256 /d $layout /p $msix | Out-Null
        Check ($LASTEXITCODE -eq 0 -and (Test-Path $msix)) 'makeappx packs the generated MSIX layout'
        & $CMake "-DBOSON_SIGNTOOL=$SignTool" "-DBOSON_SIGN_FILES=$msix" "-DBOSON_SIGN_THUMBPRINT=$thumb" "-DBOSON_SIGN_TIMESTAMP_URL=" -P $SignScript | Out-Host
        Check ($LASTEXITCODE -eq 0) 'signtool signs the MSIX (Publisher == certificate subject)'
        Check ((Get-AuthenticodeSignature $msix).SignerCertificate.Thumbprint -eq $thumb) 'MSIX signature carries the test certificate'
    } else {
        Write-Host 'SKIP: MSIX signing (makeappx or the generated AppxManifest.xml is missing)'
    }
} finally {
    if ($null -ne $cert) {
        # Delete the CNG private key file first, then the certificate from every CurrentUser store.
        try {
            $rsa = [System.Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPrivateKey($cert)
            if ($rsa -is [System.Security.Cryptography.RSACng]) { $rsa.Key.Delete() }
        } catch { Write-Host "warning: could not delete the private key: $_" }
        foreach ($name in @('My', 'CA', 'Root', 'TrustedPublisher')) {
            $store = New-Object System.Security.Cryptography.X509Certificates.X509Store($name, 'CurrentUser')
            $store.Open('ReadWrite')
            foreach ($c2 in @($store.Certificates | Where-Object { $_.Thumbprint -eq $cert.Thumbprint })) { $store.Remove($c2) }
            $store.Close()
        }
        $left = @(Get-ChildItem Cert:\CurrentUser -Recurse | Where-Object { $_.Thumbprint -eq $cert.Thumbprint })
        Check ($left.Count -eq 0) 'test certificate removed from every CurrentUser store'
    }
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
Write-Host "$($checks - $failures)/$checks signing checks passed"
if ($failures -gt 0) { exit 1 }
exit 0
