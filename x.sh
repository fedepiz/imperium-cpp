#!/bin/zsh
# x.sh — the whole build system: one clang++ invocation per root binary.
# Modules (.hpp) are included by roots (.cpp); only roots are compiled.
#
# Usage: ./x.sh [command] [--release] [--debug]
#   clean        remove build artifacts
#   build        build all roots (default command; runs third_party first when necessary)
#   run          build if out of date, then run the game
#   test         build if out of date, then run the test suite
#   third_party  build dependencies into third_party/third_party.a
#
# Profiles: default is -O1 with ASSERT/LOG enabled; --release is -O2 with them
# compiled out for now; --debug adds -g to either.
set -e
cd "$(dirname "$0")"

CMD=build
RELEASE=0
DEBUG=0
for arg in "$@"; do
    case $arg in
        clean|build|run|test|third_party) CMD=$arg ;;
        --release) RELEASE=1 ;;
        --debug)   DEBUG=1 ;;
        *) echo "unknown argument: $arg"; exit 1 ;;
    esac
done

# -Wno-reorder-init-list: out-of-order designated init is our style (ZII PODs;
# field order at the call site follows meaning, not declaration).
# -Wno-error=unused/-Wno-error=unused-parameter: unused names (variables,
# parameters, functions) warn without failing the build — stubs keep compiling.
BASE_FLAGS=(-std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror
            -Wno-error=unused -Wno-error=unused-parameter -Wno-reorder-init-list
            -Ithird_party/raylib/src)
if (( RELEASE )); then
    FLAGS=($BASE_FLAGS -O2)
    PROFILE=release
else
    FLAGS=($BASE_FLAGS -O1 -DASSERT_ENABLE -DLOG_ENABLE)
    PROFILE=default
fi
if (( DEBUG )); then
    FLAGS+=(-g)
    PROFILE="$PROFILE+debug"
fi

BIN=build/imperium
STAMP=build/flags

# Every .cpp in src/ is a root and builds to its own binary; main.cpp is the
# game. Exception: ray.cpp is the raylib boundary TU (see src/ray.hpp) —
# compiled once to build/ray.o and linked into every binary, never a root.
bin_for() {
    local name=${${1:t}:r}
    [[ $name == main ]] && { echo $BIN; return }
    echo build/$name
}

third_party_if_needed() {
    local a=third_party/third_party.a
    # Vendored library sources are effectively frozen — staleness only tracks
    # our own third_party files. After editing vendored code, run
    # `./x.sh third_party` by hand.
    [[ -f $a && ! third_party/build.sh -nt $a ]] && return 0
    cmd_third_party
}

cmd_third_party() {
    ./third_party/build.sh
}

# compile_commands.json — one entry per src file, always the default profile,
# so clangd checks every module standalone against the config most code runs in.
gen_compile_commands() {
    local cc_flags="${(j: :)BASE_FLAGS} -O1 -DASSERT_ENABLE -DLOG_ENABLE"
    {
        echo "["
        local first=1
        # **/* recursion so modules in subdirs (src/ui/...) get entries too
        for f in src/**/*.cpp(N) src/**/*.hpp(N); do
            [[ $first == 1 ]] || echo ","
            first=0
            # -x c++ makes clangd treat module .hpp files as self-contained TUs
            local cmd="clang++ $cc_flags -c $f"
            [[ $f == *.hpp ]] && cmd="clang++ -x c++ $cc_flags -c $f"
            printf '  {"directory": "%s", "file": "%s", "command": "%s"}' "$PWD" "$f" "$cmd"
        done
        echo ""
        echo "]"
    } > compile_commands.json
}

build_needed() {
    local root
    for root in src/*.cpp; do
        [[ ${root:t} == ray.cpp ]] && continue
        [[ -f $(bin_for $root) ]] || return 0
    done
    [[ "$(cat $STAMP 2>/dev/null)" == "$FLAGS" ]] || return 0
    [[ -n $(find src \( -name '*.cpp' -o -name '*.hpp' \) -newer $BIN) ]] && return 0
    [[ x.sh -nt $BIN ]] && return 0
    [[ -f third_party/third_party.a && third_party/third_party.a -nt $BIN ]] && return 0
    return 1
}

cmd_build() {
    third_party_if_needed
    mkdir -p build
    local link=()
    if [[ -f third_party/third_party.a ]]; then
        # frameworks are raylib's macOS platform dependencies
        link+=(third_party/third_party.a
               -framework Cocoa -framework IOKit -framework CoreVideo
               -framework CoreAudio -framework OpenGL)
    fi
    clang++ $FLAGS -c src/ray.cpp -o build/ray.o
    local root out outs=()
    for root in src/*.cpp; do
        [[ ${root:t} == ray.cpp ]] && continue
        out=$(bin_for $root)
        clang++ $FLAGS $root build/ray.o $link -o $out
        outs+=($out)
    done
    print -r -- $FLAGS > $STAMP
    gen_compile_commands
    echo "ok: ${(j: :)outs} ($PROFILE)"
}

cmd_run() {
    if build_needed; then
        cmd_build
    fi
    exec ./$BIN
}

cmd_test() {
    if build_needed; then
        cmd_build
    fi
    exec ./build/test
}

cmd_clean() {
    rm -rf build
    echo "ok: cleaned"
}

case $CMD in
    clean)       cmd_clean ;;
    build)       cmd_build ;;
    run)         cmd_run ;;
    test)        cmd_test ;;
    third_party) cmd_third_party ;;
esac
