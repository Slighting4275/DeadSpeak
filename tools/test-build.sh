#!/bin/sh
# Verify the complete native build and its failure/security guards locally.
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd) || exit 1
cd "$project_dir" || exit 1
sh tools/build.sh || exit 1
zig_library_dir=${ZIG_LIB_DIR:-$(sed -n 's/^deadspeak.zigLibDir=//p' local.properties 2>/dev/null)}
if [ -n "$zig_library_dir" ]; then export ZIG_LIB_DIR="$zig_library_dir"; fi
zig cc -std=c17 -O1 -Wall -Wextra -Werror tests/build_test.c \
    $(pkg-config --cflags --libs libarchive openssl libxml-2.0) -o build/host/build-test || exit 1
build/host/build-test || exit 1
zig cc -std=c17 -O1 -Wall -Wextra -Werror tests/sensor_state_test.c \
    -lm -o build/host/sensor-state-test || exit 1
build/host/sensor-state-test || exit 1
sh tests/signing_test.sh
