@echo off
setlocal EnableExtensions

REM Debug-only voice detection (Silero VAD). Prints only:
REM   DETECT VOICE
REM   DOES NOT DETECT VOICE

if exist "%~dp0build\Debug\ai-subtitler-streamerbot.exe" (
  set "EXE=%~dp0build\Debug\ai-subtitler-streamerbot.exe"
) else (
  set "EXE=%~dp0build\Release\ai-subtitler-streamerbot.exe"
)

if not exist "%EXE%" (
  echo Executable not found: %EXE%
  echo Build it first:
  echo   cmake -S . -B build -A x64
  echo   cmake --build build --config Debug
  echo   cmake --build build --config Release
  echo(
  echo Press any key to close...
  pause >nul
  exit /b 1
)

REM Download VAD model if needed:
REM   .\download-vad.cmd

set "MIC=%~1"
if "%MIC%"=="" set "MIC=0"

REM Ensure the VAD model exists (debug convenience)
if not exist "%~dp0models\ggml-silero-v6.2.0.bin" (
  echo VAD model missing: %~dp0models\ggml-silero-v6.2.0.bin
  echo Downloading now...
  call "%~dp0download-vad.cmd"
  if errorlevel 1 exit /b 1
)

REM Forward extra args AFTER the mic index.
shift
set "EXTRA_ARGS="
:collect
if "%~1"=="" goto run
set "EXTRA_ARGS=%EXTRA_ARGS% %1"
shift
goto collect

:run
"%EXE%" %MIC% --debug-voice-gate %EXTRA_ARGS%
