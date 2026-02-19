@echo off
setlocal

REM One-click launcher (avoids VS Code hyperlink copy/paste issues)

echo Starting Ai-Subtitler...
echo   Model: %~dp0models\ggml-medium.bin
echo   Device index: 1
echo   Streamer.bot WS: ws://127.0.0.1:8080/
echo   Action: AI Subtitler (arg key: AiText)
echo.

call "%~dp0run.cmd" --model "%~dp0models\ggml-medium.bin" --device-index 1 --ws-url ws://127.0.0.1:8080/ --action-name "AI Subtitler" --arg-key AiText --startup-text "[Ai-Subtitler connected]"

if errorlevel 1 (
	echo.
	echo Failed to start (errorlevel %errorlevel%).
	echo Press any key to close...
	pause >nul
)
