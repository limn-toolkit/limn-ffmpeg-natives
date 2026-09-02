#!/usr/bin/env bash
#
# Builds the trimmed FFmpeg that limn-video-ffmpeg loads, and the JNI shim in front of it. This
# repository IS that payload: what limn-toolkit used to build on release day now versions and
# releases here, with FFmpeg (ADR 037 there).
#
# NOTHING THIS PRODUCES IS COMMITTED. The whole native/ tree is gitignored: a binary in a source
# repository is a binary nobody can review, and the published jar is the only place these
# libraries are meant to exist. A release builds its own on six machines
# (.github/workflows/natives.yml); this script is how a DEVELOPER gets one locally. A developer
# who wants playback and no C toolchain needs nothing from here at all: the toolkit's own build
# resolves the published payload from Maven Central like any other dependency.
#
# A machine with neither still builds and tests the whole repository; the decoder reports itself
# unavailable and its tests skip.
#
#   ./scripts/build-ffmpeg.sh                  # the shipped decode-only library, for this JVM's arch
#   ./scripts/build-ffmpeg.sh --profile full   # + encoders and the mov muxer, for the tests and the demo
#   ./scripts/build-ffmpeg.sh --arch arm64,x86_64   # macOS universal
#   ./scripts/build-ffmpeg.sh --clean          # discard the unpacked source and every output
#
# WHY TWO PROFILES
#
#   `player` is what ships: it decodes the codecs listed below out of MP4 and Matroska/WebM and can
#   do nothing else. It holds no encoder, because an encoder is not merely bytes: MPEG-4 Visual
#   and AVC are licensed separately for encode and for decode, so shipping one buys patent surface
#   for a capability a player does not use.
#
#   `full` adds the mpeg4 and aac encoders, the mpeg4 decoder and the mov muxer, so a test can
#   write a real MP4 and read it back. No GENERATED media is committed, so the honest way to test
#   the writer is to run it; the committed corpus in media/ is the other half, the codecs nothing
#   here can encode (docs/adr/027). Both profiles compile the SAME shim from the
#   SAME source: the writer entry point is always present and asks libavcodec at run time whether
#   an encoder exists, so `player` differs from `full` by which codecs are linked in and by
#   nothing else.
#
# WHICH CODECS, AND WHY AV1 IS NOT ONE OF THEM
#
#   Video: h264, hevc, vp9, vp8.  Audio: aac, opus, vorbis.  Containers: mov (mp4/m4v/3gp) and
#   matroska (mkv/webm).  Each of those was measured on its own before it was added, because a
#   codec nobody can weigh is a codec nobody can refuse. See docs/adr/015.
#
#   AV1 IS ABSENT AND IT IS NOT AN OVERSIGHT. FFmpeg's own "av1" decoder is a hardware-accelerator
#   wrapper with no software path at all: libavcodec/av1dec.c refuses outright when no hwaccel is
#   attached, and --disable-everything switches every hwaccel off. Software AV1 needs libdav1d,
#   which is an EXTERNAL library: another pinned tarball, a meson/ninja toolchain, a cross build
#   per architecture and its own payload. That is a phase, not a flag, so adding --enable-decoder=av1
#   here would produce a build that claims AV1 and fails on the first file.
#
# LICENCE
#
#   --enable-gpl is absent and must stay absent. FFmpeg is LGPL-2.1-or-later until a GPL
#   component is switched on, and every GPL component (libpostproc, three x86 optimisation files,
#   a list of filters, the build/test tools) is off here. The libraries are built SHARED and the
#   shim links them dynamically, which is what FFmpeg's own compliance guidance asks for and what
#   keeps the relink obligation off anything downstream. See docs/adr/011.
#
#   Do not add --enable-gpl, --enable-nonfree, --enable-libx264 or --enable-libx265 to make
#   something work. LicenceTest asserts the built library still reports LGPL and still refuses
#   every protocol but file:, and it fails the build if this line drifts.

set -euo pipefail

# The size report computes with awk and formats with printf, and those two disagree about the
# decimal separator the moment the machine is not English: awk emits 4.01855, printf under a
# pt-BR locale wants a comma, and rejects the number, which fails the whole build over a line
# of output nobody depends on. Pinning the locale also keeps sort order and tool messages the
# same everywhere, which is what a build script wants anyway.
export LC_ALL=C

# ---------------------------------------------------------------------------- the pin

FFMPEG_VERSION="7.1.5"

# Recorded when this version was first fetched from ffmpeg.org, and checked on every fetch since.
# What it establishes is that every machine builds the same bytes; what it does NOT establish is
# that those bytes are upstream's, because ffmpeg.org publishes no checksum beside the tarball.
# To settle provenance independently, verify the release's GPG signature against the FFmpeg
# signing key (https://ffmpeg.org/download.html) and update this line from a verified copy.
FFMPEG_SHA256="de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK_DIR="${REPO_ROOT}/native"
SRC_DIR="${WORK_DIR}/src/ffmpeg-${FFMPEG_VERSION}"
SHIM_SRC="${REPO_ROOT}/src/main/c/limn_ffmpeg.c"

