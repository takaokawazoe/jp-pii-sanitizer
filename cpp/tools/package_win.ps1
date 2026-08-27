# ポータブル配布ZIPを生成する。
#   展開して jp-pii-sanitizer.exe を叩けば動く自己完結フォルダ（管理者権限不要）。
#   対象: Windows 11 以降（WebView2 ランタイム標準搭載に依存）。未署名。
#
# 前提: cmake --build build --target jp_pii_sanitizer jp_pii_sanitizer_cli が済んでいること
#       （build/ に GUI/CLI の exe・各DLL・index.html が揃う）と、models/ にモデル一式があること。
# 使い方: powershell -ExecutionPolicy Bypass -File cpp/tools/package_win.ps1
#         powershell -ExecutionPolicy Bypass -File cpp/tools/package_win.ps1 -Version v0.4.1
#
# ZIP 名にバージョンを入れる。入れないと、利用者の手元に複数の版が落ちたときに
# どれがどれか分からなくなる（ダウンロードフォルダで名前が衝突して "(1)" が付くだけ）。
# 既定は直近のタグ。タグが無い環境では "dev" になる。
param([string]$Version)

$ErrorActionPreference = 'Stop'
$cpp   = Split-Path $PSScriptRoot -Parent           # cpp/
$build = Join-Path $cpp 'build'
$models= Join-Path $cpp 'models'
$dist  = Join-Path $cpp 'dist'
$stage = Join-Path $dist 'jp-pii-sanitizer'
if (-not $Version) {
  $Version = (& git -C $cpp describe --tags --abbrev=0 2>$null)
  if (-not $Version) { $Version = 'dev' }
}
$zip   = Join-Path $dist "jp-pii-sanitizer-$Version-win-x64.zip"

# --- 検査 ---
$exe = Join-Path $build 'jp-pii-sanitizer.exe'
if (-not (Test-Path $exe)) { throw "先に jp_pii_sanitizer をビルドしてください: $exe が無い" }
$needModel = Join-Path $models 'model_fp16.onnx'
if (-not (Test-Path $needModel)) { throw "モデルがありません: $needModel（WSLからコピー or 取得スクリプト）" }

# --- ステージング作り直し ---
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'models') | Out-Null

# --- exe・DLL・UI（build/ から） ---
$fromBuild = @(
  'jp-pii-sanitizer.exe',
  'jp-pii-sanitizer-cli.exe',
  'index.html',
  'WebView2Loader.dll',
  'onnxruntime.dll',
  'onnxruntime_providers_shared.dll',
  'pdfium.dll'
)
foreach ($f in $fromBuild) {
  $src = Join-Path $build $f
  if (-not (Test-Path $src)) { throw "build/ に $f が無い（POST_BUILD コピー未実行？）" }
  Copy-Item $src (Join-Path $stage $f)
}

# --- モデル・辞書・設定（models/ から。fp32 の巨大モデルは同梱しない） ---
$modelFiles = @('model_fp16.onnx','tokenizer.json','labels.json','patterns.json')
foreach ($f in $modelFiles) {
  $src = Join-Path $models $f
  if (-not (Test-Path $src)) { throw "models/ に $f が無い" }
  Copy-Item $src (Join-Path $stage "models\$f")
}
Copy-Item (Join-Path $models 'sudachi') (Join-Path $stage 'models\sudachi') -Recurse

# --- 同梱 README（利用者向け） ---
$readme = @"
PIIサニタイザ（ポータブル版）

■ 使い方（GUI）
  1. このフォルダを書き込みできる場所（デスクトップ、ダウンロード等）に展開する
  2. jp-pii-sanitizer.exe をダブルクリック

■ コマンドライン版（バッチ・自動化用）
  同じフォルダの jp-pii-sanitizer-cli.exe をコマンドプロンプト/PowerShell から実行します。
    jp-pii-sanitizer-cli.exe detect <files...> -o candidates.jsonl
    jp-pii-sanitizer-cli.exe mask   <files...> --candidates candidates.jsonl -o masked.txt
    jp-pii-sanitizer-cli.exe restore --mapping mapping.jsonl -i ai_reply.txt -o final.txt
  詳細は jp-pii-sanitizer-cli.exe --help、および README のCLI節を参照。

■ 対応表のパスフレーズ保護（任意・既定はオフ）
  対応表は実名とトークンの対応そのものなので、持ち出しや紛失に備えて
  パスフレーズで保護できます。GUI は結果画面の「パスフレーズで保護する」、
  CLI は mask に --encrypt-mapping を付けます。
    jp-pii-sanitizer-cli.exe mask <files...> --reversible --encrypt-mapping \
        --mapping mapping.jsonl -o masked.txt
  ※ パスフレーズを紛失すると、サニタイズした内容を元に戻せなくなります
    （元の文書は影響を受けません）。保存先を控えてください。

■ 動作環境
  Windows 11 以降（WebView2 ランタイムが標準搭載のため追加インストール不要）
  ※ CLI は WebView2 不要。GUI のみ WebView2 を使います。

■ SmartScreen 警告について
  未署名のため、初回起動時に「Windows によって PC が保護されました」と出ることがあります。
  管理者権限は不要です。［詳細情報］→［実行］で起動できます。

■ 注意
  本ツールは検出したPIIをマスクしますが、すべてのPIIの除去を保証するものではありません。
  既定は全マスク（opt-out）ですが、社内AI等へ送信する前に必ず内容をご確認ください。
"@
[System.IO.File]::WriteAllText((Join-Path $stage 'README.txt'), $readme, (New-Object System.Text.UTF8Encoding($false)))

# --- 圧縮 ---
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal

$sizeMB = [Math]::Round((Get-Item $zip).Length / 1MB, 1)
Write-Host "作成: $zip  ($sizeMB MB)"
Write-Host "同梱物:"
Get-ChildItem $stage -Recurse -File | ForEach-Object {
  "  {0,-38} {1,8:N1} MB" -f $_.FullName.Substring($stage.Length+1), ($_.Length/1MB)
}
