#!/bin/sh
# Verify the complete native build and its failure/security guards locally.
project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd) || exit 1
cd "$project_dir" || exit 1
sh tools/build.sh || exit 1
zig_library_dir=${ZIG_LIB_DIR:-$(sed -n 's/^deadspeak.zigLibDir=//p' local.properties 2>/dev/null)}
if [ -n "$zig_library_dir" ]; then export ZIG_LIB_DIR="$zig_library_dir"; fi
# shellcheck disable=SC2046
zig cc -std=c23 -O1 -Wall -Wextra -Werror tests/build_test.c \
    $(pkg-config --cflags --libs libarchive openssl libxml-2.0) -o build/host/build-test || exit 1
build/host/build-test || exit 1
zig cc -std=c23 -O1 -Wall -Wextra -Werror tests/sensor_state_test.c \
    -lm -o build/host/sensor-state-test || exit 1
build/host/sensor-state-test || exit 1
zig cc -std=c23 -O1 -Wall -Wextra -Werror tests/camera_ui_test.c \
    -lm -o build/host/camera-ui-test || exit 1
build/host/camera-ui-test || exit 1
zig cc -std=c23 -O1 -Wall -Wextra -Werror tests/custom_words_test.c \
    app/src/main/cpp/custom_words.c -o build/host/custom-words-test || exit 1
build/host/custom-words-test || exit 1
zig build-lib -static -O ReleaseSafe -fPIC -fsingle-threaded -fno-stack-check -fno-entry \
    src/engine.zig -femit-bin=build/host/libengine-test.a || exit 1
zig cc -std=c23 -O1 -Wall -Wextra -Werror tests/engine_test.c \
    build/host/libengine-test.a -lm -o build/host/engine-test || exit 1
build/host/engine-test || exit 1
zig cc -std=c23 -O1 -Wall -Wextra -Werror tests/voice_synth_test.c \
    app/src/main/cpp/voice_synth.c -lm -o build/host/voice-synth-test || exit 1
build/host/voice-synth-test || exit 1
zig cc -std=c23 -O1 -Wall -Wextra -Werror tests/motion_filter_test.c \
    -lm -o build/host/motion-filter-test || exit 1
build/host/motion-filter-test || exit 1
zig cc -std=c23 -O1 -Wall -Wextra -Werror tests/typography_test.c \
    -lm -o build/host/typography-test || exit 1
build/host/typography-test || exit 1
zig cc -std=c23 -O1 -Wall -Wextra -Werror tests/settings_test.c \
    app/src/main/cpp/settings.c -lm -o build/host/settings-test || exit 1
build/host/settings-test || exit 1
sh tests/signing_test.sh
