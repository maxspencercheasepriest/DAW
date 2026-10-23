#!/bin/bash
# Builds Tracklayer.app on a Mac.
# Needs Xcode (free from the App Store) or just the Command Line Tools
# (run: xcode-select --install), plus CMake (brew install cmake).
set -e
cd "$(dirname "$0")"

if ! command -v cmake >/dev/null; then
  echo "CMake isn't installed. Install Homebrew from https://brew.sh then run: brew install cmake"
  exit 1
fi

cmake -B build-mac -DCMAKE_BUILD_TYPE=Release
cmake --build build-mac --config Release --parallel
rm -rf Tracklayer.app
cp -R build-mac/Tracklayer_artefacts/Release/Tracklayer.app .
echo
echo "Done: $(pwd)/Tracklayer.app"
open -R Tracklayer.app
