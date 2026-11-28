@echo off
REM Launcher for the regression runner (tests/regression/run.py).
REM Example: run.cmd run --bin-dir build\Release
where python >nul 2>nul
if %errorlevel%==0 (
    python "%~dp0run.py" %*
) else (
    py -3 "%~dp0run.py" %*
)
exit /b %errorlevel%
