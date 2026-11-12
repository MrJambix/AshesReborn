# SPDX-License-Identifier: AGPL-3.0-only
# Part of a modified SandalCore deployment (branch vps-deploy); not the original project or endorsed by Cyb.
<#
  Run on the build PC. Makes the package the VPS installs: the server programs, the game data the servers
  read, and the ops scripts. It never contains the game client. The first run also makes the TLS
  certificates for -PublicIp in -CertDir (make-certs.ps1).
#>
param(
    [Parameter(Mandatory)] [string] $PublicIp,
    [string] $CertDir = "$env:USERPROFILE\SandalCoreDeploy\vps-certs",
    [string] $OutFile = "$env:USERPROFILE\SandalCoreDeploy\sandalcore-package.tar"
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$stage = "$OutFile.stage"

& "$PSScriptRoot\make-certs.ps1" -PublicIp $PublicIp -CertDir $CertDir

if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
foreach ($d in 'dist\Release', 'dist\services', 'dist\content', 'deploy', 'ops', 'certs', 'config') { New-Item -ItemType Directory -Force "$stage\$d" | Out-Null }
Copy-Item -LiteralPath "$repo\config\realm.json", "$repo\config\rates.json" -Destination "$stage\config"
Copy-Item "$repo\dist\Release\worldd.exe", "$repo\dist\Release\*.dll" "$stage\dist\Release"
foreach ($n in 'auth', 'character', 'citynode', 'quests', 'mesh', 'gateway', 'nodectl', 'questctl', 'worldctl', 'charimport') {
    Copy-Item "$repo\dist\services\$n.exe" "$stage\dist\services"
}
# Everything but area_geom.bin (only aoc_navbuild reads it, at build time) and the extraction logs.
robocopy "$repo\dist\content" "$stage\dist\content" /E /XF area_geom.bin /XD .extract /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw 'could not copy dist\content' }
Copy-Item "$repo\PATCH_NOTES.md" "$stage\dist\services"
Copy-Item "$repo\deploy\citynode_seed.json" "$stage\deploy"
Copy-Item "$PSScriptRoot\install.ps1", "$PSScriptRoot\supervisor.ps1", "$PSScriptRoot\sandalctl.ps1", "$PSScriptRoot\sendctrlc.ps1", "$PSScriptRoot\notify-discord.ps1", "$PSScriptRoot\enable-crash-dumps.ps1", "$PSScriptRoot\README.md" "$stage\ops"
Copy-Item "$repo\LICENSE", "$repo\NOTICE" $stage
Copy-Item "$CertDir\server.pem", "$CertDir\server.key", "$CertDir\ca.crt" "$stage\certs"
(git -C $repo rev-parse HEAD) + $(if (git -C $repo status --porcelain) { ' (with uncommitted changes)' }) | Set-Content "$stage\VERSION.txt" -Encoding ascii

if (Test-Path $OutFile) { Remove-Item -Force $OutFile }
tar -cf $OutFile -C $stage .
if ($LASTEXITCODE -ne 0) { throw 'tar failed' }
Remove-Item -Recurse -Force $stage
Write-Host ("package: {0} ({1:n0} MB), SHA256 {2}" -f $OutFile, ((Get-Item $OutFile).Length / 1MB), (Get-FileHash $OutFile -Algorithm SHA256).Hash)
