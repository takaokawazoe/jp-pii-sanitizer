# Excel アドインの per-user インストーラ（setup.exe）を作る。
#
#   1. ヘルパー exe（cmake, Ninja, MSVC）と XLL（dotnet build）をビルド
#   2. 同梱物を excel/installer/stage/ に集める（XLL・ヘルパー・onnxruntime・models・README）
#   3. Inno Setup（ISCC）で excel/dist/jp-pii-sanitizer-excel-<version>-setup.exe を作る
#
# 使い方: powershell -ExecutionPolicy Bypass -File excel/installer/build_installer.ps1
#         ... -Version 0.6.1      バージョンを明示（既定は直近の git タグから v を外したもの）
#         ... -SkipBuild          ビルド済みの exe/XLL をそのまま使う
#         ... -Portable           setup.exe に加えて、インストーラ不要の zip
#                                 （同じ中身＋手順書 README-portable.txt）も作る
#
# 前提: Visual Studio（C++）、.NET SDK、Inno Setup 6（winget install JRSoftware.InnoSetup）、
#       cpp/models にモデル一式（package_win.ps1 と同じ）。
# 注意: Excel が XLL を掴んでいると dotnet build が失敗する。テスト用 Excel は閉じてから。
param(
  [string]$Version,
  [switch]$SkipBuild,
  [switch]$Portable
)

$ErrorActionPreference = 'Stop'
$root  = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent    # リポジトリのルート
$cpp   = Join-Path $root 'cpp'
$excel = Join-Path $root 'excel'
$proj  = Join-Path $excel 'JpPiiSanitizer.Excel'
$stage = Join-Path $PSScriptRoot 'stage'
$dist  = Join-Path $excel 'dist'
$iss   = Join-Path $PSScriptRoot 'jp-pii-sanitizer-excel.iss'

# --- バージョン（Inno の VersionInfoVersion は数字 4 組まで。タグ v0.6.1 → 0.6.1） ---
if (-not $Version) {
  $tag = (& git -C $root describe --tags --abbrev=0 2>$null)
  if ($tag) { $Version = $tag -replace '^v', '' } else { $Version = '0.0.0' }
}
if ($Version -notmatch '^\d+(\.\d+){0,3}$') { throw "バージョンは 0.6.1 のような数字だけの形にしてください: $Version" }

# --- Inno Setup ---
$iscc = @(
  (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'),
  'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
  'C:\Program Files\Inno Setup 6\ISCC.exe'
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) {
  $cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue
  if ($cmd) { $iscc = $cmd.Source }
}
if (-not $iscc) { throw 'Inno Setup 6 が見つかりません: winget install -e --id JRSoftware.InnoSetup' }

# --- ビルド ---
if (-not $SkipBuild) {
  Write-Host '== helper exe (cmake)'
  $bat = Join-Path $env:TEMP 'jp-pii-build-helper.bat'
  @(
    '@echo off',
    'rem vcvars のスクリプトが vswhere.exe を PATH から探して文句を言うので先に通しておく（無害だがログが汚れる）',
    'set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"',
    "call `"$cpp\winenv.bat`" || exit /b 1",
    "cd /d `"$cpp`"",
    'cmake --build build --target jp_pii_helper || exit /b 1'
  ) | Set-Content -Path $bat -Encoding Ascii
  & cmd.exe /c $bat
  if ($LASTEXITCODE -ne 0) { throw 'ヘルパーのビルドに失敗しました' }

  Write-Host '== XLL (dotnet build)'
  & dotnet build $proj -c Release -nologo -v q
  if ($LASTEXITCODE -ne 0) { throw 'XLL のビルドに失敗しました' }
}

# --- 検査 ---
$xll    = Join-Path $proj 'bin\Release\net48\publish\JpPiiSanitizer.Excel-AddIn64-packed.xll'
$helper = Join-Path $cpp 'build\jp-pii-helper.exe'
$models = Join-Path $cpp 'models'
foreach ($f in @($xll, $helper, (Join-Path $cpp 'build\onnxruntime.dll'), (Join-Path $cpp 'build\onnxruntime_providers_shared.dll'))) {
  if (-not (Test-Path $f)) { throw "見つかりません: $f" }
}
$modelFiles = @('model_fp16.onnx', 'tokenizer.json', 'labels.json', 'patterns.json')
foreach ($f in $modelFiles) {
  if (-not (Test-Path (Join-Path $models $f))) { throw "models/ に $f が無い" }
}
if (-not (Test-Path (Join-Path $models 'sudachi\system.dic'))) { throw 'models/sudachi が無い' }

# --- ステージング（作り直し） ---
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'models') | Out-Null
Copy-Item $xll    (Join-Path $stage 'jp-pii-sanitizer.xll')     # 配布名は短く。パック済み XLL は改名しても動く
Copy-Item $helper (Join-Path $stage 'jp-pii-helper.exe')
Copy-Item (Join-Path $cpp 'build\onnxruntime.dll') $stage
Copy-Item (Join-Path $cpp 'build\onnxruntime_providers_shared.dll') $stage
foreach ($f in $modelFiles) { Copy-Item (Join-Path $models $f) (Join-Path $stage "models\$f") }
Copy-Item (Join-Path $models 'sudachi') (Join-Path $stage 'models\sudachi') -Recurse
Copy-Item (Join-Path $PSScriptRoot 'README.txt') (Join-Path $stage 'README.txt')

# --- Inno Setup ---
New-Item -ItemType Directory -Force -Path $dist | Out-Null
Write-Host "== ISCC ($Version)"
& $iscc "/DAppVersion=$Version" "/DStageDir=$stage" "/DOutputDir=$dist" /Qp $iss
if ($LASTEXITCODE -ne 0) { throw 'ISCC に失敗しました' }

$setup = Join-Path $dist "jp-pii-sanitizer-excel-$Version-setup.exe"
$sizeMB = [Math]::Round((Get-Item $setup).Length / 1MB, 1)
Write-Host "作成: $setup  ($sizeMB MB)"

# --- ポータブル zip（インストーラ不要。同じ中身＋手順書） ---
# zip の中は 1 つのフォルダ（jp-pii-sanitizer\）にまとめる。展開先にばらまかないため。
if ($Portable) {
  $zip = Join-Path $dist "jp-pii-sanitizer-excel-$Version-portable.zip"
  $pstage = Join-Path $PSScriptRoot 'stage-portable\jp-pii-sanitizer'
  if (Test-Path (Split-Path $pstage -Parent)) { Remove-Item (Split-Path $pstage -Parent) -Recurse -Force }
  New-Item -ItemType Directory -Force -Path $pstage | Out-Null
  Get-ChildItem $stage | Where-Object { $_.Name -ne 'README.txt' } | Copy-Item -Destination $pstage -Recurse
  Copy-Item (Join-Path $PSScriptRoot 'README-portable.txt') (Join-Path $pstage 'README.txt')
  if (Test-Path $zip) { Remove-Item $zip -Force }
  Write-Host '== portable zip'
  Compress-Archive -Path $pstage -DestinationPath $zip -CompressionLevel Optimal
  $zipMB = [Math]::Round((Get-Item $zip).Length / 1MB, 1)
  Write-Host "作成: $zip  ($zipMB MB)"
}
