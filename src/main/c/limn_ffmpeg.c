/*
 * limn_ffmpeg.c: the only C in this repository, and the only thing that talks to libavcodec.
 *
 * It is a PLAYER HANDLE, not a binding. Nothing here mirrors the codec API: Java never sees an
 * AVFrame, an AVPacket, an AVCodecContext or a pointer to any of them. It sees a handle, a slot
 * index and a picture's geometry, which is what keeps this file small enough to review and keeps
 * every lifetime question on this side of the boundary where it can be answered.
 *
 * ============================================================================================
 * WHO OWNS THE PICTURE MEMORY, AND FOR HOW LONG
 * ============================================================================================
 *
 * libavcodec owns it. It hands out reference-counted pictures from a pool of its own, and a
 * picture stays alive exactly as long as some AVFrame holds a reference to it.
 *
 * This file keeps `slotCount` AVFrames (the slots), and each is either FREE (this side's, empty)
 * or BUSY (referenced by a picture Java is holding). The two lifetimes are tied together in
 * exactly one place, and this is it:
 *
 *     readVideo()      acquires a free slot, decodes into it, marks it BUSY.  ref taken
 *     releaseVideo()   av_frame_unref()s that slot and marks it FREE.         ref dropped
 *
 * `releaseVideo` is reached from exactly one place in Java (the VideoFrame.Recycler the pool
 * installs), so `VideoFrame.release()` and `av_frame_unref()` are the same event seen from two
 * sides. Nothing else unrefs a busy slot, and a slot that is BUSY is never touched by the decoder.
 * There is no path on which a picture is freed while Java can still read it, and none on which a
 * picture outlives the handle: closeHandle() unrefs every slot, free or busy, and after it Java
 * holds only a VideoFrame that answers `false` to being held.
 *
 * ============================================================================================
 * WHEN THE CONSUMER HOLDS EVERY SLOT
 * ============================================================================================
 *
 * readVideo returns READ_PENDING. It does not block, does not wait on a condition, and does not
 * copy the picture somewhere else to free the slot. A consumer that holds every slot is told to
 * release one and ask again, which is what VideoStreamSource.Read.PENDING means.
 *
 * ============================================================================================
 * WHAT CROSSES PER PICTURE, AND WHY IT IS NOT AN ALLOCATION
 * ============================================================================================
 *
 * One call, filling a `long[]` the caller allocated once: the presentation time, a binding epoch
 * and the three row strides. Nothing is created: no AVFrame wrapper, no ByteBuffer, no String.
 *
 * The planes themselves are direct ByteBuffers over libavcodec's own memory (NewDirectByteBuffer
 * over AVFrame.data[i]), which is the no-copy path: FFmpeg's data[]+linesize[] is already
 * planes-plus-byte-strides, the exact shape the SPI publishes, so nothing is repacked.
 *
 * Those ByteBuffers cannot be made once at startup, because a decoded picture lands on whichever
 * buffer libavcodec's pool had free, so the address a slot carries changes from picture to
 * picture. They are therefore cached BY ADDRESS in `bufferCache` and shared across slots. The set
 * of distinct addresses a decoder uses is bounded by its own pool, so the cache fills during the
 * first few pictures and is all hits afterwards, which is what makes steady-state decoding
 * allocation-free, and what AllocationProbe in the test suite is pointed at.
 *
 * `epoch` is how Java knows whether to rebind. It changes only when this slot's planes moved, so
 * Java calls VideoFrame.Writer.setPlane during warm-up and never again, which matters because
 * setPlane creates a read-only view, i.e. it allocates.
 *
 * ============================================================================================
 * EVERY PATH A MALFORMED INPUT CAN TAKE
 * ============================================================================================
 *
 * A native decoder that segfaults kills the JVM, and CrashPhase.DECODE contains a Java exception,
 * not a signal. So every input-derived quantity is checked here before it is used, and each check
 * below throws rather than dereferences:
 *
 *   open   - avformat_open_input fails                    -> throw (not a container, or unreadable)
 *          - avformat_find_stream_info fails              -> throw
 *          - no video stream                              -> throw
 *          - a codec libavcodec was not built with        -> throw, naming it
 *          - avcodec_open2 fails                          -> throw
 *          - a pixel format outside PixelFormat           -> throw, naming it
 *          - width/height outside [1..MAX_DIMENSION]      -> throw (a header can claim anything)
 *   read   - avcodec_send_packet / receive_frame error    -> throw with av_strerror's text
 *          - frame geometry differs from the header's     -> throw (the SPI fixes it at open)
 *          - a negative linesize (bottom-up rows)         -> throw; the SPI forbids them
 *          - a linesize below the plane's byte width      -> throw
 *          - data[i] == NULL for a plane of the format    -> throw
 *   audio  - a sample format this file cannot convert     -> throw, naming it
 *          - a channel count of zero                      -> throw
 *
 * Nothing here trusts a field because a well-formed file would have set it. Where a check is
 * cheap and the failure is a dereference, the check is present.
 *
 * ============================================================================================
 * THE THREAD CONTRACT
 * ============================================================================================
 *
 * Four threads reach a handle, and they are not the same thread:
 *
 *   the player's decode thread   readVideo, resetVideo          serialized by MediaPlayer
 *   any thread                   releaseVideo                   VideoFrame.release, per its
 *                                                               contract, on whichever thread
 *   the engine's streaming thread readAudio, resetAudio         serialized by the audio engine
 *   the UI thread                closeHandle, releaseAudio      after the decode thread is joined
 *
 * So this file DOES lock, and the claim that a serialized decode thread needs none is true only
 * of the video path in isolation. Three mutexes rather than one, because one would put a whole
 * video decode in front of every audio refill:
 *
 *   demuxLock  the AVFormatContext and both packet queues
 *   videoLock  the video codec context, the slots, the buffer cache
 *   audioLock  the audio codec context and its partially drained frame
 *
 * LOCK ORDER: videoLock or audioLock may be held while taking demuxLock. demuxLock is never held
 * while taking either. videoLock and audioLock are never held at the same time. That is the whole
 * rule, and it is why there is no cycle to deadlock on.
 *
 * ============================================================================================
 * HOW TWO TRACKS ARE PULLED WITHOUT ONE STARVING THE OTHER
 * ============================================================================================
 *
 * There is one demux loop and two consumers running at different rates on different threads.
 * Whichever thread needs a packet runs the loop under demuxLock; packets for the other track are
 * pushed onto that track's queue and picked up when its own thread asks. So neither track waits
 * for the other to be pulled, and neither thread owns the demuxer.
 *
 * What bounds the queue that nobody is draining: QUEUE_MAX_PACKETS and QUEUE_MAX_BYTES. A file
 * whose tracks are interleaved the way a muxer interleaves them never approaches either; the
 * bound is reached only when a consumer has genuinely stopped, which happens for real reasons (a
 * paused player, an audio engine that refused the track, a view that was detached).
 *
 * At the bound the OLDEST packet of the full queue is dropped and counted, rather than the
 * demuxing thread blocking. Blocking is the alternative and it is worse in both directions: a
 * stalled video consumer would stop the soundtrack, and AudioStreamSource.readFrames has no way
 * to say "not now" (its zero means end of stream), so an audio thread made to wait would either
 * hang the engine or lie about the track ending. Dropping is visible (droppedVideo/droppedAudio
 * are readable and asserted in the tests) and self-correcting: the consumer that resumes gets
 * recent data rather than a backlog.
 *
 * A track with no consumer at all is cheaper still: `audioClaimed` is cleared when the audio side
 * is released, and its packets are then freed as they are demuxed rather than queued.
 *
 * ============================================================================================
 * SEVERAL AUDIO TRACKS, AND WHY ONLY ONE OF THEM IS OPEN
 * ============================================================================================
 *
 * A container may hold any number of audio streams. This file records every one of their indices
 * at open and OPENS EXACTLY ONE: av_find_best_stream's answer by default, or whichever
 * selectAudio was told to take. Everything downstream of the selection stays singular: one codec,
 * one partially drained frame, one queue, one downmix.
 *
 * That is a decision and not an omission. N open decoders would be N codec contexts, N queues and
 * N downmix matrices to keep coherent across a seek, for a capability no consumer has: the audio
 * engine mixes one source per player, so a second decoded track is samples nobody reads.
 *
 * The demux filter follows the selection rather than the media type. A packet belonging to an
 * audio stream that is not the selected one is FREED as it is met, exactly as an unclaimed track's
 * is, which is the property that keeps a film's five unread language tracks from growing memory
 * for the length of the film.
 *
 * WHAT A SELECTION DOES TO THE CONSUMER THAT HAD THE OLD TRACK is the whole of the difficulty, so
 * it is a number: `audioGeneration` is bumped by every selection, and every audio entry point
 * takes the generation its caller was handed. A call from a superseded consumer is ANSWERED, not
 * punished: readAudio gives 0, which is what the end of a track means to the engine, and seek,
 * reset and release do nothing. The engine's streaming thread can genuinely be inside a refill
 * when the user interface picks another language, and the alternative to answering it is either a
 * lock held across a whole decode or a use-after-free.
 *
 * The stale RELEASE is the one that would be silent. `releaseAudio` clears `audioClaimed`, and the
 * engine closes a source it was given on every path, so a superseded track closing after the new
 * one was selected would unclaim the track somebody is listening to, and the sound would stop with
 * nothing anywhere reporting a fault. That is why the generation is checked there too.
 */

#include <jni.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>

/*
 * On macOS a decoded hardware picture is a CVPixelBuffer, and what a GL context can bind is the
 * IOSurface inside it. THIS FILE unwraps that, so the number Java receives is the one the backend
 * hands to CGLTexImageIOSurface2D and nothing above has to link CoreVideo, which matters because
 * the alternative was a second interop library in limn-backend-lwjgl reaching into a codec type.
 *
 * Two calls, and no other CoreVideo anywhere: the surface, and the pixel format type that says
 * whether it is NV12 or P010.
 */
#ifdef __APPLE__
#include <CoreVideo/CoreVideo.h>
#endif

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Mirrors of limn.video constants. LicenceTest pins every one of them against the enum it stands
 * for, so a reordered enum is a failing test rather than a wrong colour. */
#define FORMAT_I420 0
#define FORMAT_NV12 1
#define FORMAT_I444 2
#define FORMAT_I420_10LE 3
#define FORMAT_I444_10LE 4
#define FORMAT_P010 5

#define MATRIX_BT601  0
#define MATRIX_BT709  1
#define MATRIX_BT2020 2
#define MATRIX_UNSPECIFIED (-1)

#define RANGE_LIMITED 0
#define RANGE_FULL    1
#define RANGE_UNSPECIFIED (-1)

#define TRANSFER_SDR 0
#define TRANSFER_PQ  1
#define TRANSFER_HLG 2

#define READ_PENDING (-1)
#define READ_END     (-2)

/* Entries readVideo fills, mirrored by FfmpegNative.READ_LENGTH: the timestamp, the binding
 * epoch, three strides, and the device handle (0 for a planar picture). */
#define READ_LENGTH_C 6

/* PixelFormat.MAX_DIMENSION. A container header can claim any size at all, so it is checked
 * here before it is multiplied by anything. */
#define MAX_DIMENSION 65535

#define MAX_SLOTS 16
#define MAX_PLANES 3

/* Distinct picture addresses a decoder's own pool can hand out before this cache starts evicting.
 * A frame-threaded H.264 decoder uses well under this; the eviction path exists so that an
 * unusual decoder costs an allocation now and then rather than unbounded memory. */
#define BUFFER_CACHE_SIZE 64

/* What bounds the track nobody is pulling. Generous on purpose: a normally interleaved file
 * never comes near either, so reaching one means a consumer stopped rather than that the file is
 * unusual. */
#define QUEUE_MAX_PACKETS 512
#define QUEUE_MAX_BYTES (8 * 1024 * 1024)

/* Audio streams whose indices are remembered. A film ships one track per language and a generous
 * release has a dozen; beyond this the extra tracks are not listed and therefore cannot be
 * selected, which is a listing that is short rather than a decoder that is wrong. */
#define MAX_AUDIO_TRACKS 32

/* The same bound for subtitle streams, and it is reached sooner: a release carries more subtitle
 * languages than soundtracks, because a subtitle track costs kilobytes and a soundtrack costs
 * megabytes. Same consequence: a listing that is short rather than a decoder that is wrong. */
#define MAX_SUBTITLE_TRACKS 32

/* What readCue puts in out[C_STATUS]. Three answers and not two, because "a packet was consumed
 * and produced no cue" has to keep the caller's pump going: mov_text writes empty samples across
 * the gaps between lines, and a pump that stopped at the first one would drain a cue per call and
 * fall behind by exactly the number of gaps. */
#define CUE_NONE    0
#define CUE_READY   1
#define CUE_SKIPPED 2

/* Audio tracks the clip writer can be asked for. Its purpose is to make something to select
 * between, and no test needs more than a handful. */
#define MAX_WRITE_AUDIO_TRACKS 8

/* The same, for subtitle tracks and for the same reason. */
#define MAX_WRITE_SUBTITLE_TRACKS 8

/* ------------------------------------------------------------------ structures */

typedef struct PacketNode {
    AVPacket *packet;
    struct PacketNode *next;
} PacketNode;

typedef struct {
    PacketNode *head;
    PacketNode *tail;
    int count;
    int64_t bytes;
} PacketQueue;

typedef struct {
    const uint8_t *address;
    jlong capacity;
    jobject buffer; /* global ref, or NULL for a free entry */
    int64_t used;   /* for eviction: the value of `clock` when last hit */
} BufferEntry;

typedef struct {
    AVFrame *frame;
    int busy;
    int64_t epoch;
    const uint8_t *bound[MAX_PLANES];
    int strides[MAX_PLANES];
    /* Where a hardware picture lands when a consumer that cannot bind a handle asks for it back
     * (VideoFrame.toPlanar). Allocated on the first download of this slot and reused; the picture
     * it holds is unreffed with the slot, so a download costs one transfer and no allocation after
     * the first. NULL on every slot of a software decode, which is the ordinary case. */
    AVFrame *software;
    /* 1 while `bound`/`strides` describe `software` rather than `frame`, which is what planeBuffer
     * has to be reading for a downloaded picture. */
    int downloaded;
} Slot;

typedef struct {
    AVFormatContext *format;

    int videoStream;
    /* The audio stream the open codec belongs to, or -1 when none is open. This (not the media
     * type) is what the demuxer filters on, so every other audio stream's packets are freed. */
    int audioStream;

    /* Every audio stream the container holds, in the container's own order, whether or not this
     * build can decode it. Written once at open and read without a lock afterwards, which is safe
     * because nothing ever writes it again. */
    int audioStreams[MAX_AUDIO_TRACKS];
    int audioTrackCount;
    /* Positions in audioStreams: what av_find_best_stream picked, and what is open now. -1 each
     * when the container has no audio, or when it was opened without any. */
    int defaultAudioTrack;
    int selectedAudioTrack;
    /* Bumped by every selection. A consumer holding an older one is superseded, and its calls are
     * answered rather than acted on. Read and written under audioLock, and under demuxLock for the
     * release path, which is the only one that does not take audioLock. */
    int64_t audioGeneration;

    /* ---- demuxer, guarded by demuxLock */
    pthread_mutex_t demuxLock;
    PacketQueue videoQueue;
    PacketQueue audioQueue;
    int demuxEnded;
    int64_t droppedVideo;
    int64_t droppedAudio;
    int audioClaimed;
    int64_t startTimeMicros;
    /* Where the demuxer was last placed by a seek, and which track has read from there since.
     * One position serves two consumers, so a target both of them ask for is one seek. */
    int64_t seekTargetMicros;
    int seekTakenVideo;
    int seekTakenAudio;
    /* Times the container was really moved, so the dedupe above is assertable rather than claimed. */
    int64_t containerSeeks;

    /* ---- video, guarded by videoLock */
    pthread_mutex_t videoLock;
    AVCodecContext *videoCodec;
    /* A packet the decoder would not take yet. It MUST be kept rather than dropped:
     * avcodec_send_packet answers AVERROR(EAGAIN) to mean "read my output first and send this
     * again", and a shim that frees it instead loses a packet, which costs one picture at the
     * end of some files, all of a one-picture file, and nothing at all in most, so it is a bug
     * that hides behind whichever clip it was tested with. */
    AVPacket *pendingVideo;
    Slot slots[MAX_SLOTS];
    int slotCount;
    int videoEnded;
    /* Pictures decoded before this are dropped rather than delivered: how an exact seek reaches a
     * time between two independently decodable pictures. INT64_MIN when nothing is being skipped. */
    int64_t videoSkipToMicros;
    int width;
    int height;
    int planeCount;
    int pixelFormat;   /* FORMAT_* */
    enum AVPixelFormat avPixelFormat;
    BufferEntry bufferCache[BUFFER_CACHE_SIZE];
    int64_t bufferClock;
    /* The hardware device context, or NULL for a software decode. Held for the codec's life: the
     * codec keeps a reference of its own, and this one is what closeHandle unrefs. */
    AVBufferRef *hwDevice;
    /* 1 when the decoder was opened with an accelerator attached and its pictures are therefore
     * handles rather than samples. Decided once, at open, because the SPI fixes the layout there,
     * then checked against every decoded picture, because get_format is entitled to change its
     * mind and a picture that quietly became software would be published as a handle of zero. */
    int hardware;
    /* What CVPixelBufferGetPixelFormatType must answer for a hardware picture, given the depth the
     * container declared. A decoder that produced something else is a picture this SPI cannot
     * describe, and it is refused rather than reinterpreted. */
    uint32_t expectedSurfaceType;
    /* The same layout's full-range spelling. Which of the two a decoder produces follows the
     * stream's own range signalling, and the range is carried on VideoColor from the container,
     * so both are accepted here and neither is used to decide anything about colour. */
    uint32_t expectedSurfaceTypeAlt;

    /* ---- audio, guarded by audioLock */
    pthread_mutex_t audioLock;
    AVCodecContext *audioCodec;
    AVPacket *pendingAudio; /* same rule as pendingVideo */
    AVFrame *audioFrame;
    int audioFrameOffset; /* samples of audioFrame already delivered */
    int audioHasFrame;
    int audioEnded;
    /* Samples decoded before this are dropped rather than delivered: what makes a seek
     * sample-accurate on the audio side when the container landed on a video keyframe before it.
     * INT64_MIN when nothing is being skipped. */
    int64_t audioSkipToMicros;
    int audioSourceChannels;
    int audioOutChannels;
    int audioSampleRate;
    /* Downmix coefficients, [outChannel][sourceChannel]; identity for 1 and 2 channels. */
    float downmix[2][64];
    int downmixActive;

    /* ---- subtitles, guarded by subtitleLock; the queue and the epoch by demuxLock
     *
     * The subtitle side is the one consumer that NEVER demultiplexes. Video and audio each run the
     * demux loop when their queue is empty, which is right for a track that is read continuously
     * and would be ruinous here: answering "is there a cue at t" by demuxing forward until a
     * subtitle packet turns up means reading the whole gap between two lines: minutes of a film,
     * for a track that is silent by design. So the demux loop queues subtitle packets it meets and
     * readCue drains only what is already there. The consequence is that cues follow the pictures:
     * a container nobody reads video from produces none, which is stated on the SPI rather than
     * discovered. */
    pthread_mutex_t subtitleLock;
    int subtitleStreams[MAX_SUBTITLE_TRACKS];
    int subtitleTrackCount;
    /* The subtitle stream the open decoder belongs to, or -1 for none, which is what a container
     * opens with. Subtitles are off until an application asks for them, because whether a viewer
     * wants them is not a fact about the file. */
    int subtitleStream;
    int selectedSubtitleTrack;
    AVCodecContext *subtitleCodec;
    PacketQueue subtitleQueue;
    int64_t droppedSubtitle;
    /* Bumped wherever the cues a consumer is holding stop being about where the film is: a real
     * container move, and a change of track. The consumer compares it and empties its window, which
     * is the whole of "a scrub must not leave a cue from where the film used to be on the screen". */
    int64_t subtitleEpoch;
    /* Set beside the epoch and consumed by readCue. It exists because of the lock order: the flush
     * belongs to the subtitle codec and placeDemuxer runs holding demuxLock, which may not take
     * another. So the seek records that a flush is owed and the next read performs it. */
    int subtitleNeedsFlush;
} Player;

