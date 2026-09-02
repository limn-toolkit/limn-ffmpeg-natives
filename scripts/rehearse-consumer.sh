#!/usr/bin/env bash
#
# Proves the artifact RESOLVES, not merely that it uploads.
#
# limn-fonts' first four releases shipped module metadata that refused every consumer building
# for an older Java, and the defect sat in build/repo through a publication rehearsal that
# inspected jars, POMs and signatures and never once tried to consume them. This is the missing
# half: publish to the local file repository, then resolve from it with a throwaway Gradle project
# shaped like the real consumers (compiles for 17, reads Gradle metadata, names the main jar and
# every classifier the payload carries), and check that what arrives is what the loader needs.
#
# CI runs it on every push against the one slice it builds; the release runs it against all six
# before the upload. Locally: ./scripts/rehearse-consumer.sh [version]
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
VERSION="${1:-$(sed -n 's/^limn-ffmpeg-natives=\(.*\)$/\1/p' versions.properties)-SNAPSHOT}"
REPO="$ROOT/build/repo"
CONSUMER="$ROOT/build/consumer"

echo "· publishing $VERSION to ${REPO#"$ROOT/"}"
rm -rf "$REPO"
./gradlew publishAllPublicationsToBuildDirRepository -PlimnNativesVersion="$VERSION" -PlimnFfmpegProfile=player -q

# Which classifiers the payload carries: the consumer names exactly those, so a single-slice CI
# build rehearses its one slice and a release rehearses six.
DIST="$ROOT/native/dist/player/limn/video/ffmpeg/native"
CLASSIFIERS=()
for dir in "$DIST"/*/; do
  [ -f "$dir/libraries.txt" ] || continue
  CLASSIFIERS+=("natives-$(basename "$dir")")
done
if [ "${#CLASSIFIERS[@]}" -eq 0 ]; then
  echo "✗ no slice under ${DIST#"$ROOT/"}: run scripts/build-ffmpeg.sh first" >&2
  exit 1
fi
echo "· classifiers present: ${CLASSIFIERS[*]}"

# The metadata must not demand a JVM the jars do not need (see options.release in build.gradle.kts).
BAD="$(grep -rho '"org.gradle.jvm.version": [0-9]*' "$REPO" --include="*.module" | sort -u | grep -v ': 8$' || true)"
if [ -n "$BAD" ]; then
  echo "✗ a published variant declares a minimum JVM other than 8: $BAD" >&2
  exit 1
fi
echo "✓ every variant declares JVM 8"

rm -rf "$CONSUMER"; mkdir -p "$CONSUMER"
cp gradlew "$CONSUMER"/ && cp -r gradle "$CONSUMER"/
cat > "$CONSUMER/settings.gradle.kts" <<SETTINGS
rootProject.name = "consumer"
SETTINGS
{
  cat <<GRADLE
plugins { \`java-library\` }
repositories { maven { url = uri("$REPO") } }
java { toolchain { languageVersion.set(JavaLanguageVersion.of(21)) } }
tasks.withType<JavaCompile>().configureEach { options.release.set(17) }
dependencies {
    runtimeOnly("io.github.limn-toolkit:limn-ffmpeg-natives:$VERSION")
GRADLE
  for classifier in "${CLASSIFIERS[@]}"; do
    echo "    runtimeOnly(\"io.github.limn-toolkit:limn-ffmpeg-natives:$VERSION:$classifier\")"
  done
  cat <<'GRADLE'
}
tasks.register("resolveLikeAConsumer") {
    val files = configurations.runtimeClasspath.get()
    val out = layout.buildDirectory.file("resolved.txt")
    outputs.file(out)
    doLast { out.get().asFile.writeText(files.joinToString("\n", postfix = "\n") { it.absolutePath }) }
}
GRADLE
} > "$CONSUMER/build.gradle.kts"

( cd "$CONSUMER" && ./gradlew resolveLikeAConsumer -q )

status=0
# `|| [ -n "$jar" ]`: a last line without a newline is still a jar to check.
while IFS= read -r jar || [ -n "$jar" ]; do
  name="$(basename "$jar")"
  # The listing once, into a variable: under pipefail, `unzip -l | grep -q` reports unzip's
  # SIGPIPE when grep stops reading early, and a found file then reads as a missing one.
  listing="$(unzip -l "$jar" < /dev/null)"
  case "$name" in
    limn-ffmpeg-natives-*-natives-*.jar)
      platform="${name#limn-ffmpeg-natives-"$VERSION"-natives-}"; platform="${platform%.jar}"
      if grep -q "limn/video/ffmpeg/native/$platform/libraries.txt" <<< "$listing"; then
        echo "✓ $name carries $platform's manifest"
      else
        echo "✗ $name has no libraries.txt for $platform" >&2; status=1
      fi
      if grep -q "liblimnffmpeg\." <<< "$listing"; then
        echo "✗ $name carries the shim, which belongs in the main jar" >&2; status=1
      fi
      ;;
    limn-ffmpeg-natives-*.jar)
      shims="$(grep -c "liblimnffmpeg\." <<< "$listing" || true)"
      if [ "$shims" -ge "${#CLASSIFIERS[@]}" ]; then
        echo "✓ $name carries $shims shim(s)"
      else
        echo "✗ $name carries $shims shim(s) for ${#CLASSIFIERS[@]} slice(s)" >&2; status=1
      fi
      ;;
  esac
done < "$CONSUMER/build/resolved.txt"
if [ "$status" -ne 0 ]; then
  echo "The rehearsal failed: what a consumer would receive is not what the loader needs." >&2
  exit "$status"
fi
echo "Rehearsal passed: a target-17 consumer resolves $VERSION and every jar holds what it should."
