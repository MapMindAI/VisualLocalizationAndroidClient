#!/usr/bin/env bash

set -e

INSTALL_PREFIX=/usr/local
if [[ ! -z $1 ]]; then
  INSTALL_PREFIX=$1
fi

mkdir -p /tmp/installers
pushd /tmp/installers

# Check dependencies:
# https://github.com/stevenlovegrove/Pangolin#required-dependencies
wget https://github.com/stevenlovegrove/Pangolin/archive/refs/tags/v0.8.zip
unzip v0.8.zip

pushd Pangolin-0.8
sed -i '/#  include <jpeglib.h>/a #include <cstdint>' components/pango_image/src/image_io_jpg.cpp

grep -q '#include <cstdint>' components/pango_image/src/image_io_bmp.cpp || \
sed -i '2a #include <cstdint>' components/pango_image/src/image_io_bmp.cpp

grep -q '#include <cstdint>' components/pango_packetstream/src/packetstream.cpp || \
sed -i '1a #include <cstdint>' components/pango_packetstream/src/packetstream.cpp

grep -q '#include <cstdint>' components/pango_packetstream/include/pangolin/log/packetstream_tags.h || \
sed -i '1a #include <cstdint>' components/pango_packetstream/include/pangolin/log/packetstream_tags.h

mkdir build
pushd build

cmake -D CMAKE_BUILD_TYPE=Release -D CMAKE_INSTALL_PREFIX=$INSTALL_PREFIX \
  -D BUILD_PANGOLIN_PYTHON=OFF -D BUILD_PANGOLIN_VIDEO=OFF \
  -D BUILD_TOOLS=OFF -D BUILD_EXAMPLES=OFF \
  ..
make install
popd
popd

rm -rf Pangolin-0.8 v0.8.zip

popd
