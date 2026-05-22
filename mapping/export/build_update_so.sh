#!/usr/bin/env bash
set -euo pipefail

bazel build --config=android64 //mapping/export:mapmind_vlp_api_pkg
genfile_dir=$(bazel info bazel-genfiles --config=android64)

mkdir -p output libs/mapmind_vlp_api/include libs/mapmind_vlp_api/arm64-v8a

tarball="$genfile_dir/mapping/export/mapmind_vlp_api_pkg.tar.gz"
chmod u+w output/mapmind_vlp_api_pkg.tar.gz 2>/dev/null || true
cp -fv "$tarball" output/

tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

tar -xzf "$tarball" -C "$tmp_dir"

cp -v "$tmp_dir"/mapmind_vlp_api/lib/arm64-v8a/libmapmind_vlp_api.so \
  libs/mapmind_vlp_api/arm64-v8a/
cp -v "$tmp_dir"/mapmind_vlp_api/include/mapmind_vlp_api.h \
  libs/mapmind_vlp_api/include/
