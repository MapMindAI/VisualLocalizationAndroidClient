#!/usr/bin/env bash
set -e

MESH_PATH=/MobiliAndroidHUD/data/tsdf_mesh.obj

DEST_PATH=/storage/emulated/0/Android/data/com.google.ar.core.examples.c.helloar/files/tsdf_mesh.obj

adb push ${MESH_PATH} ${DEST_PATH}
