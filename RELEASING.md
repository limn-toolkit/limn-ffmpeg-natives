# Releasing

`versions.properties` is the single place the version is written. The build reads it for the
`-SNAPSHOT` default and refuses a version whose first three components are not the FFmpeg that
`scripts/build-ffmpeg.sh` pins; on every push to `main` the `tag-releases` workflow creates the
tag `v<version>` if it is not on origin yet and starts `publish`. Landing a bumped entry on
`main` is the release decision. Nobody types or pushes a tag.

```
7.1.5.0   FFmpeg 7.1.5, as first packaged
7.1.5.1   the same FFmpeg with a newer shim (or a repackaging)
7.1.6.0   the next FFmpeg — move FFMPEG_VERSION and FFMPEG_SHA256 in the script in the same commit
```

What `publish` does, in order: verifies the tag and the version, builds the six slices on five
runners (`natives.yml`), merges them, **rehearses a consumer** (`scripts/rehearse-consumer.sh`:
publish to a file repository, resolve from it as a target-17 consumer, check every jar holds
what the loader needs — before anything leaves the machine, because Central keeps what it
accepts), uploads the signed bundle, and drafts the GitHub release.

Nothing publishes itself: the deployment sits staged on the Central Portal until somebody
presses **Publish**, and the GitHub release is a draft.

## A shim change

The shim and the toolkit's `FfmpegNative` are one interface in two repositories. A change to
any native signature is: bump `LIMN_FFMPEG_ABI` here and `FfmpegLibrary`'s expected number
there, release here first (fourth component), then point the toolkit's catalog at the new
version. A toolkit that meets an older shim refuses to load it with a sentence naming both
numbers.

## After a release

If the toolkit should pick the new payload up, bump `limn-ffmpeg-natives` in limn-toolkit's
`gradle/libs.versions.toml`; its `limn-video-ffmpeg-natives-all` POM and its own dependency
follow from that one line, and its `LicenceTest` and codec-breadth tests re-verify the payload.

## When something goes wrong

**A slice failed to build.** Nothing was uploaded; the publish guard refuses a payload missing a
platform. Fix on `main`, delete the tag on the web UI (repository → Tags → ⋯), push: the next
run re-tags the fixed commit. **A staged deployment is wrong:** Drop it on the Portal, free.
**A published version is wrong:** it stays; release the next fourth component.

## Secrets

The same four as limn-toolkit: `MAVEN_CENTRAL_USERNAME`, `MAVEN_CENTRAL_PASSWORD` (Portal user
token), `SIGNING_KEY`, `SIGNING_PASSWORD`. Workstation copies live outside every repository.
