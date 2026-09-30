#!/bin/sh
# Bootstrap the C build driver. No JVM, Gradle, Java compiler, DEX, or JARs.
project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd) || exit 1
cd "$project_dir" || exit 1
zig_library_dir=${ZIG_LIB_DIR:-$(sed -n 's/^deadspeak.zigLibDir=//p' local.properties 2>/dev/null)}
if [ -n "$zig_library_dir" ]; then
    export ZIG_LIB_DIR="$zig_library_dir"
    if [ ! -f "$zig_library_dir/std/std.zig" ]; then
        echo "Configure the standard library matching the installed Zig compiler." >&2
        exit 1
    fi
    if [ -f "$zig_library_dir/../compiler-version.txt" ] &&
       [ "$(zig version)" != "$(sed -n '1p' "$zig_library_dir/../compiler-version.txt")" ]; then
        echo "Zig compiler and configured library versions differ." >&2
        exit 1
    fi
fi
mkdir -p build/host || exit 1
# shellcheck disable=SC2046
zig cc -std=c23 -O2 -Wall -Wextra -Werror -fPIE -pie -fstack-protector-strong tools/build.c \
    $(pkg-config --cflags --libs libarchive openssl libxml-2.0) -o build/host/deadspeak-build || exit 1
exec build/host/deadspeak-build "$@"
