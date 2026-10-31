# SPDX-License-Identifier: AGPL-3.0-only
# Part of a modified SandalCore deployment (branch vps-deploy); not the original project or endorsed by Cyb.
<#
  Run on the build PC. Makes the package the VPS installs: the server programs, the game data the servers
  read, and the ops scripts. It never contains the game client. The first run also makes the TLS
  certificates for -PublicIp in -CertDir; their CA key is deleted right after signing, as make_certs.ps1 does.
#>
param(
    [Parameter(Mandatory)] [string] $PublicIp,
    [string] $CertDir = "$env:USERPROFILE\SandalCoreDeploy\vps-certs",
    [string] $OutFile = "$env:USERPROFILE\SandalCoreDeploy\sandalcore-package.tar"
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$stage = "$OutFile.stage"

if (-not (Test-Path "$CertDir\server.pem")) {
    New-Item -ItemType Directory -Force $CertDir | Out-Null
    $ossl = "$env:ProgramFiles\Git\usr\bin\openssl.exe"
    if (-not (Test-Path $ossl)) { $ossl = "$env:ProgramFiles\Git\mingw64\bin\openssl.exe" }
    $env:MSYS2_ARG_CONV_EXCL = '*'; $env:MSYS_NO_PATHCONV = '1'
    $ext = "$CertDir\ext.cnf"
    try {
        cmd /c "`"$ossl`" req -quiet -x509 -newkey rsa:2048 -nodes -sha256 -days 3650 -keyout `"$CertDir\ca.key`" -out `"$CertDir\ca.crt`" -subj `"/CN=SandalCore VPS CA`" -addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,keyCertSign,cRLSign 2>nul"
        if ($LASTEXITCODE -ne 0) { throw 'could not make the CA' }
        cmd /c "`"$ossl`" req -quiet -newkey rsa:2048 -nodes -sha256 -keyout `"$CertDir\server.key`" -out `"$CertDir\server.csr`" -subj `"/CN=$PublicIp`" 2>nul"
        if ($LASTEXITCODE -ne 0) { throw 'could not make the server key' }
        Set-Content $ext -Encoding ascii -Value @(
            'basicConstraints=CA:FALSE', 'keyUsage=critical,digitalSignature,keyEncipherment',
            'extendedKeyUsage=serverAuth', "subjectAltName=IP:$PublicIp")
        cmd /c "`"$ossl`" x509 -req -sha256 -days 825 -in `"$CertDir\server.csr`" -CA `"$CertDir\ca.crt`" -CAkey `"$CertDir\ca.key`" -set_serial 0x$('{0:x}' -f (Get-Random)) -extfile `"$ext`" -out `"$CertDir\server.pem`" 2>nul"
        if ($LASTEXITCODE -ne 0) { throw 'could not sign the server certificate' }
    } finally {
        foreach ($f in "$CertDir\ca.key", "$CertDir\server.csr", $ext) { if (Test-Path $f) { Remove-Item -Force $f } }
    }
}

if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
foreach ($d in 'dist\Release', 'dist\services', 'dist\content', 'deploy', 'ops', 'certs') { New-Item -ItemType Directory -Force "$stage\$d" | Out-Null }
Copy-Item "$repo\dist\Release\worldd.exe", "$repo\dist\Release\*.dll" "$stage\dist\Release"
foreach ($n in 'auth', 'character', 'citynode', 'quests', 'mesh', 'gateway', 'nodectl', 'questctl', 'worldctl', 'charimport') {
    Copy-Item "$repo\dist\services\$n.exe" "$stage\dist\services"
}
# Everything but area_geom.bin (only aoc_navbuild reads it, at build time) and the extraction logs.
robocopy "$repo\dist\content" "$stage\dist\content" /E /XF area_geom.bin /XD .extract /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw 'could not copy dist\content' }
Copy-Item "$repo\deploy\citynode_seed.json" "$stage\deploy"
Copy-Item "$PSScriptRoot\install.ps1", "$PSScriptRoot\supervisor.ps1", "$PSScriptRoot\sandalctl.ps1", "$PSScriptRoot\sendctrlc.ps1", "$PSScriptRoot\README.md" "$stage\ops"
Copy-Item "$repo\LICENSE", "$repo\NOTICE" $stage
Copy-Item "$CertDir\server.pem", "$CertDir\server.key", "$CertDir\ca.crt" "$stage\certs"
(git -C $repo rev-parse HEAD) + $(if (git -C $repo status --porcelain) { ' (with uncommitted changes)' }) | Set-Content "$stage\VERSION.txt" -Encoding ascii

if (Test-Path $OutFile) { Remove-Item -Force $OutFile }
tar -cf $OutFile -C $stage .
if ($LASTEXITCODE -ne 0) { throw 'tar failed' }
Remove-Item -Recurse -Force $stage
Write-Host ("package: {0} ({1:n0} MB), SHA256 {2}" -f $OutFile, ((Get-Item $OutFile).Length / 1MB), (Get-FileHash $OutFile -Algorithm SHA256).Hash)