PROFILE="player"
ARCHS=""
CLEAN=0
SHIM_ONLY=0
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

while [ $# -gt 0 ]; do
    case "$1" in
        --profile) PROFILE="$2"; shift 2 ;;
        --arch)    ARCHS="$2"; shift 2 ;;
        --jobs)    JOBS="$2"; shift 2 ;;
        --clean)   CLEAN=1; shift ;;
        # Recompiles limn_ffmpeg.c against an FFmpeg that is already built, which is a second
        # rather than a minute. Only valid after a full run has produced that FFmpeg.
        --shim-only) SHIM_ONLY=1; shift ;;
        -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

if [ "$PROFILE" != "player" ] && [ "$PROFILE" != "full" ]; then
    echo "--profile must be 'player' or 'full', got '${PROFILE}'" >&2
    exit 2
fi

if [ "$CLEAN" = "1" ]; then
    echo "removing ${WORK_DIR}"
    rm -rf "${WORK_DIR}"
    exit 0
fi

# ---------------------------------------------------------------------------- platform

# WINDOWS NEEDS A POSIX SHELL, and that is not a preference. FFmpeg's configure is a shell script
# that runs hundreds of compile probes; there is no MSVC project to open. So the Windows build runs
# under MSYS2 with a mingw-w64 toolchain, from an MSYS2 shell, and `uname -s` there says MINGW64,
# CLANGARM64, MSYS or similar depending on which environment was started.
HOST_OS="$(uname -s)"
case "${HOST_OS}" in
    Darwin) LIMN_OS="macos"; LIB_EXT="dylib"; LIB_SUBDIR="lib" ;;
    Linux)  LIMN_OS="linux"; LIB_EXT="so";    LIB_SUBDIR="lib" ;;
    # mingw installs import libraries (.dll.a) under lib/ and the DLLs themselves under bin/,
    # which is why the output directory is a variable rather than the word "lib" repeated.
    MINGW*|MSYS*|CLANGARM64*|CYGWIN*|UCRT*)
            LIMN_OS="windows"; LIB_EXT="dll"; LIB_SUBDIR="bin" ;;
    *) echo "unsupported host '${HOST_OS}': this builds on macOS, Linux, and Windows under MSYS2" >&2
       exit 2 ;;
esac

# The shim is one C file and needs any C compiler; FFmpeg's own configure finds its own. `cc` is
# always there on macOS and usually on Linux, and is the one name an MSYS2 environment may not
# install, so ask for it, then settle.
SHIM_CC="${CC:-}"
if [ -z "${SHIM_CC}" ]; then
    for candidate in cc gcc clang; do
        if command -v "${candidate}" >/dev/null 2>&1; then SHIM_CC="${candidate}"; break; fi
    done
fi
if [ -z "${SHIM_CC}" ]; then
    echo "no C compiler on PATH (looked for cc, gcc, clang); set CC" >&2
    exit 2
fi

# The loader builds its directory name from Java's os.arch, normalised, and Java says aarch64
# where the C toolchain says arm64. One spelling has to win and it is Java's, because the loader
# is what has to find the directory at run time. FFmpeg's configure happens to want the same
# spelling, which is why this is defined up here rather than beside the staging that names
# directories with it.
arch_label() {
    case "$1" in
        arm64|aarch64) echo "aarch64" ;;
        x86_64|amd64)  echo "x86_64" ;;
        *) echo "$1" ;;
    esac
}

# WHICH ARCHITECTURE, and why macOS gets both by default.
#
# The library has to be loadable by the JVM that runs it, and os.arch is the JVM's, not the
# machine's. On an Apple Silicon Mac those differ all the time and having several JDKs of DIFFERENT
# architectures installed at once is the normal state, not an odd setup: an x86_64 Zulu under
# Rosetta on the PATH and an arm64 JDK bundled inside an IDE is an ordinary Tuesday. Building for
# whichever JVM happens to be on the PATH then produces a library that the IDE's JVM refuses, the
# decoder reports itself unavailable, and nothing anywhere says the word "architecture", which is
# exactly the afternoon this comment exists to prevent, because it cost one.
#
# So macOS builds BOTH slices and lipos them into one universal library per file. It costs a second
# configure and make; it removes a whole class of "it works here and not there". Pass --arch to
# build just one.
if [ -z "${ARCHS}" ]; then
    if [ "${LIMN_OS}" = "macos" ]; then
        ARCHS="arm64,x86_64"
        echo "targeting a universal build (arm64 + x86_64): any JDK on this Mac can load it"
    else
        JAVA_BIN="java"
        if [ -n "${JAVA_HOME:-}" ]; then JAVA_BIN="${JAVA_HOME}/bin/java"; fi
        # JAVA_HOME is not normalised yet at this point, and on Windows it is a native path that
        # no shell can execute. Falling back to the PATH keeps the arch probe working there
        # instead of failing with a message about a directory that plainly exists.
        command -v "${JAVA_BIN}" >/dev/null 2>&1 || JAVA_BIN="java"
        # Said here, because a JVM that is simply absent used to end this script with status 127
        # and not one word of explanation: the probe below runs inside a command substitution,
        # where "command not found" belongs to the subshell and `set -e` takes the rest. The
        # jni.h check further down has the sentence a reader needs; it was never reached.
        if ! command -v "${JAVA_BIN}" >/dev/null 2>&1; then
            echo "no JVM on PATH and no JAVA_HOME: this builds a JNI shim, so it needs a JDK" >&2
            echo "  (a JRE will not do either — jni.h ships with the JDK)" >&2
            exit 2
        fi
        JVM_ARCH="$("${JAVA_BIN}" -XshowSettings:properties -version 2>&1 \
            | sed -n 's/.*os\.arch = \(.*\)/\1/p' | tr -d ' ')"
        case "${JVM_ARCH}" in
            x86_64|amd64) ARCHS="x86_64" ;;
            aarch64|arm64) ARCHS="arm64" ;;
            *) echo "cannot map JVM os.arch='${JVM_ARCH}' to a build architecture" >&2; exit 2 ;;
        esac
        echo "targeting ${ARCHS} (the JVM at ${JAVA_BIN} reports os.arch=${JVM_ARCH})"
    fi
