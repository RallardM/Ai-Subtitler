param(
  [string]$ZipPath = "$PSScriptRoot\..\dist\Ai-Subtitler-Release.zip"
)

$ErrorActionPreference = 'Stop'

$ZipPath = (Resolve-Path $ZipPath).Path
$out = Join-Path $env:TEMP ("Ai-Subtitler-Release-smoke-" + [Guid]::NewGuid().ToString())

Expand-Archive -Force $ZipPath $out

# Ensure at least one model exists so start-ai-subtitler.cmd doesn't pause/prompt.
$modelsDir = Join-Path $out 'models'
New-Item -ItemType Directory -Force $modelsDir | Out-Null
$dummyModel = Join-Path $modelsDir 'ggml-tiny.bin'
if (-not (Test-Path $dummyModel)) {
  Set-Content -Encoding Byte -Path $dummyModel -Value ([byte[]](1..16))
}

Set-Location $out

# Run wrapper in-place (no new window) for automation.
$env:AI_SUBTITLER_NO_SPAWN = '1'
$env:AI_SUBTITLER_NO_PAUSE = '1'

& cmd.exe /c 'call "Start (Fast, Mic 0).cmd" --help'
$exitCode = $LASTEXITCODE

Write-Host "Extracted: $out"
Write-Host "Exit: $exitCode"

$stamp = Join-Path $out 'launcher-stamp-start-fast-mic0.txt'
if (Test-Path $stamp) {
  Write-Host '--- stamp (first 50 lines) ---'
  Get-Content $stamp | Select-Object -First 50
} else {
  Write-Host 'Stamp not found.'
}

exit $exitCode
