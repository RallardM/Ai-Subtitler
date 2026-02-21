param(
  [ValidateSet('silero-v6.2.0')]
  [string]$Model = 'silero-v6.2.0'
)

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ModelsDir = Join-Path $RepoRoot 'models'
New-Item -ItemType Directory -Force $ModelsDir | Out-Null

switch ($Model) {
  'silero-v6.2.0' { $File = 'ggml-silero-v6.2.0.bin' }
}

# Source used by whisper.cpp VAD docs: ggml-org/whisper-vad on Hugging Face
$BaseUrl = 'https://huggingface.co/ggml-org/whisper-vad/resolve/main'
$Url = "$BaseUrl/$File"
$OutPath = Join-Path $ModelsDir $File

Write-Host "Downloading VAD model $Model -> $OutPath"
Write-Host "URL: $Url"

Invoke-WebRequest -Uri $Url -OutFile $OutPath

if (-not (Test-Path $OutPath)) {
  throw "Download failed: file not found: $OutPath"
}

$size = (Get-Item $OutPath).Length
if ($size -lt 100*1024) {
  throw "Download looks too small ($size bytes): $OutPath"
}

Write-Host "Done. Downloaded $size bytes."
