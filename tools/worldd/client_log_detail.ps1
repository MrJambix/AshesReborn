# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
<#
.SYNOPSIS
  Turns the game client's own detailed logging on or off, for tools\worldd\client_log_probe.py to read.

.DESCRIPTION
  Writes a [Core.Log] section into the client's Saved\Config\WindowsClient\Engine.ini, keeping a copy of the file as it was.
  The client reads it when it starts, so close the game first. It changes no game file and nothing on the server. Whether this
  build honours the section shows in the next log: more lines from these categories.

  client_log_detail.ps1 on | off | status
#>
param([Parameter(Mandatory)][ValidateSet('on', 'off', 'status')][string]$Action)

$dir = Join-Path $env:LOCALAPPDATA 'AOC\Saved\Config\WindowsClient'
$ini = Join-Path $dir 'Engine.ini'
$backup = "$ini.before-log-detail"
$begin = '; --- client_log_detail.ps1 begin'
$end = '; --- client_log_detail.ps1 end'
$categories = 'LogAoC_Ability', 'LogAoC_Stats', 'LogAoC_NPC', 'LogAoC_Character', 'LogAoC_Controllers', 'LogIntrepidNet', 'LogIntrepid_Expression', 'LogIntrepid_DesignData'

$text = if (Test-Path -LiteralPath $ini) { [IO.File]::ReadAllText($ini) } else { '' }
$pattern = '(?s)\r?\n?' + [regex]::Escape($begin) + '.*?' + [regex]::Escape($end) + '\r?\n?'
$running = @(Get-Process -Name 'AOCClient-Win64-Shipping' -ErrorAction SilentlyContinue).Count -gt 0

if ($Action -eq 'status') {
    "detail logging: $(if ($text -match [regex]::Escape($begin)) { 'on' } else { 'off' }); client running: $running; $ini"
    return
}
if ($running) { throw 'Close the game first: it reads this file when it starts, and may write it back when it closes.' }

if ($Action -eq 'on') {
    if (-not (Test-Path -LiteralPath $backup)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        [IO.File]::WriteAllText($backup, $text)
    }
    $body = ($categories | ForEach-Object { "$_=Verbose" }) -join "`r`n"
    $clean = [regex]::Replace($text, $pattern, "`r`n")
    [IO.File]::WriteAllText($ini, $clean.TrimEnd() + "`r`n$begin`r`n[Core.Log]`r`n$body`r`n$end`r`n")
    "detail logging on, for the next launch ($($categories.Count) categories); the file as it was is $backup"
} else {
    [IO.File]::WriteAllText($ini, [regex]::Replace($text, $pattern, "`r`n").TrimEnd() + "`r`n")
    'detail logging off'
}
