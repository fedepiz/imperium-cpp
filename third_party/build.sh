#!/bin/zsh
# Builds every third-party dependency with its own build system and merges the
# results into one third_party.a for the app to link. Invoked by the root
# build.sh when necessary, or directly: ./third_party/build.sh
set -e
cd "$(dirname "$0")"

# raylib — its own Makefile; produces raylib/src/libraylib.a
make -C raylib/src PLATFORM=PLATFORM_DESKTOP

# clay — header-only; instantiate the implementation once
mkdir -p build
clang -std=c99 -O2 -c clay_impl.c -o build/clay_impl.o

# merge everything into the one archive
libtool -static -o third_party.a raylib/src/libraylib.a build/clay_impl.o
echo "ok: third_party/third_party.a"
