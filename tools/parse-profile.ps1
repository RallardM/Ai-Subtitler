param(
	[Parameter(Mandatory = $true, Position = 0, ValueFromRemainingArguments = $true)]
	[string[]]$Path,

	[ValidateSet('sent', 'all')]
	[string]$Outcome = 'sent'
)

$ErrorActionPreference = 'Stop'

function Parse-ProfLine {
	param([string]$Line)

	if (-not $Line.StartsWith('[PROF] ')) { return $null }

	$obj = [ordered]@{}
	foreach ($token in ($Line.Substring(7) -split ' ')) {
		if (-not $token) { continue }
		$eq = $token.IndexOf('=')
		if ($eq -lt 1) { continue }
		$k = $token.Substring(0, $eq)
		$v = $token.Substring($eq + 1)
		$obj[$k] = $v
	}

	if (-not $obj.Contains('outcome')) { return $null }

	$asInt64 = {
		param($s)
		if ($null -eq $s) { return $null }
		try { return [int64]$s } catch { return $null }
	}

	[pscustomobject]@{
		outcome    = [string]$obj['outcome']
		gated      = [int]([int64]$obj['gated'])
		block_ms   = & $asInt64 $obj['block_ms']
		gate_total = & $asInt64 $obj['gate_total']
		voice      = & $asInt64 $obj['voice']
		silent     = & $asInt64 $obj['silent']
		whisper    = & $asInt64 $obj['whisper']
		total      = & $asInt64 $obj['total']
		text_len   = & $asInt64 $obj['text_len']
		raw        = $Line
	}
}

function Summary-Stats {
	param(
		[Parameter(Mandatory = $true)]
		[string]$Label,
		[Parameter(Mandatory = $true)]
		[int64[]]$Values
	)

	$vals = $Values | Where-Object { $_ -ge 0 }
	if (-not $vals -or $vals.Count -eq 0) {
		return [pscustomobject]@{ label = $Label; n = 0; avg = $null; min = $null; max = $null }
	}

	$m = $vals | Measure-Object -Average -Minimum -Maximum
	[pscustomobject]@{
		label = $Label
		n     = [int]$m.Count
		avg   = [math]::Round($m.Average, 1)
		min   = [int64]$m.Minimum
		max   = [int64]$m.Maximum
	}
}

function Print-StatsBlock {
	param(
		[string]$Title,
		[pscustomobject[]]$Stats
	)

	Write-Host ""
	Write-Host $Title
	foreach ($s in $Stats) {
		if ($s.n -eq 0) {
			Write-Host ("  {0,-18} n=0" -f $s.label)
		} else {
			Write-Host ("  {0,-18} n={1,-4} avg={2,7}ms  min={3,5}  max={4,5}" -f $s.label, $s.n, $s.avg, $s.min, $s.max)
		}
	}
}

foreach ($p in $Path) {
	if (-not (Test-Path -LiteralPath $p)) {
		throw "File not found: $p"
	}
}

foreach ($p in $Path) {
	$lines = Get-Content -LiteralPath $p
	$rows = foreach ($ln in $lines) {
		Parse-ProfLine -Line $ln
	}
	$rows = $rows | Where-Object { $_ -ne $null }

	if ($Outcome -ne 'all') {
		$rows = $rows | Where-Object { $_.outcome -eq $Outcome }
	}

	$silent = @($rows | ForEach-Object { $_.silent })
	$whisper = @($rows | ForEach-Object { $_.whisper })
	$total = @($rows | ForEach-Object { $_.total })

	$perceived = @($rows | ForEach-Object {
		if ($_.silent -ge 0 -and $_.whisper -ge 0) { [int64]($_.silent + $_.whisper) } else { [int64]-1 }
	})

	Write-Host "================================================================================"
	Write-Host ("PROFILE SUMMARY: {0}" -f $p)
	Write-Host ("Filter: outcome={0}" -f $Outcome)

	Print-StatsBlock -Title 'Key timings' -Stats @(
		(Summary-Stats -Label 'silent' -Values $silent),
		(Summary-Stats -Label 'whisper' -Values $whisper),
		(Summary-Stats -Label 'silent+whisper' -Values $perceived),
		(Summary-Stats -Label 'total' -Values $total)
	)
}

