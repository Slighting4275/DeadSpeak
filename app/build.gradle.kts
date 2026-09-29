plugins {
    id("com.android.application")
}

android {
    namespace = "org.deadspeak"
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
                cFlags += listOf("-std=c17", "-O3", "-Wall", "-Wextra", "-Wno-unused-parameter")
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
