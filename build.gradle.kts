// limn-ffmpeg-natives: the FFmpeg payload the Limn toolkit's decoder loads, and nothing else.
//
// NOTHING HERE IS BUILT BY GRADLE. The libraries come from scripts/build-ffmpeg.sh, which is how a
// release builds them on six machines (.github/workflows/natives.yml) and how a developer builds
// one locally; Gradle never invokes a C compiler. What this file does is PACKAGE:
//
//   the main jar          the JNI shim (liblimnffmpeg) for every platform, plus the FFmpeg licence
//                         and notice. Small, and the one jar limn-video-ffmpeg depends on.
//   natives-<os>-<arch>   one classifier jar per desktop target: that platform's FFmpeg libraries
//                         and the libraries.txt manifest the loader reads. ~2 MB each; an
//                         application names the one for its machine, or the toolkit's
//                         limn-video-ffmpeg-natives-all POM names all six.
//
// The split is invisible to the loader: FfmpegLibrary resolves everything under
// limn/video/ffmpeg/native/<platform>/ as a CLASSPATH resource, and a classpath spans jars.
// libraries.txt rides with the FFmpeg libraries rather than with the shim, on purpose: it is the
// file the loader looks for first, so an application that forgot its classifier reads "this build
// carries no FFmpeg native for <platform>" instead of a link error naming a file it never heard of.
//
// NO native is committed, and none ever will be. .gitignore excludes the whole native/ tree.

import com.vanniktech.maven.publish.MavenPublishBaseExtension
import java.util.Properties

plugins {
    `java-library`
    alias(libs.plugins.central.publish)
}

group = "io.github.limn-toolkit"

// The version, from -PlimnNativesVersion (the publish workflow passes it, taken from the tag) and
// otherwise the -SNAPSHOT of versions.properties. Its first three components must be the FFmpeg
// version scripts/build-ffmpeg.sh pins: the number names what is inside, and a version that says
// 7.1.5 over a 7.1.6 payload is a lie the build can catch, so it does.
val declaredVersion = Properties().apply {
    file("versions.properties").inputStream().use { load(it) }
}.getProperty("limn-ffmpeg-natives") ?: throw GradleException("versions.properties names no version")
version = (findProperty("limnNativesVersion") as String?) ?: "$declaredVersion-SNAPSHOT"

val pinnedFfmpeg = Regex("""^FFMPEG_VERSION="([^"]+)"""", RegexOption.MULTILINE)
    .find(file("scripts/build-ffmpeg.sh").readText())?.groupValues?.get(1)
    ?: throw GradleException("scripts/build-ffmpeg.sh no longer pins FFMPEG_VERSION where this build reads it")
if (!version.toString().startsWith("$pinnedFfmpeg.") && version.toString() != pinnedFfmpeg) {
    throw GradleException(
        "version $version does not name the FFmpeg it carries: scripts/build-ffmpeg.sh pins " +
                "$pinnedFfmpeg, so the version must be $pinnedFfmpeg.<n> (see versions.properties)"
    )
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(21))
    }
    withSourcesJar()
}

// A jar of native libraries runs on ANY Java the shim was written for, and this is what says so
// where consumers listen: without it the published module metadata takes its minimum-JVM
// attribute from the toolchain above and refuses every consumer building for an older Java —
// the lesson limn-fonts' first four releases paid for. The shim is compiled against jni.h, whose
// surface has not changed in a way this code notices since Java 8.
tasks.withType<JavaCompile>().configureEach {
    options.release.set(8)
}

// Which build of the native to package. A release picks `player` explicitly and the guard below
// makes that a rule; a developer who built `full` (encoders and a muxer, for the toolkit's writer
// tests) gets it for local work and can never publish it by accident.
val ffmpegProfile: String = (findProperty("limnFfmpegProfile") as String?)
    ?: if (file("native/dist/full").isDirectory) "full" else "player"
val ffmpegNatives = file("native/dist/$ffmpegProfile")

sourceSets {
    named("main") {
        // Absent on a machine that never ran the build script, which Gradle accepts: a resource
        // directory that does not exist contributes nothing rather than failing.
        resources.srcDir(ffmpegNatives)
    }
}

