#!/usr/bin/env bash
set -e

if [ $# -ne 1 ]; then
  APP_NAME=webhud
else
  APP_NAME=$1
fi

./gradlew :app_${APP_NAME}:assembleDebug

APP_PATH=$(find app_${APP_NAME}/build/outputs/apk/debug/ -type f -name "*.apk")
echo ${APP_PATH}

mkdir -p output
mkdir -p output/app_${APP_NAME}
ls app_${APP_NAME}/build/outputs/apk/debug
cp ${APP_PATH} output/app_${APP_NAME}/${APP_NAME}.apk

if adb get-state 1>/dev/null 2>&1; then
  echo "Device found"
  adb install ${APP_PATH}
else
  echo "Device not found"
fi
