@echo off
setlocal
pushd "%~dp0java\readBytesJar"
call ".\gradlew.bat" memoryRewriterJar
set "Result=%errorlevel%"
popd
if not "%Result%"=="0" goto :done
dotnet publish ".\csharp\builder\Jar2Dll.Builder.csproj" -c Release -r win-x64 --self-contained true ^
  /p:PublishSingleFile=true /p:EnableCompressionInSingleFile=true ^
  -o ".\dist\builder"
set "Result=%errorlevel%"
if "%Result%"=="0" <nul set /p "=J2D-BUILDER-IDENTITY-V1">>".\dist\builder\jar2dll-builder.exe"
if "%Result%"=="0" copy /Y ".\dist\builder\jar2dll-builder.exe" ".\dist\builder\j2d.exe" >nul
if "%Result%"=="0" copy /Y ".\java\readBytesJar\build\libs\memory-rewriter.jar" ".\dist\builder\memory-rewriter.jar" >nul
if "%Result%"=="0" if exist ".\dist\builder\jar2dll-builder.pdb" del /Q ".\dist\builder\jar2dll-builder.pdb"
if not "%Result%"=="0" goto :done

dotnet publish ".\csharp\gui\Jar2Dll.Gui.csproj" -c Release -r win-x64 --self-contained true ^
  -o ".\dist\gui"
set "Result=%errorlevel%"
if "%Result%"=="0" copy /Y ".\dist\builder\j2d.exe" ".\dist\gui\j2d.exe" >nul
if "%Result%"=="0" copy /Y ".\dist\builder\memory-rewriter.jar" ".\dist\gui\memory-rewriter.jar" >nul
if "%Result%"=="0" if exist ".\dist\gui\jar2dll-gui.pdb" del /Q ".\dist\gui\jar2dll-gui.pdb"
:done
popd
endlocal & exit /b %Result%
