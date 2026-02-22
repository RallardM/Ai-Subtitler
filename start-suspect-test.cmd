@echo off
setlocal EnableExtensions EnableDelayedExpansion

REM Shortcut launcher for suspect logging (JSONL + WAV dumps)
REM Usage:
REM   .\start-suspect-test.cmd [mic_index] [extra args...]
REM Examples:
REM   .\start-suspect-test.cmd 1
REM   .\start-suspect-test.cmd 1 --no-voice-gate
REM   .\start-suspect-test.cmd 1 --vad-voice-thold 0.70

set "ROOT=%~dp0"
set "SUSPECT_LOG=%ROOT%suspect.jsonl"
set "SUSPECT_DIR=%ROOT%suspect-audio"

REM Optional positional mic index as first argument (if it doesn't start with '-')
set "MIC_ARG=%~1"
set "HAS_MIC=0"
if not "%MIC_ARG%"=="" if not "%MIC_ARG:~0,1%"=="-" set "HAS_MIC=1"

REM Shift past mic argument so %* becomes extra args
if "%HAS_MIC%"=="1" shift

REM Pick a default model (prefer tiny.en)
set "MODEL=%ROOT%models\ggml-tiny.en.bin"
if exist "%MODEL%" goto have_model
set "MODEL=%ROOT%models\ggml-tiny.bin"
if exist "%MODEL%" goto have_model

echo No default model found.
echo Expected one of:
echo   %ROOT%models\ggml-tiny.en.bin
echo   %ROOT%models\ggml-tiny.bin
echo(
echo Download one of the models into .\models\ then retry.
exit /b 1

:have_model

REM Start fresh logs for this run
if exist "%SUSPECT_LOG%" del /q "%SUSPECT_LOG%" >nul 2>&1
if exist "%SUSPECT_DIR%" rmdir /s /q "%SUSPECT_DIR%" >nul 2>&1
mkdir "%SUSPECT_DIR%" >nul 2>&1

set "STAMP=%ROOT%launcher-stamp-suspect-test.txt"
>"%STAMP%" echo START %date% %time%
>>"%STAMP%" echo CWD: %cd%
>>"%STAMP%" echo MODEL: %MODEL%
>>"%STAMP%" echo MIC_ARG: %MIC_ARG%
>>"%STAMP%" echo ARGS: %*
>>"%STAMP%" echo NOTE: In voice-gate mode, you must pause ~3s after speaking to flush to Whisper.

if "%HAS_MIC%"=="1" (
  call "%ROOT%run.cmd" --mic "%MIC_ARG%" --model "%MODEL%" --fast --suppress-lone-you --suspect-log "%SUSPECT_LOG%" --suspect-dump-dir "%SUSPECT_DIR%" %*
) else (
  call "%ROOT%run.cmd" --model "%MODEL%" --fast --suppress-lone-you --suspect-log "%SUSPECT_LOG%" --suspect-dump-dir "%SUSPECT_DIR%" %*
)

set "ERR=%errorlevel%"
>>"%STAMP%" echo EXIT %ERR% %date% %time%
exit /b %ERR%
