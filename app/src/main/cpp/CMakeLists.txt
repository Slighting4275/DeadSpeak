cmake_minimum_required(VERSION 3.22.1)

if(DEFINED ENV{ZIG_LIB_DIR} AND EXISTS "$ENV{ZIG_LIB_DIR}")
    set(DS_ZIG_LIB_DIR "$ENV{ZIG_LIB_DIR}")
elseif(EXISTS "/tmp/deadspeak-zig-matched/zig/lib")
    set(DS_ZIG_LIB_DIR "/tmp/deadspeak-zig-matched/zig/lib")
else()
    set(DS_ZIG_LIB_DIR "")
endif()

if (ANDROID_ABI STREQUAL "arm64-v8a")
    set(DS_ZIG_TARGET "aarch64-linux-android.30")
    set(DS_ANDROID_INCLUDE "aarch64-linux-android")
elseif (ANDROID_ABI STREQUAL "armeabi-v7a")
    set(DS_ZIG_TARGET "arm-linux-androideabi.30")
    set(DS_ANDROID_INCLUDE "arm-linux-androideabi")
elseif (ANDROID_ABI STREQUAL "x86_64")
    set(DS_ZIG_TARGET "x86_64-linux-android.30")
    set(DS_ANDROID_INCLUDE "x86_64-linux-android")
else()
    message(FATAL_ERROR "Unsupported ABI for Zig engine: ${ANDROID_ABI}")
endif()

