# Excel アドイン一式（配布 zip の excel\ フォルダの中身）を作る。
#
#   1. ヘルパー exe（cmake, Ninja, MSVC）と XLL（dotnet build）をビルド
#   2. -OutDir（既定 excel/installer/stage/）に集める:
#        jp-pii-sanitizer.xll  jp-pii-helper.exe  onnxruntime.dll  onnxruntime_providers_shared.dll
#        README.txt（Excel 用の手順書）  jp-pii-sanitizer-excel-setup.exe
#   3. setup.exe は Inno Setup（ISCC）で作る。中身は埋め込まず、実行時に隣のファイルと
#      1 つ上の models\ をコピーする（external）。だから数 MB で、単体では動かない。
#
# models は入れない。配布 zip では GUI/CLI と共有する models\ が 1 つ上にある
# （cpp/tools/package_win.ps1 がこのスクリプトを呼んで excel\ に入れる）。
#
# 使い方: powershell -ExecutionPolicy Bypass -File excel/installer/build_installer.ps1
#         ... -Version 0.7.0      バージョンを明示（既定は直近の git タグから v を外したもの）
#         ... -SkipBuild          ビルド済みの exe/XLL をそのまま使う
#         ... -OutDir <dir>       出力先（package_win.ps1 は <zip の stage>\excel を渡す）
#
# 前提: Visual Studio（C++）、.NET SDK、Inno Setup 6（winget install JRSoftware.InnoSetup）。
# 注意: Excel が XLL を掴んでいると dotnet build が失敗する。テスト用 Excel は閉じてから。
param(
  [string]$Version,
  [switch]$SkipBuild,
  [string]$OutDir
)

$ErrorActionPreference = 'Stop'
$root  = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent    # リポジトリのルート
$cpp   = Join-Path $root 'cpp'
$excel = Join-Path $root 'excel'
$proj  = Join-Path $excel 'JpPiiSanitizer.Excel'
$iss   = Join-Path $PSScriptRoot 'jp-pii-sanitizer-excel.iss'
if (-not $OutDir) { $OutDir = Join-Path $PSScriptRoot 'stage' }

# --- バージョン（Inno の VersionInfoVersion は数字 4 組まで。タグ v0.7.0 → 0.7.0） ---
if (-not $Version) {
  $tag = (& git -C $root describe --tags --abbrev=0 2>$null)
  if ($tag) { $Version = $tag -replace '^v', '' } else { $Version = '0.0.0' }
}
if ($Version -notmatch '^\d+(\.\d+){0,3}$') { throw "バージョンは 0.7.0 のような数字だけの形にしてください: $Version" }

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
foreach ($f in @($xll, $helper, (Join-Path $cpp 'build\onnxruntime.dll'), (Join-Path $cpp 'build\onnxruntime_providers_shared.dll'))) {
  if (-not (Test-Path $f)) { throw "見つかりません: $f" }
}

# --- 集める（作り直し） ---
if (Test-Path $OutDir) { Remove-Item $OutDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Copy-Item $xll    (Join-Path $OutDir 'jp-pii-sanitizer.xll')     # 配布名は短く。パック済み XLL は改名しても動く
Copy-Item $helper (Join-Path $OutDir 'jp-pii-helper.exe')
Copy-Item (Join-Path $cpp 'build\onnxruntime.dll') $OutDir
Copy-Item (Join-Path $cpp 'build\onnxruntime_providers_shared.dll') $OutDir
Copy-Item (Join-Path $PSScriptRoot 'README.txt') (Join-Path $OutDir 'README.txt')

# --- Inno Setup（出力は同じフォルダ。external なので一瞬で終わる） ---
Write-Host "== ISCC ($Version)"
& $iscc "/DAppVersion=$Version" "/DOutputDir=$OutDir" /Qp $iss
if ($LASTEXITCODE -ne 0) { throw 'ISCC に失敗しました' }

Write-Host "作成: $OutDir"
Get-ChildItem $OutDir -File | ForEach-Object {
  "  {0,-38} {1,8:N1} MB" -f $_.Name, ($_.Length/1MB)
}
