@echo off
setlocal

REM If both Release and Debug exist, you can force which one is used:
REM   set AI_SUBTITLER_CONFIG=Debug
REM   .\start-ai-subtitler.cmd 0
set "PREFER=%AI_SUBTITLER_CONFIG%"

set "EXE=%~dp0ai-subtitler-streamerbot.exe"

if exist "%EXE%" goto have_exe

if /I "%PREFER%"=="Debug" goto prefer_debug
if /I "%PREFER%"=="Release" goto prefer_release

:prefer_release
set "EXE=%~dp0build\Release\ai-subtitler-streamerbot.exe"
if exist "%EXE%" goto have_exe

set "EXE=%~dp0build\Debug\ai-subtitler-streamerbot.exe"
if exist "%EXE%" goto have_exe
goto check_bin

:prefer_debug
set "EXE=%~dp0build\Debug\ai-subtitler-streamerbot.exe"
if exist "%EXE%" goto have_exe

set "EXE=%~dp0build\Release\ai-subtitler-streamerbot.exe"
if exist "%EXE%" goto have_exe

:check_bin

set "EXE=%~dp0build\bin\Release\ai-subtitler-streamerbot.exe"
if exist "%EXE%" goto have_exe

set "EXE=%~dp0build\bin\Debug\ai-subtitler-streamerbot.exe"
if exist "%EXE%" goto have_exe

if not exist "%EXE%" (
  echo Executable not found: %EXE%
  echo Build it first:
  echo   cmake -S . -B build -A x64
  echo   cmake --build build --config Release
  echo   cmake --build build --config Debug
  exit /b 1
)

:have_exe

"%EXE%" %*
