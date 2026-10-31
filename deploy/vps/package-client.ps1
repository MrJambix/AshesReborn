# SPDX-License-Identifier: AGPL-3.0-only
# Part of a modified SandalCore deployment (branch vps-deploy); not the original project or endorsed by Cyb.
<#
  Run on the build PC, with everything committed. Makes the zip players download: the launcher, the tether, this
  server's CA certificate and settings, and the source of this commit (the AGPL asks for it). No game files, no keys.
#>
param(
    [Parameter(Mandatory)] [string] $ServerIp,
    [int] $GatewayPort = 8443,
    [int] $LoginPort = 8444,
    [string] $CaFile = "$env:USERPROFILE\SandalCoreDeploy\vps-certs\ca.crt",
    [string] $OutFile = "$env:USERPROFILE\SandalCoreDeploy\RebornLauncher.zip"
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (git -C $repo status --porcelain) { throw 'Commit first: the source in the pack must be what this server runs.' }
$stage = "$OutFile.stage\RebornLauncher"
if (Test-Path "$OutFile.stage") { Remove-Item -Recurse -Force "$OutFile.stage" }
New-Item -ItemType Directory -Force "$stage\config", "$stage\source" | Out-Null
Copy-Item "$repo\dist\Release\launcher.exe" "$stage\RebornLauncher.exe"
Copy-Item "$repo\dist\Release\tether_server.exe", "$repo\dist\Release\*.dll" $stage
Copy-Item "$PSScriptRoot\client\*" $stage
Copy-Item $CaFile "$stage\ca.crt"
Copy-Item "$repo\LICENSE", "$repo\NOTICE" $stage
Copy-Item "$repo\config\launcher.json" "$stage\config"

$server = Get-Content "$repo\config\server.json" -Raw | ConvertFrom-Json
$server.ics_host = $ServerIp
$server.ics_port = $GatewayPort
$server.auth_server_host = $ServerIp
$server.auth_server_port = $LoginPort
$server | Add-Member -NotePropertyName auth_server_tls -NotePropertyValue $true
$server.log_level = 'info'
$server | ConvertTo-Json | Set-Content "$stage\config\server.json" -Encoding ascii
'{ "game_executable": "" }' | Set-Content "$stage\config\paths.json" -Encoding ascii

$head = git -C $repo rev-parse --short HEAD
git -C $repo archive --format=zip -o "$stage\source\SandalCore-$head.zip" HEAD
if ($LASTEXITCODE -ne 0) { throw 'git archive failed' }

if (Test-Path $OutFile) { Remove-Item -Force $OutFile }
Compress-Archive -Path $stage -DestinationPath $OutFile
Remove-Item -Recurse -Force "$OutFile.stage"
Write-Host ("client pack: {0} ({1:n0} MB), commit {2}" -f $OutFile, ((Get-Item $OutFile).Length / 1MB), $head)
