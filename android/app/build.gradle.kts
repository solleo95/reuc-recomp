plugins {
    id("com.android.application")
}

// The game's libraries (libmain.so with libc++ inside, libSDL3.so) and SDL's Java come from the CMake build in build-android, which
// scripts/android/build.sh makes first; nothing of the game itself is in the APK (it reads /sdcard/UmbrellaChronicles, see LauncherActivity).
val nativeBuild: String = providers.gradleProperty("ucNativeBuild").getOrElse(rootDir.resolve("../build-android").path)

android {
    namespace = "org.reuc_recomp.umbrella_chronicles"
    compileSdk = 36
    ndkVersion = "27.1.12297006"   // the NDK the libraries were built with: Gradle strips their debug information with it

    defaultConfig {
        applicationId = "org.reuc_recomp.umbrella_chronicles"
        minSdk = 33   // Android 13, the AYN Thor's
        targetSdk = 33
        versionCode = 1
        versionName = providers.gradleProperty("ucVersion").getOrElse("development")
        ndk { abiFilters += "arm64-v8a" }
    }
    sourceSets["main"].java.srcDir("$nativeBuild/_deps/sdl3-src/android-project/app/src/main/java")
    sourceSets["main"].jniLibs.srcDir("$nativeBuild/android-jniLibs")
    packaging { jniLibs { useLegacyPackaging = true } }   // libraries unpacked into nativeLibraryDir: libadrenotools loads its hooks from there
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
