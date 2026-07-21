@echo off
setlocal

set "outputRoot=%~dp0build"
set "javaProject=%~dp0..\java\readBytesJar"

if not exist "%outputRoot%" mkdir "%outputRoot%"

pushd "%javaProject%"
call ".\gradlew.bat" --quiet jar
if errorlevel 1 (
    popd
    echo [ERROR] Failed to build reader.jar.
    endlocal
    exit /b 1
)
popd

copy /Y "%javaProject%\build\libs\reader.jar" "%outputRoot%\reader.jar" >nul
if errorlevel 1 (
    echo [ERROR] Failed to copy reader.jar.
    endlocal
    exit /b 1
)

endlocal
exit /b 0
