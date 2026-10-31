# SPDX-License-Identifier: AGPL-3.0-only
# Part of a modified SandalCore deployment (branch vps-deploy); not the original project or endorsed by Cyb.
<#
  Run on the build PC. Makes the CA and the server certificate for -PublicIp in -CertDir, unless they exist. The CA's
  name is what players read in the Windows warning. Its key is deleted right after signing, as make_certs.ps1 does.
#>
param(
    [Parameter(Mandatory)] [string] $PublicIp,
    [string] $CertDir = "$env:USERPROFILE\SandalCoreDeploy\vps-certs"
)
$ErrorActionPreference = 'Stop'
if (Test-Path "$CertDir\server.pem") { exit 0 }
New-Item -ItemType Directory -Force $CertDir | Out-Null
$ossl = "$env:ProgramFiles\Git\usr\bin\openssl.exe"
if (-not (Test-Path $ossl)) { $ossl = "$env:ProgramFiles\Git\mingw64\bin\openssl.exe" }
$env:MSYS2_ARG_CONV_EXCL = '*'; $env:MSYS_NO_PATHCONV = '1'
$ext = "$CertDir\ext.cnf"
try {
    cmd /c "`"$ossl`" req -quiet -x509 -newkey rsa:2048 -nodes -sha256 -days 3650 -keyout `"$CertDir\ca.key`" -out `"$CertDir\ca.crt`" -subj `"/CN=Reborn Launcher CA`" -addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,keyCertSign,cRLSign 2>nul"
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
