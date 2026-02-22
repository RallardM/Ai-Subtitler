@echo off
setlocal EnableExtensions EnableDelayedExpansion

REM One-click launcher (avoids VS Code hyperlink copy/paste issues)

set "MIC_ARG=%~1"
set "HAS_MIC=0"
if not "%MIC_ARG%"=="" if not "%MIC_ARG:~0,1%"=="-" set "HAS_MIC=1"

if "%HAS_MIC%"=="1" (
  echo   Mic: %MIC_ARG%
) else (
  echo   Mic: ^(will prompt^)
)
set "EXTRA_ARGS="
set "HAS_EXPLICIT_MODEL=0"
set "MODEL_INDEX="
set "KEEP_OPEN=0"

if "%HAS_MIC%"=="1" goto with_mic
goto no_mic

:with_mic
shift
:collect_with_mic
if "%~1"=="" goto run_with_mic
if /I "%~1"=="--model" set "HAS_EXPLICIT_MODEL=1"
if /I "%~1"=="--keep-open" (
  set "KEEP_OPEN=1"
  shift
  goto collect_with_mic
)
if /I "%~1"=="--model-index" (
  shift
  if "%~1"=="" (
    echo error: --model-index requires a value
    exit /b 1
  )
  set "MODEL_INDEX=%~1"
  shift
  goto collect_with_mic
)
set "EXTRA_ARGS=%EXTRA_ARGS% %1"
shift
goto collect_with_mic

:run_with_mic
call :pick_model
if errorlevel 1 goto after_run
echo Starting Ai-Subtitler...
if "%HAS_EXPLICIT_MODEL%"=="1" (
  echo   Model: ^(explicit via --model^)
) else (
  echo   Model: %MODEL%
)
echo   Streamer.bot WS: ws://127.0.0.1:8080/
echo   Action: AI Subtitler (arg key: AiText)
echo(

if "%HAS_EXPLICIT_MODEL%"=="1" (
  call "%~dp0run.cmd" --fast --mic "%MIC_ARG%" --ws-url ws://127.0.0.1:8080/ --action-name "AI Subtitler" --arg-key AiText --startup-text "[Ai-Subtitler connected]" %EXTRA_ARGS%
) else (
  call "%~dp0run.cmd" --model "%MODEL%" --fast --mic "%MIC_ARG%" --ws-url ws://127.0.0.1:8080/ --action-name "AI Subtitler" --arg-key AiText --startup-text "[Ai-Subtitler connected]" %EXTRA_ARGS%
)
goto after_run

:no_mic
:collect_no_mic
if "%~1"=="" goto run_no_mic
if /I "%~1"=="--model" set "HAS_EXPLICIT_MODEL=1"
if /I "%~1"=="--keep-open" (
  set "KEEP_OPEN=1"
  shift
  goto collect_no_mic
)
if /I "%~1"=="--model-index" (
  shift
  if "%~1"=="" (
    echo error: --model-index requires a value
    exit /b 1
  )
  set "MODEL_INDEX=%~1"
  shift
  goto collect_no_mic
)
set "EXTRA_ARGS=%EXTRA_ARGS% %1"
shift
goto collect_no_mic

:run_no_mic
call :pick_model
if errorlevel 1 goto after_run
echo Starting Ai-Subtitler...
if "%HAS_EXPLICIT_MODEL%"=="1" (
  echo   Model: ^(explicit via --model^)
) else (
  echo   Model: %MODEL%
)
echo   Streamer.bot WS: ws://127.0.0.1:8080/
echo   Action: AI Subtitler (arg key: AiText)
echo(

if "%HAS_EXPLICIT_MODEL%"=="1" (
  call "%~dp0run.cmd" --fast --ws-url ws://127.0.0.1:8080/ --action-name "AI Subtitler" --arg-key AiText --startup-text "[Ai-Subtitler connected]" %EXTRA_ARGS%
) else (
  call "%~dp0run.cmd" --model "%MODEL%" --fast --ws-url ws://127.0.0.1:8080/ --action-name "AI Subtitler" --arg-key AiText --startup-text "[Ai-Subtitler connected]" %EXTRA_ARGS%
)
goto after_run

:pick_model
if "%HAS_EXPLICIT_MODEL%"=="1" exit /b 0

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

if not "%MODEL_INDEX%"=="" (
  set "MODEL_INDEX_NUM="
  set /a MODEL_INDEX_NUM=!MODEL_INDEX! 2>nul
  if errorlevel 1 (
    echo error: --model-index must be an integer
    exit /b 1
  )
  if !MODEL_INDEX_NUM! LSS 0 (
    echo error: --model-index must be >= 0
    exit /b 1
  )
  if !MODEL_INDEX_NUM! GEQ !MODEL_COUNT! (
    echo error: --model-index out of range. Available: 0..%MODEL_COUNT%-1
    exit /b 1
  )
  for %%I in (!MODEL_INDEX_NUM!) do set "MODEL=!MODEL_PATH_%%I!"
  exit /b 0
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
