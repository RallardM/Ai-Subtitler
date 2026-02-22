@echo off
setlocal EnableExtensions

REM Interactive tuning for voice stop delay (voice_stop_ms)
REM Output events (no Whisper transcripts):
REM   VOICE_START
REM   VOICE_END
REM   FLUSH ...
REM Hotkeys:
REM   + / -  : adjust voice_stop_ms by 10ms (prints current value)
REM   q      : quit (prints FINAL voice_stop_ms=..)
REM   Ctrl+C : quit (prints FINAL voice_stop_ms=..)
REM
REM Higher voice_stop_ms => waits longer after you stop talking before FLUSH.
REM Lower  voice_stop_ms => FLUSH sooner (more responsive; may split phrases).

REM Self-spawn into a persistent cmd.exe /k window to avoid flashing on double-click.
REM Opt-out (for terminal usage): set AI_SUBTITLER_NO_SPAWN=1
if "%AI_SUBTITLER_NO_SPAWN%"=="1" goto after_spawn
if "%AI_SUBTITLER_SPAWNED%"=="1" goto after_spawn
set "AI_SUBTITLER_SPAWNED=1"
start "Ai-Subtitler Tune Voice Stop" cmd.exe /k ""%~f0" %*"
exit /b 0
:after_spawn

echo Ai-Subtitler - Tune voice stop delay
echo   Parameter: --voice-stop-ms
echo   What it does: how long voice must be absent before we FLUSH an utterance.
echo   Hotkeys: + / - adjust by 10ms; Ctrl+C quits and prints FINAL value.
echo(

set "EXE=%~dp0ai-subtitler-streamerbot.exe"
if not exist "%EXE%" (
  if exist "%~dp0build\Debug\ai-subtitler-streamerbot.exe" (
    set "EXE=%~dp0build\Debug\ai-subtitler-streamerbot.exe"
  ) else (
    set "EXE=%~dp0build\Release\ai-subtitler-streamerbot.exe"
  )
)

if not exist "%EXE%" (
  echo Executable not found: %EXE%
  echo Build it first, then re-run this script.
  echo(
  echo Press any key to close...
  pause >nul
  exit /b 1
)

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
set "STAMP=%~dp0launcher-stamp-tune-voice-stop-ms.txt"
>"%STAMP%" echo START %date% %time%
>>"%STAMP%" echo CWD: %cd%
>>"%STAMP%" echo EXE: %EXE%
>>"%STAMP%" echo ARGS: %MIC% --debug-voice-stop-ms %EXTRA_ARGS%

"%EXE%" %MIC% --debug-voice-stop-ms %EXTRA_ARGS%
set "ERR=%errorlevel%"
>>"%STAMP%" echo EXIT %ERR% %date% %time%

if not "%ERR%"=="0" echo(
if not "%ERR%"=="0" echo Failed. errorlevel=%ERR%

echo(
echo Press any key to close...
pause >nul
exit /b %ERR%
