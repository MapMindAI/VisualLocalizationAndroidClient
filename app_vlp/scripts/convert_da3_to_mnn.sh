#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ONNX_MODEL="${1:-$ROOT_DIR/python/models/da3_small_2_392x224_sim.onnx}"
OUT_MNN="${2:-$ROOT_DIR/app_vlp/app/src/main/assets/models/da3_small_2_392x224_sim.mnn}"
MNN_CONVERTER="${MNN_CONVERTER:-MNNConvert}"
if ! command -v "$MNN_CONVERTER" >/dev/null 2>&1; then
  if command -v mnnconvert >/dev/null 2>&1; then
    MNN_CONVERTER="mnnconvert"
  fi
fi

if [[ ! -f "$ONNX_MODEL" ]]; then
  echo "ONNX model not found: $ONNX_MODEL" >&2
  exit 1
fi

if ! command -v "$MNN_CONVERTER" >/dev/null 2>&1; then
  echo "Cannot find converter: $MNN_CONVERTER" >&2
  echo "Install MNNConvert and/or export MNN_CONVERTER=/path/to/MNNConvert" >&2
  exit 2
fi

mkdir -p "$(dirname "$OUT_MNN")"

set -x
"$MNN_CONVERTER" \
  -f ONNX \
  --modelFile "$ONNX_MODEL" \
  --MNNModel "$OUT_MNN" \
  --bizCode MNN
set +x

echo "Converted model written to: $OUT_MNN"
