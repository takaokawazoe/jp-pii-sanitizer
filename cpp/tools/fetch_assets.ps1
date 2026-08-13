# Download the model + dictionary bundle from the GitHub Release into cpp/models/.
# These assets (~750 MB) are not stored in the repository; they are attached to
# a Release. Set $Repo/$Tag (or the env vars) to match the release you publish.
#
#   powershell -ExecutionPolicy Bypass -File cpp/tools/fetch_assets.ps1
#
# The release must contain one asset, models-assets.zip, whose contents are:
#   model_fp16.onnx  tokenizer.json  labels.json  patterns.json  sudachi/<...>
# extracted into cpp/models/.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$Repo = if ($env:JPPII_REPO) { $env:JPPII_REPO } else { 'takaokawazoe/jp-pii-sanitizer' }
$Tag  = if ($env:JPPII_TAG)  { $env:JPPII_TAG }  else { 'v0.1.0' }

if ($Repo -like 'OWNER/*') {
  Write-Host "Set the repository first, e.g.:  `$env:JPPII_REPO='youruser/jp-pii-sanitizer'"
  Write-Host "(and optionally `$env:JPPII_TAG='v0.1.0'), then re-run."
  exit 1
}

$models = Join-Path (Split-Path $PSScriptRoot -Parent) 'models'   # cpp/models
New-Item -ItemType Directory -Force -Path $models | Out-Null
$url = "https://github.com/$Repo/releases/download/$Tag/models-assets.zip"
$zip = Join-Path $env:TEMP 'jppii-models-assets.zip'

Write-Host "Downloading $url"
Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
Write-Host ("  -> {0:N0} MB" -f ((Get-Item $zip).Length / 1MB))
Expand-Archive -Path $zip -DestinationPath $models -Force
Remove-Item $zip

Write-Host "Extracted into $models :"
Get-ChildItem $models -Recurse -File |
  ForEach-Object { "  {0,-32} {1,8:N1} MB" -f $_.FullName.Substring($models.Length + 1), ($_.Length / 1MB) }
