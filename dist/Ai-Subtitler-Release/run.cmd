@echo off
setlocal

set "EXE=%~dp0ai-subtitler-streamerbot.exe"

if exist "%EXE%" goto have_exe

set "EXE=%~dp0build\Release\ai-subtitler-streamerbot.exe"
if exist "%EXE%" goto have_exe

set "EXE=%~dp0build\bin\Release\ai-subtitler-streamerbot.exe"
if exist "%EXE%" goto have_exe

if not exist "%EXE%" (
  echo Executable not found: %EXE%
  echo Build it first:
  echo   cmake -S . -B build -A x64
  echo   cmake --build build --config Release
  exit /b 1
)

:have_exe

"%EXE%" %*
