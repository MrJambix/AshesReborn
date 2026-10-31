# SPDX-License-Identifier: AGPL-3.0-only
# Part of a modified SandalCore deployment (branch vps-deploy); not the original project or endorsed by Cyb.
# Sends Ctrl+C to the console of process -TargetPid, so a service saves its work and exits by itself. Windows
# delivers it to a whole console, hence a process of its own: this one leaves its console first.
param([Parameter(Mandatory)] [int] $TargetPid)
Add-Type -Namespace Sandal -Name Con -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool FreeConsole();
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool AttachConsole(uint pid);
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool SetConsoleCtrlHandler(IntPtr h, bool add);
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool GenerateConsoleCtrlEvent(uint ev, uint group);
'@
[void][Sandal.Con]::FreeConsole()
if (-not [Sandal.Con]::AttachConsole($TargetPid)) { exit 1 }
[void][Sandal.Con]::SetConsoleCtrlHandler([IntPtr]::Zero, $true)
if (-not [Sandal.Con]::GenerateConsoleCtrlEvent(0, 0)) { exit 1 }
Start-Sleep -Milliseconds 500
exit 0
