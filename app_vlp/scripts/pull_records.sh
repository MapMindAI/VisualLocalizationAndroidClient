#!/usr/bin/env bash
set -e

RECORD_PATH=/storage/emulated/0/Android/data/com.google.ar.core.examples.c.helloar/files
adb shell ls ${RECORD_PATH}

adb pull ${RECORD_PATH} /MobiliAndroidHUD/data
adb shell rm -rf ${RECORD_PATH}/2025*