fi

if [ -z "${JAVA_HOME:-}" ]; then
    if [ "${LIMN_OS}" = "macos" ]; then
        JAVA_HOME="$(/usr/libexec/java_home -v 17+ 2>/dev/null || /usr/libexec/java_home)"
    else
        JAVA_HOME="$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")"
    fi
elif [ "${LIMN_OS}" = "windows" ]; then
    # A Windows JDK sets JAVA_HOME to a native path (C:\Program Files\...), and every use of it
    # below is a shell path passed to a shell. Convert once, here, rather than discovering it as a
    # "no such file" from the compiler four steps later.
    JAVA_HOME="$(cygpath -u "${JAVA_HOME}" 2>/dev/null || echo "${JAVA_HOME}")"
fi
if [ ! -f "${JAVA_HOME}/include/jni.h" ]; then
    echo "no jni.h under JAVA_HOME=${JAVA_HOME}; a JDK is needed, not a JRE" >&2
    exit 2
fi
case "${LIMN_OS}" in
    macos)   JNI_MD_DIR="${JAVA_HOME}/include/darwin" ;;
    windows) JNI_MD_DIR="${JAVA_HOME}/include/win32" ;;
    *)       JNI_MD_DIR="${JAVA_HOME}/include/linux" ;;
esac

# ---------------------------------------------------------------------------- the source

mkdir -p "${WORK_DIR}/src"
TARBALL="${WORK_DIR}/src/ffmpeg-${FFMPEG_VERSION}.tar.xz"

if [ ! -d "${SRC_DIR}" ]; then
    if [ ! -f "${TARBALL}" ]; then
        echo "fetching ffmpeg-${FFMPEG_VERSION}"
        curl -fsSL --retry 3 -o "${TARBALL}.part" \
            "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz"
        mv "${TARBALL}.part" "${TARBALL}"
    fi
    echo "verifying the pin"
    if command -v shasum >/dev/null 2>&1; then
        ACTUAL="$(shasum -a 256 "${TARBALL}" | cut -d' ' -f1)"
    else
        ACTUAL="$(sha256sum "${TARBALL}" | cut -d' ' -f1)"
    fi
    if [ "${ACTUAL}" != "${FFMPEG_SHA256}" ]; then
        echo "checksum mismatch for ${TARBALL}" >&2
        echo "  expected ${FFMPEG_SHA256}" >&2
        echo "  actual   ${ACTUAL}" >&2
        echo "Refusing to build. Delete the file to re-fetch, or update the pin deliberately." >&2
        exit 1
    fi
    tar -xf "${TARBALL}" -C "${WORK_DIR}/src"
fi

# ---------------------------------------------------------------------------- the configure line
#
# --disable-everything switches off every component; what follows switches back on exactly the
# ones this project opens, and the redundant --disable-* lines beside them are deliberate. They
# state the restriction in the file rather than leaving it to be inferred from a default:
#
#   --disable-protocols --enable-protocol=file   VideoDecoder.openStream takes a Path and promises
#                                                a file. A build with the network protocols on is
#                                                an SPI that lies about its own signature, because
#                                                libavformat would happily open an http:// URL
#                                                handed to it as a "path". --disable-network makes
#                                                it structural rather than a matter of which
#                                                protocols were listed.
#   --disable-demuxers --enable-demuxer=mov      MP4 and Matroska/WebM, and nothing else. One mov
#                      --enable-demuxer=matroska demuxer reads mov, mp4, m4v and 3gp alike; the
#                                                matroska one reads mkv and webm alike.
#   --enable-decoder=hevc/vp9/vp8                What a phone records and what the web serves.
#                    =opus/vorbis                The soundtracks a WebM carries; without them a
#                                                WebM this build can demultiplex plays silent.
#                    =movtext                    Subtitles, one per container: tx3g in MP4, and
#                    =subrip/ass/webvtt          the three a Matroska or WebM carries. All four
#                                                together measured at 896 BYTES (ADR 017 §5):
#                                                they are string manipulation over shared ASS
#                                                helpers, so they are three orders of magnitude
#                                                below every other row in ADR 015 §2. No bitmap
#                                                decoder is here and none is wanted: PGS and
#                                                VobSub are pictures, and this SPI carries text
#                                                cues only, so such a track is refused by name.
#   --disable-autodetect                         Nothing is picked up from the build machine, so
#                                                two machines produce the same library. Without it
#                                                a macOS build silently acquires VideoToolbox and
#                                                a Linux one acquires whatever -dev packages
#                                                happen to be installed.
#
# swscale and avfilter are absent because nothing needs them: planar YCbCr goes to the GPU as it
# comes out of the decoder (ADR 007), and the planar-float-to-interleaved-16-bit step the audio
# track needs is arithmetic the shim does itself.
#
# libswresample IS present, and not because the shim uses it; it still does that arithmetic
# itself. FFmpeg's native Opus decoder declares a dependency on swresample, so without it configure
# switches the Opus decoder off and says so only in a warning line of its log. Since most WebM
# audio is Opus, a Matroska demuxer without it would open the file and play it silent. Measured at
# 0.18 MB for the pair, which is the whole reason it is here rather than a principle.

