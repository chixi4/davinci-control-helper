Param(
  [string]$Configuration = 'Release'
)
$OutDir = Join-Path $PSScriptRoot "..\x64\$Configuration"
$Zip = Join-Path $PSScriptRoot "..\DualSensManager-$Configuration.zip"
if(Test-Path $Zip){ Remove-Item $Zip -Force }
$files = @(
  Join-Path $OutDir 'DualSensManager.exe',
  Join-Path $OutDir 'DualSensManager.exe.config',
  Join-Path $OutDir 'wrapper.dll',
  Join-Path $OutDir 'wrapper.pdb',
  Join-Path $PSScriptRoot '..\wrapper\x64\'+$Configuration+'\Newtonsoft.Json.dll'
) | Where-Object { Test-Path $_ }
Compress-Archive -Path $files -DestinationPath $Zip -Force
Write-Host "Packed -> $Zip"
