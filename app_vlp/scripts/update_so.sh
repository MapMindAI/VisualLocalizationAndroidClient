#!/usr/bin/env bash
set -e

# bazel build --config=android64 //mapping/export:mapmind_vlp_api_pkg
echo "please call bazel build --config=android64 //mapping/export:mapmind_vlp_api_pkg before"

rm -rf data/mapmind_vlp_api_pkg.tar.gz
cp ../bazel-bin/mapping/export/mapmind_vlp_api_pkg.tar.gz data

tar --no-same-owner --no-same-permissions -xvzf data/mapmind_vlp_api_pkg.tar.gz -C libs
