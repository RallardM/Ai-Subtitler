param(
  [string]$ZipPath = "dist\\Ai-Subtitler-Release.zip",
  [string]$WavPath = "",
  [int]$TimeoutSec = 60,
  [switch]$KeepTemp
)

$ErrorActionPreference = "Stop"

function Quote-Cmd([string]$s) {
  if ($null -eq $s) { return '""' }
  '"' + ($s -replace '"','""') + '"'
}

function Invoke-Cmd([
  Parameter(Mandatory=$true)][string]$CommandLine,
  [Parameter(Mandatory=$true)][string]$WorkingDirectory,
  [int]$TimeoutSec = 60
) {
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $env:ComSpec
  $psi.Arguments = "/d /s /c $CommandLine"
  $psi.WorkingDirectory = $WorkingDirectory
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $psi.UseShellExecute = $false

  $p = New-Object System.Diagnostics.Process
  $p.StartInfo = $psi

  if (-not $p.Start()) {
    throw "Failed to start process: $CommandLine"
  }

  if (-not $p.WaitForExit([Math]::Max(1, $TimeoutSec) * 1000)) {
    try { $p.Kill($true) } catch {}
    throw "Timed out after ${TimeoutSec}s: $CommandLine"
  }

  $stdout = $p.StandardOutput.ReadToEnd()
  $stderr = $p.StandardError.ReadToEnd()

  [pscustomobject]@{
    ExitCode = $p.ExitCode
    Stdout   = $stdout
    Stderr   = $stderr
    Output   = ($stdout + $stderr)
    Command  = $CommandLine
  }
}

function Assert-True([bool]$Condition, [string]$Message) {
  if (-not $Condition) {
    throw "ASSERT FAILED: $Message"
  }
}

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $repoRoot

$zipFull = if ([IO.Path]::IsPathRooted($ZipPath)) { $ZipPath } else { Join-Path $repoRoot $ZipPath }
Assert-True (Test-Path $zipFull) "ZIP not found: $zipFull"

if ([string]::IsNullOrWhiteSpace($WavPath)) {
  $defaultWav = Join-Path $repoRoot "submodules\\whisper.cpp\\samples\\jfk.wav"
  if (Test-Path $defaultWav) {
    $WavPath = $defaultWav
  }
}

Assert-True (-not [string]::IsNullOrWhiteSpace($WavPath)) "No WAV provided. Pass -WavPath C:\\path\\file.wav"
Assert-True (Test-Path $WavPath) "WAV not found: $WavPath"

