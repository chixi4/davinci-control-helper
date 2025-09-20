[CmdletBinding()]
param(
  [Parameter(Mandatory=$true)][string]$Target,
  [string]$ProductName = 'Mouse Control Helper',
  [string]$FileDescription = 'Mouse Control Helper',
  [string]$CompanyName = '',
  [string]$Version = '1.0.0',
  [string]$IconPath
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$tools = Join-Path $root 'tools'
$rcedit = Join-Path $tools 'rcedit-x64.exe'

function Ensure-Rcedit {
  if (Test-Path $rcedit) { return }
  Write-Host '下载 rcedit-x64.exe 用于写入版本信息...' -ForegroundColor Cyan
  $urls = @(
    'https://github.com/electron/rcedit/releases/download/v2.0.0/rcedit-x64.exe',
    'https://github.com/electron/rcedit/releases/download/v1.1.1/rcedit-x64.exe'
  )
  foreach ($u in $urls) {
    try {
      Invoke-WebRequest -Uri $u -OutFile $rcedit -UseBasicParsing -TimeoutSec 20
      break
    } catch { continue }
  }
  if (-not (Test-Path $rcedit)) { throw '无法下载 rcedit-x64.exe，请检查网络或稍后重试。' }
}

Ensure-Rcedit

if (-not (Test-Path $Target)) { throw "目标不存在：$Target" }

& $rcedit "$Target" --set-version-string ProductName "$ProductName" --set-version-string FileDescription "$FileDescription"
if ($CompanyName) { & $rcedit "$Target" --set-version-string CompanyName "$CompanyName" }
& $rcedit "$Target" --set-file-version "$Version" --set-product-version "$Version"
if ($IconPath -and (Test-Path $IconPath)) { & $rcedit "$Target" --set-icon "$IconPath" }

Write-Host "已写入品牌信息：$Target" -ForegroundColor Green
