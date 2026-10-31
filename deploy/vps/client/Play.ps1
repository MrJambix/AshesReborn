# SPDX-License-Identifier: AGPL-3.0-only
# Part of a modified SandalCore deployment (branch vps-deploy); not the original project or endorsed by Cyb.
# Trusts this server's certificate for this Windows user (Windows asks once), then starts the tether and the launcher.
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$caFile = "$here\ca.crt"
$ca = New-Object Security.Cryptography.X509Certificates.X509Certificate2 $caFile
if (-not (Get-ChildItem Cert:\CurrentUser\Root | Where-Object Thumbprint -EQ $ca.Thumbprint)) {
    Write-Host 'Windows will ask whether to trust this server''s certificate. Choose Yes, or the game cannot connect.'
    & certutil -user -addstore Root $caFile | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'The certificate was not added, so the game would refuse the server.' }
}
New-Item -ItemType Directory -Force "$here\data", "$here\logs" | Out-Null
Get-Process tether_server, launcher -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($here) } | Stop-Process -Force
if (Test-Path "$here\data\ics_auth_token.txt") { Remove-Item -Force "$here\data\ics_auth_token.txt" }
Start-Process "$here\tether_server.exe" -ArgumentList '--login-token-file' -WorkingDirectory $here -WindowStyle Minimized
Start-Sleep 2
Start-Process "$here\launcher.exe" -WorkingDirectory $here
