@echo off
rem Double-click to repair this PC's texture pack: see repair-pack.ps1, which must be beside this file with IrisStudio.exe.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0repair-pack.ps1"
pause
