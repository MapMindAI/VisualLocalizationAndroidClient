#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ONNX_MODEL="${1:-$ROOT_DIR/libs/da2/depth_anything_v2_metric_vits.onnx}"
OUT_MNN="${2:-$ROOT_DIR/app_vlp/app/src/main/assets/models/depth_anything_v2_metric_vits.mnn}"
INPUT_CONFIG_FILE="${INPUT_CONFIG_FILE:-$ROOT_DIR/app_vlp/scripts/input_config.txt}"
MNN_CONVERTER="${MNN_CONVERTER:-MNNConvert}"
ONNXSIM_BIN="${ONNXSIM_BIN:-onnxsim}"

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

if [[ ! -f "$INPUT_CONFIG_FILE" ]]; then
  echo "Input config file not found: $INPUT_CONFIG_FILE" >&2
  exit 3
fi

mkdir -p "$(dirname "$OUT_MNN")"

SIMPLIFIED_ONNX="$ONNX_MODEL"
TMP_SIMPLIFIED_ONNX=""
if command -v "$ONNXSIM_BIN" >/dev/null 2>&1 || command -v python >/dev/null 2>&1 || command -v python3 >/dev/null 2>&1; then
  TMP_SIMPLIFIED_ONNX="$(mktemp "${TMPDIR:-/tmp}/da2_simplified_XXXXXX.onnx")"
  if command -v "$ONNXSIM_BIN" >/dev/null 2>&1; then
    set -x
    "$ONNXSIM_BIN" "$ONNX_MODEL" "$TMP_SIMPLIFIED_ONNX"
    set +x
  elif command -v python >/dev/null 2>&1; then
    set -x
    python -m onnxsim "$ONNX_MODEL" "$TMP_SIMPLIFIED_ONNX"
    set +x
  else
    set -x
    python3 -m onnxsim "$ONNX_MODEL" "$TMP_SIMPLIFIED_ONNX"
    set +x
  fi
  SIMPLIFIED_ONNX="$TMP_SIMPLIFIED_ONNX"
fi

cleanup() {
  if [[ -n "$TMP_SIMPLIFIED_ONNX" && -f "$TMP_SIMPLIFIED_ONNX" ]]; then
    rm -f "$TMP_SIMPLIFIED_ONNX"
  fi
}
trap cleanup EXIT


set -x
"$MNN_CONVERTER" \
  -f ONNX \
  --modelFile "$TMP_SIMPLIFIED_ONNX" \
  --MNNModel "$OUT_MNN" \
  --inputConfigFile "$INPUT_CONFIG_FILE" \
  --bizCode MNN \
  --optimizeLevel 2 \
  --optimizePrefer 2 \
  --saveStaticModel \
  --fp16
set +x
