@echo off
setlocal

REM One-click launcher (avoids VS Code hyperlink copy/paste issues)

set "MODEL_TINY=%~dp0models\ggml-tiny.bin"
set "MODEL_TINY_EN=%~dp0models\ggml-tiny.en.bin"
set "MODEL_MEDIUM=%~dp0models\ggml-medium.bin"

set "MODEL="
if exist "%MODEL_TINY_EN%" set "MODEL=%MODEL_TINY_EN%"
if not "%MODEL%"=="" goto model_found
if exist "%MODEL_TINY%" set "MODEL=%MODEL_TINY%"
if not "%MODEL%"=="" goto model_found
if exist "%MODEL_MEDIUM%" set "MODEL=%MODEL_MEDIUM%"
if not "%MODEL%"=="" goto model_found

echo No model found under %~dp0models\
echo Expected one of:
echo   %MODEL_TINY_EN%
echo   %MODEL_TINY%
echo   %MODEL_MEDIUM%
echo.
echo Download one (example):
echo   submodules\whisper.cpp\models\download-ggml-model.cmd tiny .\models
exit /b 1

:model_found

echo Starting Ai-Subtitler...
echo   Model: %MODEL%
if not "%~1"=="" (
	echo   Mic: %~1
) else (
	echo   Mic: (will prompt)
)
echo   Streamer.bot WS: ws://127.0.0.1:8080/
echo   Action: AI Subtitler (arg key: AiText)
echo.

if not "%~1"=="" (
	call "%~dp0run.cmd" --model "%MODEL%" --fast --mic "%~1" --ws-url ws://127.0.0.1:8080/ --action-name "AI Subtitler" --arg-key AiText --startup-text "[Ai-Subtitler connected]"
) else (
	call "%~dp0run.cmd" --model "%MODEL%" --fast --ws-url ws://127.0.0.1:8080/ --action-name "AI Subtitler" --arg-key AiText --startup-text "[Ai-Subtitler connected]"
)

if errorlevel 1 (
	echo.
	echo Failed to start (errorlevel %errorlevel%).
	echo Press any key to close...
	pause >nul
)
