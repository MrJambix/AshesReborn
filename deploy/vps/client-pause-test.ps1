# SPDX-License-Identifier: AGPL-3.0-only
# Part of a modified SandalCore deployment (branch vps-deploy); not the original project or endorsed by Cyb.
<#
  Run on the build PC, where the game client runs (not shipped to the VPS). The pause test of HANDOFF item 0000016:
  does the game survive a long freeze, and does worldd's silence once its 256-packet window is full decide it?
  Start it, then log in any character: about 20 s after the phase-in (read from the VPS log over ssh sun-vps) it
  freezes AOCClient-Win64-Shipping.exe for each pause (default 65 s, then 100 s), resumes it, and prints what the
  worldd log and AOC.log say. Reads the VPS log only; the game is always resumed. In the world worldd sends about
  26 packets a second, so its window fills about 10 s into a pause and it is silent after that:
    65 s dropped                -> the client times out 60 s after its last frame whatever we send: worldd's silence does not matter
    65 s kept, 100 s dropped    -> the client times out 60 s after the last packet its receive thread got: keep-alives while full would help
    both kept                   -> the client forgives freezes; the 3 November drops had another cause
  The client's own line "Connection TIMED OUT ... Real: N" names the clock it used.
#>
param([int[]] $Pauses = @(65, 100), [int] $WaitMinutes = 30, [int] $SettleSeconds = 30, [int] $InWorldSeconds = 20)
$ErrorActionPreference = 'Stop'
$game = 'AOCClient-Win64-Shipping'
$clientLog = "$env:LOCALAPPDATA\AOC\Saved\Logs\AOC.log"

Add-Type @'
using System; using System.Runtime.InteropServices;
public static class Freeze {
    [DllImport("ntdll.dll")] public static extern int NtSuspendProcess(IntPtr h);
    [DllImport("ntdll.dll")] public static extern int NtResumeProcess(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
}
'@

function Vps([string] $script) {
    $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes("`$ProgressPreference='SilentlyContinue'`n" + $script))
    ssh -o BatchMode=yes -o ConnectTimeout=15 sun-vps "powershell -NoProfile -EncodedCommand $enc" 2>$null
}
function VpsLinesSince([string] $since, [string] $pattern) {
    Vps "Get-Content 'C:\SandalCore\dist\services\logs\worldd.log' | Where-Object { `$_.Length -gt 25 -and `$_.Substring(1,19) -ge '$since' -and `$_ -match '$pattern' }"
}
function Stamp { (Get-Date).ToString('yyyy-MM-dd HH:mm:ss') }
function Say([string] $m) { Write-Host ('{0:HH:mm:ss} {1}' -f (Get-Date), $m) }

# 1. wait for a phase-in newer than now
$start = Stamp
Say "waiting up to $WaitMinutes min for a character to phase in on the VPS (since $start)"
$joined = $null
$until = (Get-Date).AddMinutes($WaitMinutes)
while (-not $joined -and (Get-Date) -lt $until) {
    Start-Sleep -Seconds 10
    $joined = VpsLinesSince $start '\[join\] .+: phase-in after' | Select-Object -Last 1
}
if (-not $joined) { Say 'nobody phased in: no test run'; exit 2 }
$name = ([regex]::Match($joined, '\[join\] (.+?): phase-in')).Groups[1].Value
Say "in the world: $joined"
Start-Sleep -Seconds $InWorldSeconds

$results = @()
foreach ($pause in $Pauses) {
    $p = Get-Process $game -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $p) { Say 'the game is not running any more: stopping'; break }
    $clientLines = (Get-Content $clientLog -ErrorAction SilentlyContinue | Measure-Object -Line).Lines
    $at = Stamp
    $h = [Freeze]::OpenProcess(0x0800, $false, $p.Id)   # PROCESS_SUSPEND_RESUME
    if ($h -eq [IntPtr]::Zero) { throw "cannot open the game process (error $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))" }
    try {
        $r = [Freeze]::NtSuspendProcess($h)
        if ($r -ne 0) { throw ('NtSuspendProcess failed: 0x{0:X8}' -f $r) }
        Say "the game is frozen for $pause s"
        Start-Sleep -Seconds $pause
    } finally {
        [Freeze]::NtResumeProcess($h) | Out-Null
        [Freeze]::CloseHandle($h) | Out-Null
        Say 'the game is running again'
    }
    Start-Sleep -Seconds $SettleSeconds
    $server = @(VpsLinesSince $at "\[link\] $([regex]::Escape($name))|\[endpoint\]")
    $client = @(Get-Content $clientLog -ErrorAction SilentlyContinue | Select-Object -Skip $clientLines |
        Where-Object { $_ -match 'TIMED OUT|Closing connection|NetworkFailure|ConnectionLost|Very long time|TravelFailure|Connection to the Realm|UNetConnection::Close' } |
        ForEach-Object { ($_ -replace '^.*"message":"', '') -replace '"\}$', '' })
    $dropped = [bool]($server | Where-Object { $_ -match '\[endpoint\].*: (the client quit|timed out|broke the protocol|threw|closed)' })
    $results += [pscustomobject]@{ Pause = $pause; Dropped = $dropped }
    Write-Host "----- pause $pause s: $(if ($dropped) { 'DROPPED' } else { 'still connected' })"
    Write-Host '  server:'; $server | ForEach-Object { '    ' + $_.Substring(0, [Math]::Min(600, $_.Length)) }
    Write-Host '  client:'; $client | ForEach-Object { '    ' + $_.Substring(0, [Math]::Min(600, $_.Length)) }
    if ($dropped) { break }
}
Write-Host '===== summary'
$results | ForEach-Object { '{0,4} s: {1}' -f $_.Pause, $(if ($_.Dropped) { 'dropped' } else { 'survived' }) }
