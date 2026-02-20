@echo off
setlocal EnableExtensions

REM One-click launcher (avoids VS Code hyperlink copy/paste issues)

set "MODEL_TINY=%~dp0models\ggml-tiny.bin"
set "MODEL_TINY_EN=%~dp0models\ggml-tiny.en.bin"
set "MODEL_MEDIUM=%~dp0models\ggml-medium.bin"

set "MODEL="
if exist "%MODEL_TINY%" set "MODEL=%MODEL_TINY%"
if not "%MODEL%"=="" goto model_found
if exist "%MODEL_TINY_EN%" set "MODEL=%MODEL_TINY_EN%"
if not "%MODEL%"=="" goto model_found
if exist "%MODEL_MEDIUM%" set "MODEL=%MODEL_MEDIUM%"
if not "%MODEL%"=="" goto model_found

echo No model found under %~dp0models\
echo Expected one of:
echo   %MODEL_TINY_EN%
echo   %MODEL_TINY%
echo   %MODEL_MEDIUM%
echo(
echo Download one (example):
echo   submodules\whisper.cpp\models\download-ggml-model.cmd tiny .\models
exit /b 1

:model_found

echo Starting Ai-Subtitler...
echo   Model: %MODEL%
if /I "%MODEL%"=="%MODEL_MEDIUM%" (
  echo   NOTE: Using medium model fallback. For a big speed boost, download tiny:
  echo     submodules\whisper.cpp\models\download-ggml-model.cmd tiny .\models
)
set "MIC_ARG=%~1"
set "HAS_MIC=0"
if not "%MIC_ARG%"=="" if not "%MIC_ARG:~0,1%"=="-" set "HAS_MIC=1"

if "%HAS_MIC%"=="1" (
  echo   Mic: %MIC_ARG%
) else (
  echo   Mic: (will prompt)
)
echo   Streamer.bot WS: ws://127.0.0.1:8080/
echo   Action: AI Subtitler (arg key: AiText)
echo(

if "%HAS_MIC%"=="1" goto with_mic
goto no_mic

:with_mic
shift
set "EXTRA_ARGS="
:collect_with_mic
if "%~1"=="" goto run_with_mic
set "EXTRA_ARGS=%EXTRA_ARGS% %1"
shift
goto collect_with_mic

:run_with_mic
call "%~dp0run.cmd" --model "%MODEL%" --fast --mic "%MIC_ARG%" --ws-url ws://127.0.0.1:8080/ --action-name "AI Subtitler" --arg-key AiText --startup-text "[Ai-Subtitler connected]" %EXTRA_ARGS%
goto after_run

:no_mic
set "EXTRA_ARGS="
:collect_no_mic
if "%~1"=="" goto run_no_mic
set "EXTRA_ARGS=%EXTRA_ARGS% %1"
shift
goto collect_no_mic

:run_no_mic
call "%~dp0run.cmd" --model "%MODEL%" --fast --ws-url ws://127.0.0.1:8080/ --action-name "AI Subtitler" --arg-key AiText --startup-text "[Ai-Subtitler connected]" %EXTRA_ARGS%
goto after_run

:after_run
set "ERR=%errorlevel%"
if not "%ERR%"=="0" echo(
if not "%ERR%"=="0" echo Failed to start (errorlevel %ERR%).
if not "%ERR%"=="0" echo Press any key to close...
if not "%ERR%"=="0" pause >nul
endlocal
exit /b
