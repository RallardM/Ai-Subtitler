param(
    [Parameter(Mandatory = $true)]
    [string]$ZipPath,

    [Parameter(Mandatory = $true)]
    [string]$Entry
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $ZipPath)) {
    throw "Zip not found: $ZipPath"
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$z = [IO.Compression.ZipFile]::OpenRead($ZipPath)
try {
    $hit = $z.Entries | Where-Object { $_.FullName -ieq $Entry }
    if ($hit) {
        Write-Output $hit.FullName
        exit 0
    }
    Write-Output 'MISSING'
    exit 2
} finally {
    $z.Dispose()
}
