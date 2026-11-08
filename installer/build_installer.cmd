@echo off
setlocal
rem Build the Inno Setup installer for Hancho.
rem Requires Inno Setup 6 and an assembled payload in dist\Hancho-v<version>-win64\.
rem The version is read from project(... VERSION x.y.z ...) in CMakeLists.txt.

set "VERSION="
for /f "tokens=4 delims=() " %%v in ('findstr /b /c:"project(Hancho VERSION " "%~dp0..\CMakeLists.txt"') do set "VERSION=%%v"
if not defined VERSION (
    echo [ERROR] Could not read the project version from CMakeLists.txt.
    exit /b 1
)

set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 7\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" (
    echo [ERROR] ISCC.exe not found. Install Inno Setup 6 from https://jrsoftware.org/isdl.php
    exit /b 1
)

"%ISCC%" "/DMyAppVersion=%VERSION%" "%~dp0Hancho.iss"
if errorlevel 1 (
    echo [ERROR] Inno Setup compilation failed.
    exit /b 1
)
echo [OK] Installer written to dist\Hancho-v%VERSION%-win64-setup.exe
exit /b 0
