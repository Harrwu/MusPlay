#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")/.."
xcrun clang++ -O2 -std=c++17 -fobjc-arc -I/usr/local/include -I/opt/homebrew/include macos/gui_macos.mm -o macos/player -framework Cocoa -framework AVFoundation