configure_flags() {
    cat <<'FLAGS'
--disable-everything
--disable-autodetect
--disable-programs
--disable-doc
--disable-debug
--disable-static
--enable-shared
--enable-pic
--disable-avdevice
--disable-avfilter
--disable-swscale
--disable-postproc
--disable-network
--disable-protocols
--enable-protocol=file
--disable-demuxers
--enable-demuxer=mov
--enable-demuxer=matroska
--disable-muxers
--disable-decoders
--enable-decoder=h264
--enable-decoder=hevc
--enable-decoder=vp9
--enable-decoder=vp8
--enable-decoder=aac
--enable-decoder=opus
--enable-decoder=vorbis
--enable-decoder=movtext
--enable-decoder=subrip
--enable-decoder=ass
--enable-decoder=webvtt
--disable-encoders
--disable-parsers
--enable-parser=h264
--enable-parser=hevc
--enable-parser=aac
--disable-bsfs
--disable-filters
FLAGS
    # HARDWARE DECODE, and why three flags rather than one.
    #
    # --enable-videotoolbox switches on the FRAMEWORK: the device context, the pixel-buffer
    # plumbing, av_hwframe_transfer_data. It switches on NO ACCELERATOR: --disable-everything turned
    # every hwaccel off and only --enable-hwaccel=<name> turns one back on. A build with the
    # framework and no hwaccel opens a device, attaches it, decodes in software and reports itself
    # as hardware decoding, which is ADR 015 §2's trap ("a configure flag is a claim") for the
    # second time, and why CodecBreadthTest asserts the accelerators out of the linked library
    # rather than out of this list.
    #
    # macOS only, and it must stay that way: VideoToolbox is an Apple framework and the flags do not
    # configure on Linux. Windows and Linux hardware decode is phase 6b and a different primitive
    # (D3D11VA and VA-API), which is ADR 014 §0's whole point.
    if [ "${LIMN_OS}" = "macos" ]; then
        cat <<'FLAGS'
--enable-videotoolbox
--enable-hwaccel=h264_videotoolbox
--enable-hwaccel=hevc_videotoolbox
FLAGS
    fi
    if [ "${PROFILE}" = "full" ]; then
        # Everything below is LGPL: none of mpeg4, mjpeg, aac (FFmpeg's own encoder, not the
        # nonfree libfdk_aac), movtext or movenc appears in FFmpeg's list of GPL parts. The
        # movtext encoder is what lets a clip carry a subtitle track, and it is the same argument
        # as the rest of this block: no media is committed, so the only honest way to read a
        # subtitle track is to write one. The H.264 encoder
        # is x264 and IS GPL, which is why the round trip encodes MPEG-4 Part 2 rather than H.264,
        # and why the round trip proves the seam rather than proving libavcodec's H.264 decoder,
        # which is FFmpeg's to test and not this repository's.
        # mov and mp4 are two SEPARATE muxers in movenc.c, however much they look like one
        # feature: --enable-muxer=mov alone leaves av_guess_format("mp4") returning nothing, and
        # the writer then reports "no encoder" while the encoder is sitting right there. The
        # demuxer side is not symmetric (one mov demuxer reads mov, mp4, m4v and 3gp alike),
        # which is exactly what makes the asymmetry easy to miss.
        cat <<'FLAGS'
--enable-muxer=mov
--enable-muxer=mp4
--enable-encoder=mpeg4
--enable-encoder=mjpeg
--enable-encoder=aac
--enable-encoder=movtext
--enable-decoder=mpeg4
--enable-decoder=mjpeg
--enable-parser=mpeg4video
FLAGS
        # THE ONE ACCELERATOR THAT SHIPS IN NO BUILD AND IS WORTH MORE THAN THE TWO THAT DO.
        #
        # This repository can encode neither H.264 nor HEVC (ADR 015 §0), so the two accelerators
        # `player` carries can never be pointed at a clip this build produced; their evidence is
        # linkage and real files, and neither says whether the IOSurface handoff, the release
        # discipline or the layout mapping are right.
        #
        # VideoToolbox also decodes MPEG-4 Part 2, which `full` CAN encode. So the round trip that
        # proves the software seam proves the hardware one too: write a clip, decode it on the
        # accelerator, and compare against the same clip decoded in software. That is tier 1 for a
        # path that would otherwise have none.
        #
        # It is in `full` only, for the reason every other line here is: what ships decodes, and
        # nothing more.
        if [ "${LIMN_OS}" = "macos" ]; then
            cat <<'FLAGS'
--enable-hwaccel=mpeg4_videotoolbox
FLAGS
        fi
    fi
}

