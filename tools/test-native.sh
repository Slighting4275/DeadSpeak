#!/bin/sh
# Run the C regression tests on an attached arm64 Android device.
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd) || exit 1
sdk_dir=$(sed -n 's/^sdk.dir=//p' "$project_dir/local.properties")
zig_library_dir=${ZIG_LIB_DIR:-$(sed -n 's/^deadspeak.zigLibDir=//p' "$project_dir/local.properties")}
if [ -n "$zig_library_dir" ]; then
    if [ ! -f "$zig_library_dir/std/std.zig" ]; then
        echo "Configure the standard library matching the installed Zig compiler." >&2
        exit 1
    fi
    if [ -f "$zig_library_dir/../compiler-version.txt" ] &&
       [ "$(zig version)" != "$(sed -n '1p' "$zig_library_dir/../compiler-version.txt")" ]; then
        echo "The configured Zig library does not match the installed compiler." >&2
        exit 1
    fi
    export ZIG_LIB_DIR="$zig_library_dir"
fi
ndk_version=$(sed -n 's/.*ndkVersion = "\([^"]*\)".*/\1/p' "$project_dir/app/build.gradle.kts")
ndk_tools="$sdk_dir/ndk/$ndk_version/toolchains/llvm/prebuilt/linux-x86_64"
if [ ! -x "$ndk_tools/bin/aarch64-linux-android30-clang" ]; then
    echo "Configure sdk.dir in local.properties and install the project's NDK." >&2
    exit 1
fi
test_build_dir=$(mktemp -d /tmp/deadspeak-tests-XXXXXX) || exit 1
trap 'rm -f "$test_build_dir/test.o" "$test_build_dir/test"; rmdir "$test_build_dir"' EXIT
zig cc -target aarch64-linux-android.30 --sysroot="$ndk_tools/sysroot" \
    -isystem "$ndk_tools/sysroot/usr/include" \
    -isystem "$ndk_tools/sysroot/usr/include/aarch64-linux-android" \
    -std=c17 -O1 -Wall -Wextra -Werror -fPIC -ffunction-sections -fdata-sections \
    -c "$project_dir/tests/explore_test.c" -o "$test_build_dir/test.o" || exit 1
"$ndk_tools/bin/aarch64-linux-android30-clang" "$test_build_dir/test.o" \
    -Wl,--gc-sections -llog -lm -o "$test_build_dir/test" || exit 1
adb push "$test_build_dir/test" /data/local/tmp/deadspeak-explore-test || exit 1
adb shell /data/local/tmp/deadspeak-explore-test
