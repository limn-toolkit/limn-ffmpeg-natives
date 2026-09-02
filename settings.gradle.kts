rootProject.name = "limn-ffmpeg-natives"

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        mavenCentral()
    }
}

// One project, one artifact, seven jars: the shim for every desktop platform in the main jar and
// the FFmpeg libraries in one natives-<os>-<arch> classifier each. It is the payload that
// limn-toolkit's limn-video-ffmpeg module used to build on release day and ship under its own
// version, moved here so that it versions with FFMPEG (ADR 037 there): a toolkit release now
// re-uploads no native byte, and an application's cache keeps the ~2 MB slice for its machine
// across every Limn upgrade that names the same payload version.
