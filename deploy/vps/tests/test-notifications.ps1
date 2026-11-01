# SPDX-License-Identifier: AGPL-3.0-only
# Run with Windows PowerShell 5.1. All requests and server processes are mocked.
param([string] $ControlScript = (Join-Path $PSScriptRoot '..\sandalctl.ps1'))
$ErrorActionPreference = 'Stop'
$sender = Join-Path $PSScriptRoot '..\notify-discord.ps1'
$scratch = Join-Path ([IO.Path]::GetTempPath()) ('AshesReborn-notifications-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null
$cases = 0
$global:DiscordNotificationFixture = @{
    Requests = [Collections.ArrayList]::new(); FailDelivery = $false
    Starts = [Collections.Queue]::new(); StopFails = $false; RestoreExit = 0; ExtractExit = 0
}

function Assert($Condition, [string] $Name) {
    if (-not $Condition) { throw "FAIL: $Name" }
    $script:cases++
    Write-Output "PASS: $Name"
}

function global:Invoke-RestMethod {
    param($Uri, $Method, $ContentType, $Body, $TimeoutSec, $ErrorAction)
    if ($global:DiscordNotificationFixture.FailDelivery) { throw "Request failed: $Uri" }
    [void]$global:DiscordNotificationFixture.Requests.Add(@{
        Uri = $Uri; Method = $Method; Timeout = $TimeoutSec
        Payload = ([Text.Encoding]::UTF8.GetString($Body) | ConvertFrom-Json)
    })
    @{ id = 'mock-message' }
}

function global:tar {
    $fixture = $global:DiscordNotificationFixture
    $global:LASTEXITCODE = $fixture.ExtractExit
    if ($fixture.ExtractExit -ne 0) { return }
    $new = Join-Path $fixture.Root 'update.new'
    foreach ($dir in @('dist', 'ops', 'deploy', 'config')) {
        New-Item -ItemType Directory -Force -Path (Join-Path $new $dir) | Out-Null
    }
    foreach ($file in @('dist\new.txt', 'ops\marker.txt', 'deploy\marker.txt', 'VERSION.txt')) {
        Set-Content -LiteralPath (Join-Path $new $file) -Value 'fixture'
    }
}

function global:cmd { $global:LASTEXITCODE = $global:DiscordNotificationFixture.RestoreExit }

function Run-Control([string] $Action, [bool[]] $Starts = @($true), [bool] $ShouldFail = $false, [string] $Expected = 'completed', [int] $Notices = 2) {
    $fixture = $global:DiscordNotificationFixture
    $fixture.Requests.Clear()
    $fixture.Starts.Clear()
    foreach ($result in $Starts) { $fixture.Starts.Enqueue($result) }
    $fixture.Root = Join-Path $scratch ([guid]::NewGuid().ToString('N'))
    foreach ($dir in @('dist\services\logs', 'ops', 'deploy', 'secrets')) {
        New-Item -ItemType Directory -Force -Path (Join-Path $fixture.Root $dir) | Out-Null
    }
    Copy-Item -LiteralPath $sender -Destination (Join-Path $fixture.Root 'notify-discord.ps1')
    Set-Content -LiteralPath (Join-Path $fixture.Root 'secrets\discord-webhook.secret') -Value 'https://discord.com/api/webhooks/123/fake-token'
    Set-Content -LiteralPath (Join-Path $fixture.Root 'dist\old.txt') -Value 'original'
    $package = Join-Path $fixture.Root 'fixture.tar'
    Set-Content -LiteralPath $package -Value 'fixture'
    $source = [IO.File]::ReadAllText((Resolve-Path -LiteralPath $ControlScript))
    $tokens = $null; $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseInput($source, [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count) { throw "Control script does not parse: $parseErrors" }
    # Replace process/database operations, retaining the real action and notification flow.
    $replacements = @{
        'Stop-Stack' = 'function Stop-Stack { if ($global:DiscordNotificationFixture.StopFails) { throw "fixture stop failed" } }'
        'Start-Stack' = 'function Start-Stack { return $global:DiscordNotificationFixture.Starts.Dequeue() }'
        'Backup' = 'function Backup { return "fixture.dump" }'
        'Use-Db' = 'function Use-Db {}'
        'Show-Status' = 'function Show-Status {}'
    }
    $definitions = $ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] }, $true) |
        Where-Object { $replacements.ContainsKey($_.Name) } | Sort-Object { $_.Extent.StartOffset } -Descending
    foreach ($definition in $definitions) {
        $extent = $definition.Extent
        $source = $source.Remove($extent.StartOffset, $extent.EndOffset - $extent.StartOffset).Insert($extent.StartOffset, $replacements[$definition.Name])
    }
    $source = $source.Replace('$root = ''C:\SandalCore''', ('$root = ''' + $fixture.Root.Replace("'", "''") + ''''))
    $scriptFile = Join-Path $fixture.Root 'sandalctl.ps1'
    [IO.File]::WriteAllText($scriptFile, $source)
    $failed = $false
    try { & $scriptFile $Action $package | Out-Null } catch { $failed = $true }
    Assert ($failed -eq $ShouldFail) "$Action preserves success/failure outcome ($Expected)"
    Assert ($fixture.Requests.Count -eq $Notices) "$Action sends $Notices notices ($Expected)"
    if ($Notices -gt 0) {
        $last = $fixture.Requests[$Notices - 1].Payload.embeds[0]
        Assert ($last.description -match $Expected) "$Action reports $Expected"
        $expectedColor = if ($ShouldFail) { 15158332 } else { 3066993 }
        Assert ($last.color -eq $expectedColor) "$Action reports the correct result level"
    }
}

try {
    $secret = Join-Path $scratch 'webhook.secret'
    $tlsBefore = [Net.ServicePointManager]::SecurityProtocol
    Assert ((& $sender -Message 'missing secret' -WebhookFile $secret) -eq $false) 'missing configuration disables notifications'
    Assert ($global:DiscordNotificationFixture.Requests.Count -eq 0) 'missing configuration makes no request'
    Set-Content -LiteralPath $secret -Value 'https://discord.com/api/webhooks/123/fake-token'
    $message = 'Update ready ' + [char]0x2713 + ' @everyone'
    Assert ((& $sender -Message $message -WebhookFile $secret) -eq $true) 'delivery confirms success'
    $request = $global:DiscordNotificationFixture.Requests[0]
    Assert ($request.Payload.embeds[0].description -ceq $message) 'UTF-8 content survives serialization'
    Assert (@($request.Payload.allowed_mentions.parse).Count -eq 0) 'mentions are disabled'
    Assert ($request.Uri.EndsWith('?wait=true') -and $request.Method -eq 'Post' -and $request.Timeout -eq 10) 'delivery waits for confirmation and has a timeout'
    Assert ([Net.ServicePointManager]::SecurityProtocol -eq $tlsBefore) 'TLS settings are restored after success'
    & $sender -Message ('x' * 5000) -WebhookFile $secret | Out-Null
    Assert ($global:DiscordNotificationFixture.Requests[1].Payload.embeds[0].description.Length -eq 4000) 'long notifications stay within the embed limit'
    $global:DiscordNotificationFixture.FailDelivery = $true
    $warnings = @()
    Assert ((& $sender -Message 'failure' -WebhookFile $secret -WarningVariable warnings -WarningAction SilentlyContinue) -eq $false) 'HTTP failure returns false without throwing'
    Assert (($warnings | Out-String) -notmatch 'fake-token|https://') 'HTTP failure warnings do not expose the webhook'
    Assert ([Net.ServicePointManager]::SecurityProtocol -eq $tlsBefore) 'TLS settings are restored after failure'
    $global:DiscordNotificationFixture.FailDelivery = $false
    Set-Content -LiteralPath $secret -Value 'https://example.com/not-discord'
    Assert ((& $sender -Message 'invalid' -WebhookFile $secret -WarningAction SilentlyContinue) -eq $false) 'invalid webhook configuration is rejected'

    Run-Control 'start' @($true) $false 'Server started'
    Run-Control 'start' @($false) $true 'failed'
    Run-Control 'restart' @($true) $false 'restart completed'
    Run-Control 'stop' @($true) $false 'Server stopped'
    $global:DiscordNotificationFixture.StopFails = $true
    Run-Control 'restart' @($true) $true 'failed'
    $global:DiscordNotificationFixture.StopFails = $false
    Run-Control 'update' @($true) $false 'update completed'
    Run-Control 'update' @($false, $true) $true 'previous programs were restored and the server is running' 3
    Assert (Test-Path -LiteralPath (Join-Path $global:DiscordNotificationFixture.Root 'dist\old.txt')) 'rollback restores the original programs'
    Run-Control 'update' @($false, $false) $true 'Rollback did not complete' 3
    $global:DiscordNotificationFixture.ExtractExit = 1
    Run-Control 'update' @($true) $true 'failed'
    $global:DiscordNotificationFixture.ExtractExit = 0
    Run-Control 'restore' @($true) $false 'restore completed'
    $global:DiscordNotificationFixture.RestoreExit = 1
    Run-Control 'restore' @($true) $true 'failed'
    $global:DiscordNotificationFixture.RestoreExit = 0
    Run-Control 'status' @($true) $false '' 0
    Run-Control 'backup' @($true) $false '' 0
    $global:DiscordNotificationFixture.FailDelivery = $true
    Run-Control 'restart' @($true) $false '' 0
    Write-Output "$cases notification checks passed. No network or live services were used."
} finally {
    Remove-Item Function:\Invoke-RestMethod, Function:\tar, Function:\cmd -ErrorAction SilentlyContinue
    Remove-Variable -Name DiscordNotificationFixture -Scope Global -ErrorAction SilentlyContinue
    $resolved = [IO.Path]::GetFullPath($scratch)
    $tempPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        -not [IO.Path]::GetFileName($resolved).StartsWith('AshesReborn-notifications-')) { throw 'Unsafe notification-test cleanup path.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
