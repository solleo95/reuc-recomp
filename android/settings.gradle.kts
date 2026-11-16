// The Android app of the port: it only packages what scripts/android/build.sh builds (the game's libraries, SDL's Java). See android/app/build.gradle.kts.
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}
dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
    }
}
rootProject.name = "UmbrellaChronicles"
include(":app")
