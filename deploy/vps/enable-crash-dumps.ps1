# SPDX-License-Identifier: AGPL-3.0-only
# Part of a modified SandalCore deployment (branch vps-deploy); not the original project or endorsed by Cyb.
<#
  Run on the VPS, as an administrator. Makes Windows keep a dump when worldd.exe crashes, so a crash can be read
  afterwards (with the worldd.pdb kept for that build, see HANDOFF.md). worldd's own handlers catch C++ exceptions
  only: an access violation ends the process, every player drops, the supervisor restarts it, and without a dump
  nothing says where it happened.

  Windows Error Reporting's LocalDumps for worldd.exe: the last 5 dumps in C:\SandalCore\crashdumps (outside dist,
  so sandalctl update keeps them), each a mini dump with the stacks, the memory they point to, the data segments,
  handles and thread information (tens of MB, written in seconds; a full dump would be 1.5 GB and hold the restart up).
  Nothing is restarted: the next crash of the running worldd is caught.

    enable-crash-dumps.ps1          set it up (again: it only rewrites the same values)
    enable-crash-dumps.ps1 -Test    also prove it under worldd's account: a throwaway PowerShell run as LOCAL SERVICE
                                    fails fast, its dump must appear; the test's task, key and dump are removed after
    enable-crash-dumps.ps1 -Remove  undo (the dumps already written stay)
#>
param([switch] $Test, [switch] $Remove)
$ErrorActionPreference = 'Stop'
$dir = 'C:\SandalCore\crashdumps'
$base = 'SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps'

function Set-Dumps([string] $exe, [string] $folder, [int] $count, [int] $type, $flags) {
    $k = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey("$base\$exe")
    try {
        $k.SetValue('DumpFolder', $folder, [Microsoft.Win32.RegistryValueKind]::ExpandString)
        $k.SetValue('DumpCount', $count, [Microsoft.Win32.RegistryValueKind]::DWord)
        $k.SetValue('DumpType', $type, [Microsoft.Win32.RegistryValueKind]::DWord)
        if ($null -ne $flags) { $k.SetValue('CustomDumpFlags', [int] $flags, [Microsoft.Win32.RegistryValueKind]::DWord) }
    } finally { $k.Close() }
}

if ($Remove) {
    [Microsoft.Win32.Registry]::LocalMachine.DeleteSubKey("$base\worldd.exe", $false)
    Write-Host "worldd.exe crash dumps are off; the dumps in $dir stay."
    return
}

New-Item -ItemType Directory -Force $dir | Out-Null
# The SandalCore-Stack task runs worldd as LOCAL SERVICE (S-1-5-19): it must be able to write the folder.
& icacls $dir /grant '*S-1-5-19:(OI)(CI)M' | Out-Null
if ($LASTEXITCODE -ne 0) { throw "icacls could not give LOCAL SERVICE write access to $dir" }
# DumpType 0 = the custom dump of CustomDumpFlags: MiniDumpWithDataSegs 0x1, WithHandleData 0x4, WithUnloadedModules 0x20,
# WithIndirectlyReferencedMemory 0x40, WithProcessThreadData 0x100, WithFullMemoryInfo 0x800, WithThreadInfo 0x1000.
Set-Dumps 'worldd.exe' $dir 5 0 0x1965
$v = Get-ItemProperty "HKLM:\$base\worldd.exe"
Write-Host ("worldd.exe crash dumps: {0}, the last {1}, DumpType {2}, CustomDumpFlags 0x{3:X}" -f $v.DumpFolder, $v.DumpCount, $v.DumpType, $v.CustomDumpFlags)

if ($Test) {
    $testDir = "$dir\wer-test"
    $task = 'SandalCore-WerTest'
    New-Item -ItemType Directory -Force $testDir | Out-Null
    Set-Dumps 'powershell.exe' $testDir 1 1 $null
    try {
        $action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -NonInteractive -Command [Environment]::FailFast('SandalCore crash dump test')"
        $principal = New-ScheduledTaskPrincipal -UserId 'NT AUTHORITY\LOCAL SERVICE' -LogonType ServiceAccount
        Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Force | Out-Null
        Start-ScheduledTask -TaskName $task
        $dump = $null
        for ($i = 0; $i -lt 30 -and -not $dump; $i++) {
            Start-Sleep -Seconds 1
            $dump = Get-ChildItem $testDir -Filter *.dmp -ErrorAction SilentlyContinue | Select-Object -First 1
        }
        if ($dump) { Write-Host ("test passed: a process of LOCAL SERVICE left {0} ({1:n0} KB)" -f $dump.Name, ($dump.Length / 1KB)) }
        else { Write-Warning 'test FAILED: no dump in 30 s. Check that the WerSvc service can start and that LOCAL SERVICE may write the folder.' }
    } finally {
        Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
        [Microsoft.Win32.Registry]::LocalMachine.DeleteSubKey("$base\powershell.exe", $false)
        Start-Sleep -Seconds 2
        Remove-Item -Recurse -Force $testDir -ErrorAction SilentlyContinue
    }
}
