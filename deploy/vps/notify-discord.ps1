# SPDX-License-Identifier: AGPL-3.0-only
# Send an operator notification without interrupting server maintenance.
param(
    [Parameter(Mandatory)] [ValidateNotNullOrEmpty()] [string] $Message,
    [ValidateSet('info', 'success', 'warning', 'error')] [string] $Level = 'info',
    [string] $Root = 'C:\SandalCore',
    [string] $WebhookFile = (Join-Path $Root 'secrets\discord-webhook.secret')
)

$previousTls = [Net.ServicePointManager]::SecurityProtocol
try {
    if (-not (Test-Path -LiteralPath $WebhookFile -PathType Leaf)) { return $false }
    $webhook = [IO.File]::ReadAllText($WebhookFile).Trim()
    if ($webhook -cnotmatch '^https://discord\.com/api/webhooks/[0-9]+/[A-Za-z0-9_-]+$') {
        throw 'Invalid Discord webhook configuration.'
    }
    $colors = @{ info = 3447003; success = 3066993; warning = 15105570; error = 15158332 }
    $body = @{
        username = 'Ashes Reborn Updates'
        allowed_mentions = @{ parse = @() }
        embeds = @(@{
            title = 'Ashes Reborn - Server / Deployment'
            description = $Message.Substring(0, [Math]::Min($Message.Length, 4000))
            color = $colors[$Level]
            timestamp = [DateTime]::UtcNow.ToString('o')
        })
    } | ConvertTo-Json -Depth 5 -Compress
    [Net.ServicePointManager]::SecurityProtocol = $previousTls -bor [Net.SecurityProtocolType]::Tls12
    # wait=true confirms Discord saved the message; never print the response or URL.
    Invoke-RestMethod -Uri ($webhook + '?wait=true') -Method Post -ContentType 'application/json; charset=utf-8' `
        -Body ([Text.Encoding]::UTF8.GetBytes($body)) -TimeoutSec 10 -ErrorAction Stop | Out-Null
    return $true
} catch {
    # HTTP exception messages can contain the secret URL, so report only a fixed warning.
    Write-Warning 'Discord notification could not be delivered; maintenance will continue. Check the webhook configuration and network.'
    return $false
} finally {
    [Net.ServicePointManager]::SecurityProtocol = $previousTls
}
