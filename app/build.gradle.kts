plugins {
    id("com.android.application")
}

android {
    namespace = "com.custom.unitycamera"
    compileSdk = 35
    ndkVersion = "28.2.13676358"

    defaultConfig {
        applicationId = "com.custom.unitycamera"
        minSdk = 28
        targetSdk = 35
        versionCode = 1
        versionName = "1.0"
        ndk { abiFilters += setOf("arm64-v8a") }
        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++17 -O2 -Wall"
                arguments += listOf("-DANDROID_STL=c++_shared")
            }
        }
    }

    buildTypes {
        getByName("debug") { isMinifyEnabled = false }
        getByName("release") {
            isMinifyEnabled = true
            isShrinkResources = true
            isDebuggable = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            signingConfig = signingConfigs.getByName("debug")
            ndk { debugSymbolLevel = "NONE" }
            externalNativeBuild {
                cmake {
                    cppFlags += "-fvisibility=hidden -fvisibility-inlines-hidden"
                }
            }
        }
    }

    externalNativeBuild {
        cmake { path = file("src/main/cpp/CMakeLists.txt") }
    }

    packaging {
        jniLibs { useLegacyPackaging = true }
    }
}

dependencies {
    implementation("androidx.core:core:1.13.1")
}
