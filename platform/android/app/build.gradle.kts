import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
}

android {
    namespace = "com.pinback.shell"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.pinback.shell"
        minSdk = 24
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"

        // Android cannot host pinback-server (no ds4-agent / 87GB model on the
        // phone): the app is a thin client onto a REMOTE pinback. The emulator
        // reaches a pinback-server on the host machine's loopback via 10.0.2.2;
        // for a physical device override PINBACK_URL with your LAN/Tailscale host.
        buildConfigField("String", "PINBACK_URL", "\"http://10.0.2.2:8088\"")
    }

    buildFeatures {
        buildConfig = true
    }

    buildTypes {
        release {
            // R8 full mode (AGP default) + resource shrinking: strips unused dex
            // and resources. Shrinking the kotlin-stdlib/androidx graph is the
            // single biggest APK-size win short of dropping AndroidX entirely.
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
    }
}

dependencies {
    implementation(libs.androidx.activity)
}
