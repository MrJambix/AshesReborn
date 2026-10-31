# SPDX-License-Identifier: AGPL-3.0-only
# Compatibility entry point for older shortcuts. The EXE now manages setup.
$ErrorActionPreference = 'Stop'
Start-Process -FilePath "$PSScriptRoot\RebornLauncher.exe" -WorkingDirectory $PSScriptRoot
