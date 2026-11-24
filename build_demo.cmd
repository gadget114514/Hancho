@echo off
REM Build the demo-mode binaries in build-demo\ (HANCHO_BUILD_DEMO=ON).
REM The normal build\ directory keeps HANCHO_BUILD_DEMO=OFF; never package a
REM release from build-demo\. See doc\demo.md.
REM
REM Usage: build_demo.cmd [Release|Debug] [--no-test]

setlocal
set ROOT=%~dp0.
set BUILD_DIR=%ROOT%\build-demo
set CONFIG=%~1
if "%CONFIG%"=="" set CONFIG=Release
if /I "%CONFIG%"=="--no-test" (
    set CONFIG=Release
    set NO_TEST=1
)
if /I "%~2"=="--no-test" set NO_TEST=1

cmake -S "%ROOT%" -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 -DHANCHO_BUILD_DEMO=ON
if errorlevel 1 (
    echo CMake configuration failed
    exit /b 1
)

cmake --build "%BUILD_DIR%" --config %CONFIG% --target Hancho hancho-cli hancho-dummy demo_smoke
if errorlevel 1 (
    echo Build failed
    exit /b 1
)

if "%NO_TEST%"=="1" goto done
"%BUILD_DIR%\%CONFIG%\demo_smoke.exe"
if errorlevel 1 (
    echo demo_smoke failed
    exit /b 1
)

:done
echo Demo build: %BUILD_DIR%\%CONFIG%
endlocal
