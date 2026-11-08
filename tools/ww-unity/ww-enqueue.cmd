@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0ww-enqueue.ps1" %*
