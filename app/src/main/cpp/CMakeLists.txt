cmake_minimum_required(VERSION 3.25)

if(DEFINED DS_ZIG_LIB_DIR)
    if(NOT EXISTS "${DS_ZIG_LIB_DIR}/std/std.zig")
        message(FATAL_ERROR "DS_ZIG_LIB_DIR must name the library matching the installed Zig compiler")
    endif()
    set(DS_ZIG_ENV "ZIG_LIB_DIR=${DS_ZIG_LIB_DIR}")
else()
    set(DS_ZIG_ENV "--unset=ZIG_LIB_DIR")
endif()

if (ANDROID_ABI STREQUAL "arm64-v8a")
    set(DS_ZIG_TARGET "aarch64-linux-android.30")
    set(DS_ANDROID_INCLUDE "aarch64-linux-android")
elseif (ANDROID_ABI STREQUAL "armeabi-v7a")
    set(DS_ZIG_TARGET "arm-linux-android.30")
    set(DS_ZIG_ABI_FLAGS -mfloat-abi=softfp)
    set(DS_ANDROID_INCLUDE "arm-linux-androideabi")
elseif (ANDROID_ABI STREQUAL "x86_64")
    set(DS_ZIG_TARGET "x86_64-linux-android.30")
    set(DS_ANDROID_INCLUDE "x86_64-linux-android")
else()
    message(FATAL_ERROR "Unsupported ABI for Zig engine: ${ANDROID_ABI}")
endif()

