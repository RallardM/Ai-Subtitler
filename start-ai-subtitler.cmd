@echo off
setlocal EnableExtensions EnableDelayedExpansion

REM One-click launcher (avoids VS Code hyperlink copy/paste issues)

echo Ai-Subtitler
echo   Mic: ^(will prompt^)

set "EXTRA_ARGS="
set "KEEP_OPEN=0"

:collect
if "%~1"=="" goto run
if /I "%~1"=="--keep-open" (
  set "KEEP_OPEN=1"
  shift
  goto collect
)
set "EXTRA_ARGS=%EXTRA_ARGS% %1"
shift
goto collect

:run
call :pick_model
if errorlevel 1 goto after_run

echo Starting Ai-Subtitler...
echo   Model: %MODEL%
echo   Streamer.bot WS: ws://127.0.0.1:8080/
echo   Action: AI Subtitler (arg key: AiText)
echo   Mode: --fast
echo   Glitch filter: --suppress-lone-you

set "VAD_MODEL=%~dp0models\ggml-silero-v6.2.0.bin"
if exist "%VAD_MODEL%" (
  echo   Voice gate: REQUIRED (Silero model found^)
) else (
  echo   Voice gate: REQUIRED but Silero model is MISSING
  echo     Expected: %VAD_MODEL%
  echo     Fix: run .\download-vad.cmd
  echo(
  exit /b 1
)
echo(

call "%~dp0run.cmd" --model "%MODEL%" --all --fast --suppress-lone-you --ws-url ws://127.0.0.1:8080/ --action-name "AI Subtitler" --arg-key AiText --startup-text "[Ai-Subtitler connected]" %EXTRA_ARGS%
goto after_run

:pick_model
set "MODELS_DIR=%~dp0models"
if not exist "%MODELS_DIR%" mkdir "%MODELS_DIR%" >nul 2>&1

set /a MODEL_COUNT=0
for %%F in ("%MODELS_DIR%\ggml-*.bin") do (
  if exist "%%~fF" (
    if /I not "%%~nxF"=="ggml-silero-v6.2.0.bin" (
      set "MODEL_PATH_!MODEL_COUNT!=%%~fF"
      set "MODEL_NAME_!MODEL_COUNT!=%%~nxF"
      set /a MODEL_COUNT+=1
    )
  )
)

if "%MODEL_COUNT%"=="0" (
  echo No Whisper model found under %MODELS_DIR%
  echo(
  echo Download options ^(Release ZIP^):
  echo   .\download-model-tiny-multilanguage.cmd
  echo   .\download-model-tiny-en.cmd
  echo(
  echo Press any key to close...
  pause >nul
  exit /b 1
)

if "%MODEL_COUNT%"=="1" (
  set "MODEL=!MODEL_PATH_0!"
  exit /b 0
)

echo Available Whisper models in .\models:
for /l %%I in (0,1,%MODEL_COUNT%-1) do (
  echo   [%%I] !MODEL_NAME_%%I!
)
echo(
set "PICK=0"
set /p "PICK=Pick model index (0..%MODEL_COUNT%-1) [default: 0]: "
if "%PICK%"=="" set "PICK=0"

for /f "delims=0123456789" %%Z in ("%PICK%") do (
  echo Invalid selection.
  exit /b 1
)
if %PICK% LSS 0 (
  echo Invalid selection.
  exit /b 1
)
if %PICK% GEQ %MODEL_COUNT% (
  echo Invalid selection.
  exit /b 1
)

for %%I in (%PICK%) do set "MODEL=!MODEL_PATH_%%I!"
exit /b 0

:after_run
set "ERR=%errorlevel%"
if not "%ERR%"=="0" echo(
if not "%ERR%"=="0" echo Failed to start (errorlevel %ERR%).
if not "%ERR%"=="0" echo Press any key to close...
if not "%ERR%"=="0" pause >nul

if "%KEEP_OPEN%"=="1" (
  echo(
  echo Press any key to close...
  pause >nul
)
endlocal
exit /b
