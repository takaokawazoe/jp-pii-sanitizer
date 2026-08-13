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
zip="$(mktemp -u).zip"

echo "Downloading $url"
curl -fSL -o "$zip" "$url"
unzip -o "$zip" -d "$models"
rm -f "$zip"
echo "Extracted into $models"
