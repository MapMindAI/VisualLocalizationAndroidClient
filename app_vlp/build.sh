#!/usr/bin/env bash
set -e

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MNN_MODEL_PATH="$ROOT_DIR/app_vlp/app/src/main/assets/models/depth_anything_v2_metric_vits.mnn"
CONVERT_SCRIPT="$ROOT_DIR/app_vlp/scripts/convert_da2_to_mnn.sh"

if [[ ! -f "$MNN_MODEL_PATH" ]]; then
  echo "MNN model not found: $MNN_MODEL_PATH"
  echo "Running model conversion..."
  bash "$CONVERT_SCRIPT"
fi

./gradlew assembleDebug

APP_PATH=$(find app/build/outputs/apk/debug/ -type f -name "*.apk")
echo ${APP_PATH}

mkdir -p ../output
mkdir -p ../output/app_vlp
ls app/build/outputs/apk/debug
cp ${APP_PATH} ../output/app_vlp/vlp.apk

if adb get-state 1>/dev/null 2>&1; then
  echo "Device found"
  adb install ${APP_PATH}
else
  echo "Device not found"
fi
