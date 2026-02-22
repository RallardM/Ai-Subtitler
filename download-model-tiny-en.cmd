@echo off
setlocal EnableExtensions

echo Downloading Whisper model: tiny.en (English-only)
echo(

set "MODELS_DIR=%~dp0models"
if not exist "%MODELS_DIR%" mkdir "%MODELS_DIR%" >nul 2>&1

set "OUT=%MODELS_DIR%\ggml-tiny.en.bin"
set "URL=https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.en.bin"

set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%PS%" set "PS=powershell"

"%PS%" -NoProfile -ExecutionPolicy Bypass -Command "Invoke-WebRequest -Uri '%URL%' -OutFile '%OUT%'"
set "ERR=%errorlevel%"
if not "%ERR%"=="0" exit /b %ERR%

if not exist "%OUT%" (
	echo Download failed: file not found: %OUT%
	exit /b 2
)

echo(
echo Done.
exit /b 0
