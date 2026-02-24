param(
	[ValidateSet(
		'none',
		'streamerbot',
		'voice-gate',
		'lang-fallback-fr',
		'pre-whisper-guards',
		'post-suppressions',
		'dedup-suffix',
		'dedup-similarity',
		'wrap-output',
		'block-stats'
	)]
	[string]$OmitFeature = 'none',

	# Optional multi-omit list (supports iterative ablation without re-enabling previously omitted flags).
	# Examples:
	#   -OmitFeatures streamerbot,lang-fallback-fr
	#   -OmitFeatures streamerbot lang-fallback-fr
	[string[]]$OmitFeatures = @(),

	# Optional mic selector. If omitted, the app will prompt interactively.
	# Examples:
	#   -Mic 0
	#   -Mic Samson
	[string]$Mic = '',

	[ValidateSet('', 'Release', 'Debug')]
	[string]$Config = 'Release',

	# Convenience for setting the app's --voice-gate-max-voice-ms without fighting PowerShell arg parsing.
	[int]$VoiceGateMaxVoiceMs = 0,
	# Convenience for setting the app's --voice-gate-check-ms.
	[int]$VoiceGateCheckMs = 0,
	# Convenience for setting the app's --voice-gate-window-ms. Use -1 to not pass; 0 means auto.
	[int]$VoiceGateWindowMs = -1,
	# Convenience for setting the app's --voice-gate-pre-roll-ms. Use -1 to not pass; 0 disables pre-roll.
	[int]$VoiceGatePreRollMs = -1,

	# Dry-run: just validate argument parsing (no mic capture).
	[switch]$ListDevices,

	# Any extra args to pass through unchanged.
	[Parameter(ValueFromRemainingArguments = $true)]
	[string[]]$ExtraArgs = @()
)

$ErrorActionPreference = 'Stop'

function Resolve-RepoRoot {
	# Prefer built-in script path vars; $MyInvocation.MyCommand.Path can be null.
	if ($PSScriptRoot) {
		return (Split-Path -Parent $PSScriptRoot)
	}
	if ($PSCommandPath) {
		$here = Split-Path -Parent $PSCommandPath
		return (Split-Path -Parent $here)
	}
	throw "Cannot determine repo root (no PSScriptRoot/PSCommandPath)"
}

function Resolve-ModelPath {
	param([string]$RepoRoot)

	$modelsDir = Join-Path $RepoRoot 'models'
	if (-not (Test-Path -LiteralPath $modelsDir)) {
		throw "models dir not found: $modelsDir"
	}

	$preferred = @(
		'ggml-tiny.en.bin',
		'ggml-tiny.bin',
		'ggml-base.en.bin',
		'ggml-base.bin'
	)

	foreach ($name in $preferred) {
		$p = Join-Path $modelsDir $name
		if (Test-Path -LiteralPath $p) {
			return $p
		}
	}

	$others = @(Get-ChildItem -LiteralPath $modelsDir -Filter 'ggml-*.bin' -File -ErrorAction SilentlyContinue |
		Where-Object { $_.Name -notlike 'ggml-silero-*.bin' } |
		Sort-Object Name)

	if ($others.Count -eq 1) {
		return $others[0].FullName
	}

	if ($others.Count -gt 1) {
		$names = ($others | ForEach-Object { $_.Name }) -join "\n  "
		throw ("Multiple Whisper models found; please pick one explicitly via -ExtraArgs --model <path> or keep only one in models/. Found:\n  {0}" -f $names)
	}

	throw "No Whisper model found in models/. Run .\\download-model-tiny-en.cmd (or place a ggml-*.bin model in .\\models)."
}

