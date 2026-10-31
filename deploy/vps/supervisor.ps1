# SPDX-License-Identifier: AGPL-3.0-only
# Part of a modified SandalCore deployment (branch vps-deploy); not the original project or endorsed by Cyb.
<#
  Keeps the SandalCore programs running: started by the SandalCore-Stack task (SYSTEM), restarts what dies, at
  below-normal priority so the other application on this PC wins the CPU. run\stop.flag (sandalctl stop) makes it
  stop them cleanly and exit. If the PC runs out of memory it stops SandalCore and stays down until started again:
  the other application on this PC matters more.
#>
$ErrorActionPreference = 'Continue'
$root = 'C:\SandalCore'; $run = "$root\run"; $logs = "$root\dist\services\logs"
$stopFlag = "$run\stop.flag"; $tripFlag = "$run\watchdog.tripped"; $svLog = "$logs\supervisor.log"
# Limits: commit charge (this PC's page file can grow to about 16 GB in all) and memory left, for a minute.
$MaxCommitMB = 13000; $MinAvailableMB = 60; $LowChecksToTrip = 12
New-Item -ItemType Directory -Force $logs, $run | Out-Null

function Log($m) { Add-Content $svLog ('{0:yyyy-MM-dd HH:mm:ss} {1}' -f (Get-Date), $m) }

$site = Get-Content "$root\ops\site.json" -Raw | ConvertFrom-Json
$content = "$root\dist\content\content.db"; $token = "$root\secrets\service.token"
$pw = [IO.File]::ReadAllText("$root\secrets\db.password")
$env:AOC_DATABASE_URL = "postgres://aoc:$pw@127.0.0.1:5432/aoc?sslmode=disable"
$env:AOC_SERVICE_TOKEN_FILE = $token
$env:AOC_CONTENT_DB = $content
$env:AOC_GATEWAY_ICS_ADDR = '0.0.0.0:8443'
$env:AOC_GATEWAY_TLS_CERT = "$root\certs\server.pem"
$env:AOC_GATEWAY_TLS_KEY = "$root\certs\server.key"
$env:AOC_MESH_STATIC_WORLD = ''

$order = 'auth', 'character', 'citynode', 'quests', 'mesh', 'gateway', 'worldd'
$state = @{}
foreach ($n in $order) { $state[$n] = @{ Fails = 0; Next = [datetime]::MinValue; Started = [datetime]::MinValue } }

function Exe($n) { if ($n -eq 'worldd') { "$root\dist\Release\worldd.exe" } else { "$root\dist\services\$n.exe" } }
function Find($n) { Get-Process -Name $n -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq (Exe $n) } | Select-Object -First 1 }

function Start-Svc($n) {
    $a = switch ($n) {
        'worldd' { " --bind 0.0.0.0:7777 --public-address $($site.public_ip):7777 --content-db `"$content`" --service-token-file `"$token`"" }
        'character' { " --content-db `"$content`"" }
        default { '' }
    }
    foreach ($f in "$logs\$n.log", "$logs\$n.out.log") { if (Test-Path $f) { Move-Item -Force $f ($f -replace '\.log$', '.prev.log') } }
    Start-Process -FilePath $env:ComSpec -WorkingDirectory $root -WindowStyle Hidden `
        -ArgumentList "/d /s /c `"`"$(Exe $n)`"$a 1>`"$logs\$n.out.log`" 2>`"$logs\$n.log`"`"" | Out-Null
    Log "starting $n"
    $state[$n].Started = Get-Date
}

function Stop-All {
    [array]::Reverse($order)
    foreach ($n in $order) {
        $p = Find $n
        if (-not $p) { continue }
        & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$root\ops\sendctrlc.ps1" -TargetPid $p.Id
        if ($p.WaitForExit(15000)) { Log "stopped $n cleanly" }
        else { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Log "stopped $n by force: it did not exit in 15 s" }
    }
    [array]::Reverse($order)
}

Remove-Item -Force $tripFlag -ErrorAction SilentlyContinue
Log "supervisor up, pid $PID"
$low = 0
while ($true) {
    if (Test-Path $stopFlag) { Log 'stop requested'; Stop-All; Remove-Item -Force $stopFlag; Log 'supervisor exit'; exit 0 }

    $os = Get-CimInstance Win32_OperatingSystem
    $commit = ($os.TotalVirtualMemorySize - $os.FreeVirtualMemory) / 1KB; $avail = $os.FreePhysicalMemory / 1KB
    if ($commit -gt $MaxCommitMB -or $avail -lt $MinAvailableMB) { $low++ } else { $low = 0 }
    if ($low -ge $LowChecksToTrip) {
        $why = 'commit {0:n0} MB, available {1:n0} MB' -f $commit, $avail
        Log "WATCHDOG: stopping SandalCore to protect this PC: $why"
        Stop-All
        Set-Content $tripFlag "$(Get-Date -Format s) $why"
        exit 0
    }

    if ((Get-Service 'SandalCore-Postgres' -ErrorAction SilentlyContinue).Status -eq 'Running') {
        foreach ($n in $order) {
            $p = Find $n
            if ($p) {
                try { if ($p.PriorityClass -ne 'BelowNormal') { $p.PriorityClass = 'BelowNormal' } } catch {}
                if ($state[$n].Fails -gt 0 -and ((Get-Date) - $state[$n].Started).TotalMinutes -ge 5) { $state[$n].Fails = 0 }
                continue
            }
            if ((Get-Date) -lt $state[$n].Next) { continue }
            # A program just launched may not show up yet: give it a few seconds before calling it down.
            if ($state[$n].Started -ne [datetime]::MinValue -and ((Get-Date) - $state[$n].Started).TotalSeconds -lt 10) { continue }
            if ($state[$n].Started -ne [datetime]::MinValue) {
                $state[$n].Fails++
                Log "$n is down (fail $($state[$n].Fails))"
                $state[$n].Next = (Get-Date).AddSeconds([Math]::Min(300, 5 * [Math]::Pow(2, $state[$n].Fails)))
                $state[$n].Started = [datetime]::MinValue
                continue
            }
            Start-Svc $n
        }
    }
    Start-Sleep -Seconds 5
}
