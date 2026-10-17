#Requires -RunAsAdministrator
<#
.SYNOPSIS
  Installs the Virtual Webcam driver from this folder.

.DESCRIPTION
  Windows only loads kernel drivers that are signed. This script signs the package with a
  certificate it generates on this machine (the private key never leaves it), trusts that
  certificate, makes sure test signing is on, and installs the driver with vcamctl.exe.

  Test signing cannot be turned on while Secure Boot is enabled. Disable Secure Boot in
  firmware first, or use a driver signed through the Microsoft Hardware Dev Center instead.
#>
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$subject = 'CN=vcam test signing'

foreach ($f in 'vcam.sys', 'vcam.inf', 'vcam.cat', 'vcamctl.exe') {
    if (-not (Test-Path (Join-Path $dir $f))) { throw "Missing $f next to this script." }
}

# 1. Test signing has to be on for a self-signed driver to load.
if ((bcdedit /enum '{current}' | Out-String) -notmatch 'testsigning\s+Yes') {
    bcdedit /set testsigning on | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not enable test signing. If Secure Boot is on, turn it off in firmware and run this again.'
    }
    Write-Host 'Test signing is now enabled. Restart Windows, then run this script again.'
    return
}

# 2. A local code-signing certificate, trusted as root and as publisher.
$cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object Subject -eq $subject | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject `
        -CertStoreLocation Cert:\LocalMachine\My -HashAlgorithm SHA256 `
        -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddYears(5)
}
foreach ($name in 'Root', 'TrustedPublisher') {
    $store = New-Object System.Security.Cryptography.X509Certificates.X509Store($name, 'LocalMachine')
    $store.Open('ReadWrite')
    $store.Add($cert)
    $store.Close()
}

# 3. Sign the driver and its catalog.
$signed = Set-AuthenticodeSignature -FilePath (Join-Path $dir 'vcam.sys'), (Join-Path $dir 'vcam.cat') `
    -Certificate $cert -HashAlgorithm SHA256
$bad = $signed | Where-Object Status -ne 'Valid'
if ($bad) { throw "Signing failed: $($bad | ForEach-Object { "$($_.Path): $($_.StatusMessage)" })" }

# 4. Create the device and install the driver.
& (Join-Path $dir 'vcamctl.exe') install (Join-Path $dir 'vcam.inf')
if ($LASTEXITCODE -ne 0) { throw "vcamctl install failed with exit code $LASTEXITCODE." }
