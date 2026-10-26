#!/bin/bash
# Builds a universal Tracklayer.app (Apple Silicon + Intel, macOS 11 or later,
# so it runs on Sequoia on Intel Macs too).
# Needs the Apple command line tools (xcode-select --install) and CMake (brew install cmake).
set -e
cd "$(dirname "$0")"

if ! command -v cmake >/dev/null; then
  echo "CMake isn't installed. Install Homebrew from https://brew.sh then run: brew install cmake"
  exit 1
fi

cmake -B build-mac -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build build-mac --config Release --parallel

APP=build-mac/Tracklayer_artefacts/Release/Tracklayer.app
[ -d "$APP" ] || APP=build-mac/Tracklayer_artefacts/Tracklayer.app
codesign --force --deep --sign - "$APP"
lipo -verify_arch x86_64 arm64 "$APP/Contents/MacOS/Tracklayer" && echo "Universal app: runs on Intel and Apple Silicon"

rm -rf Tracklayer.app
cp -R "$APP" .
ditto -c -k --keepParent Tracklayer.app Tracklayer-mac.zip
echo
echo "Done: $(pwd)/Tracklayer.app"
echo "Zip for sharing: $(pwd)/Tracklayer-mac.zip"
open -R Tracklayer.app