Write-Host "[SelfTest] ZIP: $zipFull"
Write-Host "[SelfTest] WAV: $WavPath"

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ("Ai-Subtitler-Release-Test-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force $tempRoot | Out-Null

try {
  Expand-Archive -Path $zipFull -DestinationPath $tempRoot -Force

  # Determine release dir: ZIP currently stores files at the root.
  $topFiles = Get-ChildItem -LiteralPath $tempRoot -File -ErrorAction SilentlyContinue
  $topDirs = Get-ChildItem -LiteralPath $tempRoot -Directory -ErrorAction SilentlyContinue
  $releaseDir = $tempRoot
  if ($topFiles.Count -eq 0 -and $topDirs.Count -eq 1) {
    $releaseDir = $topDirs[0].FullName
  }

  Write-Host "[SelfTest] Extracted to: $releaseDir"

  # Non-interactive: do not spawn new windows; never pause.
  $env:AI_SUBTITLER_NO_SPAWN = "1"
  $env:AI_SUBTITLER_NO_PAUSE = "1"

  $offline650 = Join-Path $releaseDir "debug-smoke-test-offline-vg-650.cmd"
  $offline930 = Join-Path $releaseDir "debug-smoke-test-offline-vg-930.cmd"
  Assert-True (Test-Path $offline650) "Missing: debug-smoke-test-offline-vg-650.cmd"
  Assert-True (Test-Path $offline930) "Missing: debug-smoke-test-offline-vg-930.cmd"

  Write-Host "[SelfTest] Offline VG A/B (650 vs 930)"

  $r650 = Invoke-Cmd -WorkingDirectory $releaseDir -TimeoutSec $TimeoutSec -CommandLine ("call " + (Quote-Cmd $offline650) + " " + (Quote-Cmd $WavPath))
  Assert-True ($r650.ExitCode -eq 0) ("offline 650 failed (exit={0}). Output:`n{1}" -f $r650.ExitCode, $r650.Output)
  Assert-True ($r650.Output -notmatch "Press any key to close") "offline 650 unexpectedly printed pause prompt"

  $r930 = Invoke-Cmd -WorkingDirectory $releaseDir -TimeoutSec $TimeoutSec -CommandLine ("call " + (Quote-Cmd $offline930) + " " + (Quote-Cmd $WavPath))
  Assert-True ($r930.ExitCode -eq 0) ("offline 930 failed (exit={0}). Output:`n{1}" -f $r930.ExitCode, $r930.Output)
  Assert-True ($r930.Output -notmatch "Press any key to close") "offline 930 unexpectedly printed pause prompt"

  $vg650Path = Join-Path $releaseDir "debug-smoke-test-offline-vg-650.vg.txt"
  $vg930Path = Join-Path $releaseDir "debug-smoke-test-offline-vg-930.vg.txt"
  Assert-True (Test-Path $vg650Path) "Missing output: debug-smoke-test-offline-vg-650.vg.txt"
  Assert-True (Test-Path $vg930Path) "Missing output: debug-smoke-test-offline-vg-930.vg.txt"

  $vg650 = Get-Content -LiteralPath $vg650Path -Raw
  $vg930 = Get-Content -LiteralPath $vg930Path -Raw

  $m650 = [regex]::Match($vg650, '\[VG\] FLUSH t=(\d+)ms')
  $m930 = [regex]::Match($vg930, '\[VG\] FLUSH t=(\d+)ms')
  if (-not $m650.Success) {
    $snippet = $vg650
    if ($snippet.Length -gt 800) { $snippet = $snippet.Substring(0, 800) }
    throw "ASSERT FAILED: No [VG] FLUSH found in 650 vg log. First bytes:`n$snippet"
  }
  if (-not $m930.Success) {
    $snippet = $vg930
    if ($snippet.Length -gt 800) { $snippet = $snippet.Substring(0, 800) }
    throw "ASSERT FAILED: No [VG] FLUSH found in 930 vg log. First bytes:`n$snippet"
  }

  $t650 = [int]$m650.Groups[1].Value
  $t930 = [int]$m930.Groups[1].Value
  $delta = $t930 - $t650

  Write-Host ("[SelfTest] FLUSH t650={0}ms t930={1}ms delta={2}ms" -f $t650, $t930, $delta)

  Assert-True ($t930 -gt $t650) "Expected t930 > t650 (voice_stop_ms A/B)"
  Assert-True ($delta -ge 200 -and $delta -le 450) "Unexpected delta (expected ~280ms). Got ${delta}ms"

  # Device list preflight (informational): helps diagnose live latency scripts failing on mic 0.
  $listCmd = Join-Path $releaseDir "list-devices.cmd"
  if (Test-Path $listCmd) {
    Write-Host "[SelfTest] Listing capture devices"
    $ld = Invoke-Cmd -WorkingDirectory $releaseDir -TimeoutSec 15 -CommandLine ("call " + (Quote-Cmd $listCmd))
    if ($ld.ExitCode -ne 0) {
      Write-Warning "list-devices failed (exit=$($ld.ExitCode)). Output:"
      Write-Host $ld.Output
    } else {
      $hasMic0 = ($ld.Output -match '(?m)^\s*\[0\]\s+')
      if (-not $hasMic0) {
        Write-Warning "Mic index 0 was not found in device list. Live latency smoke tests will fail unless you pass a valid mic index or substring."
      }
      Write-Host $ld.Output
    }
  }

  Write-Host "[SelfTest] PASS"
  exit 0
} finally {
  if ($KeepTemp) {
    Write-Host "[SelfTest] Kept temp folder: $tempRoot"
  } else {
    try {
      Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    } catch {
    }
  }
}
