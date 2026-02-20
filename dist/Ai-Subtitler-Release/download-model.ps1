param(
  [ValidateSet('tiny','tiny.en','medium')]
  [string]$Model = 'tiny'
)

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ModelsDir = Join-Path $RepoRoot 'models'
New-Item -ItemType Directory -Force $ModelsDir | Out-Null

switch ($Model) {
  'tiny'    { $File = 'ggml-tiny.bin' }
  'tiny.en' { $File = 'ggml-tiny.en.bin' }
  'medium'  { $File = 'ggml-medium.bin' }
}

# Source used by whisper.cpp: ggerganov/whisper.cpp on Hugging Face
$BaseUrl = 'https://huggingface.co/ggerganov/whisper.cpp/resolve/main'
$Url = "$BaseUrl/$File"
$OutPath = Join-Path $ModelsDir $File

Write-Host "Downloading $Model -> $OutPath"
Write-Host "URL: $Url"

# Use Invoke-WebRequest for broad compatibility.
Invoke-WebRequest -Uri $Url -OutFile $OutPath

if (-not (Test-Path $OutPath)) {
  throw "Download failed: file not found: $OutPath"
}

$size = (Get-Item $OutPath).Length
if ($size -lt 1024*1024) {
  throw "Download looks too small ($size bytes): $OutPath"
}

Write-Host "Done. Downloaded $size bytes."