# SPDX-License-Identifier: AGPL-3.0-only
# Part of a modified SandalCore deployment (branch vps-deploy); not the original project or endorsed by Cyb.
<#
  Run on the gaming PC, from the build checkout, to play on the VPS. It trusts the VPS's CA for this Windows user
  (Windows asks once), opens an SSH tunnel so the launcher's sign-in stays encrypted, and starts the tether and the
  launcher, pointed at the VPS. Sign in or register in the launcher and press LAUNCH GAME. Close the tunnel's window
  when you are done. ssh asks for the VPS password in that window.
#>
param(
    [string] $ServerIp = '149.28.38.151',
    [int] $GatewayPort = 8443,
    [string] $CaFile = "$env:USERPROFILE\SandalCoreDeploy\vps-certs\ca.crt"
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$dist = "$repo\dist\Release"

$ca = New-Object Security.Cryptography.X509Certificates.X509Certificate2 $CaFile
if (-not (Get-ChildItem Cert:\CurrentUser\Root | Where-Object Thumbprint -EQ $ca.Thumbprint)) {
    Write-Host 'Adding the SandalCore VPS CA to your trust store: choose Yes in the Windows warning.'
    & certutil -user -addstore Root $CaFile | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'the CA was not added, so the game would refuse the server' }
}

New-Item -ItemType Directory -Force "$dist\config", "$dist\data", "$dist\logs" | Out-Null
Copy-Item "$repo\config\*" "$dist\config" -Exclude '*.local.json', 'paths.json' -Force
$server = Get-Content "$repo\config\server.json" -Raw | ConvertFrom-Json
$server.ics_host = $ServerIp
$server.ics_port = $GatewayPort
# The sign-in goes to this PC's end of the tunnel, so the launcher's plain HTTP never crosses the internet.
$server.auth_server_host = '127.0.0.1'
$server.auth_server_port = 8081
$server | ConvertTo-Json | Set-Content "$dist\config\server.json" -Encoding ascii

Get-Process tether_server, launcher -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($dist) } | Stop-Process -Force
if (Test-Path "$dist\data\ics_auth_token.txt") { Remove-Item -Force "$dist\data\ics_auth_token.txt" }

Start-Process ssh -ArgumentList "-N -o ExitOnForwardFailure=yes -L 127.0.0.1:8081:127.0.0.1:8081 Administrator@$ServerIp"
Write-Host 'Waiting for the tunnel: enter the VPS password in the ssh window ...'
$until = (Get-Date).AddMinutes(2); $up = $false
while (-not $up -and (Get-Date) -lt $until) {
    Start-Sleep 2
    try { $up = (Invoke-WebRequest 'http://127.0.0.1:8081/health' -UseBasicParsing -TimeoutSec 3).StatusCode -eq 200 } catch {}
}
if (-not $up) { throw 'the sign-in server cannot be reached through the tunnel' }

Start-Process "$dist\tether_server.exe" -ArgumentList '--login-token-file' -WorkingDirectory $dist -WindowStyle Minimized
Start-Sleep 2
Start-Process "$dist\launcher.exe" -WorkingDirectory $dist
Write-Host 'Ready: sign in or register in the launcher, then press LAUNCH GAME.'
