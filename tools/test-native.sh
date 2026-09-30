#!/bin/sh
# Run the C regression tests on an attached arm64 Android device.
project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd) || exit 1
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
ndk_version=$(sed -n 's/^#define DS_NDK_VERSION "\([^"]*\)"/\1/p' "$project_dir/tools/build_config.h")
ndk_tools="$sdk_dir/ndk/$ndk_version/toolchains/llvm/prebuilt/linux-x86_64"
if [ ! -x "$ndk_tools/bin/aarch64-linux-android30-clang" ]; then
    echo "Configure sdk.dir in local.properties and install the project's NDK." >&2
    exit 1
fi
test_build_dir=$(mktemp -d /tmp/deadspeak-tests-XXXXXX) || exit 1
trap 'rm -rf "$test_build_dir"' EXIT
zig cc -target aarch64-linux-android.30 --sysroot="$ndk_tools/sysroot" \
    -isystem "$ndk_tools/sysroot/usr/include" \
    -isystem "$ndk_tools/sysroot/usr/include/aarch64-linux-android" \
    -std=c23 -O1 -Wall -Wextra -Werror -fPIC -ffunction-sections -fdata-sections \
    -c "$project_dir/tests/explore_test.c" -o "$test_build_dir/explore_test.o" || exit 1
"$ndk_tools/bin/aarch64-linux-android30-clang" "$test_build_dir/explore_test.o" \
    -Wl,--gc-sections -llog -lm -o "$test_build_dir/explore_test" || exit 1
adb push "$test_build_dir/explore_test" /data/local/tmp/deadspeak-explore-test || exit 1
adb shell /data/local/tmp/deadspeak-explore-test || exit 1

zig cc -target aarch64-linux-android.30 --sysroot="$ndk_tools/sysroot" \
    -isystem "$ndk_tools/sysroot/usr/include" \
    -isystem "$ndk_tools/sysroot/usr/include/aarch64-linux-android" \
    -I"$project_dir/app/src/main/cpp" \
    -std=c23 -O1 -Wall -Wextra -Werror -fPIC -ffunction-sections -fdata-sections \
    -c "$project_dir/tests/voice_player_test.c" -o "$test_build_dir/vptest.o" || exit 1
zig cc -target aarch64-linux-android.30 --sysroot="$ndk_tools/sysroot" \
    -isystem "$ndk_tools/sysroot/usr/include" \
    -isystem "$ndk_tools/sysroot/usr/include/aarch64-linux-android" \
    -I"$project_dir/app/src/main/cpp" \
    -std=c23 -O1 -Wall -Wextra -Werror -fPIC -ffunction-sections -fdata-sections \
    -c "$project_dir/app/src/main/cpp/voice_player.c" -o "$test_build_dir/vp.o" || exit 1
zig cc -target aarch64-linux-android.30 --sysroot="$ndk_tools/sysroot" \
    -isystem "$ndk_tools/sysroot/usr/include" \
    -isystem "$ndk_tools/sysroot/usr/include/aarch64-linux-android" \
    -I"$project_dir/app/src/main/cpp" \
    -std=c23 -O1 -Wall -Wextra -Werror -fPIC -ffunction-sections -fdata-sections \
    -c "$project_dir/app/src/main/cpp/voice_synth.c" -o "$test_build_dir/vs.o" || exit 1
"$ndk_tools/bin/aarch64-linux-android30-clang" "$test_build_dir/vptest.o" "$test_build_dir/vp.o" "$test_build_dir/vs.o" \
    -Wl,--gc-sections -llog -lm -laaudio -o "$test_build_dir/voice_player_test" || exit 1
adb push "$test_build_dir/voice_player_test" /data/local/tmp/deadspeak-voice-player-test || exit 1
adb shell /data/local/tmp/deadspeak-voice-player-test || exit 1
