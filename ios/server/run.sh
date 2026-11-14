#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")/../.."
exec python3 ios/server/music_server.py --port 1411 "$@"
