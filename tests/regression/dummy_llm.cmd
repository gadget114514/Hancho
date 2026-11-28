@echo off
REM Launcher for the Python dummy LLM (tests/regression/dummy_llm.py).
REM Uses python when it is on PATH, otherwise the Python launcher (py -3).
where python >nul 2>nul
if %errorlevel%==0 (
    python "%~dp0dummy_llm.py" %*
) else (
    py -3 "%~dp0dummy_llm.py" %*
)
exit /b %errorlevel%
