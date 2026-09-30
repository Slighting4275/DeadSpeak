import java.util.Properties

plugins {
    id("org.deadspeak.android-application")
}

val localBuildProperties = Properties().apply {
    rootProject.file("local.properties").takeIf { it.isFile }?.inputStream()?.use { load(it) }
}

android {
    namespace = "org.deadspeak"
    // This module is native C/Zig; do not add Kotlin tasks or its runtime library.
    enableKotlin = false
    compileSdk {
        version = release(37) {
            minorApiLevel = 2
        }
    }

    defaultConfig {
        applicationId = "org.deadspeak"
        minSdk = 30
        targetSdk = 37
        versionCode = 1
        versionName = "0.1.0"

        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                cFlags += listOf("-std=c17", "-O3", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Werror=deprecated-declarations")
                val zigLibDir = providers.gradleProperty("deadspeak.zigLibDir")
                    .orElse(providers.environmentVariable("ZIG_LIB_DIR")).orNull
                    ?: localBuildProperties.getProperty("deadspeak.zigLibDir")
                if (zigLibDir != null) arguments += "-DDS_ZIG_LIB_DIR=$zigLibDir"
            }
        }
    }

    ndkVersion = "30.0.16248370"
    buildToolsVersion = "37.0.0"

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "4.1.2"
        }
    }
}