function Remove-FeatureFromArgs {
	param(
		[string[]]$InputArgs,
		[string]$Feature
	)

	if ($Feature -eq 'none') { return $InputArgs }

	$out = New-Object System.Collections.Generic.List[string]
	for ($i = 0; $i -lt $InputArgs.Count; $i++) {
		$a = $InputArgs[$i]

		if ($Feature -eq 'dedup-similarity') {
			# Remove both: --dedup-similarity <value>
			if ($a -ieq '--dedup-similarity') {
				$i++
				continue
			}
		}

		if ($a -ieq ('--' + $Feature)) {
			continue
		}

		$out.Add($a)
	}

	return $out.ToArray()
}

function Normalize-OmitFeatures {
	param(
		[string]$Single,
		[string[]]$Many
	)

	$allowed = @(
		'streamerbot',
		'voice-gate',
		'lang-fallback-fr',
		'pre-whisper-guards',
		'post-suppressions',
		'dedup-suffix',
		'dedup-similarity',
		'wrap-output',
		'block-stats'
	)

	$all = New-Object System.Collections.Generic.List[string]
	$manySafe = @()
	if ($null -ne $Many) { $manySafe = $Many }
	foreach ($x in $manySafe) {
		if ([string]::IsNullOrWhiteSpace($x)) { continue }
		foreach ($part in ($x -split ',')) {
			$y = $part.Trim()
			if ([string]::IsNullOrWhiteSpace($y)) { continue }
			$all.Add($y)
		}
	}

	if (-not [string]::IsNullOrWhiteSpace($Single) -and $Single -ne 'none') {
		$all.Add($Single)
	}

	$norm = @($all.ToArray() | ForEach-Object { $_.ToLowerInvariant() } | Select-Object -Unique)
	foreach ($f in $norm) {
		if ($allowed -notcontains $f) {
			throw "Invalid -OmitFeatures entry '$f'. Allowed: $($allowed -join ', ')"
		}
	}

	return $norm
}

$repoRoot = Resolve-RepoRoot

# Avoid accidental multi-instance mic capture (which causes extreme lag and inconsistent transcripts).
try {
	Get-Process ai-subtitler-streamerbot -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
} catch {
	# best-effort
}

function Resolve-ExePath {
	param(
		[Parameter(Mandatory = $true)][string]$RepoRoot,
		[ValidateSet('', 'Release', 'Debug')][string]$Config
	)

	$exe = Join-Path $RepoRoot 'ai-subtitler-streamerbot.exe'
	if (Test-Path -LiteralPath $exe) { return $exe }

	$prefer = $Config
	if ([string]::IsNullOrWhiteSpace($prefer)) { $prefer = $env:AI_SUBTITLER_CONFIG }

	$try = New-Object System.Collections.Generic.List[string]
	if ($prefer -ieq 'Debug') {
		$try.Add((Join-Path $RepoRoot 'build\Debug\ai-subtitler-streamerbot.exe'))
		$try.Add((Join-Path $RepoRoot 'build\Release\ai-subtitler-streamerbot.exe'))
	} elseif ($prefer -ieq 'Release') {
		$try.Add((Join-Path $RepoRoot 'build\Release\ai-subtitler-streamerbot.exe'))
		$try.Add((Join-Path $RepoRoot 'build\Debug\ai-subtitler-streamerbot.exe'))
	} else {
		# Default preference: Release first, then Debug.
		$try.Add((Join-Path $RepoRoot 'build\Release\ai-subtitler-streamerbot.exe'))
		$try.Add((Join-Path $RepoRoot 'build\Debug\ai-subtitler-streamerbot.exe'))
	}

	# Fallbacks used by some build layouts.
	$try.Add((Join-Path $RepoRoot 'build\bin\Release\ai-subtitler-streamerbot.exe'))
	$try.Add((Join-Path $RepoRoot 'build\bin\Debug\ai-subtitler-streamerbot.exe'))

	foreach ($p in $try) {
		if (Test-Path -LiteralPath $p) { return $p }
	}

	throw "Executable not found. Tried: `n- " + ($try -join "\n- ")
}

$exePath = Resolve-ExePath -RepoRoot $repoRoot -Config $Config

