#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")/.."
g++ -O2 -std=c++17 main.cpp -I/usr/include/miniaudio -o linux/player -lpthread -lm -ldl
