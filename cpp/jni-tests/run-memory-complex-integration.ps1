[CmdletBinding()]
param(
    [string]$Builder,
    [string]$FixtureBuildDirectory,
    [string]$Workspace
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($Builder)) { $Builder = Join-Path $PSScriptRoot '..\..\dist\builder\j2d.exe' }
if ([string]::IsNullOrWhiteSpace($FixtureBuildDirectory)) { $FixtureBuildDirectory = Join-Path $PSScriptRoot 'build\Debug' }
if ([string]::IsNullOrWhiteSpace($Workspace)) { $Workspace = Join-Path $PSScriptRoot '..\..\Temp\memory-complex-integration' }
$Builder = [IO.Path]::GetFullPath($Builder)
$FixtureBuildDirectory = [IO.Path]::GetFullPath($FixtureBuildDirectory)
$Workspace = [IO.Path]::GetFullPath($Workspace)
if (-not (Test-Path -LiteralPath $Builder -PathType Leaf)) { throw "Missing builder: $Builder" }
if ([string]::IsNullOrWhiteSpace($env:JAVA_HOME)) { throw 'JAVA_HOME must point to an x64 JDK.' }
$javac = Join-Path $env:JAVA_HOME 'bin\javac.exe'
$java = Join-Path $env:JAVA_HOME 'bin\java.exe'
$jar = Join-Path $env:JAVA_HOME 'bin\jar.exe'
foreach ($tool in @($javac, $java, $jar)) { if (-not (Test-Path -LiteralPath $tool)) { throw "Missing JDK tool: $tool" } }
foreach ($dll in 'j2d_chain_provider.dll', 'j2d_chain_bridge.dll', 'j2d_chain_jni.dll') {
    if (-not (Test-Path -LiteralPath (Join-Path $FixtureBuildDirectory $dll))) { throw "Missing native fixture: $dll. Run build-tests.ps1 first." }
}

New-Item -ItemType Directory -Force -Path $Workspace | Out-Null
$classes = Join-Path $Workspace 'classes'
New-Item -ItemType Directory -Force -Path $classes | Out-Null
$chain = Join-Path $PSScriptRoot '..\..\java\Test\src\main\java\xh\xianhhh\test\nativebridge\ChainNative.java'
$main = Join-Path $PSScriptRoot 'java\xh\xianhhh\test\memory\MemoryChainMain.java'
& $javac -d $classes $chain $main
if ($LASTEXITCODE -ne 0) { throw 'Could not compile memory chain classes.' }
$inputJar = Join-Path $Workspace 'memory-chain-input.jar'
& $jar --create --file $inputJar -C $classes .
if ($LASTEXITCODE -ne 0) { throw 'Could not create memory chain input JAR.' }
foreach ($dll in 'j2d_chain_provider.dll', 'j2d_chain_bridge.dll', 'j2d_chain_jni.dll') {
    & $jar --update --file $inputJar -C $FixtureBuildDirectory $dll
    if ($LASTEXITCODE -ne 0) { throw "Could not add $dll to the input JAR." }
}
$outputDll = Join-Path $Workspace 'memory-chain.dll'
& $Builder -i $inputJar -run xh.xianhhh.test.memory.MemoryChainMain -native-mode memory -platform x64 -configuration Debug -o $outputDll -temp $Workspace
if ($LASTEXITCODE -ne 0) { throw 'Memory-mode build failed.' }
$hostSource = Join-Path $PSScriptRoot 'java\MemoryChainHost.java'
& $javac -d $classes $hostSource
if ($LASTEXITCODE -ne 0) { throw 'Could not compile memory chain host.' }
$previousErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$output = & $java -cp $classes MemoryChainHost $outputDll 2>&1
$javaExitCode = $LASTEXITCODE
$ErrorActionPreference = $previousErrorActionPreference
$output | ForEach-Object { Write-Host $_ }
$outputText = ($output | Out-String)
if ($javaExitCode -ne 0 -or $outputText -notmatch 'J2D_MEMORY_COMPLEX_CHAIN_OK') { throw 'Memory chain runtime verification failed.' }
Write-Host 'J2D_MEMORY_COMPLEX_INTEGRATION_OK'
