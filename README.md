# limn-ffmpeg-natives

The FFmpeg payload behind the [Limn toolkit](https://github.com/limn-toolkit/limn-toolkit)'s
`limn-video-ffmpeg` decoder, **versioned with FFmpeg rather than with the toolkit**: a Limn
release re-uploads no native byte, and an application's dependency cache keeps the ~2 MB slice
for its machine across every toolkit upgrade that names the same payload version.

One artifact, `io.github.limn-toolkit:limn-ffmpeg-natives`, seven jars:

| Jar | Carries | Who names it |
| --- | --- | --- |
| the main jar | `liblimnffmpeg`, the JNI shim, for all six platforms; the FFmpeg licence and notice | `limn-video-ffmpeg` depends on it — nothing to add |
| `natives-<os>-<arch>` ×6 | that platform's FFmpeg libraries (`avutil`, `swresample`, `avcodec`, `avformat`) and the `libraries.txt` the loader reads | the application: one for its machine, or the toolkit's `limn-video-ffmpeg-natives-all` POM for all six |

```kotlin
dependencies {
    implementation("io.github.limn-toolkit:limn-video-ffmpeg:<toolkit version>")
    runtimeOnly("io.github.limn-toolkit:limn-ffmpeg-natives:7.1.5.0:natives-macos-aarch64")
}
```

Targets: `linux-x86_64`, `linux-aarch64`, `macos-x86_64`, `macos-aarch64`, `windows-x86_64`,
`windows-aarch64`. Leave the classifier out and the toolkit still runs; the decoder reports
itself unavailable, naming the platform it looked for.

## What is inside, and what is not

FFmpeg **7.1.5**, configured with `--disable-everything` and switched back on for exactly what
the toolkit opens: H.264, HEVC, VP9 and VP8; AAC, Opus and Vorbis; text subtitles; the MP4 and
Matroska/WebM demuxers; the `file:` protocol and no network. No encoder, no GPL component,
no `--enable-nonfree`. Built **shared**, linked dynamically by the shim, and replaceable file for
file — which is how the LGPL-2.1 §6 relink freedom is satisfied. Hardware decode on macOS
through VideoToolbox. Every decision is argued in [`scripts/build-ffmpeg.sh`](scripts/build-ffmpeg.sh).

The shim, [`src/main/c/limn_ffmpeg.c`](src/main/c/limn_ffmpeg.c), is Limn's own code (Apache
2.0): a player handle, not a binding, and the other half of `limn.video.ffmpeg.FfmpegNative` in
the toolkit. The two are bound by name and release apart; the shim exports an ABI number that
the toolkit checks at load time, so a mismatch is a sentence rather than a link error mid-decode.

**No binary is in this repository**, ever. The whole `native/` tree is gitignored; a release
builds all six slices on its own runners and the published jars are the only place the
libraries exist.

## Building one locally

```
./scripts/build-ffmpeg.sh                 # your platform's player payload, about a minute
./scripts/build-ffmpeg.sh --profile full  # + encoders and the mov muxer the toolkit's writer tests need
./gradlew assemble                        # packages whatever native/dist/ holds
./scripts/rehearse-consumer.sh            # publishes to build/repo and resolves it as a consumer would
```

Needs a C compiler and a JDK (for `jni.h`); Windows builds under MSYS2 (CLANG64 / CLANGARM64).
The toolkit's `limn-video-ffmpeg` module picks up a `full` build from a sibling clone of this
repository automatically, which is how its writer tests get an encoder nothing published carries.

## Releasing

`versions.properties` holds the one version; its first three components are the FFmpeg the
script pins and the fourth is this repository's own. Bump it, push `main`, and the
`tag-releases` workflow tags, builds six slices, **rehearses a consumer**, uploads, and drafts —
see [RELEASING.md](RELEASING.md).
