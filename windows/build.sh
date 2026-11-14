#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")/.."
x86_64-w64-mingw32-g++ -O2 -std=c++17 windows/gui_win32.cpp -I/usr/include/miniaudio -o windows/player.exe \
    -static -mwindows -lole32 -lwinmm -luuid -lshell32 -lgdi32 -lwindowscodecs -lwininet -lcomctl32
