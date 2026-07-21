@echo off
setlocal

rem Normalize PATH casing. Some launchers expose both PATH and Path; legacy MSBuild then
rem fails while creating CL.exe with "key already added: Path/PATH".
set "NormalizedPath=%PATH%"
set "PATH="
set "Path=%NormalizedPath%"

set "MyJavaInclude=%JAVA_HOME%\include;%JAVA_HOME%\include\win32"
set "MSBuildExe="
set "BuildConfiguration=%~1"
set "BuildPlatform=%~2"
set "BuildTarget=%~3"
if not defined BuildConfiguration set "BuildConfiguration=Release"
if not defined BuildPlatform set "BuildPlatform=x64"
if not defined BuildTarget set "BuildTarget=Build"

rem Prefer an MSBuild already available in the current developer environment.
for /f "delims=" %%i in ('where msbuild.exe 2^>nul') do if not defined MSBuildExe set "MSBuildExe=%%i"

rem Otherwise ask Visual Studio Installer for the latest C++ Build Tools instance.
if not defined MSBuildExe if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
    for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do if not defined MSBuildExe set "MSBuildExe=%%i"
)

rem Fallback for the toolset version currently used by this project.
if not defined MSBuildExe if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe" (
    set "MSBuildExe=%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe"
)

if not defined MSBuildExe (
    echo [ERROR] MSBuild was not found. Install Visual Studio Build Tools with C++ support.
    endlocal
    exit /b 1
)

"%MSBuildExe%" ".\cpp\maindll\maindll.slnx" ^
    /nologo ^
    /verbosity:minimal ^
    /t:%BuildTarget% ^
    /p:Configuration=%BuildConfiguration% ^
    /p:Platform=%BuildPlatform% ^
    /p:IncludePath="%MyJavaInclude%;$(IncludePath)"

set "BuildResult=%errorlevel%"
endlocal & exit /b %BuildResult%