project(deadspeak C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_program(DS_ZIG_EXECUTABLE zig
    HINTS /usr/local/bin
    PATHS /usr/local/bin
    NO_DEFAULT_PATH
    REQUIRED
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DS_ZIG_EXECUTABLE}")
if(DEFINED DS_ZIG_LIB_DIR)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DS_ZIG_LIB_DIR}/std/std.zig")
endif()
if(DEFINED DS_ZIG_LIB_DIR AND EXISTS "${DS_ZIG_LIB_DIR}/../compiler-version.txt")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DS_ZIG_LIB_DIR}/../compiler-version.txt")
    file(READ "${DS_ZIG_LIB_DIR}/../compiler-version.txt" DS_EXPECTED_ZIG_VERSION)
    string(STRIP "${DS_EXPECTED_ZIG_VERSION}" DS_EXPECTED_ZIG_VERSION)
    execute_process(COMMAND ${DS_ZIG_EXECUTABLE} version
        OUTPUT_VARIABLE DS_INSTALLED_ZIG_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    if(NOT DS_EXPECTED_ZIG_VERSION STREQUAL DS_INSTALLED_ZIG_VERSION)
        message(FATAL_ERROR "Installed Zig ${DS_INSTALLED_ZIG_VERSION} does not match the configured library ${DS_EXPECTED_ZIG_VERSION}; update the library to the installed compiler's source revision")
    endif()
endif()
set(DS_ZIG_ARCHIVE "${CMAKE_CURRENT_BINARY_DIR}/libdsengine.a")
add_custom_command(
    OUTPUT "${DS_ZIG_ARCHIVE}"
    COMMAND ${CMAKE_COMMAND} -E env "${DS_ZIG_ENV}"
            ${DS_ZIG_EXECUTABLE} build-lib -static -O ReleaseSafe -fPIC -fsingle-threaded -fno-stack-check -fno-entry -target ${DS_ZIG_TARGET}
            "${CMAKE_CURRENT_SOURCE_DIR}/../../../../src/engine.zig"
            -femit-bin=${DS_ZIG_ARCHIVE}
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../../../src/engine.zig" "${DS_ZIG_EXECUTABLE}"
    COMMENT "Compiling DeadSpeak signal engine with Zig (${DS_ZIG_TARGET})"
    VERBATIM
)
add_custom_target(dsengine_build DEPENDS "${DS_ZIG_ARCHIVE}")
add_library(dsengine STATIC IMPORTED GLOBAL)
set_target_properties(dsengine PROPERTIES IMPORTED_LOCATION "${DS_ZIG_ARCHIVE}")
add_dependencies(dsengine dsengine_build)

set(DS_NATIVE_GLUE "${CMAKE_CURRENT_SOURCE_DIR}/native_activity.c")
set(DS_ZIG_OBJECT_DIR "${CMAKE_CURRENT_BINARY_DIR}/zig-objects")
set(DS_MAIN_OBJECT "${DS_ZIG_OBJECT_DIR}/main.c.o")
set(DS_GLUE_OBJECT "${DS_ZIG_OBJECT_DIR}/android_native_app_glue.c.o")
set(DS_VIDEO_OBJECT "${DS_ZIG_OBJECT_DIR}/video_capture.c.o")
set(DS_EXPLORE_OBJECT "${DS_ZIG_OBJECT_DIR}/explore.c.o")
set(DS_CUSTOM_WORDS_OBJECT "${DS_ZIG_OBJECT_DIR}/custom_words.c.o")
set(DS_VOICE_OBJECT "${DS_ZIG_OBJECT_DIR}/voice_synth.c.o")
set(DS_VOICE_PLAYER_OBJECT "${DS_ZIG_OBJECT_DIR}/voice_player.c.o")
set(DS_MEDIA_PLAYER_OBJECT "${DS_ZIG_OBJECT_DIR}/media_player.c.o")
set(DS_SETTINGS_OBJECT "${DS_ZIG_OBJECT_DIR}/settings.c.o")
set(DS_EVP_OPS_OBJECT "${DS_ZIG_OBJECT_DIR}/evp_ops.c.o")
set(DS_FONT_ATLAS_OBJECT "${DS_ZIG_OBJECT_DIR}/font_atlas.c.o")

function(ds_add_zig_c_object OUTPUT_OBJ SOURCE_FILE COMMENT_TEXT)
    set(options ALLOW_UNUSED_PARAM)
    set(multiValueArgs EXTRA_INCLUDES DEPENDS_LIST)
    cmake_parse_arguments(ARG "${options}" "" "${multiValueArgs}" ${ARGN})
    set(DS_EXTRA_WARN_FLAGS "")
    if(ARG_ALLOW_UNUSED_PARAM)
        list(APPEND DS_EXTRA_WARN_FLAGS -Wno-unused-parameter)
    endif()
    set(DS_INCLUDE_FLAGS "-I${CMAKE_CURRENT_SOURCE_DIR}")
    foreach(inc IN LISTS ARG_EXTRA_INCLUDES)
        list(APPEND DS_INCLUDE_FLAGS "-I${inc}")
    endforeach()
    add_custom_command(
        OUTPUT "${OUTPUT_OBJ}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DS_ZIG_OBJECT_DIR}"
        COMMAND ${CMAKE_COMMAND} -E env "${DS_ZIG_ENV}"
                ${DS_ZIG_EXECUTABLE} cc
                -target ${DS_ZIG_TARGET}
                --sysroot=${CMAKE_SYSROOT}
                -isystem ${CMAKE_SYSROOT}/usr/include
                -isystem ${CMAKE_SYSROOT}/usr/include/${DS_ANDROID_INCLUDE}
                ${DS_INCLUDE_FLAGS}
                -std=c23 -O3 -Wall -Wextra ${DS_EXTRA_WARN_FLAGS} -Werror=deprecated-declarations
                ${DS_ZIG_ABI_FLAGS}
                -fPIC -ffunction-sections -fdata-sections -fstack-protector-strong
                -DANDROID
                -c "${SOURCE_FILE}"
                -o "${OUTPUT_OBJ}"
        DEPENDS "${SOURCE_FILE}" ${ARG_DEPENDS_LIST} "${DS_ZIG_EXECUTABLE}"
        COMMENT "${COMMENT_TEXT} (${DS_ZIG_TARGET})"
        VERBATIM
    )
endfunction()

ds_add_zig_c_object("${DS_VOICE_OBJECT}" "${CMAKE_CURRENT_SOURCE_DIR}/voice_synth.c"
    "Compiling formant voice synthesizer with Zig cc"
    DEPENDS_LIST "${CMAKE_CURRENT_SOURCE_DIR}/voice_synth.h")

ds_add_zig_c_object("${DS_VOICE_PLAYER_OBJECT}" "${CMAKE_CURRENT_SOURCE_DIR}/voice_player.c"
    "Compiling native audio voice player with Zig cc"
    DEPENDS_LIST "${CMAKE_CURRENT_SOURCE_DIR}/voice_player.h" "${CMAKE_CURRENT_SOURCE_DIR}/voice_synth.h" "${CMAKE_CURRENT_SOURCE_DIR}/evp_ops.h")

ds_add_zig_c_object("${DS_CUSTOM_WORDS_OBJECT}" "${CMAKE_CURRENT_SOURCE_DIR}/custom_words.c"
    "Compiling local custom word bank with Zig cc"
    DEPENDS_LIST "${CMAKE_CURRENT_SOURCE_DIR}/custom_words.h")

ds_add_zig_c_object("${DS_EXPLORE_OBJECT}" "${CMAKE_CURRENT_SOURCE_DIR}/explore.c"
    "Compiling native Wikipedia Explore client with Zig cc"
    ALLOW_UNUSED_PARAM
    DEPENDS_LIST "${CMAKE_CURRENT_SOURCE_DIR}/explore.h" "${CMAKE_CURRENT_SOURCE_DIR}/jni_scope.h")

ds_add_zig_c_object("${DS_VIDEO_OBJECT}" "${CMAKE_CURRENT_SOURCE_DIR}/video_capture.c"
    "Compiling native Camera2 and MediaCodec capture with Zig cc"
    ALLOW_UNUSED_PARAM
    DEPENDS_LIST "${CMAKE_CURRENT_SOURCE_DIR}/video_capture.h" "${CMAKE_CURRENT_SOURCE_DIR}/video_shaders.h" "${CMAKE_CURRENT_SOURCE_DIR}/camera_ui.h" "${CMAKE_CURRENT_SOURCE_DIR}/typography.h" "${CMAKE_CURRENT_SOURCE_DIR}/preview_frame.h" "${CMAKE_CURRENT_SOURCE_DIR}/video_orientation.h")

ds_add_zig_c_object("${DS_MEDIA_PLAYER_OBJECT}" "${CMAKE_CURRENT_SOURCE_DIR}/media_player.c"
    "Compiling native MP4 A/V playback extractor with Zig cc"
    DEPENDS_LIST "${CMAKE_CURRENT_SOURCE_DIR}/media_player.h" "${CMAKE_CURRENT_SOURCE_DIR}/preview_frame.h" "${CMAKE_CURRENT_SOURCE_DIR}/evp_ops.h")

ds_add_zig_c_object("${DS_SETTINGS_OBJECT}" "${CMAKE_CURRENT_SOURCE_DIR}/settings.c"
    "Compiling persistent settings store with Zig cc"
    DEPENDS_LIST "${CMAKE_CURRENT_SOURCE_DIR}/settings.h" "${CMAKE_CURRENT_SOURCE_DIR}/camera_ui.h" "${CMAKE_CURRENT_SOURCE_DIR}/field_baseline.h" "${CMAKE_CURRENT_SOURCE_DIR}/video_capture.h" "${CMAKE_CURRENT_SOURCE_DIR}/voice_synth.h")

ds_add_zig_c_object("${DS_EVP_OPS_OBJECT}" "${CMAKE_CURRENT_SOURCE_DIR}/evp_ops.c"
    "Compiling EVP audio/photo/tag helpers with Zig cc"
    DEPENDS_LIST "${CMAKE_CURRENT_SOURCE_DIR}/evp_ops.h" "${CMAKE_CURRENT_SOURCE_DIR}/video_capture.h" "${CMAKE_CURRENT_SOURCE_DIR}/typography.h")

ds_add_zig_c_object("${DS_FONT_ATLAS_OBJECT}" "${CMAKE_CURRENT_SOURCE_DIR}/font_atlas.c"
    "Compiling anti-aliased font atlas data with Zig cc"
    DEPENDS_LIST "${CMAKE_CURRENT_SOURCE_DIR}/font_atlas.h")

ds_add_zig_c_object("${DS_MAIN_OBJECT}" "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
    "Compiling DeadSpeak C platform layer with Zig cc"
    ALLOW_UNUSED_PARAM
    EXTRA_INCLUDES "${ANDROID_NDK}/sources/android/native_app_glue"
    DEPENDS_LIST "${CMAKE_CURRENT_SOURCE_DIR}/typography.h" "${CMAKE_CURRENT_SOURCE_DIR}/voice_player.h" "${CMAKE_CURRENT_SOURCE_DIR}/voice_synth.h" "${CMAKE_CURRENT_SOURCE_DIR}/motion_filter.h" "${CMAKE_CURRENT_SOURCE_DIR}/field_baseline.h" "${CMAKE_CURRENT_SOURCE_DIR}/camera_ui.h" "${CMAKE_CURRENT_SOURCE_DIR}/custom_words.h" "${CMAKE_CURRENT_SOURCE_DIR}/sensor_state.h" "${CMAKE_CURRENT_SOURCE_DIR}/engine.h" "${CMAKE_CURRENT_SOURCE_DIR}/explore.h" "${CMAKE_CURRENT_SOURCE_DIR}/jni_scope.h" "${CMAKE_CURRENT_SOURCE_DIR}/ui_layout.h" "${CMAKE_CURRENT_SOURCE_DIR}/video_capture.h" "${CMAKE_CURRENT_SOURCE_DIR}/video_shaders.h" "${CMAKE_CURRENT_SOURCE_DIR}/media_player.h" "${CMAKE_CURRENT_SOURCE_DIR}/settings.h" "${CMAKE_CURRENT_SOURCE_DIR}/evp_ops.h")

ds_add_zig_c_object("${DS_GLUE_OBJECT}" "${DS_NATIVE_GLUE}"
    "Compiling Android native glue with Zig cc"
    ALLOW_UNUSED_PARAM
    EXTRA_INCLUDES "${ANDROID_NDK}/sources/android/native_app_glue"
    DEPENDS_LIST "${CMAKE_CURRENT_SOURCE_DIR}/jni_scope.h" "${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c" "${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.h")

add_custom_target(ds_c_objects DEPENDS "${DS_MAIN_OBJECT}" "${DS_GLUE_OBJECT}" "${DS_VIDEO_OBJECT}" "${DS_EXPLORE_OBJECT}" "${DS_CUSTOM_WORDS_OBJECT}" "${DS_VOICE_OBJECT}" "${DS_VOICE_PLAYER_OBJECT}" "${DS_MEDIA_PLAYER_OBJECT}" "${DS_SETTINGS_OBJECT}" "${DS_EVP_OPS_OBJECT}" "${DS_FONT_ATLAS_OBJECT}")
set_source_files_properties("${DS_MAIN_OBJECT}" "${DS_GLUE_OBJECT}" "${DS_VIDEO_OBJECT}" "${DS_EXPLORE_OBJECT}" "${DS_CUSTOM_WORDS_OBJECT}" "${DS_VOICE_OBJECT}" "${DS_VOICE_PLAYER_OBJECT}" "${DS_MEDIA_PLAYER_OBJECT}" "${DS_SETTINGS_OBJECT}" "${DS_EVP_OPS_OBJECT}" "${DS_FONT_ATLAS_OBJECT}" PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)

add_library(deadspeak SHARED
    "${DS_MAIN_OBJECT}"
    "${DS_GLUE_OBJECT}"
    "${DS_VIDEO_OBJECT}"
    "${DS_EXPLORE_OBJECT}"
    "${DS_CUSTOM_WORDS_OBJECT}"
    "${DS_VOICE_OBJECT}"
    "${DS_VOICE_PLAYER_OBJECT}"
    "${DS_MEDIA_PLAYER_OBJECT}"
    "${DS_SETTINGS_OBJECT}"
    "${DS_EVP_OPS_OBJECT}"
    "${DS_FONT_ATLAS_OBJECT}"
)
set_target_properties(deadspeak PROPERTIES LINKER_LANGUAGE C)
target_include_directories(deadspeak PRIVATE
    "${ANDROID_NDK}/sources/android/native_app_glue"
)
target_compile_options(deadspeak PRIVATE -ffunction-sections -fdata-sections -fstack-protector-strong)
target_link_options(deadspeak PRIVATE -Wl,--gc-sections -Wl,-z,relro -Wl,-z,now)
target_link_libraries(deadspeak dsengine android log EGL GLESv2 aaudio camera2ndk mediandk m)
add_dependencies(deadspeak ds_c_objects dsengine_build)
