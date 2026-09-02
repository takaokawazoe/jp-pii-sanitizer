#!/usr/bin/env bash
# Download the model + dictionary bundle from the GitHub Release into cpp/models/.
# These assets (~750 MB) are not stored in the repository; they are attached to
# a Release. Set JPPII_REPO/JPPII_TAG to match the release you publish.
#
#   JPPII_REPO=youruser/jp-pii-sanitizer ./cpp/tools/fetch_assets.sh
#
# The release must contain one asset, models-assets.zip, whose contents extract
# into cpp/models/:
#   model_fp16.onnx  tokenizer.json  labels.json  patterns.json  sudachi/<...>
set -euo pipefail

REPO="${JPPII_REPO:-takaokawazoe/jp-pii-sanitizer}"
TAG="${JPPII_TAG:-v0.1.0}"

if [[ "$REPO" == OWNER/* ]]; then
  echo "Set the repository first, e.g.:  JPPII_REPO=youruser/jp-pii-sanitizer $0"
  exit 1
fi

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
models="$(cd "$here/.." && pwd)/models"   # cpp/models
mkdir -p "$models"
url="https://github.com/$REPO/releases/download/$TAG/models-assets.zip"

# 一時ファイルは **作ってから** 使う。`mktemp -u` は名前を予約しないので、生成から
# 書き込みまでの間に他のプロセスに割り込まれる余地がある。
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
zip="$tmpdir/models-assets.zip"

echo "Downloading $url"
curl -fSL -o "$zip" "$url"

# **ハッシュを照合する。** Release のアセットは後から差し替えられるので、ダウンロードした
# ものが公開時のものと同じであることを確かめないと、モデル一式が供給網の穴になる。
want="$(awk -v t="$TAG" '$1==t {print $2}' "$here/models-assets.sha256")"
if [[ -z "$want" ]]; then
  echo "ERROR: $TAG の期待ハッシュが cpp/tools/models-assets.sha256 に無い" >&2
  exit 1
fi
got="$(sha256sum "$zip" | cut -d' ' -f1)"
if [[ "$got" != "$want" ]]; then
  echo "ERROR: models-assets.zip のハッシュが違う" >&2
  echo "  expected: $want" >&2
  echo "  actual  : $got" >&2
  exit 1
fi
echo "SHA-256 OK: $got"

unzip -o "$zip" -d "$models"
echo "Extracted into $models"
