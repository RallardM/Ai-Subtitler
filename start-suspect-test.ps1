param(
  [Parameter(ValueFromRemainingArguments=$true)]
  [string[]]$Args
)

$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
$suspectLog = Join-Path $root 'suspect.jsonl'
$suspectDir = Join-Path $root 'suspect-audio'
$stampPath = Join-Path $root 'launcher-stamp-suspect-test.txt'

# Optional positional mic index as first arg (if it doesn't start with '-')
$mic = $null
$extra = @()
if ($Args.Count -gt 0 -and -not $Args[0].StartsWith('-')) {
  $mic = $Args[0]
  if ($Args.Count -gt 1) { $extra = $Args[1..($Args.Count-1)] }
} else {
  $extra = $Args
}

# Pick a default model (prefer tiny.en)
$model = Join-Path $root 'models\ggml-tiny.en.bin'
if (-not (Test-Path $model)) {
  $model = Join-Path $root 'models\ggml-tiny.bin'
}
if (-not (Test-Path $model)) {
  Write-Host "No default model found." -ForegroundColor Yellow
  Write-Host "Expected one of:" -ForegroundColor Yellow
  Write-Host "  $root\models\ggml-tiny.en.bin" -ForegroundColor Yellow
  Write-Host "  $root\models\ggml-tiny.bin" -ForegroundColor Yellow
  exit 1
}

# Start fresh logs for this run
Remove-Item -Force $suspectLog -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force $suspectDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $suspectDir | Out-Null

@(
  ("START {0:o}" -f (Get-Date)),
  ("CWD: {0}" -f (Get-Location).Path),
  ("MODEL: {0}" -f $model),
  ("MIC_ARG: {0}" -f ($mic ?? '')),
  ("ARGS: {0}" -f ($Args -join ' ')),
  "NOTE: In voice-gate mode, you must pause ~3s after speaking to flush to Whisper."
) | Set-Content -Path $stampPath -Encoding ASCII

$baseArgs = @('--model', $model, '--fast', '--suppress-lone-you', '--suspect-log', $suspectLog, '--suspect-dump-dir', $suspectDir)
if ($null -ne $mic) {
  & (Join-Path $root 'run.ps1') '--mic' $mic @baseArgs @extra
} else {
  & (Join-Path $root 'run.ps1') @baseArgs @extra
}

$err = $LASTEXITCODE
Add-Content -Path $stampPath -Encoding ASCII -Value ("EXIT {0} {1:o}" -f $err, (Get-Date))
exit $err
