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

        // Thin client onto a remote pinback-server. Default URL is resolved at
        // runtime (emulator → 10.0.2.2:8088; physical device → setup screen).
        // Override with PINBACK_URL env or a non-empty buildConfigField for CI.
        buildConfigField("String", "PINBACK_URL", "\"\"")
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

// Single source: platform/common/ (see platform/CONTRACT.md).
tasks.register<Copy>("syncHostAssets") {
    from("../../common") {
        include("setup.html", "pinback-host.js")
    }
    into(layout.projectDirectory.dir("src/main/assets"))
}
tasks.named("preBuild") { dependsOn("syncHostAssets") }

dependencies {
    implementation(libs.androidx.activity)
}
