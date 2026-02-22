@echo off
setlocal EnableExtensions

REM Interactive tuning for Silero VAD sensitivity (vad_voice_thold)
REM Output:
REM   DETECT VOICE
REM   DOES NOT DETECT VOICE
REM Hotkeys:
REM   + / -  : adjust vad_voice_thold by 0.01 (prints current value)
REM   Ctrl+C : quit (prints FINAL vad_voice_thold=..)
REM
REM Higher vad_voice_thold => less sensitive (fewer false DETECT in silence)
REM Lower  vad_voice_thold => more sensitive (may DETECT on noise)

REM Self-spawn into a persistent cmd.exe /k window to avoid flashing on double-click.
REM Opt-out (for terminal usage): set AI_SUBTITLER_NO_SPAWN=1
if "%AI_SUBTITLER_NO_SPAWN%"=="1" goto after_spawn
if "%AI_SUBTITLER_SPAWNED%"=="1" goto after_spawn
set "AI_SUBTITLER_SPAWNED=1"
start "Ai-Subtitler Tune VAD Threshold" cmd.exe /k ""%~f0" %*"
exit /b 0
:after_spawn

echo Ai-Subtitler - Tune VAD sensitivity
echo   Parameter: --vad-voice-thold
echo   What it does: sets Silero VAD probability threshold.
echo     Higher = less sensitive (better silence).
echo     Lower  = more sensitive (may false-detect).
echo   Hotkeys: + / - adjust by 0.01; Ctrl+C quits and prints FINAL value.
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
set "STAMP=%~dp0launcher-stamp-tune-vad-voice-thold.txt"
>"%STAMP%" echo START %date% %time%
>>"%STAMP%" echo CWD: %cd%
>>"%STAMP%" echo EXE: %EXE%
>>"%STAMP%" echo ARGS: %MIC% --debug-voice-gate %EXTRA_ARGS%

"%EXE%" %MIC% --debug-voice-gate %EXTRA_ARGS%
set "ERR=%errorlevel%"
>>"%STAMP%" echo EXIT %ERR% %date% %time%

if not "%ERR%"=="0" echo(
if not "%ERR%"=="0" echo Failed. errorlevel=%ERR%

echo(
echo Press any key to close...
pause >nul
exit /b %ERR%
