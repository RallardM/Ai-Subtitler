param(
  [switch]$ListDevices,
  [string]$ModelPath,
  [int]$DeviceIndex = 0,
  [string]$DeviceName,
  [string]$WsUrl = "ws://127.0.0.1:8080/",
  [string]$ActionName = "AI Subtitler",
  [string]$ArgKey = "AiText",
  [string]$WsPassword,
  [string]$StartupText = "[Ai-Subtitler connected]",

  [Parameter(ValueFromRemainingArguments = $true)]
  [string[]]$ExtraArgs
)

$ErrorActionPreference = 'Stop'

# If someone pastes a double-dash arg (e.g. "--list-devices"), PowerShell can bind it
# positionally to $ModelPath. Treat that as an extra arg instead.
if ($ModelPath -and $ModelPath.StartsWith('--')) {
  $ExtraArgs = @($ModelPath) + @($ExtraArgs)
  $ModelPath = $null
}

$exe = Join-Path $PSScriptRoot 'build\Release\ai-subtitler-streamerbot.exe'
if (-not (Test-Path $exe)) {
  Write-Host "Executable not found: $exe" -ForegroundColor Yellow
  Write-Host "Build it first:" -ForegroundColor Yellow
  Write-Host "  cmake -S . -B build -A x64" -ForegroundColor Yellow
  Write-Host "  cmake --build build --config Release" -ForegroundColor Yellow
  exit 1
}

$passThroughListDevices = $false
if ($ExtraArgs) {
  $passThroughListDevices = $ExtraArgs -contains '--list-devices'
}

if ($ListDevices -or $passThroughListDevices) {
  & $exe --list-devices
  exit $LASTEXITCODE
}

function Get-CaptureDevices {
  $lines = & $exe --list-devices 2>$null
  $devices = @()
  foreach ($line in $lines) {
    if ($line -match '^\s*\[(\d+)\]\s*(.*)\s*$') {
      $devices += [pscustomobject]@{
        Index = [int]$matches[1]
        Name  = $matches[2]
      }
    }
  }
  return $devices
}

$devices = @(Get-CaptureDevices)
if (-not $devices -or $devices.Count -eq 0) {
  Write-Host "No SDL2 capture devices found." -ForegroundColor Yellow
  Write-Host "Try running: .\start-ai-subtitler-mic1.ps1 -ListDevices" -ForegroundColor Yellow
  exit 2
}

if ($DeviceName) {
  $micValue = $DeviceName
} else {
  # Validate requested index; if invalid but there is exactly one device, fall back to it.
  $valid = $devices.Index -contains $DeviceIndex
  if (-not $valid) {
    if ($devices.Count -eq 1) {
      $fallback = $devices[0].Index
      Write-Host "Requested DeviceIndex=$DeviceIndex is not available; using the only capture device index ${fallback}: $($devices[0].Name)" -ForegroundColor Yellow
      $DeviceIndex = $fallback
    } else {
      Write-Host "Requested DeviceIndex=$DeviceIndex is not available." -ForegroundColor Yellow
      Write-Host "Available devices:" -ForegroundColor Yellow
      foreach ($d in $devices) {
        Write-Host ("  [{0}] {1}" -f $d.Index, $d.Name) -ForegroundColor Yellow
      }
      Write-Host "Re-run with -DeviceIndex <n> or -DeviceName <substring>." -ForegroundColor Yellow
      exit 2
    }
  }
  $micValue = [string]$DeviceIndex
}

if (-not $ModelPath) {
  $ModelPath = Join-Path $PSScriptRoot 'models\ggml-medium.bin'
}

if (-not (Test-Path $ModelPath)) {
  Write-Host "Model file not found: $ModelPath" -ForegroundColor Yellow
  Write-Host "Download one (example):" -ForegroundColor Yellow
  Write-Host "  New-Item -ItemType Directory -Force models | Out-Null" -ForegroundColor Yellow
  Write-Host "  submodules\whisper.cpp\models\download-ggml-model.cmd medium .\models" -ForegroundColor Yellow
  exit 1
}

$resolvedModel = (Resolve-Path $ModelPath).Path

$argsList = @(
  '--model', $resolvedModel,
  '--mic', $micValue,
  '--ws-url', $WsUrl,
  '--action-name', $ActionName,
  '--arg-key', $ArgKey,
  '--startup-text', $StartupText
)

if ($WsPassword) {
  $argsList += @('--ws-password', $WsPassword)
}

if ($ExtraArgs) {
  $argsList += $ExtraArgs
}

Write-Host "Starting Ai-Subtitler..." -ForegroundColor Cyan
Write-Host "  Exe: $exe"
Write-Host "  Model: $resolvedModel"
if ($DeviceName) {
  Write-Host "  Device name: $DeviceName"
} else {
  Write-Host "  Device index: $DeviceIndex"
}
Write-Host "  Streamer.bot WS: $WsUrl"
Write-Host "  Action: $ActionName (arg key: $ArgKey)"

& $exe @argsList
