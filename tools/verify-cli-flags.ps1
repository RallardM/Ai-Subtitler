param(
  [string]$ExePath = "",
  [switch]$SkipOffline
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if ([string]::IsNullOrWhiteSpace($ExePath)) {
  $ExePath = Join-Path $RepoRoot "build\Release\ai-subtitler-streamerbot.exe"
}

if (-not (Test-Path $ExePath)) {
  throw "Exe not found: $ExePath (build Release first)"
}

$WavPath = Join-Path $RepoRoot "submodules\whisper.cpp\bindings\go\samples\jfk.wav"
$VadModel = Join-Path $RepoRoot "models\ggml-silero-v6.2.0.bin"

function Invoke-Check {
  param(
    [Parameter(Mandatory=$true)][string]$Label,
    [Parameter(Mandatory=$true)][string[]]$Args,
    [switch]$AllowNonZero
  )

  $out = ""
  $code = 0
  $outFile = $null
  $errFile = $null
  try {
    $outFile = New-TemporaryFile
    $errFile = New-TemporaryFile

    $p = Start-Process -FilePath $ExePath -ArgumentList $Args -WorkingDirectory $RepoRoot -NoNewWindow -Wait -PassThru -RedirectStandardOutput $outFile -RedirectStandardError $errFile
    $code = $p.ExitCode
    $out = ((Get-Content -Raw -Path $outFile) + (Get-Content -Raw -Path $errFile))
  } catch {
    $out = $_.Exception.Message
    $code = 999
  } finally {
    if ($outFile -and (Test-Path $outFile)) { Remove-Item -Force $outFile -ErrorAction SilentlyContinue }
    if ($errFile -and (Test-Path $errFile)) { Remove-Item -Force $errFile -ErrorAction SilentlyContinue }
  }

  $unknown = ($out -match "unknown argument") -or ($out -match "error:\s*unknown")
  $ok = (-not $unknown) -and ($AllowNonZero -or $code -eq 0)

  $snippet = $out
  if ($snippet.Length -gt 240) {
    $snippet = $snippet.Substring(0, 240)
  }
  $snippet = ($snippet -replace "\r\n", "\n")

  [pscustomobject]@{
    Ok = $ok
    ExitCode = $code
    Label = $Label
    Args = ($Args -join " ")
    HasUnknownArg = $unknown
    Snippet = $snippet
  }
}

$tests = @(
  @{ Label = "help"; Args = @("--help") },
  @{ Label = "list-devices"; Args = @("--list-devices") },
  @{ Label = "flag streamerbot"; Args = @("--list-devices", "--streamerbot") },
  @{ Label = "flag voice-gate"; Args = @("--list-devices", "--voice-gate") },
  @{ Label = "opt voice-gate-check-ms"; Args = @("--list-devices", "--voice-gate", "--voice-gate-check-ms", "400") },
  @{ Label = "opt voice-gate-window-ms"; Args = @("--list-devices", "--voice-gate", "--voice-gate-window-ms", "400") },
  @{ Label = "opt voice-gate-pre-roll-ms"; Args = @("--list-devices", "--voice-gate", "--voice-gate-pre-roll-ms", "250") },
  @{ Label = "flag lang-fallback-fr"; Args = @("--list-devices", "--lang-fallback-fr") },
  @{ Label = "flag pre-whisper-guards"; Args = @("--list-devices", "--pre-whisper-guards") },
  @{ Label = "flag post-suppressions"; Args = @("--list-devices", "--post-suppressions") },
  @{ Label = "flag dedup-suffix"; Args = @("--list-devices", "--dedup-suffix") },
  @{ Label = "flag wrap-output"; Args = @("--list-devices", "--wrap-output") },
  @{ Label = "flag block-stats"; Args = @("--list-devices", "--block-stats") },
  @{ Label = "opt dedup-similarity"; Args = @("--list-devices", "--dedup-similarity", "0.90") },

  @{ Label = "combo (many flags)"; Args = @(
      "--list-devices",
      "--streamerbot",
      "--voice-gate",
      "--lang-fallback-fr",
      "--pre-whisper-guards",
      "--post-suppressions",
      "--dedup-suffix",
      "--wrap-output",
      "--block-stats",
      "--dedup-similarity", "0.88"
    )
  },

  @{ Label = "order voice-gate then no-voice-gate"; Args = @("--list-devices", "--voice-gate", "--no-voice-gate") },
  @{ Label = "order no-voice-gate then voice-gate"; Args = @("--list-devices", "--no-voice-gate", "--voice-gate") },

  @{ Label = "mode minimal + one flag"; Args = @("--list-devices", "--mode", "minimal", "--streamerbot") },
  @{ Label = "mode full"; Args = @("--list-devices", "--mode", "full") },
  @{ Label = "all"; Args = @("--list-devices", "--all") },
  @{ Label = "all then override"; Args = @("--list-devices", "--all", "--no-voice-gate") }
)

$results = @()
foreach ($t in $tests) {
  $results += Invoke-Check -Label $t.Label -Args $t.Args
}

if (-not $SkipOffline) {
  if (Test-Path $WavPath) {
    if (Test-Path $VadModel) {
      $results += Invoke-Check -Label "offline test-voice-gate" -Args @("--vad-model", $VadModel, "--test-voice-gate", $WavPath)
      $results += Invoke-Check -Label "offline test-voice-gate + voice-gate flag" -Args @("--voice-gate", "--vad-model", $VadModel, "--test-voice-gate", $WavPath)
      $results += Invoke-Check -Label "offline test-voice-gate-filter" -Args @("--vad-model", $VadModel, "--test-voice-gate-filter", $WavPath)
      $results += Invoke-Check -Label "offline test-voice-gate-filter + voice-gate flag" -Args @("--voice-gate", "--vad-model", $VadModel, "--test-voice-gate-filter", $WavPath)
    } else {
      Write-Warning "Skipping offline checks; VAD model not found: $VadModel"
    }
  } else {
    Write-Warning "Skipping offline checks; wav not found: $WavPath"
  }
}

$ArtifactsDir = Join-Path $RepoRoot "artifacts"
New-Item -ItemType Directory -Force $ArtifactsDir | Out-Null
$LogPath = Join-Path $ArtifactsDir "verify-cli-flags.log"

$tableText = ($results | Format-Table -AutoSize | Out-String)

Write-Host $tableText

$bad = $results | Where-Object { -not $_.Ok }
if ($bad.Count -gt 0) {
  Write-Host "\nFailures (first 3):"
  $bad | Select-Object -First 3 | Format-List

  $badText = ($bad | Format-List | Out-String)
  @(
    "VERIFY CLI FLAGS FAILED",
    (Get-Date).ToString("s"),
    "ExePath: $ExePath",
    "",
    "RESULTS:",
    $tableText,
    "",
    "FAILURES:",
    $badText
  ) | Set-Content -Path $LogPath -Encoding UTF8

  Write-Host "\nWrote log: $LogPath"
  Write-Error ("FAILED: {0} test(s)" -f $bad.Count)
  exit 1
}

@(
  "VERIFY CLI FLAGS OK",
  (Get-Date).ToString("s"),
  "ExePath: $ExePath",
  "",
  "RESULTS:",
  $tableText
) | Set-Content -Path $LogPath -Encoding UTF8

Write-Host "OK: all CLI flag checks passed"
Write-Host "Wrote log: $LogPath"
exit 0