/**
 * The desktop targets a published payload must cover, written out rather than discovered: a
 * check that trusts what it finds calls an empty directory complete, and a slice whose build
 * failed would then publish as a decoder that plays no video on that platform and says nothing.
 */
val requiredNativePlatforms = listOf(
    "linux-aarch64", "linux-x86_64",
    "macos-aarch64", "macos-x86_64",
    "windows-aarch64", "windows-x86_64",
)

val shimName = "liblimnffmpeg."
val ffmpegLicences = listOf("LICENSE-ffmpeg.txt", "NOTICE-ffmpeg.txt")

// The main jar: the shim for every platform and the two licence files, nothing else.
tasks.named<ProcessResources>("processResources") {
    exclude {
        it.path.startsWith("limn/video/ffmpeg/native/")
                && !it.isDirectory
                && !it.name.startsWith(shimName)
                && it.name !in ffmpegLicences
    }
    doFirst {
        if (ffmpegNatives.isDirectory) {
            logger.lifecycle("limn-ffmpeg-natives: packaging the '$ffmpegProfile' payload from $ffmpegNatives")
        } else {
            logger.lifecycle("limn-ffmpeg-natives: no payload built (run scripts/build-ffmpeg.sh); the jars will be empty")
        }
    }
}

/** One jar per platform: that platform's FFmpeg libraries, its manifest, and the licences. */
val nativeJars = requiredNativePlatforms.map { platform ->
    val suffix = platform.split("-").joinToString("") { part -> part.replaceFirstChar(Char::uppercase) }
    tasks.register<Jar>("nativesJar$suffix") {
        description = "The FFmpeg libraries for $platform, published as natives-$platform."
        group = "build"
        archiveClassifier.set("natives-$platform")
        from(ffmpegNatives) {
            include("limn/video/ffmpeg/native/$platform/**")
            exclude("**/$shimName*")
        }
        from(ffmpegNatives) {
            ffmpegLicences.forEach { include("limn/video/ffmpeg/native/$it") }
        }
    }
}

tasks.named("assemble") {
    dependsOn(nativeJars)
}

/**
 * Which platforms the staged payload really carries: a directory counts only when its manifest
 * AND at least one library that is not the shim are present, which is exactly what a classifier
 * jar is made of.
 */
fun carriedPlatforms(): Set<String> {
    val root = ffmpegNatives.resolve("limn/video/ffmpeg/native")
    return (root.listFiles() ?: emptyArray())
        .filter { platform ->
            if (!platform.isDirectory) return@filter false
            val files = platform.listFiles() ?: emptyArray()
            files.any { it.name == "libraries.txt" }
                    && files.any { it.isFile && !it.name.startsWith(shimName) && it.name != "libraries.txt" }
        }
        .map { it.name }
        .toSet()
}

// A release ships `player` and ships all six, and this is what turns both sentences into rules.
tasks.withType<AbstractPublishToMaven>().configureEach {
    // The local file repository is where a partial payload is LOOKED AT (a developer's own slice,
    // CI's single-slice rehearsal); the guard is for anything that leaves the machine.
    if (name.contains("BuildDir")) return@configureEach
    doFirst {
        if (ffmpegProfile != "player") {
            throw GradleException(
                "refusing to publish limn-ffmpeg-natives with the '$ffmpegProfile' payload: a " +
                        "published artifact carries the 'player' build. Re-run with " +
                        "-PlimnFfmpegProfile=player."
            )
        }
        val missing = requiredNativePlatforms.filterNot { carriedPlatforms().contains(it) }
        if (missing.isNotEmpty()) {
            throw GradleException(
                "refusing to publish limn-ffmpeg-natives without its whole payload: " +
                        "${missing.joinToString(", ")} " +
                        (if (missing.size == 1) "is" else "are") + " missing from " +
                        "${ffmpegNatives.relativeTo(rootDir)}. A release builds all six on its own " +
                        "runners (.github/workflows/natives.yml) and merges them in before this " +
                        "task runs; a machine that ran scripts/build-ffmpeg.sh has only its own."
            )
        }
    }
}

