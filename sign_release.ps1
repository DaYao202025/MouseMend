<#
.SYNOPSIS
    Signs the MouseMend executables with a self-signed code signing certificate.

.DESCRIPTION
    What self-signing actually buys you (be realistic):
      + the file gets a verifiable integrity signature, so tampering is detectable
      + on machines that trust the certificate, the UAC prompt shows a name
        instead of "Unknown publisher"
      - it does NOT buy SmartScreen reputation. On a fresh machine Windows will
        still show "Windows protected your PC" until the certificate is trusted
        there (or until the file has enough download history).

    The certificate is created in the current user's personal store. -InstallTrust
    additionally trusts it for the current user (no admin needed), which is what
    makes the signature validate locally. -Remove undoes everything.

.PARAMETER InstallTrust
    Trust the certificate for the current user (Root + TrustedPublisher) so the
    signature validates on this machine.

.PARAMETER Remove
    Delete the certificate from the current user's stores and remove the exported
    .cer. Signatures already embedded in files stop validating afterwards.

.PARAMETER TimestampUrl
    RFC 3161 timestamp server. Pass an empty string to skip timestamping; without
    one the signature expires together with the certificate.

.EXAMPLE
    pwsh -File sign_release.ps1
    pwsh -File sign_release.ps1 -InstallTrust
    pwsh -File sign_release.ps1 -Remove
#>

