#!/usr/bin/env bash
set -e

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
