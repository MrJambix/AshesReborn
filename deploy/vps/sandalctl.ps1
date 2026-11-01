# SPDX-License-Identifier: AGPL-3.0-only
# Part of a modified SandalCore deployment (branch vps-deploy); not the original project or endorsed by Cyb.
<#
  sandalctl start | stop | restart | status | logs [name] | diag | backup | restore <file> | update <package.tar> | allow-ip <ip> | open | close
  Only ever touches C:\SandalCore, the SandalCore-* task, service and firewall rules. See ops\README.md.
#>
param(
    [Parameter(Mandatory, Position = 0)] [ValidateSet('start', 'stop', 'restart', 'status', 'logs', 'diag', 'backup', 'restore', 'update', 'allow-ip', 'open', 'close')] [string] $Action,
    [Parameter(Position = 1)] [string] $Arg
)
$ErrorActionPreference = 'Stop'
$root = 'C:\SandalCore'; $run = "$root\run"; $logs = "$root\dist\services\logs"; $pg = "$root\pgsql\bin"
$task = 'SandalCore-Stack'; $names = 'auth', 'character', 'citynode', 'quests', 'mesh', 'gateway', 'worldd'
$tcp = 8081, 8443, 8444, 50051, 50052, 50053, 50054, 50055, 50056, 50057, 50058, 5432

function Exe($n) { if ($n -eq 'worldd') { "$root\dist\Release\worldd.exe" } else { "$root\dist\services\$n.exe" } }
function Find($n) { Get-Process -Name $n -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq (Exe $n) } | Select-Object -First 1 }
function Running { @($names | ForEach-Object { Find $_ } | Where-Object { $_ }) }
function Use-Db { $env:PGPASSWORD = [IO.File]::ReadAllText("$root\secrets\db.password") }

function Notify([string] $Message, [string] $Level = 'info') {
    # Optional for older installations; delivery failures never prevent maintenance.
    try {
        $sender = Join-Path $PSScriptRoot 'notify-discord.ps1'
        if (Test-Path -LiteralPath $sender) { & $sender -Message $Message -Level $Level -Root $root | Out-Null }
    } catch { Write-Warning 'Discord notification failed; maintenance will continue.' }
}

function Stop-Stack {
    if ((Get-ScheduledTask $task).State -eq 'Running') {
        Set-Content "$run\stop.flag" 'stop'
        $until = (Get-Date).AddSeconds(150)
        while ((Get-ScheduledTask $task).State -eq 'Running' -and (Get-Date) -lt $until) { Start-Sleep 2 }
        if ((Get-ScheduledTask $task).State -eq 'Running') { Stop-ScheduledTask $task; Write-Host 'the supervisor did not stop in time and was ended' }
        Remove-Item -Force "$run\stop.flag" -ErrorAction SilentlyContinue
    }
    $left = Running
    if ($left) { $left | Stop-Process -Force; Write-Host "ended by force: $(($left | ForEach-Object Name) -join ', ')" }
}
function Start-Stack {
    Remove-Item -Force "$run\stop.flag" -ErrorAction SilentlyContinue
    if ((Get-Service 'SandalCore-Postgres').Status -ne 'Running') { Start-Service 'SandalCore-Postgres' }
    Start-ScheduledTask $task
    $until = (Get-Date).AddSeconds(120)
    while ((Get-Date) -lt $until) {
        Start-Sleep 3
        if ((Running).Count -eq $names.Count -and (Get-NetUDPEndpoint -LocalPort 7777 -ErrorAction SilentlyContinue)) { Write-Host 'all programs are running'; return $true }
    }
    Write-Host "not everything came up: running are $(((Running) | ForEach-Object Name) -join ', '). Look in $logs"
    return $false
}
function Backup {
    New-Item -ItemType Directory -Force "$root\backups" | Out-Null
    Use-Db
    $f = "$root\backups\aoc-{0:yyyyMMdd-HHmmss}.dump" -f (Get-Date)
    cmd /c "`"$pg\pg_dump.exe`" -h 127.0.0.1 -U aoc -Fc -f `"$f`" aoc 2>&1"
    if ($LASTEXITCODE -ne 0) { if (Test-Path $f) { Remove-Item -Force $f }; throw 'pg_dump failed' }
    Get-ChildItem "$root\backups\aoc-*.dump" | Sort-Object LastWriteTime -Descending | Select-Object -Skip 14 | Remove-Item -Force
    Write-Host "backup: $f ($([int]((Get-Item $f).Length / 1KB)) KB)"
    return $f
}
function Show-Status {
    Write-Host "version : $((Get-Content "$root\VERSION.txt" -ErrorAction SilentlyContinue))"
    Write-Host "task    : $((Get-ScheduledTask $task).State); postgres: $((Get-Service 'SandalCore-Postgres').Status)"
    if (Test-Path "$run\watchdog.tripped") { Write-Host "WATCHDOG STOPPED THE STACK: $(Get-Content "$run\watchdog.tripped")" }
    foreach ($n in $names) {
        $p = Find $n
        if ($p) { Write-Host ('{0,-10} up   pid {1,-6} {2,5:n0} MB  since {3:MM-dd HH:mm}' -f $n, $p.Id, ($p.WorkingSet64 / 1MB), $p.StartTime) } else { Write-Host ('{0,-10} DOWN' -f $n) }
    }
    try { $h = Invoke-WebRequest 'http://127.0.0.1:8081/health' -UseBasicParsing -TimeoutSec 5; Write-Host "auth /health: $($h.StatusCode)" } catch { Write-Host "auth /health: FAILED ($($_.Exception.Message))" }
    $os = Get-CimInstance Win32_OperatingSystem
    Write-Host ('memory  : {0:n0} MB free, commit {1:n0} of {2:n0} MB' -f ($os.FreePhysicalMemory / 1KB), (($os.TotalVirtualMemorySize - $os.FreeVirtualMemory) / 1KB), ($os.TotalVirtualMemorySize / 1KB))
}