// The payload reaches the main source set through resources.srcDir above, which is what a
// sources jar copies as well: this jar answers "what was this compiled from", and the answer is
// one C file, which is exactly what is left in it.
tasks.named<Jar>("sourcesJar") {
    exclude("limn/video/ffmpeg/native/**")
    from("src/main/c")
}

mavenPublishing {
    // Uploads and stops: the deployment sits staged on the Central Portal until somebody presses
    // Publish, which is the last moment a release can still be dropped.
    publishToMavenCentral()

    if (providers.gradleProperty("signingInMemoryKey").isPresent ||
            providers.gradleProperty("signing.keyId").isPresent) {
        signAllPublications()
    }

    pom {
        name.set("limn-ffmpeg-natives")
        description.set(
            "The FFmpeg payload behind the Limn toolkit's limn-video-ffmpeg decoder: the JNI " +
                    "shim for macOS, Linux and Windows on x86_64 and aarch64 in this jar, and " +
                    "FFmpeg $pinnedFfmpeg (H.264/HEVC/VP9/VP8, AAC/Opus/Vorbis, MP4 and Matroska, " +
                    "LGPL-2.1-or-later, dynamically linked and replaceable) in one " +
                    "natives-<os>-<arch> classifier per platform. Versions with FFmpeg, not with " +
                    "the toolkit."
        )
        url.set("https://github.com/limn-toolkit/limn-ffmpeg-natives")
        scm {
            url.set("https://github.com/limn-toolkit/limn-ffmpeg-natives")
            connection.set("scm:git:https://github.com/limn-toolkit/limn-ffmpeg-natives.git")
            developerConnection.set("scm:git:ssh://git@github.com/limn-toolkit/limn-ffmpeg-natives.git")
        }
        // Both, because the artifact is both: the shim in the main jar is Limn's own code under
        // Apache 2.0; the libraries in the classifier jars are FFmpeg under the LGPL, whose text
        // and notice travel in every jar.
        licenses {
            license {
                name.set("The Apache License, Version 2.0")
                url.set("https://www.apache.org/licenses/LICENSE-2.0.txt")
                comments.set("liblimnffmpeg, the JNI shim in the main jar")
            }
            license {
                name.set("GNU Lesser General Public License, version 2.1 or later")
                url.set("https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt")
                comments.set("The FFmpeg libraries in the natives-<os>-<arch> classifier jars; see NOTICE-ffmpeg.txt in each")
            }
        }
        developers {
            developer {
                id.set("dyorgio")
                name.set("Dyorgio Nascimento")
                url.set("https://github.com/dyorgio")
            }
        }
    }
}

// The classifier jars join the one publication the plugin configured.
publishing {
    publications.withType<MavenPublication>().configureEach {
        nativeJars.forEach { artifact(it) }
    }
    // A plain file repository under build/repo, for looking at what would ship without sending
    // it anywhere — and for scripts/rehearse-consumer.sh to resolve from.
    repositories {
        maven {
            name = "buildDir"
            url = uri(layout.buildDirectory.dir("repo"))
        }
    }
}

// A release that Central would reject on validation (unsigned), caught before anything leaves.
gradle.taskGraph.whenReady {
    val releasing = allTasks.any {
        it.name == "publishToMavenCentral" || it.name == "publishAndReleaseToMavenCentral" ||
                it.name.startsWith("publishAllPublicationsToMavenCentral")
    }
    if (!releasing) return@whenReady
    if (project.version.toString().endsWith("-SNAPSHOT")) {
        logger.lifecycle("publishing ${project.version} to Central's SNAPSHOT repository. This is " +
                "not a release: bump versions.properties and push for one (see RELEASING.md).")
        return@whenReady
    }
    if (!providers.gradleProperty("signingInMemoryKey").isPresent &&
            !providers.gradleProperty("signing.keyId").isPresent) {
        throw GradleException(
            "refusing to publish ${project.version} to Maven Central unsigned: no signing key is " +
                    "configured, and Central requires a signature on every artifact of a release."
        )
    }
}
