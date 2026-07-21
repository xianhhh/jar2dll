[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $PSScriptRoot 'build'
}
if ([string]::IsNullOrWhiteSpace($env:JAVA_HOME)) { throw 'JAVA_HOME must point to a JDK.' }
if (-not (Test-Path -LiteralPath (Join-Path $env:JAVA_HOME 'include\jni.h'))) { throw 'JAVA_HOME does not contain include\jni.h.' }
$cmake = (Get-Command cmake.exe -ErrorAction SilentlyContinue).Source
if ([string]::IsNullOrWhiteSpace($cmake)) {
    $cmake = @(
        (Join-Path $env:ProgramFiles 'CMake\bin\cmake.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'CMake\bin\cmake.exe')
    ) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if ([string]::IsNullOrWhiteSpace($cmake)) {
    throw 'CMake was not found. Install Visual Studio C++ CMake tools or add cmake.exe to PATH.'
}
$vsDevCmd = $null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path -LiteralPath $vswhere) {
    $vsInstall = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not [string]::IsNullOrWhiteSpace($vsInstall)) {
        $candidate = Join-Path $vsInstall 'Common7\Tools\VsDevCmd.bat'
        if (Test-Path -LiteralPath $candidate) { $vsDevCmd = $candidate }
    }
}

function Invoke-CMake([string]$Arguments) {
    if (-not [string]::IsNullOrWhiteSpace($vsDevCmd)) {
        $command = 'call "{0}" -arch=x64 -host_arch=x64 >nul && "{1}" {2}' -f $vsDevCmd, $cmake, $Arguments
        & cmd.exe /d /c $command
    }
    else {
        & $cmake $Arguments.Split(' ')
    }
}

Invoke-CMake "-S `"$PSScriptRoot`" -B `"$OutputDirectory`" -G `"Visual Studio 18 2026`" -A x64"
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
Invoke-CMake "--build `"$OutputDirectory`" --config $Configuration --parallel"
if ($LASTEXITCODE -ne 0) { throw 'CMake build failed.' }
Write-Host "[OK] JNI fixtures: $OutputDirectory\$Configuration"
