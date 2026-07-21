[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [Alias('i')]
    [string]$InputJar,

    [Parameter(Position = 1)]
    [Alias('run')]
    [string]$MainClass,

    [Parameter(Position = 2)]
    [Alias('method', 'm')]
    [string]$MainMethod = 'load',

    [Alias('signature')]
    [ValidateSet('()V', '([Ljava/lang/String;)V')]
    [string]$MainMethodSignature = '()V',

    [string]$ArgumentText,
    [string]$ArgumentFile,

    [Parameter(Position = 3)]
    [Alias('cld')]
    [string[]]$ClassLoaders = @(),

    [Alias('o')]
    [string]$OutputPath,

    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [ValidateSet('x64', 'Win32')]
    [string]$Platform = 'x64',

    [Alias('candump')]
    [bool]$CanDump = $true,

    [switch]$Clean,
    [switch]$Rebuild,
    [Alias('rebuild-tools')]
    [switch]$RebuildTools,
    [Alias('keep-temp')]
    [switch]$KeepTemp,
    [Alias('h', '?')]
    [switch]$Help
)

$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot

function Show-Help {
    @'
jar2dll build options

Required:
  -i <jar>                 Input JAR file.
  -run <class>             Static entry class.

Runtime options:
  -method <name>           Static no-argument void method. Default: load
  -signature <descriptor>  ()V or ([Ljava/lang/String;)V. Default: ()V
  -ArgumentText <text>     Pass text as args[0] to a String[] entry method
  -ArgumentFile <file>     Pass UTF-8 file content as args[0]
  -cld <list>              ClassLoader class names separated by comma, semicolon or |.
                           Omit -cld to use
                           Thread.currentThread().getContextClassLoader().

Output/build options:
  -o <dll>                 Output DLL. Default: build\jar2dll.dll
  -configuration <value>   Release or Debug. Default: Release
  -platform <value>        x64 or Win32. Default: x64
  -candump <true|false>    Embed an opt-in class dump marker. Default: true
  -clean                   Clean all generated files before building.
  -rebuild                 Ask MSBuild to rebuild instead of incrementally build.
  -rebuild-tools           Rebuild reader.jar even when its cache exists.
  -keep-temp               Keep J2DF, config and reader log files after success.
  -verbose                 Print the JAR reader log after packing.
  -help                    Show this help.

Examples:
  .\build.ps1 -i app.jar -run com.example.Main -method load
  .\build.ps1 -i app.jar -run com.example.Main -o dist\app.dll
  .\build.ps1 -i app.jar -run com.example.Main -cld "loader.One;loader.Two"
  .\build.ps1 -i app.jar -run com.example.Main -candump true

Legacy positional syntax remains valid:
  .\build.ps1 app.jar com.example.Main load com.example.Loader
'@ | Write-Host
}

function Invoke-Batch([string]$Path, [string[]]$Arguments = @()) {
    & $Path @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $Path"
    }
}

if ($Help) {
    Show-Help
    exit 0
}
if ([string]::IsNullOrWhiteSpace($InputJar) -or [string]::IsNullOrWhiteSpace($MainClass)) {
    Show-Help
    throw '-i and -run are required.'
}
if ([string]::IsNullOrWhiteSpace($MainMethod)) {
    throw '-method cannot be empty.'
}
if (($null -ne $ArgumentText -or -not [string]::IsNullOrWhiteSpace($ArgumentFile)) -and $MainMethodSignature -ne '([Ljava/lang/String;)V') {
    throw '-ArgumentText and -ArgumentFile require the ([Ljava/lang/String;)V signature.'
}
if (-not [string]::IsNullOrWhiteSpace($ArgumentFile) -and -not (Test-Path -LiteralPath $ArgumentFile -PathType Leaf)) {
    throw "Argument file not found: $ArgumentFile"
}

$inputFullPath = [System.IO.Path]::GetFullPath($InputJar)
if (-not [System.IO.File]::Exists($inputFullPath)) {
    throw "Input JAR not found: $inputFullPath"
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $outputFullPath = Join-Path $projectRoot 'build\jar2dll.dll'
}
else {
    $outputFullPath = [System.IO.Path]::GetFullPath($OutputPath)
}

# Accept PowerShell arrays as well as convenient comma/semicolon/pipe-delimited strings.
$loaderCandidates = [System.Collections.Generic.List[string]]::new()
foreach ($value in $ClassLoaders) {
    foreach ($candidate in ($value -split '[,;|]')) {
        $trimmed = $candidate.Trim()
        if ($trimmed.Length -gt 0 -and -not $loaderCandidates.Contains($trimmed)) {
            $loaderCandidates.Add($trimmed)
        }
    }
}
$loaderConfig = [string]::Join(';', $loaderCandidates)

$tempDirectory = Join-Path $projectRoot 'build\temp'
$toolsDirectory = Join-Path $projectRoot 'tools\build'
$generatedResourceScript = Join-Path $projectRoot 'cpp\maindll\maindll\readJar_resources.rc'
$readerJar = Join-Path $toolsDirectory 'reader.jar'
$utf8WithoutBom = [System.Text.UTF8Encoding]::new($false)

