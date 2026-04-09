#!/usr/bin/env bash

set -e

[[ -n $ANDROID_HOME ]] || (
  echo >&2 "Please provide the ANDROID_HOME environment variable"
  exit 1
)
[[ -n $ANDROID_NDK_VERSION ]] || (
  echo >&2 "Please provide the ANDROID_NDK_VERSION environment variable"
  exit 1
)

# Use Android command line tools instead of Android Studio.
# See https://developer.android.com/studio#command-tools.
ANDROID_TOOLS_RELEASE=commandlinetools-linux-6858069_latest
wget https://dl.google.com/android/repository/$ANDROID_TOOLS_RELEASE.zip
mkdir -p $ANDROID_HOME
unzip $ANDROID_TOOLS_RELEASE.zip -d $ANDROID_HOME
rm $ANDROID_TOOLS_RELEASE.zip

# https://developer.android.com/studio/releases/build-tools
# https://developer.android.com/studio/releases/platforms
yes | $ANDROID_HOME/cmdline-tools/bin/sdkmanager \
  --sdk_root=$ANDROID_HOME \
  "ndk;$ANDROID_NDK_VERSION" \
  "platforms;android-33" \
  "build-tools;30.0.3"
