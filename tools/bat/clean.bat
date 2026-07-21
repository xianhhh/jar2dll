@echo off
setlocal

rem Resolve the project root so this script also works when launched directly.
pushd "%~dp0..\.."

if exist ".\build" rmdir /S /Q ".\build"
if exist ".\tools\build" rmdir /S /Q ".\tools\build"
if exist ".\cpp\maindll\x64" rmdir /S /Q ".\cpp\maindll\x64"
if exist ".\cpp\maindll\Debug" rmdir /S /Q ".\cpp\maindll\Debug"
if exist ".\cpp\maindll\Release" rmdir /S /Q ".\cpp\maindll\Release"
if exist ".\cpp\maindll\maindll\x64" rmdir /S /Q ".\cpp\maindll\maindll\x64"
if exist ".\cpp\maindll\maindll\Debug" rmdir /S /Q ".\cpp\maindll\maindll\Debug"
if exist ".\cpp\maindll\maindll\Release" rmdir /S /Q ".\cpp\maindll\maindll\Release"
if exist ".\cpp\maindll\maindll\readJar_resources.rc" del /Q ".\cpp\maindll\maindll\readJar_resources.rc"

popd
endlocal
exit /b 0