project(deadspeak C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_program(DS_ZIG_EXECUTABLE zig
    HINTS /usr/local/bin
    PATHS /usr/local/bin
    NO_DEFAULT_PATH
    REQUIRED
)
set(DS_ZIG_ARCHIVE "${CMAKE_CURRENT_BINARY_DIR}/libdsengine.a")
add_custom_command(
    OUTPUT "${DS_ZIG_ARCHIVE}"
    COMMAND ${CMAKE_COMMAND} -E env "ZIG_LIB_DIR=${DS_ZIG_LIB_DIR}"
            ${DS_ZIG_EXECUTABLE} build-lib -static -O ReleaseSafe -fPIC -fsingle-threaded -fno-stack-check -fno-entry -target ${DS_ZIG_TARGET}
            "${CMAKE_CURRENT_SOURCE_DIR}/../../../../src/engine.zig"
            -femit-bin=${DS_ZIG_ARCHIVE}
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../../../src/engine.zig"
    COMMENT "Compiling DeadSpeak signal engine with Zig (${DS_ZIG_TARGET})"
    VERBATIM
)
add_custom_target(dsengine_build DEPENDS "${DS_ZIG_ARCHIVE}")
add_library(dsengine STATIC IMPORTED GLOBAL)
set_target_properties(dsengine PROPERTIES IMPORTED_LOCATION "${DS_ZIG_ARCHIVE}")
add_dependencies(dsengine dsengine_build)

set(DS_NATIVE_GLUE "${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c")
set(DS_ZIG_OBJECT_DIR "${CMAKE_CURRENT_BINARY_DIR}/zig-objects")
set(DS_MAIN_OBJECT "${DS_ZIG_OBJECT_DIR}/main.c.o")
set(DS_GLUE_OBJECT "${DS_ZIG_OBJECT_DIR}/android_native_app_glue.c.o")
set(DS_VIDEO_OBJECT "${DS_ZIG_OBJECT_DIR}/video_capture.c.o")
set(DS_EXPLORE_OBJECT "${DS_ZIG_OBJECT_DIR}/explore.c.o")

add_custom_command(
    OUTPUT "${DS_EXPLORE_OBJECT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DS_ZIG_OBJECT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E env "ZIG_LIB_DIR=${DS_ZIG_LIB_DIR}"
            ${DS_ZIG_EXECUTABLE} cc
            -target ${DS_ZIG_TARGET}
            --sysroot=${CMAKE_SYSROOT}
            -I${CMAKE_SYSROOT}/usr/include
            -I${CMAKE_SYSROOT}/usr/include/${DS_ANDROID_INCLUDE}
            -I${CMAKE_CURRENT_SOURCE_DIR}
            -std=c17 -O3 -fPIC -ffunction-sections -fdata-sections
            -DANDROID
            -c "${CMAKE_CURRENT_SOURCE_DIR}/explore.c"
            -o "${DS_EXPLORE_OBJECT}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/explore.c" "${CMAKE_CURRENT_SOURCE_DIR}/explore.h"
    COMMENT "Compiling native Wikipedia Explore client with Zig cc (${DS_ZIG_TARGET})"
    VERBATIM
)

add_custom_command(
    OUTPUT "${DS_VIDEO_OBJECT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DS_ZIG_OBJECT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E env "ZIG_LIB_DIR=${DS_ZIG_LIB_DIR}"
            ${DS_ZIG_EXECUTABLE} cc
            -target ${DS_ZIG_TARGET}
            --sysroot=${CMAKE_SYSROOT}
            -I${CMAKE_SYSROOT}/usr/include
            -I${CMAKE_SYSROOT}/usr/include/${DS_ANDROID_INCLUDE}
            -I${CMAKE_CURRENT_SOURCE_DIR}
            -std=c17 -O3 -fPIC -ffunction-sections -fdata-sections
            -DANDROID
            -c "${CMAKE_CURRENT_SOURCE_DIR}/video_capture.c"
            -o "${DS_VIDEO_OBJECT}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/video_capture.c" "${CMAKE_CURRENT_SOURCE_DIR}/video_capture.h"
    COMMENT "Compiling native Camera2 and MediaCodec capture with Zig cc (${DS_ZIG_TARGET})"
    VERBATIM
)

add_custom_command(
    OUTPUT "${DS_MAIN_OBJECT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DS_ZIG_OBJECT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E env "ZIG_LIB_DIR=${DS_ZIG_LIB_DIR}"
            ${DS_ZIG_EXECUTABLE} cc
            -target ${DS_ZIG_TARGET}
            --sysroot=${CMAKE_SYSROOT}
            -I${CMAKE_SYSROOT}/usr/include
            -I${CMAKE_SYSROOT}/usr/include/${DS_ANDROID_INCLUDE}
            -I${CMAKE_CURRENT_SOURCE_DIR}
            -I${ANDROID_NDK}/sources/android/native_app_glue
            -std=c17 -O3 -fPIC -ffunction-sections -fdata-sections
            -DANDROID
            -c "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
            -o "${DS_MAIN_OBJECT}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/main.c" "${CMAKE_CURRENT_SOURCE_DIR}/engine.h"
    COMMENT "Compiling DeadSpeak C platform layer with Zig cc (${DS_ZIG_TARGET})"
    VERBATIM
)

add_custom_command(
    OUTPUT "${DS_GLUE_OBJECT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DS_ZIG_OBJECT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E env "ZIG_LIB_DIR=${DS_ZIG_LIB_DIR}"
            ${DS_ZIG_EXECUTABLE} cc
            -target ${DS_ZIG_TARGET}
            --sysroot=${CMAKE_SYSROOT}
            -I${CMAKE_SYSROOT}/usr/include
            -I${CMAKE_SYSROOT}/usr/include/${DS_ANDROID_INCLUDE}
            -I${ANDROID_NDK}/sources/android/native_app_glue
            -std=c17 -O3 -fPIC -ffunction-sections -fdata-sections
            -DANDROID
            -c "${DS_NATIVE_GLUE}"
            -o "${DS_GLUE_OBJECT}"
    DEPENDS "${DS_NATIVE_GLUE}"
    COMMENT "Compiling Android native glue with Zig cc (${DS_ZIG_TARGET})"
    VERBATIM
)

add_custom_target(ds_c_objects DEPENDS "${DS_MAIN_OBJECT}" "${DS_GLUE_OBJECT}" "${DS_VIDEO_OBJECT}" "${DS_EXPLORE_OBJECT}")
set_source_files_properties("${DS_MAIN_OBJECT}" "${DS_GLUE_OBJECT}" "${DS_VIDEO_OBJECT}" "${DS_EXPLORE_OBJECT}" PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)

add_library(deadspeak SHARED
    "${DS_MAIN_OBJECT}"
    "${DS_GLUE_OBJECT}"
    "${DS_VIDEO_OBJECT}"
    "${DS_EXPLORE_OBJECT}"
)
set_target_properties(deadspeak PROPERTIES LINKER_LANGUAGE C)
target_include_directories(deadspeak PRIVATE
    "${ANDROID_NDK}/sources/android/native_app_glue"
)
target_compile_options(deadspeak PRIVATE -ffunction-sections -fdata-sections)
target_link_options(deadspeak PRIVATE -Wl,--gc-sections)
target_link_libraries(deadspeak dsengine android log EGL GLESv2 aaudio camera2ndk mediandk m)
add_dependencies(deadspeak ds_c_objects dsengine_build)