# ---------------------------------------------------------------------------- build, per arch

build_one_arch() {
    local arch="$1"
    local prefix="${WORK_DIR}/build/${PROFILE}/${arch}"
    local objdir="${WORK_DIR}/obj/${PROFILE}/${arch}"

    echo
    echo "=== ffmpeg ${FFMPEG_VERSION} (${PROFILE}) for ${LIMN_OS}-${arch} ==="

    if [ "${SHIM_ONLY}" = "1" ]; then
        if [ ! -d "${prefix}/lib" ]; then
            echo "  --shim-only needs a previous full build of the '${PROFILE}' profile" >&2
            exit 2
        fi
        build_shim "${arch}" "${prefix}"
        return
    fi

    rm -rf "${objdir}" "${prefix}"
    mkdir -p "${objdir}" "${prefix}"

    local extra=()
    if [ "${LIMN_OS}" = "macos" ]; then
        # install-name-dir=@rpath is what lets the four libraries sit in one extracted directory
        # and find each other: every install name becomes @rpath/libX.dylib, and the shim is
        # linked with an rpath of @loader_path, so dyld resolves siblings with no DYLD_ variable
        # and no absolute path baked in. Without it the install names carry ${prefix}, which
        # exists only on the machine that built them.
        extra+=(--install-name-dir='@rpath')
        if [ "${arch}" != "$(uname -m)" ]; then
            extra+=(--enable-cross-compile --target-os=darwin --arch="${arch}")
        fi
        extra+=(--cc="clang -arch ${arch}" --extra-cflags="-arch ${arch}"
                --extra-ldflags="-arch ${arch}")
    elif [ "${LIMN_OS}" = "windows" ]; then
        # No rpath on Windows, and none is wanted: the loader extracts every library into one
        # directory and System.load()s them in dependency order, so by the time the shim is
        # loaded each import is already resolved by module name. That is what libraries.txt's
        # order buys: on the other two platforms it buys the same thing through rpath, here it
        # is the only mechanism there is.
        #
        # Three things this platform will not answer the usual way.
        #
        # FFmpeg's configure defaults to gcc for both the target compiler and the host compiler
        # that builds its build-time tools. The CLANGARM64 environment ships no gcc at all, so
        # both have to be named or configure stops on the compiler test, then on C11 support.
        #
        # And the architecture cannot be detected, because `uname -m` describes the MSYS2 runtime
        # rather than the machine or the target: that runtime is x86_64 even on an ARM64 host, so
        # it reports x86_64 while the toolchain emits aarch64. Left alone, configure would build
        # x86 assembly for an ARM64 library. Naming --arch is not optional here.
        #
        # It is deliberately NOT a cross build. Windows on ARM64 runs ARM64 binaries, so the
        # output executes on the machine that produced it and configure's run-time probes are
        # meaningful: the x86_64 in uname is the emulated shell, not a different target.
        extra+=(--cc="${SHIM_CC}" --host-cc="${SHIM_CC}"
                --target-os=mingw32 --arch="$(arch_label "${arch}")")
    else
        extra+=(--extra-ldflags='-Wl,-rpath,$ORIGIN')
    fi

    # x86_64 wants nasm for its assembly. Without it the build still succeeds and still decodes
    # correctly, just markedly slower, so it degrades rather than failing, and says which it did.
    if [ "${arch}" = "x86_64" ] && ! command -v nasm >/dev/null 2>&1 \
            && ! command -v yasm >/dev/null 2>&1; then
        echo "  nasm/yasm not found: building with --disable-x86asm (correct, but slower)"
        extra+=(--disable-x86asm)
    fi

    (
        cd "${objdir}"
        # configure is silent for a minute or two while it runs hundreds of serial compile probes.
        # It is not hung.
        # shellcheck disable=SC2046
        "${SRC_DIR}/configure" --prefix="${prefix}" $(configure_flags) "${extra[@]}" \
            > configure.log 2>&1 || { tail -40 configure.log >&2; exit 1; }
        make -j"${JOBS}" > make.log 2>&1 || { tail -40 make.log >&2; exit 1; }
        make install > install.log 2>&1 || { tail -40 install.log >&2; exit 1; }
    )

    echo "  libraries:"
    ( cd "${prefix}/${LIB_SUBDIR}" && ls -l ./*.${LIB_EXT}* 2>/dev/null | grep -v '^l' \
        | awk '{ printf "    %-32s %8.2f MB\n", $9, $5/1048576 }' )

    build_shim "${arch}" "${prefix}"
}

build_shim() {
    local arch="$1" prefix="$2"
    local shim_out="${prefix}/${LIB_SUBDIR}/liblimnffmpeg.${LIB_EXT}"
    # -fPIC everywhere but Windows, where everything is already relocatable and mingw warns that
    # the flag is ignored, which -Werror would turn into a failed build over nothing. Import
    # libraries stay under lib/ on every platform, including the one whose DLLs do not.
    local shim_flags=(-O2 -Wall -Wextra -Werror -std=c11
        -I"${JAVA_HOME}/include" -I"${JNI_MD_DIR}" -I"${prefix}/include")
    # Linker inputs, kept separate because they go AFTER the source file. A static archive named
    # before the object that needs it is discarded, so the order is load-bearing rather than
    # stylistic (see the pthread note below).
    local shim_libs=(-L"${prefix}/lib" -lavformat -lavcodec -lavutil)
    if [ "${LIMN_OS}" != "windows" ]; then
        shim_flags=(-fPIC "${shim_flags[@]}")
    fi
    if [ "${LIMN_OS}" = "windows" ]; then
        # The output name is forced by -o below: mingw would otherwise call it limnffmpeg.dll,
        # and libraries.txt names liblimnffmpeg on all three platforms.
        #
        # The shim's mutexes are pthreads, which Windows gets from mingw-w64's winpthreads, and
        # it is linked STATICALLY on purpose. FFmpeg here uses Windows' own threads and depends
        # on no such library, so linking it dynamically would add libwinpthread-1.dll to the
        # payload for one component's benefit: another file to extract, another entry to order
        # in libraries.txt, and another binary to account for. Ninety kilobytes inside the shim
        # buys a directory that holds exactly what it did before.
        shim_flags=(-shared "${shim_flags[@]}")
        shim_libs+=(-Wl,-Bstatic -lpthread -Wl,-Bdynamic)
    elif [ "${LIMN_OS}" = "macos" ]; then
        # CoreVideo for exactly two calls: CVPixelBufferGetIOSurface and
        # CVPixelBufferGetPixelFormatType. Unwrapping the surface HERE is what keeps CoreVideo out
        # of limn-backend-lwjgl, so the handle Java carries is already the one GL wants. The
        # framework is present on every macOS and adds nothing to the payload; it is linked, not
        # bundled.
        shim_flags=(-arch "${arch}" -dynamiclib
            -install_name "@rpath/liblimnffmpeg.dylib" -Wl,-rpath,@loader_path
            -framework CoreVideo "${shim_flags[@]}")
    else
        shim_flags=(-shared -Wl,-rpath,'$ORIGIN' "${shim_flags[@]}")
    fi
    echo "  shim:"
    "${SHIM_CC}" "${shim_flags[@]}" -o "${shim_out}" "${SHIM_SRC}" "${shim_libs[@]}"
    ls -l "${shim_out}" | awk '{ printf "    %-32s %8.2f MB\n", "liblimnffmpeg", $5/1048576 }'
}

IFS=',' read -r -a ARCH_LIST <<< "${ARCHS}"
for arch in "${ARCH_LIST[@]}"; do
    build_one_arch "${arch}"
done

# ---------------------------------------------------------------------------- stage the result
#
# The loader looks for resources under limn/video/ffmpeg/native/<os>-<arch>/, and the Gradle build
# adds this directory as a resource root when it exists. The `player` tree is committed (it is
# what a published jar ships) and everything else here, `full` included, is gitignored.

file_size() {
    stat -f%z "$1" 2>/dev/null || stat -c%s "$1"
}

DIST="${WORK_DIR}/dist/${PROFILE}/limn/video/ffmpeg/native"

stage() {
    local arch="$1"
    local out="${DIST}/${LIMN_OS}-$(arch_label "${arch}")"
    rm -rf "${out}"; mkdir -p "${out}"
    # Real files only: FFmpeg installs libavcodec.dylib and libavcodec.61.dylib as a symlink pair,
    # and a jar entry cannot be a symlink. The shim's dependency records name the versioned file,
    # which is therefore the one that must be extracted beside it.
    local f
    for f in "${WORK_DIR}/build/${PROFILE}/${arch}/${LIB_SUBDIR}"/*."${LIB_EXT}"*; do
        if [ -L "$f" ] || [ ! -f "$f" ]; then continue; fi
        cp "$f" "${out}/"
    done

    # The loader cannot list a directory inside a jar, and the FFmpeg libraries carry their full
    # version in their file names, so the names cannot be spelled in Java either. This manifest is
    # how the loader learns both what to extract and what order to load it in: dependencies
    # first, the shim last.
    #
    # Two spellings, because the version does not go in the same place on every platform:
    # libavcodec.61.19.101.dylib and libavcodec.so.61 keep the lib prefix, while mingw produces
    # avcodec-61.dll. Globbing both is what keeps the manifest a description of what is on disk
    # rather than a guess at what the toolchain was going to call it.
    local stem
    {
        for stem in avutil swresample avcodec avformat; do
            for f in "${out}/lib${stem}."* "${out}/${stem}-"*; do
                [ -f "$f" ] && basename "$f"
            done
        done
        echo "liblimnffmpeg.${LIB_EXT}"
    } > "${out}/libraries.txt"

    if [ "${LIMN_OS}" = "windows" ]; then
        verify_windows_payload "${out}"
    fi
    if [ "${LIMN_OS}" = "linux" ]; then
        verify_linux_payload "${out}"
    fi
}

# The oldest glibc a shipped Linux payload must run on. 2.28 is RHEL 8, Debian 10, Ubuntu 20.04,
# every mainstream distribution from 2018 onwards. The number is here rather than in a comment
# beside the build instructions because verify_linux_payload enforces it, so raising it is a
# deliberate edit that says which machines are being dropped.
LINUX_GLIBC_FLOOR="2.28"

# Refuses a Linux payload that needs a newer glibc than the floor above.
#
# The Windows twin below exists because a payload imported a DLL it did not carry. Linux has the
# same failure with a different mechanism and a worse disguise: a .so records the *version* of each
# glibc symbol it uses, the loader refuses the library when the running glibc is older, and nothing
# about the build says so. It happened here. A first Linux build made on a Fedora 44 guest linked,
# staged, passed its tests on that guest and required GLIBC_2.43, a glibc released weeks earlier.
# It would have loaded on the machine that built it and on nothing else: not Ubuntu 24.04, not
# Debian 12, not RHEL 9. The tests that prove the decoder works prove it on the build machine, and
# that is exactly the evidence this failure does not disturb.
#
# So the build machine's glibc is the thing being checked, and the fix is to build somewhere old
# rather than to relax the number. A container image with an old glibc is the ordinary way; the
# manylinux images exist for this and carry 2.28.
verify_linux_payload() {
    local out="$1" lib required worst=0
    if ! command -v objdump >/dev/null 2>&1; then
        echo "  WARNING: objdump not found, so the payload's glibc floor was NOT verified" >&2
        return 0
    fi
    for lib in "${out}"/*.so*; do
        [ -f "${lib}" ] || continue
        # The highest version any symbol asks for IS the library's floor: the loader needs every
        # one of them, so the newest decides.
        required="$(objdump -T "${lib}" 2>/dev/null \
                        | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/^GLIBC_//' \
                        | sort -V | tail -1)"
        [ -n "${required}" ] || continue
        if [ "$(printf '%s\n%s\n' "${LINUX_GLIBC_FLOOR}" "${required}" | sort -V | tail -1)" \
                != "${LINUX_GLIBC_FLOOR}" ]; then
            echo "  TOO NEW: $(basename "${lib}") needs glibc ${required}, above the" \
                 "${LINUX_GLIBC_FLOOR} this payload promises" >&2
            worst=1
        fi
    done
    if [ "${worst}" = "1" ]; then
        echo "  The library would load on the machine that built it and on newer ones only." >&2
        echo "  Build in an image whose glibc is ${LINUX_GLIBC_FLOOR} or older, e.g." >&2
        echo "    docker run --platform linux/\$ARCH -v \"\$PWD:/repo\" -w /repo \\" >&2
        echo "      quay.io/pypa/manylinux_2_28_\$(uname -m) ./scripts/build-ffmpeg.sh" >&2
        echo "  (that image needs a JDK for jni.h: dnf install -y java-17-openjdk-devel)" >&2
        exit 1
    fi
    echo "  payload verified: nothing here needs a glibc newer than ${LINUX_GLIBC_FLOOR}"
}

# Refuses a Windows payload that imports something it does not carry.
#
# There is no rpath on Windows: the loader finds a dependency because the payload already loaded a
# module of that name, or it does not find it at all. A library the directory is missing therefore
# fails at LoadLibrary with "Can't find dependent libraries", and the way that reaches a developer
# is the worst possible shape. It happened: an x86_64 build linked, staged and reported success
# while avutil imported libwinpthread-1.dll, which is not here; the decoder then reported itself
# unavailable, every test that needs it stood down, and the suite passed with 102 of 124 skipped.
# A build that cannot run is not a build, and it must not be allowed to look like one.
#
# The toolchain decides this and not the code: MSYS2's MINGW64 gcc uses the POSIX threading model
# and pulls winpthreads into everything, while CLANG64 and CLANGARM64 use Windows' own threads and
# pull nothing. Rather than pin an environment in a comment nobody reads at the right moment, ask
# the binaries.
verify_windows_payload() {
    local out="$1" dll dep lower missing=0
    if ! command -v objdump >/dev/null 2>&1; then
        echo "  WARNING: objdump not found, so the payload's imports were NOT verified" >&2
        return 0
    fi
    for dll in "${out}"/*.dll; do
        [ -f "${dll}" ] || continue
        while read -r dep; do
            [ -n "${dep}" ] || continue
            lower="$(printf '%s' "${dep}" | tr '[:upper:]' '[:lower:]')"
            case "${lower}" in
                # Shipped with Windows itself. Anything outside this list is either in the payload
                # or a mistake, and the build stops rather than guessing which.
                api-ms-win-*|kernel32.dll|kernelbase.dll|ntdll.dll|msvcrt.dll|ucrtbase.dll \
                |advapi32.dll|user32.dll|gdi32.dll|ole32.dll|oleaut32.dll|shell32.dll \
                |bcrypt.dll|secur32.dll|ws2_32.dll|psapi.dll|version.dll|winmm.dll) continue ;;
            esac
            if [ ! -f "${out}/${dep}" ]; then
                echo "  MISSING DEPENDENCY: $(basename "${dll}") imports ${dep}, which this" \
                     "payload does not carry and Windows does not provide" >&2
                missing=1
            fi
        done < <(objdump -p "${dll}" 2>/dev/null \
                    | sed -n 's/^[[:space:]]*DLL Name:[[:space:]]*//p')
    done
    if [ "${missing}" = "1" ]; then
        echo "  The library would load on the machine that built it and nowhere else." >&2
        echo "  Build under CLANG64 (x86_64) or CLANGARM64 (arm64), not MINGW64." >&2
        exit 1
    fi
    echo "  payload verified: every import is in this directory or in Windows"
}

# The licence travels with the binary, because the jar is now the thing being distributed and a
# jar's reader has no repository to look in. LGPL-2.1 asks for the text; what makes the rest of
# §6 satisfiable is already true of this layout: the FFmpeg libraries are shared, thin and
# separate from the shim, so a user who wants a different FFmpeg replaces three files and nothing
# is rebuilt. Written from the source tree rather than kept as a checked-in copy, so it cannot
# describe a version that is no longer the one being shipped.
stage_licence() {
    mkdir -p "${DIST}"
    cp "${SRC_DIR}/COPYING.LGPLv2.1" "${DIST}/LICENSE-ffmpeg.txt"
    cat > "${DIST}/NOTICE-ffmpeg.txt" <<NOTICE
This artifact contains unmodified binaries of FFmpeg ${FFMPEG_VERSION}, built from the upstream
release tarball by scripts/build-ffmpeg.sh in the limn-toolkit/limn-ffmpeg-natives repository.

FFmpeg is licensed under the GNU Lesser General Public License, version 2.1 or later. The full
text is in LICENSE-ffmpeg.txt beside this file. FFmpeg's own copyright and authors are in the
source release; nothing here modifies it.

The build deliberately omits --enable-gpl and --enable-nonfree, so no GPL or non-redistributable
component is present. It is configured with --disable-everything and switches back on only the
demuxers, decoders and parsers this toolkit opens.

The libraries are built SHARED and loaded dynamically by liblimnffmpeg, which is Limn's own code
under Apache-2.0. Replacing libavutil, libswresample, libavcodec and libavformat in this
directory with a different build of the same soname is supported and requires nothing to be
recompiled; that is how the relink freedom LGPL-2.1 section 6 protects is satisfied here.

profile: ${PROFILE}
NOTICE
}

echo
for arch in "${ARCH_LIST[@]}"; do
    stage "${arch}"
done
stage_licence

# DELIBERATELY NOT lipo'd into universal binaries.
#
# It was, briefly, and it was the wrong shape: the loader picks a directory from os.arch, so a
# universal library has to be copied into BOTH directories to be found, which puts every byte of
# both architectures in the jar twice, four times the size of the one slice any given JVM will
# load. A fat binary earns its keep in a single-file app bundle, not in a classifier layout that
# already selects by architecture.
#
# So each architecture keeps its own thin build in its own directory. One jar carrying both is the
# sum of the two; a per-classifier jar is just one of them, and needs no change here or in the
# loader: the resource path is already <os>-<arch>.

# ---------------------------------------------------------------------------- report

echo
echo "=== payload (${PROFILE}) ==="
for d in "${DIST}"/*/; do
    [ -d "$d" ] || continue
    total=0
    for f in "$d"*; do
        total=$(( total + $(file_size "$f") ))
    done
    printf "  %s\n" "$(basename "$d")"
    for f in "$d"*; do
        printf "    %-30s %8.2f MB\n" "$(basename "$f")" \
            "$(awk -v b="$(file_size "$f")" 'BEGIN { print b/1048576 }')"
    done
    printf "    %-30s %8.2f MB\n" "TOTAL uncompressed" \
        "$(awk -v b="${total}" 'BEGIN { print b/1048576 }')"
    if command -v zip >/dev/null 2>&1; then
        zipped="${WORK_DIR}/.size-probe.zip"
        rm -f "${zipped}"
        ( cd "$d" && zip -q -9 -r "${zipped}" . )
        printf "    %-30s %8.2f MB\n" "TOTAL in a jar (deflate)" \
            "$(awk -v b="$(file_size "${zipped}")" 'BEGIN { print b/1048576 }')"
        rm -f "${zipped}"
    fi
done

echo
echo "Built. The Gradle build picks it up from"
echo "  ${WORK_DIR}/dist/${PROFILE}"
echo "It is yours and it is local: nothing under native/ is committed, ever. What a release"
echo "ships is built by .github/workflows/natives.yml, on six machines, into the jar."
