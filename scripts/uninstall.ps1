#Requires -RunAsAdministrator
<#
.SYNOPSIS
  Removes the Virtual Webcam device, its driver package and the test certificate.
  Test signing is left as it is; turn it off with `bcdedit /set testsigning off` if you want.
#>
$ErrorActionPreference = 'Stop'
$subject = 'CN=vcam test signing'

& (Join-Path $PSScriptRoot 'vcamctl.exe') uninstall
if ($LASTEXITCODE -ne 0) { throw "vcamctl uninstall failed with exit code $LASTEXITCODE." }

foreach ($name in 'My', 'Root', 'TrustedPublisher') {
    Get-ChildItem "Cert:\LocalMachine\$name" | Where-Object Subject -eq $subject | Remove-Item
}
