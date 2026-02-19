@echo off
setlocal

REM One-click launcher (avoids VS Code hyperlink copy/paste issues)

echo Starting Ai-Subtitler...
echo   Model: %~dp0models\ggml-medium.bin
if not "%~1"=="" (
	echo   Mic: %~1
) else (
	echo   Mic: (will prompt)
)
echo   Streamer.bot WS: ws://127.0.0.1:8080/
echo   Action: AI Subtitler (arg key: AiText)
echo.

if not "%~1"=="" (
	call "%~dp0run.cmd" --model "%~dp0models\ggml-medium.bin" --fast --mic "%~1" --ws-url ws://127.0.0.1:8080/ --action-name "AI Subtitler" --arg-key AiText --startup-text "[Ai-Subtitler connected]"
) else (
	call "%~dp0run.cmd" --model "%~dp0models\ggml-medium.bin" --fast --ws-url ws://127.0.0.1:8080/ --action-name "AI Subtitler" --arg-key AiText --startup-text "[Ai-Subtitler connected]"
)

if errorlevel 1 (
	echo.
	echo Failed to start (errorlevel %errorlevel%).
	echo Press any key to close...
	pause >nul
)
