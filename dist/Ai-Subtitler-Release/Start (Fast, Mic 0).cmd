@echo off
setlocal EnableExtensions
call "%~dp0start-ai-subtitler.cmd" 0 --fast %*
set "ERR=%errorlevel%"
if not "%ERR%"=="0" echo(
if not "%ERR%"=="0" echo Failed. errorlevel=%ERR%
if not "%ERR%"=="0" pause >nul
exit /b %ERR%