$modelPath = Resolve-ModelPath -RepoRoot $repoRoot
$vadModelPath = Join-Path $repoRoot 'models\ggml-silero-v6.2.0.bin'

# Baseline args (match launcher behavior, but with explicit per-feature flags; no --all)
$featureArgs = @(
	'--streamerbot',
	'--voice-gate',
	'--lang-fallback-fr',
	'--pre-whisper-guards',
	'--post-suppressions',
	'--dedup-suffix',
	'--wrap-output',
	'--block-stats',
	'--dedup-similarity', '0.90'
)

$omitList = Normalize-OmitFeatures -Single $OmitFeature -Many $OmitFeatures
foreach ($f in $omitList) {
	$featureArgs = Remove-FeatureFromArgs $featureArgs $f
}

if (($featureArgs -contains '--voice-gate') -and (-not (Test-Path -LiteralPath $vadModelPath))) {
	Write-Warning "Voice gate requested but VAD model is missing: $vadModelPath"
	Write-Warning "Run .\\download-vad.cmd to enable Silero voice gate (otherwise it will fall back to simple VAD)."
}

$commonArgs = @(
	'--model', $modelPath,
	'--fast',
	'--suppress-lone-you',
	'--ws-url', 'ws://127.0.0.1:8080/',
	'--action-name', 'AI Subtitler',
	'--arg-key', 'AiText',
	'--startup-text', '[Ai-Subtitler connected]'
)

if (($featureArgs -contains '--voice-gate') -and (Test-Path -LiteralPath $vadModelPath)) {
	$commonArgs += @('--vad-model', $vadModelPath)
}

if ($VoiceGateMaxVoiceMs -gt 0 -and ($featureArgs -contains '--voice-gate')) {
	$commonArgs += @('--voice-gate-max-voice-ms', "$VoiceGateMaxVoiceMs")
}

if ($VoiceGateCheckMs -gt 0 -and ($featureArgs -contains '--voice-gate')) {
	$commonArgs += @('--voice-gate-check-ms', "$VoiceGateCheckMs")
}

if ($VoiceGateWindowMs -ge 0 -and ($featureArgs -contains '--voice-gate')) {
	$commonArgs += @('--voice-gate-window-ms', "$VoiceGateWindowMs")
}

if ($VoiceGatePreRollMs -ge 0 -and ($featureArgs -contains '--voice-gate')) {
	$commonArgs += @('--voice-gate-pre-roll-ms', "$VoiceGatePreRollMs")
}

$args = @()
if ($ListDevices) {
	$args += @('--list-devices')
}

if (-not [string]::IsNullOrWhiteSpace($Mic)) {
	$args += @('--mic', $Mic)
}

$args += $commonArgs
$args += $featureArgs
if ($ExtraArgs) {
	$args += $ExtraArgs
}

if ($omitList.Count -gt 0) {
	Write-Output ("[Ai-Subtitler] OmitFeatures: {0}" -f ($omitList -join ', '))
} else {
	Write-Output "[Ai-Subtitler] OmitFeatures: (none)"
}
if ($ListDevices) {
  Write-Output "[Ai-Subtitler] Mode: --list-devices (dry run)"
} else {
	if ([string]::IsNullOrWhiteSpace($Mic)) {
		Write-Output "[Ai-Subtitler] Mic: (interactive prompt)"
	} else {
		Write-Output "[Ai-Subtitler] Mic: $Mic"
	}
}

Write-Output ("[Ai-Subtitler] Args: {0}" -f ($args -join ' '))

$oldCwd = Get-Location
try {
	Set-Location -LiteralPath $repoRoot

	# Merge stderr into stdout so startup status / warnings are visible.
	& $exePath @args 2>&1
	$code = $LASTEXITCODE
	Write-Output ("[Ai-Subtitler] Exited with code: {0}" -f $code)
	exit $code
} finally {
	Set-Location -LiteralPath $oldCwd
}