/* ------------------------------------------------------------------ library init */

/*
 * FFmpeg logs to stderr by default, at a level that reports the encoder's average quantiser and
 * every concealed error in a damaged file. None of it reaches the caller, all of it reaches a
 * build log, and the tests that decode deliberately corrupt input would print pages of it, so it
 * is off, and what a caller needs instead is on the exception: every failure carries av_strerror's
 * own words.
 *
 * LIMN_FFMPEG_LOG turns it back on for a debugging session, taking any of FFmpeg's level names.
 */
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void) vm;
    (void) reserved;
    const char *level = getenv("LIMN_FFMPEG_LOG");
    if (level == NULL) {
        av_log_set_level(AV_LOG_QUIET);
    } else if (strcmp(level, "trace") == 0) {
        av_log_set_level(AV_LOG_TRACE);
    } else if (strcmp(level, "debug") == 0) {
        av_log_set_level(AV_LOG_DEBUG);
    } else if (strcmp(level, "verbose") == 0) {
        av_log_set_level(AV_LOG_VERBOSE);
    } else if (strcmp(level, "warning") == 0) {
        av_log_set_level(AV_LOG_WARNING);
    } else {
        av_log_set_level(AV_LOG_INFO);
    }
    return JNI_VERSION_1_8;
}

/* ------------------------------------------------------------------ errors */