$notifyAction = $Action -in @('start', 'stop', 'restart', 'restore', 'update', 'allow-ip', 'open', 'close')
$failureMessage = "Server operation '$Action' failed. Check the server status and logs before retrying."
if ($notifyAction) { Notify "Server operation '$Action' is starting. Service may be interrupted during maintenance." }
try {
switch ($Action) {
    'start' { if (-not (Start-Stack)) { throw 'The server did not start.' } }
    'stop' { Stop-Stack; Write-Host 'stopped' }
    'restart' { Stop-Stack; if (-not (Start-Stack)) { throw 'The server did not restart.' } }
    'status' { Show-Status }
    'logs' {
        $n = if ($Arg) { $Arg } else { 'worldd' }
        Get-Content "$logs\$n.log" -Tail 40 -Wait
    }
    'diag' {
        Show-Status
        Write-Host '--- ports'
        foreach ($t in $tcp) { $c = Get-NetTCPConnection -State Listen -LocalPort $t -ErrorAction SilentlyContinue; Write-Host ('tcp {0,-6} {1}' -f $t, $(if ($c) { "$($c[0].LocalAddress) $((Get-Process -Id $c[0].OwningProcess).Name)" } else { 'NOT LISTENING' })) }
        $u = Get-NetUDPEndpoint -LocalPort 7777 -ErrorAction SilentlyContinue; Write-Host ('udp 7777   {0}' -f $(if ($u) { "$($u[0].LocalAddress) $((Get-Process -Id $u[0].OwningProcess).Name)" } else { 'NOT BOUND' }))
        Write-Host '--- firewall'
        Get-NetFirewallRule -DisplayName 'SandalCore-*' | ForEach-Object { Write-Host "$($_.DisplayName): enabled=$($_.Enabled) from $(($_ | Get-NetFirewallAddressFilter).RemoteAddress)" }
        Write-Host '--- certificate'
        $c = New-Object Security.Cryptography.X509Certificates.X509Certificate2 "$root\certs\server.pem"; Write-Host "$($c.Subject), expires $($c.NotAfter)"
        Write-Host '--- backups'
        Get-ChildItem "$root\backups\*.dump" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 3 | ForEach-Object { Write-Host "$($_.Name) $([int]($_.Length / 1KB)) KB" }
        Write-Host '--- disk'
        Write-Host ('{0:n1} GB free; logs {1:n0} MB' -f ((Get-PSDrive C).Free / 1GB), ((Get-ChildItem $logs -File | Measure-Object Length -Sum).Sum / 1MB))
        Write-Host '--- errors in the current logs'
        Get-ChildItem "$logs\*.log" | Where-Object Name -NotMatch 'prev' | ForEach-Object {
            $e = @(Select-String -Path $_.FullName -Pattern 'level=ERROR|\[error\]|\[critical\]|^panic' -ErrorAction SilentlyContinue)
            if ($e.Count) { Write-Host "$($_.Name): $($e.Count); last: $($e[-1].Line)" }
        }
    }
    'backup' { Backup | Out-Null }
    'restore' {
        if (-not $Arg -or -not (Test-Path $Arg)) { throw "restore needs a backup file: $Arg" }
        $safety = Backup
        Stop-Stack
        Use-Db
        cmd /c "`"$pg\pg_restore.exe`" -h 127.0.0.1 -U aoc -d aoc --clean --if-exists `"$Arg`" 2>&1"
        $rc = $LASTEXITCODE
        if ($rc -ne 0) { Write-Host "pg_restore reported problems (exit $rc). The backup taken first: $safety" }
        if (-not (Start-Stack)) { throw 'The server did not start after restore.' }
        if ($rc -ne 0) { throw "pg_restore reported problems (exit $rc)." }
    }
    'update' {
        if (-not $Arg -or -not (Test-Path $Arg)) { throw "update needs a package: $Arg" }
        Backup | Out-Null
        $new = "$root\update.new"
        if (Test-Path $new) { Remove-Item -Recurse -Force $new }
        New-Item -ItemType Directory $new | Out-Null
        tar -xf $Arg -C $new
        if ($LASTEXITCODE -ne 0) { throw 'the package could not be unpacked; nothing has changed' }
        Stop-Stack
        if (Test-Path "$root\dist.prev") { Remove-Item -Recurse -Force "$root\dist.prev" }
        Move-Item "$root\dist" "$root\dist.prev"
        Move-Item "$new\dist" "$root\dist"
        New-Item -ItemType Directory -Force $logs | Out-Null
        Copy-Item "$new\ops\*" "$root\ops" -Exclude site.json -Force
        Copy-Item "$new\deploy\*" "$root\deploy" -Force
        # Seed deployment settings once; later program updates preserve operator rate/name changes.
        New-Item -ItemType Directory -Force "$root\config" | Out-Null
        foreach ($setting in @('realm.json', 'rates.json')) {
            if ((Test-Path -LiteralPath "$new\config\$setting") -and -not (Test-Path -LiteralPath "$root\config\$setting")) {
                Copy-Item -LiteralPath "$new\config\$setting" -Destination "$root\config\$setting"
            }
        }
        Copy-Item "$new\VERSION.txt" $root -Force
        Remove-Item -Recurse -Force $new
        if (Start-Stack) { Write-Host "updated; the old programs are in $root\dist.prev"; break }
        Write-Host 'the new version did not come up: going back'
        Notify 'The updated server did not start. Rolling back to the previous programs.' 'warning'
        $failureMessage = 'Server update failed. Rollback did not complete; check the server status and logs immediately.'
        Stop-Stack
        Move-Item "$root\dist" "$root\dist.failed"
        Move-Item "$root\dist.prev" "$root\dist"
        if (Start-Stack) { $failureMessage = 'Server update failed. The previous programs were restored and the server is running.' }
        throw $failureMessage
    }
    'allow-ip' {
        if (-not $Arg) { throw 'allow-ip needs the IP address' }
        Get-NetFirewallRule -DisplayName 'SandalCore-*' | Set-NetFirewallRule -RemoteAddress $Arg
        Write-Host "SandalCore's game ports now answer only $Arg"
    }
    'open' {
        Get-NetFirewallRule -DisplayName 'SandalCore-*' | Set-NetFirewallRule -RemoteAddress Any
        Write-Host "SandalCore's game ports now answer everyone"
    }
    'close' {
        $site = Get-Content "$root\ops\site.json" -Raw | ConvertFrom-Json
        Get-NetFirewallRule -DisplayName 'SandalCore-*' | Set-NetFirewallRule -RemoteAddress $site.owner_ip
        Write-Host "SandalCore's game ports now answer only $($site.owner_ip)"
    }
}
} catch {
    if ($notifyAction) { Notify $failureMessage 'error' }
    throw
}
if ($notifyAction) {
    $completed = switch ($Action) {
        'start' { 'Server started. All programs are running.' }
        'stop' { 'Server stopped for maintenance.' }
        'restart' { 'Server restart completed. All programs are running.' }
        'restore' { 'Database restore completed. All programs are running.' }
        'update' { 'Server update completed. All programs are running.' }
        'open' { 'Server access is open to players.' }
        'close' { 'Server access is restricted to the owner for maintenance.' }
        'allow-ip' { 'Server access is restricted to the configured IP address.' }
    }
    Notify $completed 'success'
}
