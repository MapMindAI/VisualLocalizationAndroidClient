#!/usr/bin/env bash
set -e

# bazel build --config=android64 //export:mobili_vlp_api_pkg
echo "please call bazel build --config=android64 //export:mobili_vlp_api_pkg before"

rm -rf data/mobili_vlp_api_pkg.tar.gz
cp ../Mobili/bazel-bin/export/mobili_vlp_api_pkg.tar.gz data

sudo tar --no-same-owner --no-same-permissions -xvzf data/mobili_vlp_api_pkg.tar.gz -C libs
rm libs/mobili_vlp_api/lib/arm64-v8a/libopencv_java4.so
