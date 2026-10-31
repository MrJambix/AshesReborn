# SPDX-License-Identifier: AGPL-3.0-only
# Part of a modified SandalCore deployment; not the original project or endorsed by Cyb.
# Run after building and committing. Ships a native, self-extracting EXE plus a
# portable ZIP, including corresponding source, public CA and the MSVC runtime.
param(
    [Parameter(Mandatory)] [ValidateNotNullOrEmpty()] [string] $ServerIp,
    [ValidateRange(1,65535)] [int] $GatewayPort = 8443,
    [ValidateRange(1,65535)] [int] $LoginPort = 8444,
    [string] $CaFile = "$env:USERPROFILE\SandalCoreDeploy\vps-certs\ca.crt",
    [string] $OutFile = "$env:USERPROFILE\SandalCoreDeploy\RebornLauncher.exe",
    [string] $ZipFile
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (git -C $repo status --porcelain) { throw 'Commit first: the source in the pack must match the built launcher.' }
if ($LASTEXITCODE -ne 0) { throw 'Could not check the source checkout.' }
$OutFile = [IO.Path]::GetFullPath($OutFile)
if ([IO.Path]::GetExtension($OutFile) -ne '.exe') { throw '-OutFile must end in .exe.' }
if (-not $ZipFile) { $ZipFile = [IO.Path]::ChangeExtension($OutFile, '.zip') }
$ZipFile = [IO.Path]::GetFullPath($ZipFile)
if ([IO.Path]::GetExtension($ZipFile) -ne '.zip') { throw '-ZipFile must end in .zip.' }
foreach ($file in @("$repo\dist\Release\launcher.exe", "$repo\dist\Release\reborn_bootstrap.exe", $CaFile)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Required file missing: $file. Build launcher and reborn_bootstrap first." }
}

# Deploy the CRT beside the launcher so a clean Windows PC needs no installer.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsInstall = & $vswhere -latest -products '*' -property installationPath
$crt = Get-ChildItem -Path "$vsInstall\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT" -Directory |
    Sort-Object FullName -Descending | Select-Object -First 1
if (-not $crt) { throw 'The Visual C++ x64 runtime was not found in the build tools installation.' }

$stageRoot = Join-Path $repo ('dist\player-package-' + [guid]::NewGuid().ToString('N'))
$stage = Join-Path $stageRoot 'RebornLauncher'
New-Item -ItemType Directory -Force "$stage\config", "$stage\source" | Out-Null
try {
    Copy-Item -LiteralPath "$repo\dist\Release\launcher.exe" -Destination "$stage\RebornLauncher.exe"
    Copy-Item -Path "$repo\dist\Release\*.dll", "$($crt.FullName)\*.dll" -Destination $stage
    Copy-Item -Path "$PSScriptRoot\client\*" -Destination $stage
    Copy-Item -LiteralPath $CaFile -Destination "$stage\ca.crt"
    Copy-Item -LiteralPath "$repo\LICENSE", "$repo\NOTICE" -Destination $stage

    $server = Get-Content "$repo\config\server.json" -Raw | ConvertFrom-Json
    $server.ics_host = $ServerIp
    $server.ics_port = $GatewayPort
    $server.auth_server_host = $ServerIp
    $server.auth_server_port = $LoginPort
    $server | Add-Member -Force -NotePropertyName auth_server_tls -NotePropertyValue $true
    $server | Add-Member -Force -NotePropertyName managed_tether -NotePropertyValue $true
    $server.log_level = 'info'
    $server | ConvertTo-Json | Set-Content "$stage\config\server.json" -Encoding ascii
    @{ channel = 'alpha'; auth_base_url = "https://${ServerIp}:$LoginPort" } |
        ConvertTo-Json | Set-Content "$stage\config\launcher.json" -Encoding ascii
    '{ "game_executable": "" }' | Set-Content "$stage\config\paths.json" -Encoding ascii

    $head = git -C $repo rev-parse HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Could not determine source revision.' }
    git -C $repo archive --format=zip -o "$stage\source\SandalCore-$head.zip" HEAD
    if ($LASTEXITCODE -ne 0) { throw 'git archive failed' }
    $head | Set-Content "$stage\VERSION.txt" -Encoding ascii

    $files = @(Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName)
    $entries = @()
    for ($i = 0; $i -lt $files.Count; $i++) {
        $entries += [ordered]@{
            id = $i + 2
            path = $files[$i].FullName.Substring($stage.Length + 1).Replace('\', '/')
            sha256 = (Get-FileHash -LiteralPath $files[$i].FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    $manifest = [ordered]@{ commit = $head; files = $entries }
    $utf8 = New-Object Text.UTF8Encoding $false
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $digest = $sha.ComputeHash($utf8.GetBytes(($manifest | ConvertTo-Json -Depth 5 -Compress)))
    } finally { $sha.Dispose() }
    $manifest.payload_id = ([BitConverter]::ToString($digest)).Replace('-', '').ToLowerInvariant()
    $manifestFile = Join-Path $stageRoot 'manifest.json'
    [IO.File]::WriteAllText($manifestFile, ($manifest | ConvertTo-Json -Depth 5 -Compress), $utf8)

    # Embed files as Win32 resources, preserving the entry point's PE resources.
    if (-not ('RebornPackage.Resources' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
namespace RebornPackage {
    public static class Resources {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern IntPtr BeginUpdateResource(string file, bool deleteExisting);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool UpdateResource(IntPtr update, IntPtr type, IntPtr name, ushort language, byte[] data, uint length);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool EndUpdateResource(IntPtr update, bool discard);
        public static void Embed(string executable, string manifest, string[] files) {
            IntPtr update = BeginUpdateResource(executable, false);
            if (update == IntPtr.Zero) throw new Win32Exception();
            try {
                string[] all = new string[files.Length + 1];
                all[0] = manifest;
                Array.Copy(files, 0, all, 1, files.Length);
                for (int i = 0; i < all.Length; i++) {
                    byte[] bytes = File.ReadAllBytes(all[i]);
                    if (!UpdateResource(update, new IntPtr(10), new IntPtr(i + 1), 0, bytes, (uint)bytes.Length))
                        throw new Win32Exception();
                }
                IntPtr finished = update;
                update = IntPtr.Zero;
                if (!EndUpdateResource(finished, false)) throw new Win32Exception();
            } finally {
                if (update != IntPtr.Zero) EndUpdateResource(update, true);
            }
        }
    }
}
'@
    }
    $packed = Join-Path $stageRoot 'RebornLauncher.exe'
    Copy-Item -LiteralPath "$repo\dist\Release\reborn_bootstrap.exe" -Destination $packed
    [RebornPackage.Resources]::Embed($packed, $manifestFile, [string[]]@($files.FullName))

    # Exercise the actual download before replacing an existing release.
    $verify = Join-Path $stageRoot 'verify files'
    $smoke = Start-Process -FilePath $packed -ArgumentList @('--extract-only', ('"' + $verify + '"')) -Wait -PassThru -WindowStyle Hidden
    if ($smoke.ExitCode -ne 0) { throw "Packaged executable failed extraction (exit $($smoke.ExitCode))." }
    foreach ($entry in $entries) {
        $actual = (Get-FileHash -LiteralPath (Join-Path $verify $entry.path) -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $entry.sha256) { throw "Extraction verification failed: $($entry.path)" }
    }
    # A second launch preserves player settings and repairs missing/changed
    # application files, including when installed in a path containing spaces.
    $savedPath = '{ "game_executable": "D:\\Games\\Example Client.exe" }'
    [IO.File]::WriteAllText((Join-Path $verify 'config\paths.json'), $savedPath, $utf8)
    [IO.File]::WriteAllText((Join-Path $verify 'VERSION.txt'), 'damaged', $utf8)
    Remove-Item -LiteralPath (Join-Path $verify 'fmt.dll')
    $repair = Start-Process -FilePath $packed -ArgumentList @('--extract-only', ('"' + $verify + '"')) -Wait -PassThru -WindowStyle Hidden
    if ($repair.ExitCode -ne 0) { throw 'Packaged executable failed its repeat-launch check.' }
    if ([IO.File]::ReadAllText((Join-Path $verify 'config\paths.json')) -ne $savedPath) { throw 'Repeat launch replaced player settings.' }
    foreach ($name in @('VERSION.txt', 'fmt.dll')) {
        $entry = $entries | Where-Object path -EQ $name
        if ((Get-FileHash -LiteralPath (Join-Path $verify $name) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.sha256) {
            throw "Repeat launch did not repair $name."
        }
    }
    New-Item -ItemType Directory -Force ([IO.Path]::GetDirectoryName($OutFile)), ([IO.Path]::GetDirectoryName($ZipFile)) | Out-Null
    $zip = Join-Path $stageRoot 'RebornLauncher.zip'
    Compress-Archive -LiteralPath $stage -DestinationPath $zip
    Copy-Item -LiteralPath $packed -Destination $OutFile -Force
    Copy-Item -LiteralPath $zip -Destination $ZipFile -Force
    $checksum = (Get-FileHash -LiteralPath $OutFile -Algorithm SHA256).Hash.ToLowerInvariant()
    "$checksum  $([IO.Path]::GetFileName($OutFile))" | Set-Content "$OutFile.sha256" -Encoding ascii
    Write-Host ("Player EXE: {0} ({1:n1} MB), source {2}" -f $OutFile, ((Get-Item $OutFile).Length / 1MB), $head)
    Write-Host "Portable ZIP: $ZipFile"
    Write-Host "SHA256: $checksum"
} finally {
    # The only cleanup target is the GUID directory created above under dist.
    $expectedParent = [IO.Path]::GetFullPath((Join-Path $repo 'dist')).TrimEnd('\')
    if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($stageRoot)) -ne $expectedParent) {
        throw 'Refusing to clean a staging directory outside dist.'
    }
    Remove-Item -LiteralPath $stageRoot -Recurse -Force
}
