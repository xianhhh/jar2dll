[CmdletBinding()]
param([string]$BuildDirectory, [int]$StressRounds = 1000)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) { $BuildDirectory = Join-Path $PSScriptRoot 'build\Debug' }
$BuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)

function Run-Test([string]$Exe, [string[]]$Arguments) {
    $path = Join-Path $BuildDirectory $Exe
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing test executable: $path" }
    & $path @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed with exit code $LASTEXITCODE" }
}

Run-Test 'j2d_memory_pe_test.exe' @((Join-Path $BuildDirectory 'j2d_basic_native.dll'))
Run-Test 'j2d_jni_exports_test.exe' @()
Run-Test 'j2d_memory_bundle_test.exe' @()
Run-Test 'j2d_memory_import_test.exe' @((Join-Path $BuildDirectory 'j2d_fixture_provider.dll'), (Join-Path $BuildDirectory 'j2d_dependent_native.dll'), $StressRounds.ToString())
Run-Test 'j2d_memory_registry_test.exe' @((Join-Path $BuildDirectory 'j2d_chain_provider.dll'), (Join-Path $BuildDirectory 'j2d_chain_bridge.dll'), (Join-Path $BuildDirectory 'j2d_chain_jni.dll'))
Run-Test 'j2d_memory_lifecycle_test.exe' @((Join-Path $BuildDirectory 'j2d_lifecycle_fixture.dll'), $StressRounds.ToString())
Run-Test 'j2d_memory_negative_test.exe' @((Join-Path $BuildDirectory 'j2d_basic_native.dll'))
Run-Test 'j2d_memory_registry_negative_test.exe' @((Join-Path $BuildDirectory 'j2d_fixture_provider.dll'), (Join-Path $BuildDirectory 'j2d_dependent_native.dll'))
Write-Host "J2D_MEMORY_TEST_SUITE_OK rounds=$StressRounds"