static void throwFfmpeg(JNIEnv *env, const char *message, int error) {
    char detail[AV_ERROR_MAX_STRING_SIZE + 256];
    if (error != 0) {
        char reason[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(error, reason, sizeof(reason));
        snprintf(detail, sizeof(detail), "%s: %s", message, reason);
    } else {
        snprintf(detail, sizeof(detail), "%s", message);
    }
    jclass type = (*env)->FindClass(env, "limn/video/ffmpeg/FfmpegException");
    if (type == NULL) {
        /* The class is in the same jar as this library; if it cannot be found the classpath is
         * broken and a RuntimeException is the most honest thing left. */
        (*env)->ExceptionClear(env);
        type = (*env)->FindClass(env, "java/lang/RuntimeException");
        if (type == NULL) {
            return;
        }
    }
    (*env)->ThrowNew(env, type, detail);
}

/* ------------------------------------------------------------------ packet queues */

static void queueClear(PacketQueue *queue) {
    PacketNode *node = queue->head;
    while (node != NULL) {
        PacketNode *next = node->next;
        av_packet_free(&node->packet);
        free(node);
        node = next;
    }
    queue->head = NULL;
    queue->tail = NULL;
    queue->count = 0;
    queue->bytes = 0;
}

static AVPacket *queuePop(PacketQueue *queue) {
    PacketNode *node = queue->head;
    if (node == NULL) {
        return NULL;
    }
    queue->head = node->next;
    if (queue->head == NULL) {
        queue->tail = NULL;
    }
    queue->count--;
    queue->bytes -= node->packet->size;
    AVPacket *packet = node->packet;
    free(node);
    return packet;
}

/* Takes ownership of `packet` on every path, including the drop and the allocation failure, so a
 * caller never has to reason about whether a push freed what it handed over.
 *
 * @return 1 if a packet was dropped to make room, 0 otherwise
 */
static int queuePush(PacketQueue *queue, AVPacket *packet) {
    int dropped = 0;
    PacketNode *node = malloc(sizeof(PacketNode));
    if (node == NULL) {
        av_packet_free(&packet);
        return 0;
    }
    node->packet = packet;
    node->next = NULL;
    while (queue->count >= QUEUE_MAX_PACKETS || queue->bytes >= QUEUE_MAX_BYTES) {
        AVPacket *oldest = queuePop(queue);
        if (oldest == NULL) {
            break;
        }
        av_packet_free(&oldest);
        dropped = 1;
    }
    if (queue->tail == NULL) {
        queue->head = node;
    } else {
        queue->tail->next = node;
    }
    queue->tail = node;
    queue->count++;
    queue->bytes += packet->size;
    return dropped;
}

/*
 * Returns the next packet for `wantStream`, demuxing as far as it takes and queueing whatever it
 * meets for the other track. NULL means the input ended, never "not right now", because the
 * queues drop rather than refuse, so this always makes progress.
 *
 * Caller must NOT hold demuxLock. Takes and releases it.
 */
static AVPacket *pullPacket(Player *player, int wantStream) {
    pthread_mutex_lock(&player->demuxLock);
    PacketQueue *mine = wantStream == player->videoStream ? &player->videoQueue
                                                          : &player->audioQueue;
    AVPacket *ready = queuePop(mine);
    if (ready != NULL) {
        pthread_mutex_unlock(&player->demuxLock);
        return ready;
    }
    while (!player->demuxEnded) {
        AVPacket *packet = av_packet_alloc();
        if (packet == NULL) {
            break;
        }
        int result = av_read_frame(player->format, packet);
        if (result < 0) {
            /* Every negative result ends the input here, including a mid-file read error: a
             * container that cannot be read further has no further packets, and reporting the end
             * is what a consumer can act on. */
            av_packet_free(&packet);
            player->demuxEnded = 1;
            break;
        }
        if (packet->stream_index == wantStream) {
            pthread_mutex_unlock(&player->demuxLock);
            return packet;
        }
        if (packet->stream_index == player->videoStream) {
            player->droppedVideo += queuePush(&player->videoQueue, packet);
        } else if (packet->stream_index == player->audioStream && player->audioClaimed) {
            player->droppedAudio += queuePush(&player->audioQueue, packet);
        } else if (packet->stream_index == player->subtitleStream) {
            /* The selected subtitle track rides here and is pulled nowhere else; see the subtitle
             * section of Player for why that direction is the only safe one. */
            player->droppedSubtitle += queuePush(&player->subtitleQueue, packet);
        } else {
            /* Everything else: every audio stream that is not the selected one, every subtitle
             * stream that is not the selected one, and every data stream there may be. Freed here
             * rather than queued, because a track nobody is decoding must cost nothing and a
             * release carries one of each per language. */
            av_packet_free(&packet);
        }
    }
    pthread_mutex_unlock(&player->demuxLock);
    return NULL;
}

/* ------------------------------------------------------------------ timestamps */

/*
 * Container ticks to microseconds, in integers.
 *
 * A stream's timestamps are in its own AVStream.time_base (a rational, typically 1/90000 or
 * 1/timescale), and VideoFrame publishes microseconds. av_rescale_q does it exactly and with
 * rounding that does not accumulate; the same conversion written as a multiply through double
 * drifts, and drift is precisely the failure VideoClock's rational-rate arithmetic exists to
 * avoid.
 *
 * The container's start_time is subtracted from BOTH tracks, in microseconds, after the rescale.
 * That is what puts them on one timeline: subtracting each stream's own start_time instead would
 * zero them independently and hide a real offset between picture and sound, which is the whole
 * quantity AudioMasterClock is judging.
 */
static int64_t toMicros(Player *player, AVStream *stream, int64_t timestamp) {
    if (timestamp == AV_NOPTS_VALUE) {
        return INT64_MIN; /* VideoFrame.PTS_UNKNOWN */
    }
    int64_t micros = av_rescale_q(timestamp, stream->time_base, AV_TIME_BASE_Q);
    if (player->startTimeMicros != AV_NOPTS_VALUE) {
        micros -= player->startTimeMicros;
    }
    return micros;
}

/* ------------------------------------------------------------------ buffer cache */

static jobject cachedBuffer(JNIEnv *env, Player *player, const uint8_t *address, jlong capacity) {
    int freeIndex = -1;
    int oldestIndex = 0;
    int64_t oldestUse = INT64_MAX;
    for (int i = 0; i < BUFFER_CACHE_SIZE; i++) {
        BufferEntry *entry = &player->bufferCache[i];
        if (entry->buffer == NULL) {
            if (freeIndex < 0) {
                freeIndex = i;
            }
            continue;
        }
        if (entry->address == address && entry->capacity == capacity) {
            entry->used = ++player->bufferClock;
            return entry->buffer;
        }
        if (entry->used < oldestUse) {
            oldestUse = entry->used;
            oldestIndex = i;
        }
    }
    jobject local = (*env)->NewDirectByteBuffer(env, (void *) (uintptr_t) address, capacity);
    if (local == NULL) {
        return NULL;
    }
    jobject global = (*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
    if (global == NULL) {
        return NULL;
    }
    int index = freeIndex >= 0 ? freeIndex : oldestIndex;
    BufferEntry *entry = &player->bufferCache[index];
    if (entry->buffer != NULL) {
        /* Safe while Java still holds a picture over this address: VideoFrame.Writer.setPlane
         * kept a read-only VIEW of its own, and a view keeps its origin alive independently of
         * this reference. Dropping it here only evicts it from this cache. */
        (*env)->DeleteGlobalRef(env, entry->buffer);
    }
    entry->address = address;
    entry->capacity = capacity;
    entry->buffer = global;
    entry->used = ++player->bufferClock;
    return global;
}

static void clearBufferCache(JNIEnv *env, Player *player) {
    for (int i = 0; i < BUFFER_CACHE_SIZE; i++) {
        if (player->bufferCache[i].buffer != NULL) {
            (*env)->DeleteGlobalRef(env, player->bufferCache[i].buffer);
            player->bufferCache[i].buffer = NULL;
        }
    }
}

/* ------------------------------------------------------------------ geometry */

static int planeCountOf(int format) {
    return (format == FORMAT_NV12 || format == FORMAT_P010) ? 2 : 3;
}

static int mapPixelFormat(enum AVPixelFormat format) {
    switch (format) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P:
            return FORMAT_I420;
        case AV_PIX_FMT_NV12:
            return FORMAT_NV12;
        case AV_PIX_FMT_YUV444P:
        case AV_PIX_FMT_YUVJ444P:
            return FORMAT_I444;
        /* Little-endian only, deliberately. Every decoder on every platform this ships to produces
         * the LE variant; a BE picture handed over as if it were LE is uniform noise, so the format
         * is refused by name rather than guessed at. */
        case AV_PIX_FMT_YUV420P10LE:
            return FORMAT_I420_10LE;
        case AV_PIX_FMT_YUV444P10LE:
            return FORMAT_I444_10LE;
        default:
            return -1;
    }
}

/* Bytes one component of this layout occupies. The 10-bit layouts store a code right-justified in
 * a 16-bit word, so every byte width and every stride check below is twice what the sample count
 * would suggest. */
static int componentBytes(int format) {
    return (format == FORMAT_I420_10LE || format == FORMAT_I444_10LE
            || format == FORMAT_P010) ? 2 : 1;
}

static int isFullChroma(int format) {
    return format == FORMAT_I444 || format == FORMAT_I444_10LE;
}

static int isSubsampled(int format) {
    return format == FORMAT_I420 || format == FORMAT_NV12 || format == FORMAT_I420_10LE
            || format == FORMAT_P010;
}

/* PixelFormat.planeWidth times PixelFormat.bytesPerSample, computed the same way on this side:
 * chroma rounds UP, so an odd width keeps its last column. Getting this wrong on one side of the
 * boundary is the coloured stripe down one edge that ADR 007 is about. */
static int planeByteWidth(int format, int plane, int width) {
    int bytes = componentBytes(format);
    if (plane == 0) {
        return width * bytes;
    }
    if (format == FORMAT_NV12 || format == FORMAT_P010) {
        /* Two components in one sample, so the chroma row is the width rounded up to an even
         * number of PIXELS times two components: one byte WIDER than the luma row at an odd
         * width, which is the arithmetic that defeats "chroma rows are smaller". */
        return ((width + 1) / 2) * 2 * bytes;
    }
    if (isSubsampled(format)) {
        return ((width + 1) / 2) * bytes;
    }
    return width * bytes;
}

static int planeRows(int format, int plane, int height) {
    if (plane == 0 || isFullChroma(format)) {
        return height;
    }
    return (height + 1) / 2;
}

/* ------------------------------------------------------------------ hardware decode
 *
 * WHAT A HARDWARE PICTURE IS, AND WHY IT IS NOT A SECOND KIND OF SLOT
 *
 * With an accelerator attached, avcodec_receive_frame fills a slot exactly as before: same
 * AVFrame, same pool, same reference counting, same handback through releaseVideo. What changes is
 * only what is INSIDE it: data[3] is a CVPixelBufferRef instead of data[0..2] being sample memory.
 * So the slot machinery is untouched and the binding epoch keeps meaning what it meant; a picture
 * simply crosses as one handle rather than as three addresses.
 *
 * The handle Java gets is the IOSurface and not the CVPixelBuffer, because the IOSurface is what
 * CGLTexImageIOSurface2D takes. Unwrapping it here is what keeps CoreVideo out of the backend.
 *
 * WHAT A CONSUMER WITHOUT A DEVICE DOES is downloadVideo, which is route A:
 * av_hwframe_transfer_data into a software frame this slot keeps, after which the slot's planes are
 * that frame's and planeBuffer answers as it always did. It is a whole picture across the bus, and
 * it is the price of not being broken by a decoder the consumer did not choose.
 */

#ifdef __APPLE__

/*
 * Chooses the hardware pixel format when libavcodec offers it, and otherwise takes whatever it
 * would have taken. Called on the first picture, not at open, which is why `hardware` is checked
 * again per picture below rather than trusted from open.
 */
static enum AVPixelFormat pickHardwareFormat(AVCodecContext *context,
                                             const enum AVPixelFormat *formats) {
    (void) context;
    for (const enum AVPixelFormat *candidate = formats; *candidate != AV_PIX_FMT_NONE; candidate++) {
        if (*candidate == AV_PIX_FMT_VIDEOTOOLBOX) {
            return *candidate;
        }
    }
    return formats[0];
}

/* Whether this decoder can actually be driven by VideoToolbox through a device context. A configure
 * flag is a claim (ADR 015 §2); this asks the linked decoder. */
static int codecTakesVideoToolbox(const AVCodec *codec) {
    for (int index = 0;; index++) {
        const AVCodecHWConfig *config = avcodec_get_hw_config(codec, index);
        if (config == NULL) {
            return 0;
        }
        if (config->device_type == AV_HWDEVICE_TYPE_VIDEOTOOLBOX
                && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0) {
            return 1;
        }
    }
}

/*
 * The layout a VideoToolbox picture will have, from the depth the container declared.
 *
 * Only two exist and they are the two PixelFormat now carries: 8-bit is NV12 and 10-bit is P010,
 * each in a video-range and a full-range spelling that differ by nothing this side cares about
 * (the range is carried on VideoColor and read from the container, not from the surface type).
 *
 * @return a FORMAT_* value, or -1 when the coded format is one no accelerator here produces
 */
static int hardwareFormatFor(int softwareFormat, uint32_t *surfaceType, uint32_t *alternate) {
    if (softwareFormat == FORMAT_I420 || softwareFormat == FORMAT_NV12) {
        *surfaceType = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        *alternate = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
        return FORMAT_NV12;
    }
    if (softwareFormat == FORMAT_I420_10LE || softwareFormat == FORMAT_P010) {
        *surfaceType = kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange;
        *alternate = kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
        return FORMAT_P010;
    }
    return -1;
}

/*
 * Decodes one picture and answers whether it actually came back as a hardware surface.
 *
 * THIS IS NOT PARANOIA, IT IS THE ONLY MOMENT THE ANSWER EXISTS. get_format runs on the first
 * picture, not at avcodec_open2, and if the accelerator then refuses the stream (an unsupported
 * profile, a level this machine's decoder does not do, a codec VideoToolbox has in its table and
 * not in its silicon), libavcodec quietly asks again without the hardware format and decodes in
 * software. Everything would work, and the layout the SPI fixed at open would be a lie: NV12
 * promised, planar I420 delivered, and every picture refused by readVideo's own format check with
 * a message about the stream changing format when it never did.
 *
 * So the probe happens here, where the answer can still change what open() reports. It costs one
 * picture (which avformat_find_stream_info already decoded several of), and the input is then put
 * back at the start.
 *
 * Called from open() before any other thread exists.
 */
static int confirmsHardware(Player *player) {
    AVFrame *frame = player->slots[0].frame;
    int hardware = 0;
    for (int guard = 0; guard < 256; guard++) {
        int result = avcodec_receive_frame(player->videoCodec, frame);
        if (result == 0) {
            hardware = frame->format == AV_PIX_FMT_VIDEOTOOLBOX;
            av_frame_unref(frame);
            break;
        }
        if (result != AVERROR(EAGAIN)) {
            break; /* the end, or an error the real read will meet and report properly */
        }
        AVPacket *packet = pullPacket(player, player->videoStream);
        if (packet == NULL) {
            if (avcodec_send_packet(player->videoCodec, NULL) < 0) {
                break;
            }
            continue;
        }
        result = avcodec_send_packet(player->videoCodec, packet);
        /* Freed rather than kept on EAGAIN, unlike the real read: everything decoded here is
         * thrown away and the input is rewound, so a lost packet costs nothing. */
        av_packet_free(&packet);
        if (result < 0 && result != AVERROR(EAGAIN)) {
            break;
        }
    }
    return hardware;
}

/* Puts the input back where the probe found it. The audio track is not open yet, so its packets
 * were freed as they were met rather than queued. */
static void rewindAfterProbe(Player *player) {
    AVStream *stream = player->format->streams[player->videoStream];
    int64_t base = player->startTimeMicros != AV_NOPTS_VALUE ? player->startTimeMicros : 0;
    av_seek_frame(player->format, player->videoStream,
                  av_rescale_q(base, AV_TIME_BASE_Q, stream->time_base), AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(player->videoCodec);
    av_packet_free(&player->pendingVideo);
    queueClear(&player->videoQueue);
    queueClear(&player->audioQueue);
    player->demuxEnded = 0;
    player->videoEnded = 0;
}

#endif /* __APPLE__ */

/* ------------------------------------------------------------------ open */

static int findChannel(const AVChannelLayout *layout, enum AVChannel channel) {
    int index = av_channel_layout_index_from_channel(layout, channel);
    return index;
}

/*
 * Builds the matrix that folds a multi-channel track down to the stereo the engine will admit.
 *
 * OpenAlAudio refuses any channel count but 1 and 2 at admission, so a 5.1 track has three
 * possible answers: refuse the file, play it silent, or fold it. Folding is what every player
 * does and it is the only one that plays the film, so it is what happens here.
 *
 * The coefficients are ITU-R BS.775's: the centre and the matching surround enter each front
 * channel at -3 dB, and the LFE is dropped (it carries no content a stereo pair should reproduce,
 * and adding it is how a downmix turns into a rumble). Where the coefficients for one output
 * channel sum above 1 they are scaled so they do not, which is what stops a loud passage
 * clipping: the same normalisation libswresample applies, done here because pulling in
 * libswresample to multiply six numbers would be the largest single addition to the payload.
 */
static void buildDownmix(Player *player, const AVChannelLayout *layout) {
    player->downmixActive = 0;
    memset(player->downmix, 0, sizeof(player->downmix));
    int channels = player->audioSourceChannels;
    if (channels <= 2 || channels > 64) {
        return;
    }
    static const float ATTENUATED = 0.7071068f;
    int left = findChannel(layout, AV_CHAN_FRONT_LEFT);
    int right = findChannel(layout, AV_CHAN_FRONT_RIGHT);
    int centre = findChannel(layout, AV_CHAN_FRONT_CENTER);
    int backLeft = findChannel(layout, AV_CHAN_BACK_LEFT);
    int backRight = findChannel(layout, AV_CHAN_BACK_RIGHT);
    if (backLeft < 0) {
        backLeft = findChannel(layout, AV_CHAN_SIDE_LEFT);
    }
    if (backRight < 0) {
        backRight = findChannel(layout, AV_CHAN_SIDE_RIGHT);
    }
    /* An unlabelled layout says nothing about which channel is which, so the first two are taken
     * as the front pair and the rest are left out rather than guessed into the mix. */
    if (left < 0 || right < 0) {
        left = 0;
        right = 1;
        centre = backLeft = backRight = -1;
    }
    player->downmix[0][left] = 1.0f;
    player->downmix[1][right] = 1.0f;
    if (centre >= 0) {
        player->downmix[0][centre] = ATTENUATED;
        player->downmix[1][centre] = ATTENUATED;
    }
    if (backLeft >= 0) {
        player->downmix[0][backLeft] = ATTENUATED;
    }
    if (backRight >= 0) {
        player->downmix[1][backRight] = ATTENUATED;
    }
    for (int out = 0; out < 2; out++) {
        float sum = 0;
        for (int in = 0; in < channels; in++) {
            sum += player->downmix[out][in];
        }
        if (sum > 1.0f) {
            for (int in = 0; in < channels; in++) {
                player->downmix[out][in] /= sum;
            }
        }
    }
    player->downmixActive = 1;
}

/*
 * Opens a decoder for `stream`. `player` is NULL for the audio track and non-NULL for the video
 * one; `wantHardware` asks for an accelerator and is honoured only where one exists, because a
 * hardware decode that cannot be had must be a software decode and never a failure to play.
 */
static AVCodecContext *openCodec(JNIEnv *env, AVStream *stream, const char *what,
                                 Player *player, int wantHardware) {
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (codec == NULL) {
        char message[160];
        snprintf(message, sizeof(message),
                 "this build of FFmpeg has no %s decoder for '%s'", what,
                 avcodec_get_name(stream->codecpar->codec_id));
        throwFfmpeg(env, message, 0);
        return NULL;
    }
    AVCodecContext *context = avcodec_alloc_context3(codec);
    if (context == NULL) {
        throwFfmpeg(env, "cannot allocate a codec context", AVERROR(ENOMEM));
        return NULL;
    }
    int result = avcodec_parameters_to_context(context, stream->codecpar);
    if (result < 0) {
        avcodec_free_context(&context);
        throwFfmpeg(env, "cannot apply the stream's parameters", result);
        return NULL;
    }
    /* Without this the timestamps on decoded frames are in the codec's time base rather than the
     * stream's, and every rescale afterwards is against the wrong denominator. */
    context->pkt_timebase = stream->time_base;
    context->thread_count = 0; /* as many as the machine has */

#ifdef __APPLE__
    /* Attached BEFORE avcodec_open2, which is the only moment it can be: the decoder chooses its
     * internal path there, and a device context handed over afterwards is ignored in silence. */
    if (wantHardware && player != NULL && codecTakesVideoToolbox(codec)) {
        if (av_hwdevice_ctx_create(&player->hwDevice, AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
                                   NULL, NULL, 0) >= 0) {
            context->hw_device_ctx = av_buffer_ref(player->hwDevice);
            context->get_format = pickHardwareFormat;
            player->hardware = 1;
        }
        /* A failure to create the device is not a failure to play: the decoder opens without one
         * and produces samples, which is what every machine without an accelerator does. */
    }
#else
    (void) wantHardware;
    (void) player;
#endif

    result = avcodec_open2(context, codec, NULL);
    if (result < 0) {
        avcodec_free_context(&context);
        throwFfmpeg(env, "cannot open the decoder", result);
        return NULL;
    }
    return context;
}

/*
 * Opens audio track `track` (a position in `audioStreams`, not a container stream index) and
 * makes it the one this handle decodes, replacing whatever was open before.
 *
 * The new decoder is built BEFORE anything is torn down, so a track this build cannot decode
 * leaves the handle exactly as it was and throws. What follows the swap is the part that is easy
 * to leave out and impossible to see afterwards:
 *
 *   audioGeneration++   every call from the consumer that had the old track is answered from here
 *                       on rather than acted on, including its close, which would otherwise
 *                       unclaim the track somebody is now listening to
 *   queueClear          what was queued belongs to a stream this codec would refuse
 *   seekTakenAudio=1    ONLY when replacing. One container placement serves two consumers, and the
 *                       packets it produced for the audio side were queued for the track being
 *                       replaced and then freed, so the next seek from this side must really move
 *                       the container rather than find its target already current. At the first
 *                       open nothing has been placed and nothing has been consumed, so the flag
 *                       stays where it is and the ordinary two-track dedupe still holds.
 *
 * @return 1 on success; 0 with a pending Java exception otherwise
 */
static int openAudioTrack(JNIEnv *env, Player *player, int track) {
    int streamIndex = player->audioStreams[track];
    AVStream *stream = player->format->streams[streamIndex];
    AVCodecContext *codec = openCodec(env, stream, "audio", NULL, 0);
    if (codec == NULL) {
        return 0;
    }
    AVFrame *frame = av_frame_alloc();
    int sourceChannels = codec->ch_layout.nb_channels;
    int sampleRate = codec->sample_rate;
    if (frame == NULL || sourceChannels < 1 || sampleRate < 1) {
        avcodec_free_context(&codec);
        av_frame_free(&frame);
        throwFfmpeg(env, "the audio track declares no channels or no sample rate", 0);
        return 0;
    }

    pthread_mutex_lock(&player->audioLock);
    avcodec_free_context(&player->audioCodec);
    av_packet_free(&player->pendingAudio);
    av_frame_free(&player->audioFrame);
    player->audioCodec = codec;
    player->audioFrame = frame;
    player->audioHasFrame = 0;
    player->audioFrameOffset = 0;
    player->audioEnded = 0;
    player->audioSkipToMicros = INT64_MIN;
    player->audioSourceChannels = sourceChannels;
    player->audioSampleRate = sampleRate;
    player->audioOutChannels = sourceChannels >= 2 ? 2 : 1;
    buildDownmix(player, &codec->ch_layout);

    pthread_mutex_lock(&player->demuxLock);
    if (player->selectedAudioTrack >= 0) {
        player->seekTakenAudio = 1;
    }
    player->audioStream = streamIndex;
    player->selectedAudioTrack = track;
    player->audioGeneration++;
    queueClear(&player->audioQueue);
    player->audioClaimed = 1;
    pthread_mutex_unlock(&player->demuxLock);
    pthread_mutex_unlock(&player->audioLock);
    return 1;
}

/*
 * Whether the codec behind `parameters` produces text rather than a paletted bitmap.
 *
 * Read from the CODEC DESCRIPTOR and not from whether a decoder is linked, because the two answer
 * different questions and a caller needs both: "this build cannot decode it" is a property of the
 * build and changes when the configure line does, while "this SPI does not carry it" is a property
 * of the format and never changes. A PGS track in a build that grew a pgssub decoder must still be
 * refused, and by the second reason.
 */
static int isTextSubtitle(const AVCodecParameters *parameters) {
    const AVCodecDescriptor *descriptor = avcodec_descriptor_get(parameters->codec_id);
    return descriptor != NULL && (descriptor->props & AV_CODEC_PROP_TEXT_SUB) != 0;
}

/*
 * Opens subtitle track `track` (a position in `subtitleStreams`) or, for track < 0, closes
 * whichever was open and leaves the container with none.
 *
 * The new decoder is built before the old one is torn down, so a track this build cannot decode
 * leaves the handle as it was and throws. The epoch is bumped for the same reason a seek bumps it:
 * the cues a consumer is holding are the other track's.
 *
 * @return 1 on success; 0 with a pending Java exception otherwise
 */
static int openSubtitleTrack(JNIEnv *env, Player *player, int track) {
    AVCodecContext *codec = NULL;
    int streamIndex = -1;
    if (track >= 0) {
        streamIndex = player->subtitleStreams[track];
        AVStream *stream = player->format->streams[streamIndex];
        if (!isTextSubtitle(stream->codecpar)) {
            /* Refused by name rather than opened and drawn as nothing. A paletted bitmap is a
             * picture with its own rectangle and palette, and carrying one would mean a second
             * image lifetime across this boundary and a placement contract in the video's own
             * coordinate space, which is the positioning policy this SPI exists not to hold. */
            char message[160];
            snprintf(message, sizeof(message),
                     "subtitle track %d is '%s', which is a bitmap format; this SPI carries text "
                     "cues only", track, avcodec_get_name(stream->codecpar->codec_id));
            throwFfmpeg(env, message, 0);
            return 0;
        }
        codec = openCodec(env, stream, "subtitle", NULL, 0);
        if (codec == NULL) {
            return 0;
        }
    }

    pthread_mutex_lock(&player->subtitleLock);
    avcodec_free_context(&player->subtitleCodec);
    player->subtitleCodec = codec;

    pthread_mutex_lock(&player->demuxLock);
    player->subtitleStream = streamIndex;
    player->selectedSubtitleTrack = track;
    queueClear(&player->subtitleQueue);
    player->subtitleEpoch++;
    player->subtitleNeedsFlush = 0; /* a fresh decoder has nothing to flush */
    pthread_mutex_unlock(&player->demuxLock);
    pthread_mutex_unlock(&player->subtitleLock);
    return 1;
}

JNIEXPORT jlong JNICALL Java_limn_video_ffmpeg_FfmpegNative_open(
        JNIEnv *env, jclass cls, jstring pathString, jboolean wantAudio, jint slots,
        jboolean wantHardware) {
    (void) cls;
    if (slots < 1 || slots > MAX_SLOTS) {
        throwFfmpeg(env, "slot count out of range", 0);
        return 0;
    }
    const char *path = (*env)->GetStringUTFChars(env, pathString, NULL);
    if (path == NULL) {
        return 0;
    }

    Player *player = calloc(1, sizeof(Player));
    if (player == NULL) {
        (*env)->ReleaseStringUTFChars(env, pathString, path);
        throwFfmpeg(env, "out of memory", 0);
        return 0;
    }
    player->videoStream = -1;
    player->audioStream = -1;
    player->defaultAudioTrack = -1;
    player->selectedAudioTrack = -1;
    player->subtitleStream = -1;
    player->selectedSubtitleTrack = -1;
    /* Not 0, which calloc would have given: 0 is a legitimate seek target, and a container that
     * started out claiming to be placed there would answer the first rewind after a hundred
     * pictures by flushing a codec and moving nothing. */
    player->seekTargetMicros = INT64_MIN;
    player->videoSkipToMicros = INT64_MIN;
    player->audioSkipToMicros = INT64_MIN;
    pthread_mutex_init(&player->demuxLock, NULL);
    pthread_mutex_init(&player->videoLock, NULL);
    pthread_mutex_init(&player->audioLock, NULL);
    pthread_mutex_init(&player->subtitleLock, NULL);

    AVDictionary *options = NULL;
    /* Belt and braces over the configure line. The library is built --disable-protocols
     * --enable-protocol=file, so there is no other protocol to reach; this says the same thing
     * again at run time, so a library someone rebuilt with the network protocols on still cannot
     * be steered at a URL through a method whose parameter is called `file`. */
    av_dict_set(&options, "protocol_whitelist", "file", 0);

    int result = avformat_open_input(&player->format, path, NULL, &options);
    av_dict_free(&options);
    (*env)->ReleaseStringUTFChars(env, pathString, path);
    if (result < 0) {
        pthread_mutex_destroy(&player->demuxLock);
        pthread_mutex_destroy(&player->videoLock);
        pthread_mutex_destroy(&player->audioLock);
        free(player);
        throwFfmpeg(env, "cannot open the input", result);
        return 0;
    }

    /* This reads and decodes real data to fill in what the header did not say, which is exactly
     * why it is here and never in supports(). */
    result = avformat_find_stream_info(player->format, NULL);
    if (result < 0) {
        goto fail;
    }

    player->startTimeMicros = player->format->start_time;
    player->videoStream =
            av_find_best_stream(player->format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (player->videoStream < 0) {
        avformat_close_input(&player->format);
        pthread_mutex_destroy(&player->demuxLock);
        pthread_mutex_destroy(&player->videoLock);
        pthread_mutex_destroy(&player->audioLock);
        free(player);
        throwFfmpeg(env, "the container holds no video stream", 0);
        return 0;
    }

    AVStream *video = player->format->streams[player->videoStream];

    /* Read before the codec is opened, because whether an accelerator is worth attaching depends on
     * what the container says the samples are: a layout this SPI cannot carry is a software decode
     * that then fails cleanly, not a hardware decode of something nobody can describe. */
    player->width = video->codecpar->width;
    player->height = video->codecpar->height;
    if (player->width < 1 || player->width > MAX_DIMENSION
            || player->height < 1 || player->height > MAX_DIMENSION) {
        char message[128];
        snprintf(message, sizeof(message), "the header claims a %dx%d picture",
                 player->width, player->height);
        avformat_close_input(&player->format);
        pthread_mutex_destroy(&player->demuxLock);
        pthread_mutex_destroy(&player->videoLock);
        pthread_mutex_destroy(&player->audioLock);
        free(player);
        throwFfmpeg(env, message, 0);
        return 0;
    }

    player->avPixelFormat = (enum AVPixelFormat) video->codecpar->format;
    player->pixelFormat = mapPixelFormat(player->avPixelFormat);
#ifdef __APPLE__
    /* The layout the container coded, captured before a hardware decoder replaces
     * player->pixelFormat below. Declared inside the branch that reads it: everywhere else there
     * is no accelerator to choose and an unused variable is a -Werror build failure. */
    int codedFormat = player->pixelFormat;
    int hardwareFormat = -1;
    uint32_t surfaceType = 0;
    uint32_t alternateSurfaceType = 0;
    if (codedFormat >= 0) {
        hardwareFormat = hardwareFormatFor(codedFormat, &surfaceType, &alternateSurfaceType);
    }
    int tryHardware = wantHardware && hardwareFormat >= 0;
#else
    int tryHardware = 0;
    (void) wantHardware;
#endif

    player->videoCodec = openCodec(env, video, "video", player, tryHardware);
    if (player->videoCodec == NULL) {
        goto failThrown;
    }

#ifdef __APPLE__
    if (player->hardware) {
        /* The decoder now produces IOSurfaces of this layout rather than the coded planar one, and
         * the SPI fixes the layout at open, so this is what is reported, and every picture is
         * checked against it. */
        player->pixelFormat = hardwareFormat;
        player->expectedSurfaceType = surfaceType;
        player->expectedSurfaceTypeAlt = alternateSurfaceType;
    }
#endif
    if (player->pixelFormat < 0) {
        char message[160];
        const char *name = av_get_pix_fmt_name(player->avPixelFormat);
        snprintf(message, sizeof(message),
                 "pixel format '%s' is not one this SPI carries (8-bit I420, NV12, I444; "
                 "10-bit I420 and I444, little-endian; P010 from a hardware decoder)",
                 name != NULL ? name : "unknown");
        avcodec_free_context(&player->videoCodec);
        av_buffer_unref(&player->hwDevice);
        avformat_close_input(&player->format);
        pthread_mutex_destroy(&player->demuxLock);
        pthread_mutex_destroy(&player->videoLock);
        pthread_mutex_destroy(&player->audioLock);
        free(player);
        throwFfmpeg(env, message, 0);
        return 0;
    }
    player->planeCount = planeCountOf(player->pixelFormat);

    player->slotCount = slots;
    for (int i = 0; i < slots; i++) {
        player->slots[i].frame = av_frame_alloc();
        if (player->slots[i].frame == NULL) {
            result = AVERROR(ENOMEM);
            goto fail;
        }
    }

#ifdef __APPLE__
    int probed = player->hardware;
    if (probed && !confirmsHardware(player)) {
        /* The accelerator was attached and then declined the stream. What open() reports has to be
         * what the pictures will be, so the codec is thrown away and opened again without it, and
         * isHardwareDecoding() answers false, which is the whole reason it is a question and not an
         * assumption. */
        avcodec_free_context(&player->videoCodec);
        av_buffer_unref(&player->hwDevice);
        player->hardware = 0;
        player->pixelFormat = codedFormat;
        player->expectedSurfaceType = 0;
        player->expectedSurfaceTypeAlt = 0;
        player->planeCount = planeCountOf(player->pixelFormat);
        player->videoCodec = openCodec(env, video, "video", player, 0);
        if (player->videoCodec == NULL) {
            goto failThrown;
        }
    }
    if (probed) {
        rewindAfterProbe(player);
    }
#endif

    /* Every audio stream, listed whether or not this build can decode it and whether or not the
     * caller asked for sound: what a container HOLDS is a fact about the file, and a listing that
     * hid the tracks a trimmed build cannot play would report a two-language film as having one
     * language. Whether a track can be selected is answered per track by describeAudioTrack. */
    for (unsigned int i = 0;
            i < player->format->nb_streams && player->audioTrackCount < MAX_AUDIO_TRACKS; i++) {
        if (player->format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            player->audioStreams[player->audioTrackCount++] = (int) i;
        }
    }
    int bestAudio = av_find_best_stream(player->format, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    for (int track = 0; track < player->audioTrackCount; track++) {
        if (player->audioStreams[track] == bestAudio) {
            player->defaultAudioTrack = track;
            break;
        }
    }
    if (player->defaultAudioTrack < 0 && player->audioTrackCount > 0) {
        /* Only reachable when the container holds more audio streams than are listed and the best
         * one fell off the end. The first listed track is then the default, because a film that
         * plays the wrong language is better than one that plays silent. */
        player->defaultAudioTrack = 0;
    }

    /* Every subtitle stream, listed for the same reason the audio ones are and selected for none of
     * them: NOTHING is opened here. Whether a viewer wants subtitles is not a fact about the file,
     * and a container that decoded a track nobody asked for would spend memory queueing its packets
     * for a consumer that never polls. The application selects one or the container carries none. */
    for (unsigned int i = 0;
            i < player->format->nb_streams && player->subtitleTrackCount < MAX_SUBTITLE_TRACKS;
            i++) {
        if (player->format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            player->subtitleStreams[player->subtitleTrackCount++] = (int) i;
        }
    }

    if (wantAudio && player->defaultAudioTrack >= 0) {
        /* The default one, which is av_find_best_stream's answer, so a caller that asks for
         * nothing gets exactly what it got before this file could list tracks at all. */
        if (!openAudioTrack(env, player, player->defaultAudioTrack)) {
            /* A container whose video is fine and whose audio is a codec this build does not have
             * is played without sound, which is better than not played. */
            (*env)->ExceptionClear(env);
        }
    }

    return (jlong) (uintptr_t) player;

fail:
    throwFfmpeg(env, "cannot read the container", result);
failThrown:
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (player->slots[i].frame != NULL) {
            av_frame_free(&player->slots[i].frame);
        }
        av_frame_free(&player->slots[i].software);
    }
    avcodec_free_context(&player->videoCodec);
    av_buffer_unref(&player->hwDevice);
    avcodec_free_context(&player->audioCodec);
    av_frame_free(&player->audioFrame);
    avformat_close_input(&player->format);
    pthread_mutex_destroy(&player->demuxLock);
    pthread_mutex_destroy(&player->videoLock);
    pthread_mutex_destroy(&player->audioLock);
    pthread_mutex_destroy(&player->subtitleLock);
    free(player);
    return 0;
}

/* ------------------------------------------------------------------ describe */

/*
 * The quarter turn a recording asks to be displayed at, from the container's display matrix.
 *
 * av_display_rotation_get answers the angle the picture must be rotated ANTICLOCKWISE to be seen
 * upright, in degrees, as a double which can be NaN when the matrix is not a rotation at all. The
 * SPI carries a clockwise quarter turn, so the sign is flipped and anything that is not one of the
 * four is reported as 0; a matrix that also flips or shears is not approximable by an angle, and a
 * mirrored recording shown unmirrored is a defect nobody attributes to the container.
 */
static int rotationOf(AVStream *stream) {
    const AVPacketSideData *side = av_packet_side_data_get(stream->codecpar->coded_side_data,
                                                           stream->codecpar->nb_coded_side_data,
                                                           AV_PKT_DATA_DISPLAYMATRIX);
    if (side == NULL || side->size < 9 * sizeof(int32_t)) {
        return 0;
    }
    double angle = av_display_rotation_get((const int32_t *) side->data);
    if (isnan(angle)) {
        return 0;
    }
    long quarter = lround(-angle / 90.0);
    if (fabs(-angle - quarter * 90.0) > 1.0) {
        return 0; /* not a quarter turn; carrying the nearest one would be a guess */
    }
    quarter %= 4;
    if (quarter < 0) {
        quarter += 4;
    }
    return (int) (quarter * 90);
}

JNIEXPORT void JNICALL Java_limn_video_ffmpeg_FfmpegNative_describe(
        JNIEnv *env, jclass cls, jlong handle, jlongArray out) {
    (void) cls;
    Player *player = (Player *) (uintptr_t) handle;
    AVStream *video = player->format->streams[player->videoStream];

    int matrix;
    switch (player->videoCodec->colorspace) {
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
            matrix = MATRIX_BT601;
            break;
        case AVCOL_SPC_BT709:
            matrix = MATRIX_BT709;
            break;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            matrix = MATRIX_BT2020;
            break;
        default:
            matrix = MATRIX_UNSPECIFIED;
            break;
    }
    int range;
    if (player->videoCodec->color_range == AVCOL_RANGE_MPEG) {
        range = RANGE_LIMITED;
    } else if (player->videoCodec->color_range == AVCOL_RANGE_JPEG
            || player->avPixelFormat == AV_PIX_FMT_YUVJ420P
            || player->avPixelFormat == AV_PIX_FMT_YUVJ444P) {
        range = RANGE_FULL;
    } else {
        range = RANGE_UNSPECIFIED;
    }
    /* Only the two curves that change what a consumer must DO are reported. Every other value
     * (gamma 2.2, sRGB, BT.601, unspecified) is display-referred and lands on SDR, because telling
     * them apart would give a caller a distinction it has no use for and no way to act on. */
    int transfer;
    switch (player->videoCodec->color_trc) {
        case AVCOL_TRC_SMPTE2084:
            transfer = TRANSFER_PQ;
            break;
        case AVCOL_TRC_ARIB_STD_B67:
            transfer = TRANSFER_HLG;
            break;
        default:
            transfer = TRANSFER_SDR;
            break;
    }

    AVRational rate = av_guess_frame_rate(player->format, video, NULL);
    int64_t duration = -1;
    if (player->format->duration != AV_NOPTS_VALUE && player->format->duration >= 0) {
        duration = player->format->duration;
    }

    jlong values[18];
    values[0] = player->width;
    values[1] = player->height;
    values[2] = player->pixelFormat;
    values[3] = matrix;
    values[4] = range;
    values[5] = rate.num > 0 && rate.den > 0 ? rate.num : 0;
    values[6] = rate.num > 0 && rate.den > 0 ? rate.den : 1;
    values[7] = duration;
    values[8] = player->audioStream >= 0 ? 1 : 0;
    values[9] = player->audioOutChannels;
    values[10] = player->audioSampleRate;
    values[11] = player->slotCount;
    values[12] = player->audioSourceChannels;
    values[13] = rotationOf(video);
    values[14] = transfer;
    values[15] = player->hardware;
    /* A count, not a list: the tracks themselves are read one call each, because a variable number
     * of them cannot be carried in an array whose length is fixed on both sides of the boundary. */
    values[16] = player->audioTrackCount;
    /* Likewise a count. There is no "has subtitles" entry beside it because there is nothing for one
     * to mean: a container that holds tracks still shows none until an application picks one. */
    values[17] = player->subtitleTrackCount;
    (*env)->SetLongArrayRegion(env, out, 0, 18, values);
}

/* ------------------------------------------------------------------ the audio tracks */

/*
 * What audio track `track` is: its numbers into `out`, its two names as the returned array.
 *
 * Read from the container's own stream parameters rather than from a decoder, so a track this
 * build cannot decode is still described in full; `out[AT_DECODABLE]` is what says it cannot be
 * selected, and a listing that simply omitted it would report a two-language film as having one
 * language.
 *
 * The strings cross as strings and that is deliberate. ADR 012 §3's allocation discipline is about
 * what crosses PER PICTURE; this is read once, at open, by a caller building a list. Contorting it
 * into a code table would buy nothing and would have to be extended for every codec FFmpeg adds.
 *
 * THE LANGUAGE IS WHATEVER libavformat PUT THERE, and the entry is absent rather than empty when
 * the container states nothing, which is the case a caller must be able to tell apart, so it
 * arrives as a null element and never as "". What the two containers this build reads actually do
 * with an unstated language is asserted in AudioTrackListTest rather than assumed here.
 *
 * @return {codec name, language or null}
 */
JNIEXPORT jobjectArray JNICALL Java_limn_video_ffmpeg_FfmpegNative_describeAudioTrack(
        JNIEnv *env, jclass cls, jlong handle, jint track, jlongArray out) {
    (void) cls;
    Player *player = (Player *) (uintptr_t) handle;
    if (track < 0 || track >= player->audioTrackCount) {
        throwFfmpeg(env, "no such audio track", 0);
        return NULL;
    }
    int streamIndex = player->audioStreams[track];
    AVStream *stream = player->format->streams[streamIndex];
    AVCodecParameters *parameters = stream->codecpar;
    int sourceChannels = parameters->ch_layout.nb_channels;

    jlong values[6];
    values[0] = streamIndex;
    values[1] = sourceChannels;
    /* The same fold the decode path applies, computed the same way: a track's delivered channel
     * count must not depend on whether it happens to be the one that is open. */
    values[2] = sourceChannels >= 2 ? 2 : 1;
    values[3] = parameters->sample_rate;
    values[4] = track == player->defaultAudioTrack;
    values[5] = avcodec_find_decoder(parameters->codec_id) != NULL;
    (*env)->SetLongArrayRegion(env, out, 0, 6, values);

    const char *codecName = avcodec_get_name(parameters->codec_id);
    AVDictionaryEntry *language = av_dict_get(stream->metadata, "language", NULL, 0);

    jclass stringClass = (*env)->FindClass(env, "java/lang/String");
    if (stringClass == NULL) {
        return NULL;
    }
    jobjectArray names = (*env)->NewObjectArray(env, 2, stringClass, NULL);
    if (names == NULL) {
        return NULL;
    }
    jstring codecString = (*env)->NewStringUTF(env, codecName != NULL ? codecName : "unknown");
    if (codecString == NULL) {
        return NULL;
    }
    (*env)->SetObjectArrayElement(env, names, 0, codecString);
    (*env)->DeleteLocalRef(env, codecString);
    if (language != NULL && language->value != NULL && language->value[0] != '\0') {
        jstring languageString = (*env)->NewStringUTF(env, language->value);
        if (languageString == NULL) {
            return NULL;
        }
        (*env)->SetObjectArrayElement(env, names, 1, languageString);
        (*env)->DeleteLocalRef(env, languageString);
    }
    return names;
}

/*
 * Makes audio track `track` the one this handle decodes. See openAudioTrack for what that costs
 * the consumer that had the old one.
 *
 * Calling it for the track already selected is not an error and is not free either: it opens a
 * second decoder for the same stream and supersedes the consumer holding the first. FfmpegMedia is
 * the only caller and it makes that choice deliberately, when the consumer it had is closed.
 */
JNIEXPORT void JNICALL Java_limn_video_ffmpeg_FfmpegNative_selectAudio(
        JNIEnv *env, jclass cls, jlong handle, jint track) {
    (void) cls;
    Player *player = (Player *) (uintptr_t) handle;
    if (track < 0 || track >= player->audioTrackCount) {
        throwFfmpeg(env, "no such audio track", 0);
        return;
    }
    openAudioTrack(env, player, track);
}

/* @return the generation a consumer of the currently selected track must present */
JNIEXPORT jlong JNICALL Java_limn_video_ffmpeg_FfmpegNative_audioGeneration(
        JNIEnv *env, jclass cls, jlong handle) {
    (void) env;
    (void) cls;
    Player *player = (Player *) (uintptr_t) handle;
    pthread_mutex_lock(&player->audioLock);
    int64_t generation = player->audioGeneration;
    pthread_mutex_unlock(&player->audioLock);
    return (jlong) generation;
}

/* ------------------------------------------------------------------ the subtitle tracks */

/*
 * What subtitle track `track` is: its numbers into `out`, its two names as the returned array.
 * Read from the container's stream parameters, like the audio listing and for the same reason: a
 * track this build cannot decode, or one this SPI will not carry, is still a track the file has.
 *
 * @return {codec name, language or null}
 */
JNIEXPORT jobjectArray JNICALL Java_limn_video_ffmpeg_FfmpegNative_describeSubtitleTrack(
        JNIEnv *env, jclass cls, jlong handle, jint track, jlongArray out) {
    (void) cls;
    Player *player = (Player *) (uintptr_t) handle;
    if (track < 0 || track >= player->subtitleTrackCount) {
        throwFfmpeg(env, "no such subtitle track", 0);
        return NULL;
    }
    int streamIndex = player->subtitleStreams[track];
    AVStream *stream = player->format->streams[streamIndex];
    AVCodecParameters *parameters = stream->codecpar;

    jlong values[5];
    values[0] = streamIndex;
    values[1] = isTextSubtitle(parameters);
    values[2] = avcodec_find_decoder(parameters->codec_id) != NULL;
    /* The container's own dispositions, reported and not acted on. Which track a viewer wants is
     * the application's question; these are the two facts the file offers towards it. */
    values[3] = (stream->disposition & AV_DISPOSITION_DEFAULT) != 0;
    values[4] = (stream->disposition & AV_DISPOSITION_FORCED) != 0;
    (*env)->SetLongArrayRegion(env, out, 0, 5, values);

    const char *codecName = avcodec_get_name(parameters->codec_id);
    AVDictionaryEntry *language = av_dict_get(stream->metadata, "language", NULL, 0);

    jclass stringClass = (*env)->FindClass(env, "java/lang/String");
    if (stringClass == NULL) {
        return NULL;
    }
    jobjectArray names = (*env)->NewObjectArray(env, 2, stringClass, NULL);
    if (names == NULL) {
        return NULL;
    }
    jstring codecString = (*env)->NewStringUTF(env, codecName != NULL ? codecName : "unknown");
    if (codecString == NULL) {
        return NULL;
    }
    (*env)->SetObjectArrayElement(env, names, 0, codecString);
    (*env)->DeleteLocalRef(env, codecString);
    if (language != NULL && language->value != NULL && language->value[0] != '\0') {
        jstring languageString = (*env)->NewStringUTF(env, language->value);
        if (languageString == NULL) {
            return NULL;
        }
        (*env)->SetObjectArrayElement(env, names, 1, languageString);
        (*env)->DeleteLocalRef(env, languageString);
    }
    return names;
}

/* Makes `track` the subtitle stream this handle decodes; a negative track opens none and frees
 * whatever was open, which is how subtitles are turned off. */
JNIEXPORT void JNICALL Java_limn_video_ffmpeg_FfmpegNative_selectSubtitle(
        JNIEnv *env, jclass cls, jlong handle, jint track) {
    (void) cls;
    Player *player = (Player *) (uintptr_t) handle;
    if (track >= player->subtitleTrackCount) {
        throwFfmpeg(env, "no such subtitle track", 0);
        return;
    }
    openSubtitleTrack(env, player, track < 0 ? -1 : track);
}

/*
 * An ASS dialogue line to plain text, which is what this SPI publishes and all of what it publishes.
 *
 * Every text subtitle decoder in this build produces SUBTITLE_ASS rects, whatever the file was:
 * `rect->ass` is ff_ass_get_dialog's "readorder,layer,Style,Name,ML,MR,MV,Effect,Text" and
 * `rect->text` is empty, because for that type the ASS line is the authoritative one. So markup
 * arrives even from an SRT, and handing it over is the failure this phase exists to avoid: an
 * application drawing it with an ordinary text stack draws `0,0,Default,,0,0,0,,{\an8}Hello`.
 *
 * What goes, and why none of it is kept:
 *
 *   the eight leading fields  a layout vocabulary this SPI does not have and will not invent
 *   {...} override runs       position, colour, karaoke, fades (an ASS interpreter's work, and
 *                             that interpreter is libass, which this build refuses)
 *   \N                        a hard line break, and it becomes one
 *   \n and \h                 a soft break and a hard space; both become a space, because where a
 *                             line may be broken belongs to whoever measures the text
 *
 * Writes into `out`, which must have room for strlen(ass) + 1; nothing here lengthens the input.
 *
 * @return bytes written, not counting the terminator
 */
static size_t assToPlain(const char *ass, char *out) {
    /* Eight commas: ff_ass_get_dialog writes eight fields before the text and ASS forbids a comma
     * inside any of them. A line with fewer commas than that did not come from there, and is taken
     * whole rather than truncated at a comma that means something else. */
    const char *text = ass;
    int commas = 0;
    for (const char *scan = ass; *scan != '\0'; scan++) {
        if (*scan == ',' && ++commas == 8) {
            text = scan + 1;
            break;
        }
    }
    size_t written = 0;
    for (const char *scan = text; *scan != '\0'; scan++) {
        if (*scan == '{') {
            const char *closing = strchr(scan, '}');
            if (closing != NULL) {
                scan = closing;
                continue;
            }
            /* An unmatched brace is a literal brace to every ASS renderer, so it is one here. */
        }
        if (*scan == '\\' && scan[1] != '\0') {
            if (scan[1] == 'N') {
                out[written++] = '\n';
                scan++;
                continue;
            }
            if (scan[1] == 'n' || scan[1] == 'h') {
                out[written++] = ' ';
                scan++;
                continue;
            }
        }
        out[written++] = *scan;
    }
    out[written] = '\0';
    return written;
}

/*
 * The next cue from whatever the demuxer has already put in the subtitle queue.
 *
 * IT NEVER DEMULTIPLEXES, and that is the load-bearing sentence. Video and audio run the demux loop
 * when their queue is empty; a subtitle track is silent between lines, so doing the same here would
 * read forward through the whole gap (minutes of a film) to answer a question about now. Cues
 * therefore ride the pictures' demuxing, and a container nobody reads video from produces none.
 *
 * `out[C_EPOCH]` is filled on EVERY call, including the ones that produce nothing, because a seek
 * that emptied the queue has to reach the consumer whether or not a cue happens to follow it.
 *
 * A packet the decoder refuses is skipped rather than thrown for: a corrupt cue costs one line and
 * a thrown exception costs the film. `CUE_SKIPPED` is what keeps the caller's pump going past it.
 *
 * @return the cue's text, or NULL when this call produced none
 */
JNIEXPORT jstring JNICALL Java_limn_video_ffmpeg_FfmpegNative_readCue(
        JNIEnv *env, jclass cls, jlong handle, jlongArray out) {
    (void) cls;
    Player *player = (Player *) (uintptr_t) handle;
    jlong values[4] = {CUE_NONE, 0, 0, 0};

    pthread_mutex_lock(&player->subtitleLock);
    /* The epoch and the packet are taken together, under one hold, so that the cue this call
     * returns cannot be attributed to the wrong side of a seek. */
    pthread_mutex_lock(&player->demuxLock);
    values[1] = player->subtitleEpoch;
    int needsFlush = player->subtitleNeedsFlush;
    player->subtitleNeedsFlush = 0;
    AVPacket *packet = player->subtitleCodec == NULL ? NULL : queuePop(&player->subtitleQueue);
    pthread_mutex_unlock(&player->demuxLock);

    if (needsFlush && player->subtitleCodec != NULL) {
        avcodec_flush_buffers(player->subtitleCodec);
    }
    if (packet == NULL) {
        pthread_mutex_unlock(&player->subtitleLock);
        (*env)->SetLongArrayRegion(env, out, 0, 4, values);
        return NULL;
    }

    AVStream *stream = player->format->streams[packet->stream_index];
    int64_t startMicros = toMicros(player, stream, packet->pts);
    int64_t durationMicros = packet->duration > 0
            ? av_rescale_q(packet->duration, stream->time_base, AV_TIME_BASE_Q) : 0;

    AVSubtitle subtitle = {0};
    int got = 0;
    int decoded = avcodec_decode_subtitle2(player->subtitleCodec, &subtitle, &got, packet);
    av_packet_free(&packet);

    /* A packet was consumed either way, so the pump must come back for the next one. */
    values[0] = CUE_SKIPPED;
    char *text = NULL;
    if (decoded >= 0 && got) {
        size_t room = 1;
        for (unsigned int i = 0; i < subtitle.num_rects; i++) {
            const AVSubtitleRect *rect = subtitle.rects[i];
            if (rect->type == SUBTITLE_ASS && rect->ass != NULL) {
                room += strlen(rect->ass) + 1;
            } else if (rect->type == SUBTITLE_TEXT && rect->text != NULL) {
                room += strlen(rect->text) + 1;
            }
        }
        text = malloc(room);
        size_t used = 0;
        if (text != NULL) {
            for (unsigned int i = 0; i < subtitle.num_rects; i++) {
                const AVSubtitleRect *rect = subtitle.rects[i];
                const char *plain = NULL;
                if (rect->type == SUBTITLE_ASS && rect->ass != NULL) {
                    plain = rect->ass;
                } else if (rect->type == SUBTITLE_TEXT && rect->text != NULL) {
                    plain = rect->text;
                } else {
                    /* A bitmap rect on a track this SPI opened only as text. Skipped rather than
                     * refused: the track was checked at selection and this is the decoder changing
                     * its mind, which costs a line rather than the film. */
                    continue;
                }
                /* Several rects are several lines shown at the same moment, so they become one cue
                 * with a line break between them, not several cues with identical timing, which
                 * would make an application reconstruct the grouping the decoder already knew. */
                if (used > 0) {
                    text[used++] = '\n';
                }
                used += rect->type == SUBTITLE_ASS ? assToPlain(plain, text + used)
                                                   : (size_t) snprintf(text + used, room - used,
                                                                       "%s", plain);
            }
            text[used] = '\0';
        }
        if (used == 0) {
            /* mov_text writes an empty sample across every gap between lines, so this is the
             * ordinary case and not a fault: a gap is not a cue. */
            free(text);
            text = NULL;
        } else if (startMicros != INT64_MIN) {
            int64_t begin = startMicros + (int64_t) subtitle.start_display_time * 1000;
            int64_t end = startMicros + (int64_t) subtitle.end_display_time * 1000;
            if (end <= begin) {
                /* libavcodec fills end_display_time from the packet's duration when the decoder
                 * left it at zero, so reaching here means the container stated no duration either.
                 * INT64_MIN says so; the consumer ends such a cue where the next one begins, which
                 * is the only reading that neither invents a length nor shows nothing. */
                end = durationMicros > 0 ? begin + durationMicros : INT64_MIN;
            }
            values[0] = CUE_READY;
            values[2] = begin;
            values[3] = end;
        } else {
            /* No presentation time at all. There is nowhere to put it, so it is dropped rather
             * than shown at zero. */
            free(text);
            text = NULL;
        }
    }
    avsubtitle_free(&subtitle);
    pthread_mutex_unlock(&player->subtitleLock);

    (*env)->SetLongArrayRegion(env, out, 0, 4, values);
    if (text == NULL) {
        return NULL;
    }
    jstring result = (*env)->NewStringUTF(env, text);
    free(text);
    return result;
}

/* ------------------------------------------------------------------ read video */

JNIEXPORT jint JNICALL Java_limn_video_ffmpeg_FfmpegNative_readVideo(
        JNIEnv *env, jclass cls, jlong handle, jlongArray out) {
    (void) cls;
    Player *player = (Player *) (uintptr_t) handle;

    pthread_mutex_lock(&player->videoLock);
    if (player->videoEnded) {
        pthread_mutex_unlock(&player->videoLock);
        return READ_END;
    }
    int index = -1;
    for (int i = 0; i < player->slotCount; i++) {
        if (!player->slots[i].busy) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        /* Every picture is out with the consumer. Not an end, not an error, and emphatically not
         * a copy into somewhere else: release one and ask again. */
        pthread_mutex_unlock(&player->videoLock);
        return READ_PENDING;
    }
    Slot *slot = &player->slots[index];

    int status = 0;
    for (;;) {
        int result = avcodec_receive_frame(player->videoCodec, slot->frame);
        if (result == 0) {
            if (player->videoSkipToMicros != INT64_MIN) {
                /* An exact seek, being paid for here rather than in seekVideo: the container placed
                 * the demuxer on an independently decodable picture at or before the target, and
                 * everything between it and the target is decoded and dropped. A picture with no
                 * timestamp cannot be compared, so it satisfies the skip rather than looping
                 * forever against a sentinel. */
                int64_t stamp = slot->frame->best_effort_timestamp;
                if (stamp == AV_NOPTS_VALUE) {
                    stamp = slot->frame->pts;
                }
                int64_t when = toMicros(player, player->format->streams[player->videoStream], stamp);
                if (when != INT64_MIN && when < player->videoSkipToMicros) {
                    av_frame_unref(slot->frame);
                    continue;
                }
                player->videoSkipToMicros = INT64_MIN;
            }
            break;
        }
        if (result == AVERROR_EOF) {
            player->videoEnded = 1;
            pthread_mutex_unlock(&player->videoLock);
            return READ_END;
        }
        if (result != AVERROR(EAGAIN)) {
            status = result;
            break;
        }
        if (player->pendingVideo == NULL) {
            player->pendingVideo = pullPacket(player, player->videoStream);
        }
        if (player->pendingVideo == NULL) {
            /* Nothing left to demux: ask the decoder for what it is still holding. Sending the
             * flush packet more than once is harmless, and one of the receives after it answers
             * AVERROR_EOF, which is what ends the loop. */
            result = avcodec_send_packet(player->videoCodec, NULL);
            if (result < 0 && result != AVERROR_EOF) {
                status = result;
                break;
            }
            continue;
        }
        result = avcodec_send_packet(player->videoCodec, player->pendingVideo);
        if (result == AVERROR(EAGAIN)) {
            /* The decoder has output waiting and will not take more input until it is read. The
             * packet stays exactly where it is and is offered again after the next receive. */
            continue;
        }
        av_packet_free(&player->pendingVideo);
        if (result < 0) {
            status = result;
            break;
        }
    }
    if (status != 0) {
        pthread_mutex_unlock(&player->videoLock);
        throwFfmpeg(env, "the video stream could not be decoded", status);
        return READ_END;
    }

    /* Everything below is read off a decoded frame, and every one of these checks is a
     * dereference that does not happen when a file lies. */
    if (slot->frame->width != player->width || slot->frame->height != player->height) {
        char message[160];
        snprintf(message, sizeof(message),
                 "the stream changed size from %dx%d to %dx%d, which this SPI fixes at open",
                 player->width, player->height, slot->frame->width, slot->frame->height);
        av_frame_unref(slot->frame);
        pthread_mutex_unlock(&player->videoLock);
        throwFfmpeg(env, message, 0);
        return READ_END;
    }
    /* A downloaded picture from this slot's previous lease: the planes below are about to be
     * re-derived, and a stale software frame would keep a whole picture alive per slot. */
    slot->downloaded = 0;
    if (slot->software != NULL) {
        av_frame_unref(slot->software);
    }

    int64_t surfaceHandle = 0;
#ifdef __APPLE__
    if (slot->frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
        /* get_format is entitled to change its mind between pictures, and a decode that fell back
         * to software mid-stream would otherwise be published as a handle of zero. */
        if (!player->hardware) {
            av_frame_unref(slot->frame);
            pthread_mutex_unlock(&player->videoLock);
            throwFfmpeg(env, "the decoder produced a hardware picture without an accelerator", 0);
            return READ_END;
        }
        CVPixelBufferRef pixelBuffer = (CVPixelBufferRef) slot->frame->data[3];
        if (pixelBuffer == NULL) {
            av_frame_unref(slot->frame);
            pthread_mutex_unlock(&player->videoLock);
            throwFfmpeg(env, "a hardware picture carries no pixel buffer", 0);
            return READ_END;
        }
        uint32_t type = CVPixelBufferGetPixelFormatType(pixelBuffer);
        if (type != player->expectedSurfaceType && type != player->expectedSurfaceTypeAlt) {
            char message[192];
            snprintf(message, sizeof(message),
                     "the accelerator produced a '%c%c%c%c' surface where the container's depth "
                     "says it should produce '%c%c%c%c', and this SPI fixes the layout at open",
                     (char) (type >> 24), (char) (type >> 16), (char) (type >> 8), (char) type,
                     (char) (player->expectedSurfaceType >> 24),
                     (char) (player->expectedSurfaceType >> 16),
                     (char) (player->expectedSurfaceType >> 8),
                     (char) player->expectedSurfaceType);
            av_frame_unref(slot->frame);
            pthread_mutex_unlock(&player->videoLock);
            throwFfmpeg(env, message, 0);
            return READ_END;
        }
        IOSurfaceRef surface = CVPixelBufferGetIOSurface(pixelBuffer);
        if (surface == NULL) {
            /* Every VideoToolbox buffer is IOSurface-backed, so this is not a fallback path; it is
             * the check that stops a zero being published as a handle Java would then bind. */
            av_frame_unref(slot->frame);
            pthread_mutex_unlock(&player->videoLock);
            throwFfmpeg(env, "a hardware picture has no IOSurface to bind", 0);
            return READ_END;
        }
        surfaceHandle = (int64_t) (uintptr_t) surface;
        for (int plane = 0; plane < MAX_PLANES; plane++) {
            /* No addresses on this side of the boundary any more. Clearing them is what makes the
             * epoch move when a download re-derives them. */
            slot->bound[plane] = NULL;
            slot->strides[plane] = 0;
        }
        slot->epoch++;
    } else
#endif
    if (mapPixelFormat((enum AVPixelFormat) slot->frame->format) != player->pixelFormat) {
        av_frame_unref(slot->frame);
        pthread_mutex_unlock(&player->videoLock);
        throwFfmpeg(env, "the stream changed pixel format, which this SPI fixes at open", 0);
        return READ_END;
    }

    if (surfaceHandle != 0) {
        int64_t best = slot->frame->best_effort_timestamp;
        if (best == AV_NOPTS_VALUE) {
            best = slot->frame->pts;
        }
        jlong handleValues[READ_LENGTH_C];
        handleValues[0] = toMicros(player, player->format->streams[player->videoStream], best);
        handleValues[1] = slot->epoch;
        handleValues[2] = 0;
        handleValues[3] = 0;
        handleValues[4] = 0;
        handleValues[5] = surfaceHandle;
        (*env)->SetLongArrayRegion(env, out, 0, READ_LENGTH_C, handleValues);
        slot->busy = 1;
        pthread_mutex_unlock(&player->videoLock);
        return index;
    }

    int64_t epochBefore = slot->epoch;
    for (int plane = 0; plane < player->planeCount; plane++) {
        const uint8_t *address = slot->frame->data[plane];
        int stride = slot->frame->linesize[plane];
        if (address == NULL) {
            av_frame_unref(slot->frame);
            pthread_mutex_unlock(&player->videoLock);
            throwFfmpeg(env, "a decoded picture has no data for one of its planes", 0);
            return READ_END;
        }
        if (stride < 0) {
            /* linesize may be negative for a bottom-up picture, and the SPI says rows run
             * top-down: "a producer holding bottom-up rows flips on its own side". Nothing this
             * build decodes produces one, so this refuses rather than silently flipping; a copy
             * per picture introduced here would be invisible from the outside. */
            av_frame_unref(slot->frame);
            pthread_mutex_unlock(&player->videoLock);
            throwFfmpeg(env, "the decoder produced bottom-up rows (a negative stride), which "
                             "this SPI does not carry", 0);
            return READ_END;
        }
        int byteWidth = planeByteWidth(player->pixelFormat, plane, player->width);
        if (stride < byteWidth) {
            av_frame_unref(slot->frame);
            pthread_mutex_unlock(&player->videoLock);
            throwFfmpeg(env, "a decoded picture's stride is below its own row width", 0);
            return READ_END;
        }
        if (address != slot->bound[plane] || stride != slot->strides[plane]) {
            slot->bound[plane] = address;
            slot->strides[plane] = stride;
            slot->epoch++;
        }
    }
    if (slot->epoch != epochBefore) {
        /* Collapse a multi-plane rebind into one epoch step, so Java rebinds every plane once. */
        slot->epoch = epochBefore + 1;
    }

    int64_t best = slot->frame->best_effort_timestamp;
    if (best == AV_NOPTS_VALUE) {
        best = slot->frame->pts;
    }
    jlong values[READ_LENGTH_C];
    values[0] = toMicros(player, player->format->streams[player->videoStream], best);
    values[1] = slot->epoch;
    values[2] = slot->strides[0];
    values[3] = player->planeCount > 1 ? slot->strides[1] : 0;
    values[4] = player->planeCount > 2 ? slot->strides[2] : 0;
    values[5] = 0; /* planar: there is no handle, and 0 is what VideoFrame refuses to publish */
    (*env)->SetLongArrayRegion(env, out, 0, READ_LENGTH_C, values);

    slot->busy = 1;
    pthread_mutex_unlock(&player->videoLock);
    return index;
}

/*
 * The plane a slot is currently pointing at, as a direct ByteBuffer over libavcodec's own memory.
 * Called only when readVideo reported a new epoch, which is during warm-up and after a reset.
 */
JNIEXPORT jobject JNICALL Java_limn_video_ffmpeg_FfmpegNative_planeBuffer(
        JNIEnv *env, jclass cls, jlong handle, jint slotIndex, jint plane) {
    (void) cls;
    Player *player = (Player *) (uintptr_t) handle;
    if (slotIndex < 0 || slotIndex >= player->slotCount || plane < 0
            || plane >= player->planeCount) {
        throwFfmpeg(env, "no such slot or plane", 0);
        return NULL;
    }
    pthread_mutex_lock(&player->videoLock);
    Slot *slot = &player->slots[slotIndex];
    const uint8_t *address = slot->bound[plane];
    if (address == NULL) {
        pthread_mutex_unlock(&player->videoLock);
        throwFfmpeg(env, "this slot holds no picture", 0);
        return NULL;
    }
    /* Exactly PixelFormat.minPlaneBytes: every row but the last occupies a full stride and the
     * last needs only its byte width. Sizing this at stride * rows would name bytes past the end
     * of the plane, which is the read a padded allocation happens to survive and an exact one
     * does not. */
    int rows = planeRows(player->pixelFormat, plane, player->height);
    int byteWidth = planeByteWidth(player->pixelFormat, plane, player->width);
    jlong capacity = (jlong) slot->strides[plane] * (rows - 1) + byteWidth;
    jobject buffer = cachedBuffer(env, player, address, capacity);
    pthread_mutex_unlock(&player->videoLock);
    if (buffer == NULL) {
        throwFfmpeg(env, "cannot wrap a picture plane", 0);
        return NULL;
    }
    return buffer;
}

/*
 * Route A, on demand: reads a hardware picture back into memory a consumer can address, and points
 * this slot's planes at it. VideoFrame.toPlanar() is what reaches here.
 *
 * It is a whole picture across whatever bus separates the decoder from the CPU (3.1 MB at 1080p
 * 4:2:0, 12.4 MB at 4K, per picture), and it exists because YuvConverter, Y4mWriter and every test
 * read planes, so a consumer that cannot bind a handle must not simply be broken by a decoder it
 * did not choose. A consumer that CAN bind one must not call this.
 *
 * The software frame is the slot's and is reused: the transfer allocates its buffers the first time
 * and refills them afterwards, so a consumer that downloads every picture pays one copy and no
 * allocation. It is unreffed with the slot, and cleared at the top of the next read into it.
 *
 * Any thread, per VideoFrame's contract, which is why it takes videoLock rather than assuming the
 * decode thread.
 */
JNIEXPORT void JNICALL Java_limn_video_ffmpeg_FfmpegNative_downloadVideo(
        JNIEnv *env, jclass cls, jlong handle, jint slotIndex, jlongArray out) {
    (void) cls;
    Player *player = (Player *) (uintptr_t) handle;
    if (slotIndex < 0 || slotIndex >= player->slotCount) {
        throwFfmpeg(env, "no such slot", 0);
        return;
    }
    pthread_mutex_lock(&player->videoLock);
    Slot *slot = &player->slots[slotIndex];
    if (!slot->busy) {
        pthread_mutex_unlock(&player->videoLock);
        throwFfmpeg(env, "this slot holds no picture to read back", 0);
        return;
    }
    if (slot->downloaded) {
        /* Already in memory. Answering with the bindings it already has is what makes
         * VideoFrame.toPlanar idempotent without a second transfer. */
        goto answer;
    }
    if (slot->software == NULL) {
        slot->software = av_frame_alloc();
        if (slot->software == NULL) {
            pthread_mutex_unlock(&player->videoLock);
            throwFfmpeg(env, "out of memory reading a picture back", AVERROR(ENOMEM));
            return;
        }
    }
    av_frame_unref(slot->software);
    /* AV_PIX_FMT_NONE lets the transfer choose the layout the accelerator actually has (NV12 or
     * P010), which is the same one open() reported. Naming a format here instead would ask for a
     * conversion that libavutil cannot do and would fail with a message about the wrong thing. */
    slot->software->format = AV_PIX_FMT_NONE;
    int result = av_hwframe_transfer_data(slot->software, slot->frame, 0);
    if (result < 0) {
        av_frame_unref(slot->software);
        pthread_mutex_unlock(&player->videoLock);
        throwFfmpeg(env, "cannot read a hardware picture back into memory", result);
        return;
    }
    if (mapPixelFormat((enum AVPixelFormat) slot->software->format) != player->pixelFormat) {
        char message[160];
        const char *name = av_get_pix_fmt_name((enum AVPixelFormat) slot->software->format);
        snprintf(message, sizeof(message),
                 "reading the picture back produced '%s', which is not the layout this stream "
                 "was opened with", name != NULL ? name : "unknown");
        av_frame_unref(slot->software);
        pthread_mutex_unlock(&player->videoLock);
        throwFfmpeg(env, message, 0);
        return;
    }
    for (int plane = 0; plane < player->planeCount; plane++) {
        const uint8_t *address = slot->software->data[plane];
        int stride = slot->software->linesize[plane];
        if (address == NULL || stride < planeByteWidth(player->pixelFormat, plane, player->width)) {
            av_frame_unref(slot->software);
            pthread_mutex_unlock(&player->videoLock);
            throwFfmpeg(env, "a picture read back has no usable plane", 0);
            return;
        }
        slot->bound[plane] = address;
        slot->strides[plane] = stride;
    }
    slot->epoch++;
    slot->downloaded = 1;

answer:
    {
        jlong values[READ_LENGTH_C];
        values[0] = 0;
        values[1] = slot->epoch;
        values[2] = slot->strides[0];
        values[3] = player->planeCount > 1 ? slot->strides[1] : 0;
        values[4] = player->planeCount > 2 ? slot->strides[2] : 0;
        values[5] = 0; /* it is samples now, and no longer a handle */
        pthread_mutex_unlock(&player->videoLock);
        (*env)->SetLongArrayRegion(env, out, 0, READ_LENGTH_C, values);
    }
}

/*
 * The other half of the ownership pair: VideoFrame.release() reaches exactly this, carrying the
 * slot integer and never an address, and the picture's reference is dropped here.
 */
JNIEXPORT void JNICALL Java_limn_video_ffmpeg_FfmpegNative_releaseVideo(
        JNIEnv *env, jclass cls, jlong handle, jint slotIndex) {
    (void) env;
    (void) cls;
    Player *player = (Player *) (uintptr_t) handle;
    if (slotIndex < 0 || slotIndex >= player->slotCount) {
        return;
    }
    pthread_mutex_lock(&player->videoLock);
    Slot *slot = &player->slots[slotIndex];
    if (slot->busy) {
        av_frame_unref(slot->frame);
        if (slot->software != NULL) {
            /* A picture read back is a copy this side made and holds; the slot coming free is what
             * frees it, exactly as it frees the reference to the decoder's own. */
            av_frame_unref(slot->software);
        }
        slot->downloaded = 0;
        slot->busy = 0;
    }
    pthread_mutex_unlock(&player->videoLock);
}

/*
 * Places the demuxer at or before `micros`, unless the other track has already placed it there.
 *
 * ONE CONTAINER, ONE DEMUXER POSITION, TWO CONSUMERS. After a seek both tracks are asked to move to
 * the same target, by two threads, in an order nobody controls: the player's decode thread for the
 * pictures and the audio engine's service thread for the sound. Two real seeks to one target would
 * mean the second one throwing away the packets the first had queued for the other track, which is
 * heard as a fraction of a second of sound repeating after every scrub.
 *
 * So the placement is recorded and each track says when it has taken it up. The first to ask moves
 * the container; the second finds its target already current and only flushes its own decoder. The
 * result does not depend on which arrives first, which is the property worth having, because the
 * order is decided by an audio device's timer.
 *
 * Called with demuxLock held and NEITHER codec lock, per this file's lock order.
 *
 * @return 0, or a negative FFmpeg error when the container could not be placed
 */
static int placeDemuxer(Player *player, int64_t micros, int *takenByMe, int *takenByOther) {
    if (player->seekTargetMicros == micros && !*takenByMe) {
        /* The other track placed it here and this one has not read from there yet. */
        *takenByMe = 1;
        return 0;
    }
    int64_t base = micros;
    if (player->startTimeMicros != AV_NOPTS_VALUE) {
        base += player->startTimeMicros; /* toMicros subtracts it; av_seek_frame wants it back */
    }
    AVStream *stream = player->format->streams[player->videoStream];
    int64_t timestamp = av_rescale_q(base, AV_TIME_BASE_Q, stream->time_base);
    int result = av_seek_frame(player->format, player->videoStream, timestamp,
                               AVSEEK_FLAG_BACKWARD);
    if (result < 0) {
        return result;
    }
    queueClear(&player->videoQueue);
    queueClear(&player->audioQueue);
    /* The subtitle side has no seek entry point of its own and needs none: it holds no position,
     * only a window, and this is where that window stops describing the film. Both halves matter:
     * the queued packets belong to where the container was, and the decoder may be part-way through
     * a cue that will never be finished. */
    queueClear(&player->subtitleQueue);
    player->subtitleEpoch++;
    player->subtitleNeedsFlush = 1;
    player->demuxEnded = 0;
    player->seekTargetMicros = micros;
    player->containerSeeks++;
    *takenByMe = 1;
    *takenByOther = 0;
    return 0;
}

/*
 * Moves the pictures to `micros`.
 *
 * `exact` does NOT decode here. It arms a discard threshold that readVideo applies as it decodes,
 * so the pictures between the container's placement and the target are dropped inside the loop that
 * was going to decode them anyway. The alternative (a decode loop of its own here) has to stop
 * one picture late and then has nowhere to put the picture it wanted, because avcodec has no way to
 * put a frame back; holding one back would be a second picture lifetime to get right on the one
 * path where a mistake is a pooled slot that never comes home.
 */
JNIEXPORT void JNICALL Java_limn_video_ffmpeg_FfmpegNative_seekVideo(
        JNIEnv *env, jclass cls, jlong handle, jlong micros, jboolean exact) {
    (void) cls;
    Player *player = (Player *) (uintptr_t) handle;
    pthread_mutex_lock(&player->videoLock);
    pthread_mutex_lock(&player->demuxLock);
    int moved = player->seekTargetMicros != micros || player->seekTakenVideo;
    int result = placeDemuxer(player, micros, &player->seekTakenVideo, &player->seekTakenAudio);
    pthread_mutex_unlock(&player->demuxLock);
    if (result >= 0) {
        avcodec_flush_buffers(player->videoCodec);
        /* Held back from before the seek, so it belongs to a position the stream has left. */
        av_packet_free(&player->pendingVideo);
        player->videoEnded = 0;
        player->videoSkipToMicros = exact ? micros : INT64_MIN;
    }
    pthread_mutex_unlock(&player->videoLock);

    pthread_mutex_lock(&player->audioLock);
    if (player->audioCodec != NULL && result >= 0 && moved) {
        /* Only when the container actually moved: flushing the audio side because the OTHER track
         * placed the demuxer would throw away samples the engine has already been handed. */
        avcodec_flush_buffers(player->audioCodec);
        av_packet_free(&player->pendingAudio);
        av_frame_unref(player->audioFrame);
        player->audioHasFrame = 0;
        player->audioFrameOffset = 0;
        player->audioEnded = 0;
        player->audioSkipToMicros = micros;
    }
    pthread_mutex_unlock(&player->audioLock);

    if (result < 0) {
        throwFfmpeg(env, "the input cannot be seeked", result);
    }
}

JNIEXPORT void JNICALL Java_limn_video_ffmpeg_FfmpegNative_resetVideo(
        JNIEnv *env, jclass cls, jlong handle) {
    Java_limn_video_ffmpeg_FfmpegNative_seekVideo(env, cls, handle, 0, JNI_FALSE);
}

/*
 * Moves the soundtrack to `micros`. The container placement is shared with the pictures (see
 * placeDemuxer), and what is specific here is that the audio is then sample-accurate: the demuxer
 * lands on a video keyframe at or before the target, and everything decoded before the target is
 * dropped inside readAudio rather than handed to the engine.
 */
JNIEXPORT void JNICALL Java_limn_video_ffmpeg_FfmpegNative_seekAudio(
        JNIEnv *env, jclass cls, jlong handle, jlong micros, jlong generation) {
    (void) cls;
    Player *player = (Player *) (uintptr_t) handle;
    if (player->audioStream < 0) {
        return;
    }
    pthread_mutex_lock(&player->audioLock);
    if (generation != player->audioGeneration) {
        /* A superseded track. Moving the container on its behalf would drag the pictures and the
         * track somebody IS listening to somewhere neither asked to go. */
        pthread_mutex_unlock(&player->audioLock);
        return;
    }
    pthread_mutex_lock(&player->demuxLock);
    int moved = player->seekTargetMicros != micros || player->seekTakenAudio;
    int result = placeDemuxer(player, micros, &player->seekTakenAudio, &player->seekTakenVideo);
    pthread_mutex_unlock(&player->demuxLock);
    if (result >= 0) {
        avcodec_flush_buffers(player->audioCodec);
        av_packet_free(&player->pendingAudio);
        av_frame_unref(player->audioFrame);
        player->audioHasFrame = 0;
        player->audioFrameOffset = 0;
        player->audioEnded = 0;
        player->audioSkipToMicros = micros;
    }
    pthread_mutex_unlock(&player->audioLock);

    if (result >= 0 && moved) {
        /* The pictures' packets went with the placement, so the video side is flushed for the same
         * reason the audio side is when the pictures move first. */
        pthread_mutex_lock(&player->videoLock);
        avcodec_flush_buffers(player->videoCodec);
        av_packet_free(&player->pendingVideo);
        player->videoEnded = 0;
        pthread_mutex_unlock(&player->videoLock);
    }
    if (result < 0) {
        throwFfmpeg(env, "the input cannot be seeked", result);
    }
}

/* ------------------------------------------------------------------ read audio */

static float sampleAt(const AVFrame *frame, int channel, int index, int *unsupported) {
    switch (frame->format) {
        case AV_SAMPLE_FMT_FLTP:
            return ((const float *) frame->data[channel])[index];
        case AV_SAMPLE_FMT_FLT:
            return ((const float *) frame->data[0])[index * frame->ch_layout.nb_channels + channel];
        case AV_SAMPLE_FMT_S16P:
            return ((const int16_t *) frame->data[channel])[index] / 32768.0f;
        case AV_SAMPLE_FMT_S16:
            return ((const int16_t *) frame->data[0])
                    [index * frame->ch_layout.nb_channels + channel] / 32768.0f;
        case AV_SAMPLE_FMT_S32P:
            return ((const int32_t *) frame->data[channel])[index] / 2147483648.0f;
        case AV_SAMPLE_FMT_S32:
            return ((const int32_t *) frame->data[0])
                    [index * frame->ch_layout.nb_channels + channel] / 2147483648.0f;
        default:
            *unsupported = 1;
            return 0;
    }
}

/*
 * Positions the just-decoded audio frame at `target`, dropping it whole when it ends before then
 * and entering it partway when it straddles it. Called with audioLock held and audioHasFrame set.
 *
 * A frame with no timestamp cannot be placed on the timeline, so it satisfies the skip rather than
 * being discarded forever: a track that carries none is delivered from wherever the container put
 * it, which is the honest answer and not a silence.
 */
static void skipAudioTo(Player *player, int64_t target) {
    AVFrame *frame = player->audioFrame;
    int64_t stamp = frame->best_effort_timestamp;
    if (stamp == AV_NOPTS_VALUE) {
        stamp = frame->pts;
    }
    int64_t start = toMicros(player, player->format->streams[player->audioStream], stamp);
    if (start == INT64_MIN || player->audioSampleRate <= 0) {
        player->audioSkipToMicros = INT64_MIN;
        return;
    }
    int64_t ahead = target - start;
    if (ahead <= 0) {
        player->audioSkipToMicros = INT64_MIN;
        return;
    }
    int64_t samples = av_rescale(ahead, player->audioSampleRate, 1000000);
    if (samples >= frame->nb_samples) {
        av_frame_unref(frame);
        player->audioHasFrame = 0;
        player->audioFrameOffset = 0;
        return; /* still armed: the next frame is tested the same way */
    }
    player->audioFrameOffset = (int) samples;
    player->audioSkipToMicros = INT64_MIN;
}

static int16_t toPcm(float value) {
    /* AudioStreamSource.readFrames wants signed 16-bit. 32767 rather than 32768 so that +1.0 maps
     * to the largest representable value instead of wrapping to the most negative one. */
    float scaled = value * 32767.0f;
    if (scaled > 32767.0f) {
        scaled = 32767.0f;
    }
    if (scaled < -32768.0f) {
        scaled = -32768.0f;
    }
    return (int16_t) lrintf(scaled);
}

/*
 * Fills `out` with interleaved signed 16-bit frames, which is the shape AudioStreamSource wants
 * and NOT the shape AAC decodes to: libavcodec hands back planar float (AV_SAMPLE_FMT_FLTP). The
 * conversion is the arithmetic below and needs no resampler, because nothing is being resampled:
 * AudioStreamSource.sampleRate() reports whatever the file declared and the engine takes it.
 *
 * @return frames written; 0 only at the true end of the track
 */
JNIEXPORT jint JNICALL Java_limn_video_ffmpeg_FfmpegNative_readAudio(
        JNIEnv *env, jclass cls, jlong handle, jobject out, jint maxFrames, jlong generation) {
    (void) cls;
    Player *player = (Player *) (uintptr_t) handle;
    if (player->audioStream < 0 || maxFrames <= 0) {
        return 0;
    }
    int16_t *destination = (int16_t *) (*env)->GetDirectBufferAddress(env, out);
    if (destination == NULL) {
        throwFfmpeg(env, "the audio destination is not a direct buffer", 0);
        return 0;
    }
    jlong capacityBytes = (*env)->GetDirectBufferCapacity(env, out);

    pthread_mutex_lock(&player->audioLock);
    if (generation != player->audioGeneration) {
        /* Another track was selected while this consumer was between refills. Zero is the end of
         * the track, which is exactly what this track is to it. The engine's answer to an end is
         * to stop and close the source, which is the handover this needs. */
        pthread_mutex_unlock(&player->audioLock);
        return 0;
    }
    int channels = player->audioOutChannels;
    if ((jlong) maxFrames * channels * 2 > capacityBytes) {
        maxFrames = (jint) (capacityBytes / (channels * 2));
    }
    int written = 0;
    int status = 0;
    int unsupported = 0;

    while (written < maxFrames) {
        if (player->audioHasFrame) {
            AVFrame *frame = player->audioFrame;
            int available = frame->nb_samples - player->audioFrameOffset;
            int take = available < maxFrames - written ? available : maxFrames - written;
            for (int i = 0; i < take; i++) {
                int source = player->audioFrameOffset + i;
                if (player->downmixActive) {
                    for (int out2 = 0; out2 < channels; out2++) {
                        float sum = 0;
                        for (int in = 0; in < player->audioSourceChannels; in++) {
                            float weight = player->downmix[out2][in];
                            if (weight != 0.0f) {
                                sum += weight * sampleAt(frame, in, source, &unsupported);
                            }
                        }
                        destination[(written + i) * channels + out2] = toPcm(sum);
                    }
                } else {
                    for (int out2 = 0; out2 < channels; out2++) {
                        destination[(written + i) * channels + out2] =
                                toPcm(sampleAt(frame, out2, source, &unsupported));
                    }
                }
            }
            if (unsupported) {
                break;
            }
            written += take;
            player->audioFrameOffset += take;
            if (player->audioFrameOffset >= frame->nb_samples) {
                av_frame_unref(frame);
                player->audioHasFrame = 0;
                player->audioFrameOffset = 0;
            }
            continue;
        }
        if (player->audioEnded) {
            break;
        }
        int result = avcodec_receive_frame(player->audioCodec, player->audioFrame);
        if (result == 0) {
            if (player->audioFrame->nb_samples <= 0) {
                av_frame_unref(player->audioFrame);
                continue;
            }
            player->audioHasFrame = 1;
            player->audioFrameOffset = 0;
            if (player->audioSkipToMicros != INT64_MIN) {
                /* What makes a seek sample-accurate on this side. The container was placed on a
                 * picture at or before the target, so the first samples decoded are early; the
                 * whole frames before the target are dropped and the one straddling it is entered
                 * partway, at a sample offset rather than at its start. */
                skipAudioTo(player, player->audioSkipToMicros);
            }
            continue;
        }
        if (result == AVERROR_EOF) {
            player->audioEnded = 1;
            break;
        }
        if (result != AVERROR(EAGAIN)) {
            status = result;
            break;
        }
        if (player->pendingAudio == NULL) {
            player->pendingAudio = pullPacket(player, player->audioStream);
        }
        if (player->pendingAudio == NULL) {
            result = avcodec_send_packet(player->audioCodec, NULL);
            if (result < 0 && result != AVERROR_EOF) {
                status = result;
                break;
            }
            continue;
        }
        result = avcodec_send_packet(player->audioCodec, player->pendingAudio);
        if (result == AVERROR(EAGAIN)) {
            continue; /* kept, not dropped; see pendingVideo */
        }
        av_packet_free(&player->pendingAudio);
        if (result < 0) {
            status = result;
            break;
        }
    }
    int format = player->audioFrame != NULL ? player->audioFrame->format : -1;
    pthread_mutex_unlock(&player->audioLock);

    if (unsupported) {
        char message[160];
        const char *name = av_get_sample_fmt_name((enum AVSampleFormat) format);
        snprintf(message, sizeof(message),
                 "sample format '%s' is not one this shim converts", name != NULL ? name : "?");
        throwFfmpeg(env, message, 0);
        return 0;
    }
    if (status != 0) {
        throwFfmpeg(env, "the audio track could not be decoded", status);
        return 0;
    }
    return written;
}

JNIEXPORT void JNICALL Java_limn_video_ffmpeg_FfmpegNative_resetAudio(
        JNIEnv *env, jclass cls, jlong handle, jlong generation) {
    (void) env;
    (void) cls;
    Player *player = (Player *) (uintptr_t) handle;
    if (player->audioStream < 0) {
        return;
    }
    pthread_mutex_lock(&player->audioLock);
    if (generation != player->audioGeneration) {
        pthread_mutex_unlock(&player->audioLock); /* superseded: not this track's codec to flush */
        return;
    }
    avcodec_flush_buffers(player->audioCodec);
    av_frame_unref(player->audioFrame);
    player->audioHasFrame = 0;
    player->audioFrameOffset = 0;
    player->audioEnded = 0;
    player->audioSkipToMicros = INT64_MIN; /* deliver from wherever the demuxer is, not from a target */
    pthread_mutex_unlock(&player->audioLock);

    pthread_mutex_lock(&player->demuxLock);
    /* The audio track cannot rewind on its own without moving the pictures, so this only drops
     * what was queued for it; the position comes from the demuxer, which the video side owns. */
    queueClear(&player->audioQueue);
    pthread_mutex_unlock(&player->demuxLock);
}

/*
 * The audio consumer is gone. Its packets stop being queued from here on and whatever was queued
 * is freed, which is what keeps a container whose track the engine refused from filling memory
 * with packets nobody will ever read.
 *
 * A SUPERSEDED consumer releases nothing. The engine closes a source it was given on every path,
 * so the close of a track that was replaced arrives after the replacement, and a release that
 * ignored the generation would unclaim the track somebody is listening to, stopping the sound with
 * nothing anywhere reporting a fault.
 */
JNIEXPORT void JNICALL Java_limn_video_ffmpeg_FfmpegNative_releaseAudio(
        JNIEnv *env, jclass cls, jlong handle, jlong generation) {
    (void) env;
    (void) cls;
    Player *player = (Player *) (uintptr_t) handle;
    pthread_mutex_lock(&player->demuxLock);
    if (generation == player->audioGeneration) {
        player->audioClaimed = 0;
        queueClear(&player->audioQueue);
    }
    pthread_mutex_unlock(&player->demuxLock);
}

JNIEXPORT void JNICALL Java_limn_video_ffmpeg_FfmpegNative_stats(
        JNIEnv *env, jclass cls, jlong handle, jlongArray out) {
    (void) cls;
    Player *player = (Player *) (uintptr_t) handle;
    pthread_mutex_lock(&player->demuxLock);
    jlong values[7];
    values[0] = player->droppedVideo;
    values[1] = player->droppedAudio;
    values[2] = player->videoQueue.count;
    values[3] = player->audioQueue.count;
    values[4] = player->containerSeeks;
    values[5] = player->droppedSubtitle;
    values[6] = player->subtitleQueue.count;
    pthread_mutex_unlock(&player->demuxLock);
    (*env)->SetLongArrayRegion(env, out, 0, 7, values);
}

/* ------------------------------------------------------------------ close */

JNIEXPORT void JNICALL Java_limn_video_ffmpeg_FfmpegNative_close(
        JNIEnv *env, jclass cls, jlong handle) {
    (void) cls;
    Player *player = (Player *) (uintptr_t) handle;
    if (player == NULL) {
        return;
    }
    /* Every lock is taken so that this cannot run beside a read that is still inside libavcodec.
     * Java guarantees the same thing one layer up (MediaPlayer joins its decode thread before
     * closing), and both are cheap, so both are here. */
    pthread_mutex_lock(&player->videoLock);
    pthread_mutex_lock(&player->audioLock);
    pthread_mutex_lock(&player->subtitleLock);
    pthread_mutex_lock(&player->demuxLock);

    queueClear(&player->videoQueue);
    queueClear(&player->audioQueue);
    queueClear(&player->subtitleQueue);
    av_packet_free(&player->pendingVideo);
    av_packet_free(&player->pendingAudio);
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (player->slots[i].frame != NULL) {
            av_frame_free(&player->slots[i].frame);
        }
        av_frame_free(&player->slots[i].software);
    }
    clearBufferCache(env, player);
    avcodec_free_context(&player->videoCodec);
    /* After the codec, which holds a reference of its own: unreffing the device first would leave
     * the decoder shutting down against a context that had already gone. */
    av_buffer_unref(&player->hwDevice);
    avcodec_free_context(&player->audioCodec);
    av_frame_free(&player->audioFrame);
    avcodec_free_context(&player->subtitleCodec);
    avformat_close_input(&player->format);

    pthread_mutex_unlock(&player->demuxLock);
    pthread_mutex_unlock(&player->subtitleLock);
    pthread_mutex_unlock(&player->audioLock);
    pthread_mutex_unlock(&player->videoLock);
    pthread_mutex_destroy(&player->demuxLock);
    pthread_mutex_destroy(&player->videoLock);
    pthread_mutex_destroy(&player->audioLock);
    pthread_mutex_destroy(&player->subtitleLock);
    free(player);
}

/* ------------------------------------------------------------------ identity */

/*
 * What this library actually is, so a test can assert it rather than a comment claim it:
 * the licence libavutil reports, its version, and the configure line it was built from.
 * LicenceTest reads all three and fails on --enable-gpl or on a protocol other than file.
 */
JNIEXPORT jstring JNICALL Java_limn_video_ffmpeg_FfmpegNative_identity(JNIEnv *env, jclass cls) {
    (void) cls;
    char text[4096];
    snprintf(text, sizeof(text), "%s\n%s\n%s",
             avutil_license(), av_version_info(), avutil_configuration());
    return (*env)->NewStringUTF(env, text);
}

/*
 * Every codec and container the linked libraries actually hold, read out of the libraries rather
 * than recited from the configure line. One line per component, "decoder:h264", "encoder:mpeg4",
 * "demuxer:mov", "muxer:mp4".
 *
 * This exists because a configure flag is a claim and a linked symbol is a fact: a --enable-decoder
 * line that a later edit drops, or that configure silently refuses because a dependency was off,
 * leaves a build that says it plays a codec and does not. The Java side asserts the set it
 * advertises against this.
 */
JNIEXPORT jstring JNICALL Java_limn_video_ffmpeg_FfmpegNative_components(JNIEnv *env, jclass cls) {
    (void) cls;
    /* Generous: the trimmed build holds a couple of dozen components and a full FFmpeg a few
     * thousand, and a truncated list would read as a missing codec. */
    size_t capacity = 1 << 16;
    char *text = (char *) malloc(capacity);
    if (text == NULL) {
        return NULL;
    }
    size_t used = 0;
    text[0] = '\0';

    void *iterator = NULL;
    const AVCodec *codec;
    while ((codec = av_codec_iterate(&iterator)) != NULL) {
        const char *kind = av_codec_is_decoder(codec) ? "decoder" : "encoder";
        int written = snprintf(text + used, capacity - used, "%s:%s\n", kind, codec->name);
        if (written < 0 || (size_t) written >= capacity - used) {
            break;
        }
        used += (size_t) written;
        /* And the accelerators each decoder can actually be driven by, read the same way and for
         * the same reason: --enable-videotoolbox on its own switches NO hwaccel on, and a build
         * that claims hardware decode and has none of it would be discovered on a user's file. */
        if (!av_codec_is_decoder(codec)) {
            continue;
        }
        for (int index = 0;; index++) {
            const AVCodecHWConfig *config = avcodec_get_hw_config(codec, index);
            if (config == NULL) {
                break;
            }
            const char *device = av_hwdevice_get_type_name(config->device_type);
            if (device == NULL) {
                continue;
            }
            written = snprintf(text + used, capacity - used, "hwaccel:%s:%s\n",
                               codec->name, device);
            if (written < 0 || (size_t) written >= capacity - used) {
                break;
            }
            used += (size_t) written;
        }
    }
    iterator = NULL;
    const AVInputFormat *input;
    while ((input = av_demuxer_iterate(&iterator)) != NULL) {
        int written = snprintf(text + used, capacity - used, "demuxer:%s\n", input->name);
        if (written < 0 || (size_t) written >= capacity - used) {
            break;
        }
        used += (size_t) written;
    }
    iterator = NULL;
    const AVOutputFormat *output;
    while ((output = av_muxer_iterate(&iterator)) != NULL) {
        int written = snprintf(text + used, capacity - used, "muxer:%s\n", output->name);
        if (written < 0 || (size_t) written >= capacity - used) {
            break;
        }
        used += (size_t) written;
    }

    jstring result = (*env)->NewStringUTF(env, text);
    free(text);
    return result;
}

JNIEXPORT jboolean JNICALL Java_limn_video_ffmpeg_FfmpegNative_canWrite(JNIEnv *env, jclass cls) {
    (void) env;
    (void) cls;
    return avcodec_find_encoder(AV_CODEC_ID_MJPEG) != NULL
            && avcodec_find_encoder(AV_CODEC_ID_MPEG4) != NULL
            /* The subtitle encoder is part of the same question rather than a second one: a build
             * that can write pictures and sound but no cues is not the `full` profile this
             * repository defines, and the answer a caller needs is the same instruction either
             * way: rebuild. Asking separately would let a stale build fail inside a test instead
             * of skipping it. */
            && avcodec_find_encoder(AV_CODEC_ID_MOV_TEXT) != NULL
            && av_guess_format("mp4", NULL, NULL) != NULL;
}

/* Pictures one written cue covers. Ten rather than one, so that a clip short enough to decode in a
 * test still holds several cues and a seek has somewhere to land that is not where it started. */
#define CLIP_PICTURES_PER_CUE 10

/*
 * The ASS header the movtext encoder parses at open. It is REQUIRED: mov_text_encode_init calls
 * ff_ass_split on avctx->subtitle_header, and a NULL one leaves the encoder with no style context,
 * so every later encode refuses the dialogue line it is given.
 *
 * It is written out here rather than borrowed from libavcodec because ff_ass_subtitle_header_default
 * is internal to that library and not among the av_* symbols a shared build exports.
 */
static const char CLIP_ASS_HEADER[] =
        "[Script Info]\n"
        "ScriptType: v4.00+\n"
        "PlayResX: 384\n"
        "PlayResY: 288\n"
        "\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, "
        "BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, "
        "BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Arial,16,&Hffffff,&Hffffff,&H0,&H0,0,0,0,0,100,100,0,0,1,1,0,2,10,10,10,1\n"
        "\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";

/* ------------------------------------------------------------------ the clip writer */

/*
 * Writes a real MP4 holding a real encoded video track and, optionally, a real AAC track.
 *
 * It exists because nothing this build can generate is committed, so the only honest way to test
 * a demuxer is to produce something for it to demux. The same entry point
 * is what the Kitchen Sink uses to make the clip it plays.
 *
 * It cannot encode H.264, because FFmpeg's H.264 encoder is x264 and x264 is GPL. What that costs
 * is stated plainly: this round trip proves the SEAM (the demuxer, the packet-to-frame path, the
 * planar handoff, the pool, the release discipline and the timestamp rescale), and it does not
 * prove libavcodec's H.264 decoder, which is FFmpeg's to test and is covered by FFmpeg's suite.
 *
 * TWO CODECS, because they exercise different halves of the decode path:
 *
 *   codecId 0, MJPEG          Intra-only, no reordering and no decoder delay. Nearly lossless at
 *                             the quality used, which is what lets the sample assertions be tight.
 *                             This is the one to reach for by default.
 *   codecId 1, MPEG-4 Part 2  An inter-frame codec with a real group of pictures, which is the
 *                             SHAPE H.264 has even though it is not H.264: pictures that need
 *                             their predecessors, and a decoder that carries state across packets.
 *
 * Both deliver exactly as many pictures as were written, and a test may assert that count for
 * either. Note for anyone who changes the muxing below: that is a property of setting each
 * packet's duration, not something the codecs give for free.
 *
 * The picture is deliberately flat colour bars rather than a gradient or noise: a lossy encoder
 * moves every sample, so an assertion can only be about an area's mean, and large flat areas are
 * what make that tight enough to catch a swapped chroma pair or a plane read at the wrong stride.
 *
 * In the shipped `player` build there is no encoder at all and avcodec_find_encoder returns NULL,
 * so this throws instead of writing. That is the whole difference between the two builds.
 *
 * SEVERAL AUDIO TRACKS, because selecting between tracks cannot be tested against a file that has
 * one. Each gets a language tag of its own and a tone of its own (440 Hz times an odd multiple per
 * track, doubled per channel), so a test that asked for track 1 and is hearing track 0 sees it as a
 * frequency and not as a subtle difference in level. Track 0 channel 0 is 440 Hz exactly as it was
 * before there could be a track 1, which is what keeps the existing sample assertions meaning what
 * they meant.
 *
 * SUBTITLE TRACKS follow the same rule and add one of their own. Each cue's text NAMES its track
 * and its own index ("T0 C3"), so a reader that has the wrong track, or a cue from where the film
 * used to be, sees which rather than having to infer it from timing. The cues are CONTIGUOUS: cue i
 * covers pictures [10i, 10i+10), with no gaps between them. That is not tidiness, it is what makes
 * the seek assertion sharp: at every instant of the clip exactly one cue is on screen and its text
 * says where in the film it belongs, so "a scrub left a stale cue up" is a wrong string and not a
 * judgement about an interval.
 *
 * The first cue of every track carries ASS override tags and a hard line break, and the rest do
 * not. That is what makes the markup rule testable in both directions from one file: cue 0 asserts
 * that the tags were removed and that \N became a newline, and cue 1 asserts that stripping does
 * not damage a line that never had any.
 */
JNIEXPORT void JNICALL Java_limn_video_ffmpeg_FfmpegNative_writeClip(
        JNIEnv *env, jclass cls, jstring pathString, jint codecId, jint width, jint height,
        jint frames, jint rateNum, jint rateDen, jintArray audioChannelsArray,
        jobjectArray audioLanguages, jint sampleRate, jobjectArray subtitleLanguages) {
    (void) cls;
    enum AVCodecID wanted = codecId == 1 ? AV_CODEC_ID_MPEG4 : AV_CODEC_ID_MJPEG;
    const AVCodec *videoEncoder = avcodec_find_encoder(wanted);
    if (videoEncoder == NULL) {
        throwFfmpeg(env, "this build has no encoder: rebuild with --profile full", 0);
        return;
    }

    jint channelsOf[MAX_WRITE_AUDIO_TRACKS];
    int trackCount = 0;
    if (audioChannelsArray != NULL) {
        trackCount = (*env)->GetArrayLength(env, audioChannelsArray);
        if (trackCount > MAX_WRITE_AUDIO_TRACKS) {
            throwFfmpeg(env, "too many audio tracks for the clip writer", 0);
            return;
        }
        (*env)->GetIntArrayRegion(env, audioChannelsArray, 0, trackCount, channelsOf);
    }
    for (int track = 0; track < trackCount; track++) {
        /* A tone above the Nyquist frequency is not a distinct tone, it is a wrong one that reads
         * as a decoder fault. Refused by name rather than clamped, because clamping would give two
         * tracks the same tone and make the test that tells them apart pass by accident.
         *
         * The highest tone is this track's own, so a clip of many MONO tracks is not refused for
         * frequencies none of its channels would have carried. */
        if (channelsOf[track] <= 0) {
            continue;
        }
        int topChannel = channelsOf[track] - 1;
        double top = 440.0 * (2 * track + 1) * (1 << (topChannel > 3 ? 3 : topChannel));
        if (top > sampleRate * 0.4) {
            throwFfmpeg(env, "the clip writer cannot give every track a distinct tone below the "
                             "Nyquist frequency at this sample rate", 0);
            return;
        }
    }

    const char *path = (*env)->GetStringUTFChars(env, pathString, NULL);
    if (path == NULL) {
        return;
    }

    AVFormatContext *format = NULL;
    AVCodecContext *videoCodec = NULL;
    AVCodecContext *audioCodecs[MAX_WRITE_AUDIO_TRACKS] = {NULL};
    AVStream *audioStreams[MAX_WRITE_AUDIO_TRACKS] = {NULL};
    AVCodecContext *subtitleCodecs[MAX_WRITE_SUBTITLE_TRACKS] = {NULL};
    AVStream *subtitleStreams[MAX_WRITE_SUBTITLE_TRACKS] = {NULL};
    int subtitleTracks = 0;
    AVFrame *picture = NULL;
    AVFrame *samples = NULL;
    AVPacket *packet = NULL;
    const char *problem = NULL;
    int result = 0;

    result = avformat_alloc_output_context2(&format, NULL, "mp4", path);
    if (result < 0 || format == NULL) {
        problem = "cannot create an MP4 output";
        goto done;
    }

    AVStream *videoStream = avformat_new_stream(format, NULL);
    if (videoStream == NULL) {
        problem = "cannot add a video stream";
        goto done;
    }
    videoCodec = avcodec_alloc_context3(videoEncoder);
    if (videoCodec == NULL) {
        problem = "cannot allocate the video encoder";
        goto done;
    }
    /* MJPEG stores JPEG, and JPEG is full range by construction, so it takes YUVJ420P and the
     * decoder reports the picture back as full range. Asking it for studio range instead would
     * either be refused or silently re-encoded, and the samples a test wrote would not be the
     * samples it read. */
    int intraOnly = wanted == AV_CODEC_ID_MJPEG;
    videoCodec->width = width;
    videoCodec->height = height;
    videoCodec->pix_fmt = intraOnly ? AV_PIX_FMT_YUVJ420P : AV_PIX_FMT_YUV420P;
    videoCodec->time_base = (AVRational) {rateDen, rateNum};
    videoCodec->framerate = (AVRational) {rateNum, rateDen};
    videoCodec->colorspace = AVCOL_SPC_BT709;
    videoCodec->color_range = intraOnly ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    if (intraOnly) {
        /* A fixed, high quality rather than a bit rate: what a test asserts about samples is only
         * as stable as the quantiser that produced them, and a rate controller moves it. */
        videoCodec->flags |= AV_CODEC_FLAG_QSCALE;
        videoCodec->global_quality = FF_QP2LAMBDA * 2;
        videoCodec->gop_size = 1;
    } else {
        videoCodec->gop_size = 12;
        videoCodec->bit_rate = (int64_t) width * height * 4;
    }
    if (format->oformat->flags & AVFMT_GLOBALHEADER) {
        videoCodec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    result = avcodec_open2(videoCodec, videoEncoder, NULL);
    if (result < 0) {
        problem = "cannot open the video encoder";
        goto done;
    }
    videoStream->time_base = videoCodec->time_base;
    result = avcodec_parameters_from_context(videoStream->codecpar, videoCodec);
    if (result < 0) {
        problem = "cannot describe the video stream";
        goto done;
    }

    for (int track = 0; track < trackCount; track++) {
        int channels = channelsOf[track];
        if (channels <= 0) {
            continue; /* 0 is how the one-track caller says "no soundtrack at all" */
        }
        const AVCodec *audioEncoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (audioEncoder == NULL) {
            problem = "this build has no AAC encoder";
            goto done;
        }
        AVStream *stream = avformat_new_stream(format, NULL);
        AVCodecContext *codec = avcodec_alloc_context3(audioEncoder);
        audioStreams[track] = stream;
        audioCodecs[track] = codec;
        if (stream == NULL || codec == NULL) {
            problem = "cannot add an audio stream";
            goto done;
        }
        codec->sample_fmt = AV_SAMPLE_FMT_FLTP;
        codec->sample_rate = sampleRate;
        codec->bit_rate = 96000 * (channels > 2 ? 3 : channels);
        av_channel_layout_default(&codec->ch_layout, channels);
        codec->time_base = (AVRational) {1, sampleRate};
        if (format->oformat->flags & AVFMT_GLOBALHEADER) {
            codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        result = avcodec_open2(codec, audioEncoder, NULL);
        if (result < 0) {
            problem = "cannot open the AAC encoder";
            goto done;
        }
        stream->time_base = codec->time_base;
        result = avcodec_parameters_from_context(stream->codecpar, codec);
        if (result < 0) {
            problem = "cannot describe the audio stream";
            goto done;
        }
        /* On the STREAM's dictionary and not the codec context's: the muxer writes the track
         * header from the stream, and a tag left on the context reaches the file from nowhere. */
        if (audioLanguages != NULL) {
            jstring tag = (jstring) (*env)->GetObjectArrayElement(env, audioLanguages, track);
            if (tag != NULL) {
                const char *text = (*env)->GetStringUTFChars(env, tag, NULL);
                if (text != NULL) {
                    av_dict_set(&stream->metadata, "language", text, 0);
                    (*env)->ReleaseStringUTFChars(env, tag, text);
                }
                (*env)->DeleteLocalRef(env, tag);
            }
        }
    }

    if (subtitleLanguages != NULL) {
        subtitleTracks = (*env)->GetArrayLength(env, subtitleLanguages);
        if (subtitleTracks > MAX_WRITE_SUBTITLE_TRACKS) {
            problem = "too many subtitle tracks for the clip writer";
            goto done;
        }
    }
    for (int track = 0; track < subtitleTracks; track++) {
        const AVCodec *subtitleEncoder = avcodec_find_encoder(AV_CODEC_ID_MOV_TEXT);
        if (subtitleEncoder == NULL) {
            problem = "this build has no mov_text encoder";
            goto done;
        }
        AVStream *stream = avformat_new_stream(format, NULL);
        AVCodecContext *codec = avcodec_alloc_context3(subtitleEncoder);
        subtitleStreams[track] = stream;
        subtitleCodecs[track] = codec;
        if (stream == NULL || codec == NULL) {
            problem = "cannot add a subtitle stream";
            goto done;
        }
        /* Milliseconds, which is the unit tx3g samples are timed in and what the decoder rescales
         * back out of. */
        codec->time_base = (AVRational) {1, 1000};
        /* The encoder parses this at open and keeps a style context from it; without one every
         * later encode refuses the line it is handed. av_memdup because avcodec_free_context frees
         * it, and a pointer into static storage would be freed with it. */
        codec->subtitle_header = av_memdup(CLIP_ASS_HEADER, sizeof(CLIP_ASS_HEADER));
        if (codec->subtitle_header == NULL) {
            problem = "out of memory";
            goto done;
        }
        codec->subtitle_header_size = (int) sizeof(CLIP_ASS_HEADER) - 1;
        if (format->oformat->flags & AVFMT_GLOBALHEADER) {
            codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        result = avcodec_open2(codec, subtitleEncoder, NULL);
        if (result < 0) {
            problem = "cannot open the mov_text encoder";
            goto done;
        }
        stream->time_base = codec->time_base;
        result = avcodec_parameters_from_context(stream->codecpar, codec);
        if (result < 0) {
            problem = "cannot describe the subtitle stream";
            goto done;
        }
        jstring tag = (jstring) (*env)->GetObjectArrayElement(env, subtitleLanguages, track);
        if (tag != NULL) {
            const char *text = (*env)->GetStringUTFChars(env, tag, NULL);
            if (text != NULL) {
                av_dict_set(&stream->metadata, "language", text, 0);
                (*env)->ReleaseStringUTFChars(env, tag, text);
            }
            (*env)->DeleteLocalRef(env, tag);
        }
    }

    result = avio_open(&format->pb, path, AVIO_FLAG_WRITE);
    if (result < 0) {
        problem = "cannot write to that path";
        goto done;
    }
    result = avformat_write_header(format, NULL);
    if (result < 0) {
        problem = "cannot write the MP4 header";
        goto done;
    }

    picture = av_frame_alloc();
    packet = av_packet_alloc();
    if (picture == NULL || packet == NULL) {
        problem = "out of memory";
        goto done;
    }
    picture->format = videoCodec->pix_fmt;
    picture->width = width;
    picture->height = height;
    result = av_frame_get_buffer(picture, 0);
    if (result < 0) {
        problem = "cannot allocate a picture";
        goto done;
    }

    /* Eight vertical bars in the studio code table, shifting one bar per picture so that a stream
     * which is not advancing is visible without a stopwatch and a picture delivered out of order
     * is visible without a timestamp. */
    static const uint8_t BAR_Y[8] = {235, 210, 170, 145, 106, 81, 41, 16};
    static const uint8_t BAR_CB[8] = {128, 16, 166, 54, 202, 90, 240, 128};
    static const uint8_t BAR_CR[8] = {128, 146, 16, 34, 222, 240, 110, 128};

    for (int index = 0; index < frames; index++) {
        result = av_frame_make_writable(picture);
        if (result < 0) {
            problem = "cannot make a picture writable";
            goto done;
        }
        for (int y = 0; y < height; y++) {
            uint8_t *row = picture->data[0] + (size_t) y * picture->linesize[0];
            for (int x = 0; x < width; x++) {
                row[x] = BAR_Y[((x * 8 / width) + index) % 8];
            }
        }
        for (int y = 0; y < (height + 1) / 2; y++) {
            uint8_t *cb = picture->data[1] + (size_t) y * picture->linesize[1];
            uint8_t *cr = picture->data[2] + (size_t) y * picture->linesize[2];
            for (int x = 0; x < (width + 1) / 2; x++) {
                int bar = ((x * 2 * 8 / width) + index) % 8;
                cb[x] = BAR_CB[bar];
                cr[x] = BAR_CR[bar];
            }
        }
        picture->pts = index;
        result = avcodec_send_frame(videoCodec, picture);
        if (result < 0) {
            problem = "cannot encode a picture";
            goto done;
        }
        while ((result = avcodec_receive_packet(videoCodec, packet)) == 0) {
            /* One picture's worth, in the encoder's time base, before the rescale takes it to the
             * muxer's. Without it the interleaver has no way to know when the last sample of a
             * track ends: it holds that sample back waiting for a successor that never comes, and
             * av_write_trailer discards it, so the file is written with one picture fewer than
             * was encoded, silently, and only for the clips where the buffer happened to be
             * holding one. It also gives the track an honest duration; derived from timestamps
             * alone the last sample appears to last no time at all. */
            packet->duration = 1;
            av_packet_rescale_ts(packet, videoCodec->time_base, videoStream->time_base);
            packet->stream_index = videoStream->index;
            result = av_interleaved_write_frame(format, packet);
            av_packet_unref(packet);
            if (result < 0) {
                problem = "cannot write a video packet";
                goto done;
            }
        }
        if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
            problem = "the video encoder failed";
            goto done;
        }
    }
    avcodec_send_frame(videoCodec, NULL);
    while (avcodec_receive_packet(videoCodec, packet) == 0) {
        packet->duration = 1;
        av_packet_rescale_ts(packet, videoCodec->time_base, videoStream->time_base);
        packet->stream_index = videoStream->index;
        av_interleaved_write_frame(format, packet);
        av_packet_unref(packet);
    }

    for (int track = 0; track < trackCount; track++) {
        AVStream *audioStream = audioStreams[track];
        AVCodecContext *audioCodec = audioCodecs[track];
        if (audioStream == NULL) {
            continue;
        }
        av_frame_free(&samples);
        samples = av_frame_alloc();
        if (samples == NULL) {
            problem = "out of memory";
            goto done;
        }
        samples->format = AV_SAMPLE_FMT_FLTP;
        samples->sample_rate = sampleRate;
        samples->nb_samples = audioCodec->frame_size > 0 ? audioCodec->frame_size : 1024;
        av_channel_layout_copy(&samples->ch_layout, &audioCodec->ch_layout);
        result = av_frame_get_buffer(samples, 0);
        if (result < 0) {
            problem = "cannot allocate an audio frame";
            goto done;
        }
        /* As many samples as the pictures last, so every track ends with them and the file has
         * something to interleave. The tone is 440 Hz for the first track's first channel: one
         * octave up per extra channel, so a test can tell a downmix from a copy of channel 0, and
         * an odd multiple per extra track, so a test can tell which TRACK it is hearing. Odd times
         * a power of two is unique, so no two of these frequencies collide. */
        int64_t wanted = (int64_t) frames * sampleRate * rateDen / rateNum;
        int64_t position = 0;
        while (position < wanted) {
            result = av_frame_make_writable(samples);
            if (result < 0) {
                problem = "cannot make an audio frame writable";
                goto done;
            }
            for (int channel = 0; channel < channelsOf[track]; channel++) {
                float *data = (float *) samples->data[channel];
                double frequency = 440.0 * (2 * track + 1) * (1 << (channel % 4));
                for (int i = 0; i < samples->nb_samples; i++) {
                    double t = (double) (position + i) / sampleRate;
                    data[i] = (float) (0.25 * sin(2.0 * M_PI * frequency * t));
                }
            }
            samples->pts = position;
            result = avcodec_send_frame(audioCodec, samples);
            if (result < 0) {
                problem = "cannot encode audio";
                goto done;
            }
            while ((result = avcodec_receive_packet(audioCodec, packet)) == 0) {
                av_packet_rescale_ts(packet, audioCodec->time_base, audioStream->time_base);
                packet->stream_index = audioStream->index;
                result = av_interleaved_write_frame(format, packet);
                av_packet_unref(packet);
                if (result < 0) {
                    problem = "cannot write an audio packet";
                    goto done;
                }
            }
            if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
                problem = "the audio encoder failed";
                goto done;
            }
            position += samples->nb_samples;
        }
        avcodec_send_frame(audioCodec, NULL);
        while (avcodec_receive_packet(audioCodec, packet) == 0) {
            av_packet_rescale_ts(packet, audioCodec->time_base, audioStream->time_base);
            packet->stream_index = audioStream->index;
            av_interleaved_write_frame(format, packet);
            av_packet_unref(packet);
        }
    }

    for (int track = 0; track < subtitleTracks; track++) {
        AVStream *subtitleStream = subtitleStreams[track];
        AVCodecContext *subtitleCodec = subtitleCodecs[track];
        int cues = (frames + CLIP_PICTURES_PER_CUE - 1) / CLIP_PICTURES_PER_CUE;
        if (cues < 1) {
            cues = 1;
        }
        for (int cue = 0; cue < cues; cue++) {
            char dialogue[160];
            char body[96];
            if (cue == 0) {
                /* Markup, and only here; see this function's note. The tags are ones the mov_text
                 * encoder carries into tx3g style records and the decoder reconstructs, so what
                 * comes back really is a line with override codes in it rather than a line that
                 * merely had some when it was written. */
                snprintf(body, sizeof(body), "{\\i1}T%d C0{\\i0}\\Nsecond line", track);
            } else {
                snprintf(body, sizeof(body), "T%d C%d", track, cue);
            }
            /* ff_ass_split_dialog's nine fields: ReadOrder, Layer, Style, Name, the three margins,
             * Effect, Text. The encoder parses exactly this and nothing else. */
            snprintf(dialogue, sizeof(dialogue), "%d,0,Default,,0,0,0,,%s", cue, body);

            AVSubtitleRect rect = {0};
            rect.type = SUBTITLE_ASS;
            rect.ass = dialogue;
            AVSubtitleRect *rects[1] = {&rect};
            AVSubtitle subtitle = {0};
            subtitle.format = 1; /* text */
            subtitle.num_rects = 1;
            subtitle.rects = rects;
            /* Contiguous: this cue ends exactly where the next one begins, so the track covers the
             * clip with no gap. An MP4 subtitle track with gaps needs empty samples across them,
             * and not needing any is the whole reason the cues are written this way. */
            int64_t startMs = (int64_t) cue * CLIP_PICTURES_PER_CUE * 1000 * rateDen / rateNum;
            int64_t endPicture = (int64_t) (cue + 1) * CLIP_PICTURES_PER_CUE;
            if (endPicture > frames) {
                endPicture = frames;
            }
            int64_t endMs = endPicture * 1000 * rateDen / rateNum;
            if (endMs <= startMs) {
                endMs = startMs + 1;
            }
            subtitle.end_display_time = (uint32_t) (endMs - startMs);

            /* avcodec_encode_subtitle writes into a buffer the caller sizes, which is the one place
             * in this file where a fixed size is a judgement rather than a fact: a tx3g sample is
             * the text plus a couple of style records, and these lines are under fifty bytes. The
             * encoder reports overflow rather than writing past the end. */
            uint8_t encoded[1024];
            int written = avcodec_encode_subtitle(subtitleCodec, encoded, sizeof(encoded),
                                                  &subtitle);
            /* No avsubtitle_free: nothing above was allocated by libavcodec, and freeing a stack
             * rect through it would be a wild free. */
            if (written < 0) {
                result = written;
                problem = "cannot encode a subtitle cue";
                goto done;
            }
            result = av_new_packet(packet, written);
            if (result < 0) {
                problem = "out of memory";
                goto done;
            }
            memcpy(packet->data, encoded, (size_t) written);
            packet->pts = startMs;
            packet->dts = startMs;
            packet->duration = endMs - startMs;
            av_packet_rescale_ts(packet, subtitleCodec->time_base, subtitleStream->time_base);
            packet->stream_index = subtitleStream->index;
            result = av_interleaved_write_frame(format, packet);
            av_packet_unref(packet);
            if (result < 0) {
                problem = "cannot write a subtitle packet";
                goto done;
            }
        }
    }

    result = av_write_trailer(format);
    if (result < 0) {
        problem = "cannot finish the MP4";
    }

done:
    (*env)->ReleaseStringUTFChars(env, pathString, path);
    av_frame_free(&picture);
    av_frame_free(&samples);
    av_packet_free(&packet);
    avcodec_free_context(&videoCodec);
    for (int track = 0; track < MAX_WRITE_AUDIO_TRACKS; track++) {
        avcodec_free_context(&audioCodecs[track]);
    }
    for (int track = 0; track < MAX_WRITE_SUBTITLE_TRACKS; track++) {
        avcodec_free_context(&subtitleCodecs[track]);
    }
    if (format != NULL) {
        if (format->pb != NULL) {
            avio_closep(&format->pb);
        }
        avformat_free_context(format);
    }
    if (problem != NULL) {
        throwFfmpeg(env, problem, result < 0 ? result : 0);
    }
}

/*
 * The JNI surface's version, checked by FfmpegLibrary before anything else is called.
 *
 * This file and limn.video.ffmpeg.FfmpegNative are two halves of one interface that now live in
 * two repositories and release apart (ADR 037). Every entry point above is bound by NAME, so a
 * Java half that expects a method this shim does not have finds out as an UnsatisfiedLinkError in
 * the middle of a decode. This number turns that into a sentence at load time: bump it in both
 * places whenever a native signature changes, and never otherwise.
 */
#define LIMN_FFMPEG_ABI 1

JNIEXPORT jint JNICALL Java_limn_video_ffmpeg_FfmpegNative_abi(JNIEnv *env, jclass cls) {
    (void) env;
    (void) cls;
    return LIMN_FFMPEG_ABI;
}