[CmdletBinding()]
param(
    [string]$Subject = 'CN=MouseMend Self-Signed, O=MouseMend, C=CN',
    [string]$CertificatePath = '',
    [string]$TimestampUrl = 'http://timestamp.digicert.com',
    [switch]$InstallTrust,
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'

$repoRoot = $PSScriptRoot
$certFile = if ($CertificatePath) { $CertificatePath } else { Join-Path $repoRoot 'MouseMend-selfsigned.cer' }
$targets = @(
    (Join-Path $repoRoot 'MouseMend\bin\x64\Release\MouseMend-x64.exe'),
    (Join-Path $repoRoot 'MouseMend\bin\x86\Release\MouseMend-x86.exe')
)

# Matching is done by CN prefix rather than by the full subject, because the
# certificate created before the rename (CN=MouseFix Self-Signed) is still
# trusted in this user's Root store. -Remove has to be able to find it,
# otherwise a stale trusted root survives the rename forever.
$certSubjectPatterns = @(
    $Subject,
    'CN=MouseFix Self-Signed*'
)

function Get-MouseMendCertificates {
    foreach ($pattern in $certSubjectPatterns) {
        Get-ChildItem Cert:\CurrentUser\My -ErrorAction SilentlyContinue |
            Where-Object { $_.Subject -like $pattern }
    }
}

function Get-MouseMendCertificate {
    Get-MouseMendCertificates |
        Where-Object { $_.HasPrivateKey } |
        Sort-Object NotAfter -Descending |
        Select-Object -First 1
}

# Remove-Item on Cert:\CurrentUser\Root pops a confirmation dialog, which fails
# in a non-interactive shell. The .NET store API deletes the entry directly.
function Remove-CertificateFromStore {
    param(
        [string]$StoreName,
        [System.Security.Cryptography.X509Certificates.X509Certificate2]$Certificate
    )

    $store = New-Object System.Security.Cryptography.X509Certificates.X509Store($StoreName, 'CurrentUser')
    try {
        $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
        $matches = @($store.Certificates | Where-Object { $_.Thumbprint -eq $Certificate.Thumbprint })
        foreach ($entry in $matches) { $store.Remove($entry) }
        return $matches.Count
    }
    finally {
        $store.Close()
    }
}

function Find-SignTool {
    $sdk = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
    if (-not (Test-Path $sdk)) { return $null }

    Get-ChildItem $sdk -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\x64\\signtool\.exe$' } |
        Sort-Object FullName -Descending |
        Select-Object -First 1 -ExpandProperty FullName
}

# ---------------------------------------------------------------- Remove
if ($Remove) {
    $stale = @(Get-MouseMendCertificates)
    if ($stale.Count -eq 0) {
        Write-Host 'No MouseMend certificate found in Cert:\CurrentUser\My' -ForegroundColor Yellow
    }
    else {
        foreach ($old in $stale) {
            # Trust anchors first, the copy that owns the private key last.
            foreach ($storeName in @('Root', 'TrustedPublisher', 'My')) {
                $removed = Remove-CertificateFromStore -StoreName $storeName -Certificate $old
                Write-Host ("  {0,-16} {1} entry(ies)" -f $storeName, $removed) -ForegroundColor DarkGray
            }

            Write-Host "Removed $($old.Thumbprint)  $($old.Subject)" -ForegroundColor Green
        }
    }

    foreach ($file in @($certFile, (Join-Path $repoRoot 'MouseFix-selfsigned.cer'))) {
        if (Test-Path $file) {
            Remove-Item $file -Force
            Write-Host "Removed $file" -ForegroundColor Green
        }
    }

    exit 0
}

# ---------------------------------------------------------------- Certificate
$cert = Get-MouseMendCertificate

if (-not $cert) {
    Write-Host "Creating self-signed code signing certificate: $Subject" -ForegroundColor Cyan

    $cert = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject $Subject `
        -KeyAlgorithm RSA `
        -KeyLength 3072 `
        -HashAlgorithm SHA256 `
        -KeyExportPolicy Exportable `
        -KeyUsage DigitalSignature `
        -CertStoreLocation 'Cert:\CurrentUser\My' `
        -NotAfter (Get-Date).AddYears(5)

    Write-Host "Created $($cert.Thumbprint), valid until $($cert.NotAfter.ToString('yyyy-MM-dd'))" -ForegroundColor Green
}
else {
    Write-Host "Reusing $($cert.Thumbprint) (expires $($cert.NotAfter.ToString('yyyy-MM-dd')))" -ForegroundColor Cyan
}

if ($cert.NotAfter -lt (Get-Date)) {
    Write-Host 'The certificate has expired. Run with -Remove and then again.' -ForegroundColor Red
    exit 1
}

# Export the public certificate so other machines can trust it
Export-Certificate -Cert $cert -FilePath $certFile -Type CERT | Out-Null
Write-Host "Public certificate: $certFile" -ForegroundColor Cyan

# ---------------------------------------------------------------- Trust (optional, per user)
if ($InstallTrust) {
    foreach ($store in @('Root', 'TrustedPublisher')) {
        $destination = "Cert:\CurrentUser\$store"
        $already = Get-ChildItem $destination -ErrorAction SilentlyContinue |
            Where-Object { $_.Thumbprint -eq $cert.Thumbprint }

        if ($already) {
            Write-Host "Already trusted in $store" -ForegroundColor DarkGray
            continue
        }

        Import-Certificate -FilePath $certFile -CertStoreLocation $destination | Out-Null
        Write-Host "Trusted in $store (current user only)" -ForegroundColor Green
    }
}

# ---------------------------------------------------------------- Sign
$signTool = Find-SignTool
if ($signTool) {
    Write-Host "Using $signTool" -ForegroundColor DarkGray
}
else {
    Write-Host 'signtool.exe not found; falling back to Set-AuthenticodeSignature (requires -InstallTrust)' -ForegroundColor Yellow
}

$signed = 0
$failed = 0
$pending = 0

foreach ($exe in $targets) {
    if (-not (Test-Path $exe)) {
        Write-Host "Skipping (not built): $exe" -ForegroundColor DarkGray
        $pending++
        continue
    }

    $name = [System.IO.Path]::GetFileName($exe)
    $ok = $false

    if ($signTool) {
        $arguments = @('sign', '/sha1', $cert.Thumbprint, '/fd', 'SHA256')

        if ($TimestampUrl) {
            $arguments += @('/tr', $TimestampUrl, '/td', 'SHA256')
        }

        $arguments += @('/v', $exe)

        $output = & $signTool @arguments 2>&1
        $ok = ($LASTEXITCODE -eq 0)

        if (-not $ok -and $TimestampUrl) {
            # Offline or blocked timestamp server: sign without a timestamp
            Write-Host "Timestamp failed for $name; retrying without it" -ForegroundColor Yellow
            $retry = @('sign', '/sha1', $cert.Thumbprint, '/fd', 'SHA256', $exe)
            $output = & $signTool @retry 2>&1
            $ok = ($LASTEXITCODE -eq 0)
        }

        if (-not $ok) {
            $output | Where-Object { $_ -match 'Error|error' } | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
        }
    }
    else {
        try {
            $result = Set-AuthenticodeSignature -FilePath $exe -Certificate $cert -HashAlgorithm SHA256
            $ok = ($result.Status -eq 'Success')

            if (-not $ok) {
                Write-Host "  $($result.Status): $($result.StatusMessage)" -ForegroundColor DarkGray
            }
        }
        catch {
            Write-Host "  $($_.Exception.Message)" -ForegroundColor DarkGray
        }
    }

    if ($ok) {
        Write-Host "Signed $name" -ForegroundColor Green
        $signed++
    }
    else {
        Write-Host "Failed to sign $name" -ForegroundColor Red
        $failed++
    }
}

# ---------------------------------------------------------------- Verify
Write-Host ''
Write-Host 'Verification:' -ForegroundColor Cyan

foreach ($exe in $targets) {
    if (-not (Test-Path $exe)) { continue }

    $signature = Get-AuthenticodeSignature $exe
    $name = [System.IO.Path]::GetFileName($exe)
    $colour = if ($signature.Status -eq 'Valid') { 'Green' } else { 'Yellow' }

    Write-Host ("  {0,-18} {1}" -f $name, $signature.Status) -ForegroundColor $colour

    if ($signature.Status -eq 'UnknownError') {
        Write-Host '                   signature present, but this machine does not trust the certificate.' -ForegroundColor DarkGray
        Write-Host '                   run with -InstallTrust (current user) or import the .cer into Trusted Root.' -ForegroundColor DarkGray
    }
}

Write-Host ''

if ($signed -eq 0 -and $failed -eq 0) {
    Write-Host 'Nothing was signed. Build Release first:' -ForegroundColor Yellow
    Write-Host '  msbuild MouseMend\MouseMend.sln /t:Rebuild /p:Configuration=Release /p:Platform=x64' -ForegroundColor DarkGray
    exit 1
}

Write-Host "Done. $signed signed, $failed failed." -ForegroundColor Cyan
Write-Host 'Every rebuild overwrites the signature, so sign last.' -ForegroundColor DarkGray

if ($failed -gt 0) { exit 1 } else { exit 0 }
