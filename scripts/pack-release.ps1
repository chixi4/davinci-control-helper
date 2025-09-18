Param(
  [string]$Configuration = 'Release'
)

$Root = Split-Path -Parent $PSScriptRoot
$OutDir = Join-Path $Root "x64\$Configuration"
$Zip = Join-Path $Root "DualSensManager-$Configuration.zip"

if (Test-Path $Zip) { Remove-Item $Zip -Force }

$files = @()
$files += (Join-Path $OutDir 'DualSensManager.exe')
$files += (Join-Path $OutDir 'DualSensManager.exe.config')
$files += (Join-Path $OutDir 'wrapper.dll')
$files += (Join-Path $OutDir 'wrapper.pdb')

# Newtonsoft.Json.dll candidates
$jsonCandidates = @(
  (Join-Path $Root "wrapper\x64\$Configuration\Newtonsoft.Json.dll"),
  (Join-Path $OutDir 'Newtonsoft.Json.dll')
)
foreach ($c in $jsonCandidates) { if (Test-Path $c) { $files += $c; break } }

$existing = $files | Where-Object { $_ -and (Test-Path $_) }
if (-not $existing -or $existing.Count -eq 0) { Write-Error "No build outputs found under $OutDir"; exit 1 }

Compress-Archive -Path $existing -DestinationPath $Zip -Force
Write-Host "Packed -> $Zip"

