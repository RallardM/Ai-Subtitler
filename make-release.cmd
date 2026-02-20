@echo off
setlocal

set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%PS%" set "PS=powershell"

"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0make-release.ps1" %*
set "ERR=%errorlevel%"
if not "%ERR%"=="0" echo(
if not "%ERR%"=="0" echo Release packaging failed. errorlevel=%ERR%
exit /b %ERR%
