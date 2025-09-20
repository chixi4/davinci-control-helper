<#
 .SYNOPSIS
  使用 Windows 内置 IExpress 将 x64/Release 产物打成单文件 exe，运行时自动解压到临时目录并启动主程序。

 .REQUIREMENTS
  - Windows 自带 iexpress.exe（通常位于 %WINDIR%\System32\iexpress.exe）。
  - 已完成 Release|x64 构建，输出位于 repo 根目录的 x64/Release。

 .USAGE
  powershell -ExecutionPolicy Bypass -File scripts/pack-iexpress.ps1 [-ReleaseDir <path>] [-OutDir <path>] [-Name <baseName>]

 .NOTES
  - 不依赖 7-Zip；但仍属于“单文件分发”：首次运行需解压到临时目录（系统自动），再启动 DualSensManager.exe。
  - 仍需目标机存在 .NET Framework 4.7.2+（Win10/11 通常已内置 4.8）。
#>

[CmdletBinding()]
param(
  [string]$ReleaseDir = (Join-Path (Get-Location) 'x64/Release'),
  [string]$OutDir = (Join-Path (Get-Location) 'dist'),
  [string]$Name = 'DualSensManager-Portable-IEXPRESS',
  [switch]$UseCmdStart,
  [switch]$ShowExtractUI
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-File([string]$path) {
  if (-not (Test-Path $path)) { throw "缺少必要文件：$path" }
}

if (-not (Test-Path $ReleaseDir)) {
  throw "未找到 Release 输出目录：$ReleaseDir，请先构建项目（Release|x64）。"
}

$iexpress = Get-Command iexpress.exe -ErrorAction SilentlyContinue
if (-not $iexpress) {
  $candidate = Join-Path $env:WINDIR 'System32/iexpress.exe'
  if (Test-Path $candidate) { $iexpress = @{ Source = $candidate } }
}
if (-not $iexpress) { throw '未找到 iexpress.exe（Windows 组件）。请确认系统为标准 Windows 环境。' }

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$stageDir = Join-Path $OutDir 'portable-staging-iexpress'
if (Test-Path $stageDir) { Remove-Item $stageDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stageDir | Out-Null

Write-Host "[1/4] 准备文件..." -ForegroundColor Cyan
$include = @(
  'DualSensManager.exe',
  'DualSensManager.exe.config',
  'wrapper.dll',
  'Newtonsoft.Json.dll'
)
foreach ($f in $include) {
  $src = Join-Path $ReleaseDir $f
  Assert-File $src
  Copy-Item $src -Destination $stageDir -Force
}

Write-Host "[2/4] 生成 IExpress 配置..." -ForegroundColor Cyan
$outExe = Join-Path $OutDir ($Name + '.exe')
$sedPath = Join-Path $OutDir ($Name + '.sed')

# 注意：IExpress 对空格路径支持较好，这里不加引号；路径需为绝对路径
$hideAnim = if ($ShowExtractUI) { '0' } else { '1' }
$appLaunch = if ($UseCmdStart) { 'cmd /c start "" "DualSensManager.exe"' } else { 'DualSensManager.exe' }
$showInstall = if ($UseCmdStart) { '1' } else { '0' }
$sed = @"
[Version]
Class=IEXPRESS
SEDVersion=3

[Options]
PackagePurpose=InstallApp
ShowInstallProgramWindow=$showInstall
HideExtractAnimation=$hideAnim
UseLongFileName=1
InsideCompressed=0
CAB_FixedSize=0
CAB_ResvCodeSigning=0
RebootMode=I
InstallPrompt=
DisplayLicense=
FinishMessage=
TargetName=$outExe
FriendlyName=DualSensManager Portable
AppLaunched=$appLaunch
PostInstallCmd=<None>
AdminQuietInstCmd=
UserQuietInstCmd=
SourceFiles=SourceFiles

[SourceFiles]
SourceFiles0=$stageDir

[SourceFiles0]
DualSensManager.exe=
DualSensManager.exe.config=
Newtonsoft.Json.dll=
wrapper.dll=
"@

Set-Content -Path $sedPath -Value $sed -Encoding Default

Write-Host "[3/4] 调用 IExpress 打包..." -ForegroundColor Cyan
if (Test-Path $outExe) { Remove-Item $outExe -Force }

& $iexpress.Source /N $sedPath | Out-Null

if (-not (Test-Path $outExe)) {
  throw 'IExpress 未生成目标 exe，请检查脚本输出或系统策略。'
}

Write-Host "[4/4] 完成：$outExe" -ForegroundColor Green
Write-Host '提示：运行该 exe 会在临时目录解包并启动 DualSensManager。' -ForegroundColor DarkGray
