#!/bin/zsh
# Builds every third-party dependency with its own build system and merges the
# results into one third_party.a for the app to link. Invoked by the root
# build.sh when necessary, or directly: ./third_party/build.sh
set -e
cd "$(dirname "$0")"

# raylib — its own Makefile; produces raylib/src/libraylib.a
make -C raylib/src PLATFORM=PLATFORM_DESKTOP

# merge everything into the one archive
libtool -static -o third_party.a raylib/src/libraylib.a
echo "ok: third_party/third_party.a"
