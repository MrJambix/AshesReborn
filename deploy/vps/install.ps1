# SPDX-License-Identifier: AGPL-3.0-only
# Part of a modified SandalCore deployment (branch vps-deploy); not the original project or endorsed by Cyb.
<#
  Run on the VPS from C:\SandalCore\ops, once, as Administrator. Safe to run again: what exists is kept.
  Adds only things named SandalCore*: a PostgreSQL service on loopback, a start-up task, a backup task and three
  firewall rules for -AllowIp (an address, or Any for everyone). It does not read or change anything of another application.
#>
param(
    [Parameter(Mandatory)] [string] $PublicIp,
    [Parameter(Mandatory)] [string] $AllowIp,
    [Parameter(Mandatory)] [string] $OwnerIp   # what sandalctl close goes back to
)
$ErrorActionPreference = 'Stop'
$root = 'C:\SandalCore'; $secrets = "$root\secrets"; $pg = "$root\pgsql"; $data = "$root\pgdata"; $pgService = 'SandalCore-Postgres'

function Protect($path) {
    & icacls $path /inheritance:r /grant:r 'SYSTEM:(OI)(CI)F' 'BUILTIN\Administrators:(OI)(CI)F' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "could not protect $path" }
}
function New-Secret($bytes) {
    $b = New-Object byte[] $bytes
    [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($b)
    -join ($b | ForEach-Object { $_.ToString('x2') })
}

New-Item -ItemType Directory -Force $secrets, "$root\backups", "$root\run" | Out-Null
Protect $secrets; Protect "$root\certs"; Protect "$root\backups"
if (-not (Test-Path "$secrets\db.password")) { [IO.File]::WriteAllText("$secrets\db.password", (New-Secret 24)) }
if (-not (Test-Path "$secrets\service.token")) { [IO.File]::WriteAllText("$secrets\service.token", (New-Secret 32)) }
$pw = [IO.File]::ReadAllText("$secrets\db.password")
@{ public_ip = $PublicIp; owner_ip = $OwnerIp } | ConvertTo-Json | Set-Content "$root\ops\site.json" -Encoding ascii

if (-not (Test-Path "$data\PG_VERSION")) {
    # initdb drops the Administrators group, so it can read neither secrets\ nor a file only that group may open.
    $me = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    New-Item -ItemType Directory -Force $data | Out-Null
    & icacls $data /grant "${me}:(OI)(CI)F" | Out-Null
    $pwf = "$root\run\initdb.pw"; [IO.File]::WriteAllText($pwf, '')
    & icacls $pwf /inheritance:r /grant:r "${me}:F" | Out-Null
    [IO.File]::WriteAllText($pwf, $pw)
    try {
        cmd /c "`"$pg\bin\initdb.exe`" -D `"$data`" -U aoc --pwfile=`"$pwf`" -A scram-sha-256 -E UTF8 --locale=C >`"$root\run\initdb.log`" 2>&1"
        if ($LASTEXITCODE -ne 0) { throw "initdb failed, see $root\run\initdb.log" }
    } finally { Remove-Item -Force $pwf }
    # Loopback only, and small: this PC also runs another application's server and is short of memory.
    Add-Content "$data\postgresql.conf" @"

listen_addresses = '127.0.0.1'
port = 5432
shared_buffers = 32MB
max_connections = 30
work_mem = 2MB
maintenance_work_mem = 16MB
logging_collector = on
log_directory = 'log'
log_filename = 'postgresql-%a.log'
log_truncate_on_rotation = on
log_rotation_age = 1d
log_rotation_size = 0
"@
}
& icacls $data /grant 'NT AUTHORITY\NetworkService:(OI)(CI)M' | Out-Null
& icacls $pg /grant 'NT AUTHORITY\NetworkService:(OI)(CI)RX' | Out-Null
if (-not (Get-Service $pgService -ErrorAction SilentlyContinue)) {
    cmd /c "`"$pg\bin\pg_ctl.exe`" register -N $pgService -D `"$data`" -S auto -U `"NT AUTHORITY\NetworkService`" >nul 2>&1"
    if ($LASTEXITCODE -ne 0) { throw 'could not register the PostgreSQL service' }
}
if ((Get-Service $pgService).Status -ne 'Running') { Start-Service $pgService }
(Get-Service $pgService).WaitForStatus('Running', '00:00:30')
$env:PGPASSWORD = $pw
$have = cmd /c "`"$pg\bin\psql.exe`" -h 127.0.0.1 -U aoc -d postgres -Atc `"select 1 from pg_database where datname='aoc'`" 2>&1"
if ($LASTEXITCODE -ne 0) { throw "psql failed: $have" }
if ("$have".Trim() -ne '1') {
    cmd /c "`"$pg\bin\createdb.exe`" -h 127.0.0.1 -U aoc aoc 2>&1"
    if ($LASTEXITCODE -ne 0) { throw 'createdb failed' }
}

# What faces the internet runs as LOCAL SERVICE: it may read its own files, its key and database password, and write only logs and run\.
$svc = 'NT AUTHORITY\LOCAL SERVICE'
foreach ($p in "$root\run", "$root\dist\services\logs") {
    New-Item -ItemType Directory -Force $p | Out-Null
    & icacls $p /grant "${svc}:(OI)(CI)M" | Out-Null
}
& icacls $secrets /grant "${svc}:(OI)(CI)RX" | Out-Null
& icacls "$root\certs" /grant "${svc}:(OI)(CI)RX" | Out-Null

Get-NetFirewallRule -DisplayName 'SandalCore-*one IP only*' -ErrorAction SilentlyContinue | Remove-NetFirewallRule
$rules = @(
    @{ Name = 'SandalCore-Gateway (TCP 8443)'; Protocol = 'TCP'; Port = 8443 },
    @{ Name = 'SandalCore-Login (TCP 8444, TLS)'; Protocol = 'TCP'; Port = 8444 },
    @{ Name = 'SandalCore-World (UDP 7777)'; Protocol = 'UDP'; Port = 7777 })
foreach ($r in $rules) {
    if (Get-NetFirewallRule -DisplayName $r.Name -ErrorAction SilentlyContinue) { Set-NetFirewallRule -DisplayName $r.Name -RemoteAddress $AllowIp }
    else { New-NetFirewallRule -DisplayName $r.Name -Direction Inbound -Action Allow -Protocol $r.Protocol -LocalPort $r.Port -RemoteAddress $AllowIp -Profile Any | Out-Null }
}

$system = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
$ps = "-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File"

# Three minutes after a boot, so the other application on this PC gets its memory first.
$boot = New-ScheduledTaskTrigger -AtStartup; $boot.Delay = 'PT3M'
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable -RestartCount 5 -RestartInterval (New-TimeSpan -Minutes 1) -Priority 7 -MultipleInstances IgnoreNew
$localService = New-ScheduledTaskPrincipal -UserId $svc -LogonType ServiceAccount -RunLevel Limited
Register-ScheduledTask -TaskName 'SandalCore-Stack' -Force -Principal $localService -Trigger $boot -Settings $settings `
    -Action (New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "$ps `"$root\ops\supervisor.ps1`"" -WorkingDirectory $root) | Out-Null

$daily = New-ScheduledTaskTrigger -Daily -At '04:30'
$bset = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable -Priority 7
Register-ScheduledTask -TaskName 'SandalCore-DbBackup' -Force -Principal $system -Trigger $daily -Settings $bset `
    -Action (New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "$ps `"$root\ops\sandalctl.ps1`" backup" -WorkingDirectory $root) | Out-Null

Write-Host 'installed. Start it with: powershell -File C:\SandalCore\ops\sandalctl.ps1 start'
