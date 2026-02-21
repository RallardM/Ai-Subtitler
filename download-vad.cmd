@echo off
setlocal EnableExtensions

set "MODEL=%~1"
if "%MODEL%"=="" set "MODEL=silero-v6.2.0"

set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%PS%" set "PS=powershell"

"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0download-vad.ps1" -Model "%MODEL%"
set "ERR=%errorlevel%"
if not "%ERR%"=="0" echo(
if not "%ERR%"=="0" echo Download failed. errorlevel=%ERR%
if not "%ERR%"=="0" echo Press any key to close...
if not "%ERR%"=="0" pause >nul
exit /b %ERR%
