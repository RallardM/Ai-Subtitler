@echo off
setlocal

REM Wrapper to avoid VS Code markdown-link paste issues.
REM Usage examples:
REM   start-ai-subtitler-mic1.cmd -ListDevices
REM   start-ai-subtitler-mic1.cmd

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start-ai-subtitler-mic1.ps1" %*
