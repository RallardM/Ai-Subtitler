param(
  [Parameter(ValueFromRemainingArguments=$true)]
  [string[]]$Args
)

$ErrorActionPreference = 'Stop'

$exe = Join-Path $PSScriptRoot 'build\Release\ai-subtitler-streamerbot.exe'

if (-not (Test-Path $exe)) {
  Write-Host "Executable not found: $exe" -ForegroundColor Yellow
  Write-Host "Build it first:" -ForegroundColor Yellow
  Write-Host "  cmake -S . -B build -A x64" -ForegroundColor Yellow
  Write-Host "  cmake --build build --config Release" -ForegroundColor Yellow
  exit 1
}

& $exe @Args
