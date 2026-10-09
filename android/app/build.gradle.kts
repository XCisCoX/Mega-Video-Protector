plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "org.megavideoprotect.app"
    compileSdk = 36
    // The SDK image ships no build-tools; the app build script provides a
    // 35.0.0 shim (zipalign/apksigner via apt + source.properties). AGP 8.13
    // refuses anything below its minimum build-tools revision (35.0.0).
    buildToolsVersion = "35.0.0"

    // The SDK image ships NDK 30.0.14904198; AGP's default ndkVersion (27.x)
    // is not installed and cannot be downloaded on this network.
    ndkVersion = "30.0.14904198"

    defaultConfig {
        applicationId = "org.megavideoprotect.app"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "1.0.0"
        ndk {
            abiFilters += listOf("arm64-v8a")
        }
        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++20")
                arguments += listOf(
                    "-DANDROID_STL=c++_static",
                    "-DMVP_ANDROID_PREFIX=${System.getenv("MVP_ANDROID_PREFIX") ?: "/repo/out/android-arm64/prefix"}"
                )
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    buildFeatures {
        compose = true
    }
    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
    }
}

dependencies {
    implementation(platform("androidx.compose:compose-bom:2025.05.00"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.foundation:foundation")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-core")
    implementation("androidx.activity:activity-compose:1.10.1")
    implementation("androidx.core:core-ktx:1.16.0")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.9.0")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.9.0")
    // Real playback (hardware-decoded video + audio) straight from the vault:
    // ExoPlayer reads the decrypted stream through VaultDataSource (JNI).
    implementation("androidx.media3:media3-exoplayer:1.6.1")
    implementation("androidx.media3:media3-ui:1.6.1")
    // Login-screen scanner for the PC share code.
    implementation("com.journeyapps:zxing-android-embedded:4.3.0")
}
