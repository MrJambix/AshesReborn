# SPDX-License-Identifier: AGPL-3.0-only
# Exercise the real runner in an isolated checkout layout with tiny native test processes.
$ErrorActionPreference = 'Stop'
$runner = Join-Path (Split-Path -Parent $PSScriptRoot) 'run_tests.ps1'
$scratch = Join-Path ([IO.Path]::GetTempPath()) ('AshesReborn-runner-tests-' + [Guid]::NewGuid().ToString('N'))
$dist = Join-Path $scratch 'dist\Release'
$script = Join-Path $scratch 'scripts\run_tests.ps1'
$baseline = Join-Path $scratch 'baseline.txt'
New-Item -ItemType Directory -Path $dist, (Split-Path -Parent $script) -Force | Out-Null
Copy-Item -LiteralPath $runner -Destination $script
$fixture = Join-Path $scratch 'fixture.exe'
$cases = 0

function Set-Tests([string[]] $Names) {
    foreach ($file in Get-ChildItem -LiteralPath $dist -File) { Remove-Item -LiteralPath $file.FullName }
    foreach ($name in $Names) { Copy-Item -LiteralPath $fixture -Destination (Join-Path $dist $name) }
}

function Run-Case([string] $Name, [int] $Exit, [string[]] $Extra = @(), [string] $MustContain = '') {
    $output = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script -NoBuild @Extra 2>&1 | Out-String
    $actual = $LASTEXITCODE
    if ($actual -ne $Exit -or ($MustContain -and $output -notmatch $MustContain)) {
        throw ("{0}: expected exit {1}, got {2}`n{3}" -f $Name, $Exit, $actual, $output)
    }
    $script:cases++
    Write-Output ('PASS ' + $Name)
}

try {
    Add-Type -OutputAssembly $fixture -OutputType ConsoleApplication -TypeDefinition @'
using System;
using System.IO;
using System.Reflection;
public static class Fixture {
    public static int Main() {
        string name = Path.GetFileNameWithoutExtension(Assembly.GetExecutingAssembly().Location);
        if (name.Contains("fail") || name == "test_worldd_region_load") { Console.Error.WriteLine("fixture failure"); return 7; }
        if (name == "test_skip") Console.WriteLine("[skip] optional fixture unavailable");
        return 0;
    }
}
'@
    Set-Tests @('test_pass.exe')
    Run-Case 'all-passing run succeeds and saves its baseline' 0 @('-SaveBaseline', $baseline) '1 tests, 0 failing'
    Run-Case 'matching healthy baseline succeeds' 0 @('-CompareTo', $baseline) 'Matches baseline'
    Set-Tests @('test_pass.exe', 'test_fail.exe')
    Run-Case 'a native failure makes the runner fail' 1 @() '2 tests, 1 failing'
    Run-Case 'a changed baseline fails' 1 @('-CompareTo', $baseline) 'DIFFERS FROM BASELINE'
    Run-Case 'saving a failing baseline still fails' 1 @('-SaveBaseline', $baseline) '2 tests, 1 failing'
    Run-Case 'matching a failing baseline still fails' 1 @('-CompareTo', $baseline) 'Matches baseline'
    Set-Tests @('test_pass.exe', 'test_skip.exe')
    Run-Case 'skipped checks remain visible' 0 @() 'NOTE: skipped part or all.*test_skip'
    Set-Tests @('test_pass.exe', 'test_worldd_region_load.exe')
    Run-Case 'heavy tests are excluded explicitly by default' 0 @() 'NOTE: not run.*test_worldd_region_load'
    Run-Case 'an explicitly requested heavy failure fails the run' 1 @('-Heavy') '2 tests, 1 failing'
    Set-Tests @('test_a_pass.exe')
    Set-Content -LiteralPath (Join-Path $dist 'test_broken.exe') -Value 'not an executable' -Encoding ASCII
    Run-Case 'an unstartable test cannot inherit the previous success' 1 @() '2 tests, 1 failing'
    Write-Output ("{0} runner scenarios passed." -f $cases)
} finally {
    $resolved = [IO.Path]::GetFullPath($scratch)
    $tempPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        -not [IO.Path]::GetFileName($resolved).StartsWith('AshesReborn-runner-tests-')) { throw 'Unsafe runner-test cleanup path.' }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