Push-Location $projectRoot
try {
    if ($Clean) {
        Invoke-Batch (Join-Path $projectRoot 'tools\bat\clean.bat')
    }
    if ($RebuildTools -and (Test-Path -LiteralPath $readerJar)) {
        Remove-Item -LiteralPath $readerJar -Force
    }

    [System.IO.Directory]::CreateDirectory($tempDirectory) | Out-Null
    [System.IO.Directory]::CreateDirectory($toolsDirectory) | Out-Null
    Invoke-Batch (Join-Path $projectRoot 'tools\buildTools.bat')

    Write-Host '[1/3] Packing JAR...'
    $readerLog = Join-Path $tempDirectory 'reader.log'
    $readerOutput = Join-Path $tempDirectory (([System.IO.Path]::GetFileName($inputFullPath)) + '.j2df')
    $j2dfPath = Join-Path $tempDirectory 'readJar.j2df'
    Remove-Item -LiteralPath $readerOutput, $j2dfPath -Force -ErrorAction SilentlyContinue

    & java.exe -jar $readerJar $inputFullPath $tempDirectory *> $readerLog
    if ($LASTEXITCODE -ne 0) {
        Get-Content -LiteralPath $readerLog | Write-Host
        throw "JAR packing failed with exit code $LASTEXITCODE."
    }
    if (-not (Test-Path -LiteralPath $readerOutput)) {
        throw "J2DF output was not generated: $readerOutput"
    }
    Move-Item -LiteralPath $readerOutput -Destination $j2dfPath -Force
    if ($VerbosePreference -ne 'SilentlyContinue') {
        Get-Content -LiteralPath $readerLog | Write-Host
    }

    $mainClassConfig = Join-Path $tempDirectory 'mainClass.cfg'
    $mainMethodConfig = Join-Path $tempDirectory 'mainMethodName.cfg'
    $mainMethodSignatureConfig = Join-Path $tempDirectory 'mainMethodSignature.cfg'
    $mainArgumentConfig = Join-Path $tempDirectory 'mainArgument.cfg'
    $mainHasArgumentConfig = Join-Path $tempDirectory 'mainHasArgument.cfg'
    $classLoaderConfig = Join-Path $tempDirectory 'mainClassLoaderName.cfg'
    [System.IO.File]::WriteAllText($mainClassConfig, $MainClass.Trim(), $utf8WithoutBom)
    [System.IO.File]::WriteAllText($mainMethodConfig, $MainMethod.Trim(), $utf8WithoutBom)
    [System.IO.File]::WriteAllText($mainMethodSignatureConfig, $MainMethodSignature, $utf8WithoutBom)
    $hasArgument = $null -ne $ArgumentText -or -not [string]::IsNullOrWhiteSpace($ArgumentFile)
    $argumentValue = if (-not [string]::IsNullOrWhiteSpace($ArgumentFile)) { [System.IO.File]::ReadAllText([System.IO.Path]::GetFullPath($ArgumentFile), $utf8WithoutBom) } else { $ArgumentText }
    [System.IO.File]::WriteAllText($mainArgumentConfig, $argumentValue, $utf8WithoutBom)
    [System.IO.File]::WriteAllText($mainHasArgumentConfig, $hasArgument.ToString().ToLowerInvariant(), $utf8WithoutBom)
    [System.IO.File]::WriteAllText($classLoaderConfig, $loaderConfig, $utf8WithoutBom)

    Write-Host '[2/3] Embedding binary resources...'
    $resourcePayload = Join-Path $tempDirectory 'readJar.payload'
    [System.IO.File]::Copy($j2dfPath, $resourcePayload, $true)
    if ($CanDump) {
        $stream = [System.IO.File]::Open($resourcePayload, [System.IO.FileMode]::Append, [System.IO.FileAccess]::Write)
        try {
            $magic = [System.Text.Encoding]::ASCII.GetBytes('J2D-CANDUMP-V1!!')
            $stream.Write($magic, 0, $magic.Length)
            $length = [System.BitConverter]::GetBytes([uint32](Get-Item -LiteralPath $j2dfPath).Length)
            $stream.Write($length, 0, $length.Length)
        }
        finally { $stream.Dispose() }
    }
    $resourcePairs = @(
        @($resourcePayload, 101), @($mainClassConfig, 102), @($mainMethodConfig, 103),
        @($mainMethodSignatureConfig, 104), @($mainArgumentConfig, 105),
        @($mainHasArgumentConfig, 106), @($classLoaderConfig, 107)
    )
    $resourceLines = @('#include <windows.h>')
    foreach ($pair in $resourcePairs) {
        $escapedPath = [System.IO.Path]::GetFullPath($pair[0]).Replace('\', '\\').Replace('"', '\"')
        $resourceLines += "$($pair[1]) RCDATA `"$escapedPath`""
    }
    [System.IO.File]::WriteAllLines($generatedResourceScript, $resourceLines, $utf8WithoutBom)

    Write-Host '[3/3] Building DLL...'
    $target = if ($Rebuild) { 'Rebuild' } else { 'Build' }
    Invoke-Batch (Join-Path $projectRoot 'tools\bat\msbuild.bat') @(
        $Configuration,
        $Platform,
        $target
    )

    $compiledDll = if ($Platform -eq 'x64') {
        Join-Path $projectRoot "cpp\maindll\x64\$Configuration\maindll.dll"
    }
    else {
        Join-Path $projectRoot "cpp\maindll\$Configuration\maindll.dll"
    }
    if (-not (Test-Path -LiteralPath $compiledDll)) {
        throw "Compiled DLL not found: $compiledDll"
    }

    $outputDirectory = [System.IO.Path]::GetDirectoryName($outputFullPath)
    [System.IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
    Copy-Item -LiteralPath $compiledDll -Destination $outputFullPath -Force

    if (-not $KeepTemp) {
        Remove-Item -LiteralPath $tempDirectory -Recurse -Force -ErrorAction SilentlyContinue
    }
    Write-Host "[OK] Output: $outputFullPath"
}
catch {
    Write-Error $_
    exit 1
}
finally {
    Pop-Location
}
