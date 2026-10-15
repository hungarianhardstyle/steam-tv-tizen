#include "media.h"
#include "video_codec.h"
#include "video_timing.h"
#include "audio_batch.h"
#include "media_append.h"
#include "video_metrics.h"
#include "decoder_mode.h"
#include <emscripten.h>
#include <opus.h>
#include <atomic>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>
extern "C" {
#include "session/packet.h"
}
#include "samsung/html/html_media_element.h"
#include "samsung/wasm/elementary_audio_track_config.h"
#include "samsung/wasm/elementary_video_track_config.h"
#include "samsung/wasm/elementary_media_packet.h"
#include "samsung/wasm/elementary_media_stream_source.h"
#include "samsung/wasm/elementary_media_stream_source_listener.h"
#include "samsung/wasm/elementary_media_track_listener.h"

using namespace samsung::wasm;
/* Never call JavaScript from a library thread here. The session's socket receive thread runs the
 * media start callbacks, and on this TV a MAIN_THREAD_EM_ASM proxy call from such a thread does not
 * return: on 2026-10-14 10:56 the receive path stopped dead 170 ms after the audio channel started
 * (the host's own log shows it kept sending, the client counted 0 control packets for the next 24
 * seconds), which is why StartVideoData could never arrive. Status() therefore only buffers the
 * line; the main loop writes it on screen and forwards it to the host. */
static void Status(const char *text) {
    TVStatus(text);
}

// Opus TOC frame duration and count, RFC 6716 sections 3.1 and 3.2.
static unsigned OpusDurationUs(const uint8_t *data, size_t size) {
    if (!size) return 0;
    unsigned config = data[0] >> 3;
    unsigned frame = config >= 16 ? 2500u << (config & 3) : config >= 12 ? 10000u << (config & 1) : (config & 3) == 3 ? 60000u : 10000u << (config & 3);
    unsigned code = data[0] & 3;
    unsigned count = code == 0 ? 1 : code == 3 ? (size >= 2 ? data[1] & 63 : 0) : 2;
    unsigned duration = frame * count;
    return duration <= 120000 ? duration : 0;
}

extern "C" void TVMediaSelfCheck(void) {
    const uint8_t tiny[] = { 0x80 }, twenty[] = { 0x98 }, invalid[] = { 0x9b, 0x3f };
    assert(OpusDurationUs(tiny, 1) == 2500);
    assert(OpusDurationUs(twenty, 1) == 20000);
    assert(OpusDurationUs(invalid, 2) == 0);
    assert(OpusDurationUs(tiny, 0) == 0);
    // The codec string decides which hardware decoder the TV picks, so the level ladder is
    // checked on the device. A mismatch is REPORTED, never asserted: on Tizen an assert inside
    // the proxied main() thread kills the app without any message, which looks exactly like a
    // hang on the startup screen.
    const struct { IHS_StreamVideoCodec codec; uint32_t width, height, fps; const char *expected; } ladder[] = {
        { IHS_StreamVideoCodecH264, 1024, 576, 30, "video/mp4; codecs=\"avc1.64001F\"" },
        { IHS_StreamVideoCodecH264, 1920, 1080, 60, "video/mp4; codecs=\"avc1.64002A\"" },
        { IHS_StreamVideoCodecH264, 2560, 1440, 60, "video/mp4; codecs=\"avc1.640033\"" },
        { IHS_StreamVideoCodecH264, 3840, 2160, 60, "video/mp4; codecs=\"avc1.640034\"" },
        { IHS_StreamVideoCodecHEVC, 2560, 1440, 60, "video/mp4; codecs=\"hvc1.2.4.L150.B0\"" },
        { IHS_StreamVideoCodecHEVC, 3840, 2160, 60, "video/mp4; codecs=\"hvc1.2.4.L153.B0\"" },
    };
    char codec[64], mismatches[192] = { 0 };
    for (size_t index = 0; index < sizeof(ladder) / sizeof(ladder[0]); index++) {
        VideoCodecString(ladder[index].codec, ladder[index].width, ladder[index].height, ladder[index].fps, codec, sizeof(codec));
        if (strcmp(codec, ladder[index].expected) != 0) {
            size_t used = strlen(mismatches);
            snprintf(mismatches + used, sizeof(mismatches) - used, "%ux%u@%u=%s ", ladder[index].width, ladder[index].height, ladder[index].fps, codec);
        }
    }
    if (mismatches[0]) MAIN_THREAD_EM_ASM({ TVLink.log('Kodek', UTF8ToString($0)); }, mismatches);
    int error; OpusDecoder *decoder = opus_decoder_create(48000, 2, &error);
    assert(decoder && error == OPUS_OK);
    const uint8_t silence[] = {0xf8, 0xff, 0xfe}; opus_int16 pcm[1920];
    assert(opus_decode(decoder, silence, sizeof(silence), pcm, 960, 0) == 960);
    opus_decoder_destroy(decoder);
}

// The TV only supports the ultra-low latency pipeline on some models; ask before using it.
// TVLatencyOverride() is a temporary A/B test switch (0 = automatic); the mode finally used is logged
// with the decoder line so a measurement can always be tied to it.
extern "C" int TVLatencyOverride(void);
static EmssLatencyMode LatencyMode() {
    static const bool ultraLow = EmssVersionInfo::Create().has_ultra_low_latency;
    switch (TVLatencyOverride()) {
        case 1: return EmssLatencyMode::kNormal;
        case 2: return EmssLatencyMode::kLow;
        case 3: return ultraLow ? EmssLatencyMode::kUltraLow : EmssLatencyMode::kLow;
        default: break;
    }
    return ultraLow ? EmssLatencyMode::kUltraLow : EmssLatencyMode::kNormal;
}
/* Names the exact mode so an A/B run can never be mis-attributed (kLow and kNormal used to be logged
 * as the same word). */
static const char *LatencyModeName(EmssLatencyMode mode) {
    switch (mode) {
        case EmssLatencyMode::kNormal: return "normal";
        case EmssLatencyMode::kLow: return "low";
        case EmssLatencyMode::kUltraLow: return "ultra-low";
    }
    return "?";
}

/**
 * Ask the TV whether it accepts a video configuration before a track is created. The SDK
 * forwards the codec string, resolution and frame rate to the platform, which is the only
 * authority on what it can decode. Devices without config validation report success and the
 * rejection then surfaces as a track that never opens.
 */
static OperationResult ValidateVideo(IHS_StreamVideoCodec codec, uint32_t width, uint32_t height,
                                     uint32_t fpsNum, uint32_t fpsDen, const std::vector<uint8_t> &extra) {
    static const bool canValidate = EmssVersionInfo::Create().has_config_validation;
    if (!canValidate) return OperationResult::kSuccess;
    char codecString[64];
    VideoCodecString(codec, width, height, fpsDen ? (fpsNum + fpsDen - 1) / fpsDen : 60, codecString, sizeof(codecString));
    ElementaryVideoTrackConfig candidate{codecString, extra, DecodingMode::kHardware, width, height, fpsNum, fpsDen};
    return ElementaryMediaTrack::ValidateVideoConfig(candidate, LatencyMode(), EmssRenderingMode::kMediaElement).operation_result;
}

struct Player : ElementaryMediaStreamSourceListener {
    struct TrackListener : ElementaryMediaTrackListener {
        Player *player;
        bool audio;
        std::atomic<bool> open{false};
        std::atomic<SessionId> id{0};
        explicit TrackListener(Player *owner, bool isAudio = false) : player(owner), audio(isAudio) {}
        void OnTrackOpen() override { std::lock_guard<std::mutex> lock(player->mutex); open = true; player->changed.notify_all(); }
        void OnTrackClosed(ElementaryMediaTrack::CloseReason) override { std::lock_guard<std::mutex> lock(player->mutex); open = false; player->changed.notify_all(); }
        void OnSessionIdChanged(SessionId value) override { id = value; }
        void OnAppendError(OperationResult) override {
            /* An audio append error must never touch player->failed: VideoSubmit checks that flag,
             * so an audio problem used to take the PICTURE down with it (measured 2026-10-14: after
             * the packet-loss concealment was added, an audio append error stopped the video). The
             * audio track is simply marked closed; the picture keeps running. */
            if (audio) { open = false; Status("A hang leallt; a kep folytatodik."); }
            else player->failed = true;
            player->changed.notify_all();
        }
    };
    std::mutex mutex, videoAppendMutex, audioAppendMutex;
    std::condition_variable changed;
    std::atomic<bool> failed{false}, closed{false};
    std::atomic<bool> sourceReady{false};
    std::shared_ptr<std::atomic<bool>> asyncFailed{new std::atomic<bool>(false)};
    TrackListener videoListener{this}, audioListener{this, true};
    samsung::html::HTMLMediaElement element{"video"};
    ElementaryMediaStreamSource source{LatencyMode(), EmssRenderingMode::kMediaElement};
    ElementaryMediaTrack video, audio;
    IHS_StreamVideoCodec codec = IHS_StreamVideoCodecH264;
    uint32_t width = 0, height = 0;
    double frameDuration = 1.0 / 60;
    double videoPts = 0;
    uint32_t mediaOrigin = 0;
    bool haveMediaOrigin = false, haveVideoTimestamp = false, haveAudioTimestamp = false;
    double nextAudioPts = 0;
    AudioPcmBatch pcmBatch{};
    TVVideoTiming videoTiming{};
    // Both media callbacks and configuration changes hold configMutex.
    uint64_t audioPtsUs = 0;
    bool needKeyframe = true;
    void OnSourceClosed() override { std::lock_guard<std::mutex> lock(mutex); closed = true; changed.notify_all(); }
    void OnSourceOpenPending() override { std::lock_guard<std::mutex> lock(mutex); sourceReady = true; changed.notify_all(); }
    void OnSourceOpen() override { std::lock_guard<std::mutex> lock(mutex); sourceReady = true; changed.notify_all(); }
    void OnPipelineError(MediaPipelineError, const char *message) override { failed = true; changed.notify_all(); Status(message); }
    bool Wait(std::function<bool()> predicate) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5), [&] { return failed || *asyncFailed || predicate(); }) && !failed && !*asyncFailed;
    }
};

static std::shared_ptr<Player> player;
static std::mutex configMutex;
static std::atomic<bool> mediaStopping{false};
extern "C" void TVMediaBeginStop(void) { mediaStopping = true; }
static bool haveVideo = false, haveAudio = false;
static IHS_StreamVideoConfig videoConfig{};
static IHS_StreamAudioConfig audioConfig{};
static std::vector<uint8_t> videoExtra;
static OpusDecoder *decoder;
static uint32_t fpsNum = 60, fpsDen = 1;
/* Counters for the JavaScript watchdog: when the picture freezes the client log goes silent, and the
 * browser thread is then the only reporter left (see TVLink.watchdog). Audio problems are counted
 * instead of logged: a log line per bad packet costs a ring write plus a stats-channel packet while
 * the media lock is held, which is exactly what a stuttering stream cannot afford. */
static std::atomic<uint32_t> videoSubmitted{0}, videoRefused{0};
static std::atomic<uint32_t> audioFrames{0}, audioErrors{0}, audioRefused{0};
/* Why a video frame never reached the platform. A plain "refused" total cannot tell a missing track
 * from an oversized frame from a decoder refusal, and the fixes differ completely - so each return
 * path of VideoSubmit has its own counter, reported in the MERES line. Diagnostics only. */
enum TVVideoDropReason {
    TVVideoDropNoTrack = 0,   /* track not open yet, player failed or stopping */
    TVVideoDropEmpty,         /* empty buffer */
    TVVideoDropTooLarge,      /* above the frame size ceiling */
    TVVideoDropNeedKeyframe,  /* waiting for a keyframe, this frame is a delta */
    TVVideoDropPts,           /* timestamp did not advance */
    TVVideoDropAppend,        /* the platform refused the append */
    TVVideoDropReasonCount
};
static std::atomic<uint32_t> videoDrops[TVVideoDropReasonCount];
// configMutex protects these; the old SDK's 64-bit compare_exchange has a broken return contract.
static uint64_t audioAppendUs = 0, audioAppendMaxUs = 0, audioAppendCalls = 0;
static uint32_t audioMissingFrames = 0;
static std::atomic<uint32_t> videoEntries{0}, videoAppends{0}, videoReturns{0}, videoStage{0};
static std::atomic<uint32_t> audioEntries{0}, audioAppends{0}, audioReturns{0}, audioStage{0}, audioPositionMs{0};
extern "C" void TVMediaCounters(unsigned videoSubmittedCount, unsigned videoRefusedCount,
                                unsigned audioPacketCount, unsigned audioErrorCount,
                                unsigned audioRefusedCount, unsigned audioPtsMs);
/* Defined in base.c: upstream packets the kernel refused instead of parking the sender. Declared here
 * because media.cpp builds the measurement line and the probe owns the equivalent declaration. */
extern "C" unsigned long IHS_BaseFailedSendCount(void);
// Close waits for both tracks; unrelated audio/video appends can run concurrently.
static void ClosePlayer() {
    TVMediaBeginStop();
    if (!player) return;
    auto closing = player;
    std::lock_guard<std::mutex> videoLock(closing->videoAppendMutex);
    std::lock_guard<std::mutex> audioLock(closing->audioAppendMutex);
    player->video.SetListener(nullptr);
    if (player->audio.IsValid()) player->audio.SetListener(nullptr);
    player->source.SetListener(nullptr);
    player->element.Pause();
    player->source.Close([](OperationResult) {});
    if (player->element.HasSrc()) player->element.SetSrc(nullptr);
    player.reset();
}

static int Prepare() {
    ClosePlayer();
    if (!haveVideo) return 0;
    player.reset(new Player());
    mediaStopping = false;
    {
        auto caps = EmssVersionInfo::Create();
        char text[192];
        snprintf(text, sizeof(text), "EMSS: available=%d decodingMode=%d ultraLow=%d texture=%d lowTexture=%d configValidation=%d; latency=%s rendering=media-element.",
                 caps.has_emss, caps.has_decoding_mode, caps.has_ultra_low_latency, caps.has_video_texture, caps.has_low_latency_video_texture,
                 /* LatencyModeName(), not a two-way ternary: the ternary printed "normal" for kLow as
                  * well, so a latency-mode A/B run in Low mode would have been logged as "normal" and
                  * could not be told apart from a real Normal run (measurement bug, found 2026-10-15). */
                 caps.has_config_validation, LatencyModeName(LatencyMode()));
        Status(text);
    }
    player->codec = videoConfig.codec;
    player->width = videoConfig.width;
    player->height = videoConfig.height;
    player->frameDuration = double(fpsDen) / fpsNum;
    char codecString[64];
    VideoCodecString(videoConfig.codec, videoConfig.width, videoConfig.height,
                     fpsDen ? (fpsNum + fpsDen - 1) / fpsDen : 60, codecString, sizeof(codecString));
    if (!player->source.SetListener(player.get()) || !player->element.IsValid() || !player->element.SetSrc(&player->source) || !player->Wait([] { return player->closed.load(); })) {
        player->failed = true; Status("A tévé médialejátszója nem nyílt meg."); return -1;
    }
    auto video = player->source.AddTrack(ElementaryVideoTrackConfig{
        codecString, videoExtra, DecodingMode::kHardware,
        videoConfig.width, videoConfig.height, fpsNum, fpsDen
    });
    if (!video) { player->failed = true; Status("A tévé nem fogadta el a H.264 videócsatornát."); return -1; }
    player->video = std::move(*video);
    if (!player->video.SetListener(&player->videoListener)) { player->failed = true; return -1; }
    auto videoId = player->video.GetSessionId();
    if (videoId) player->videoListener.id = *videoId;
    if (haveAudio) {
    auto audio = player->source.AddTrack(ElementaryAudioTrackConfig{
        "audio/webm; codecs=\"pcm\"", {}, DecodingMode::kHardware,
        SampleFormat::kS16, ChannelLayout::kStereo, audioConfig.frequency
    });
    if (!audio) { Status("A hangcsatorna nem nyílt meg; a kép hang nélkül indul."); }
    else {
    player->audio = std::move(*audio);
    player->audio.SetListener(&player->audioListener);
    auto audioId = player->audio.GetSessionId();
    if (audioId) player->audioListener.id = *audioId;
    }
    }
    auto asyncFailed = player->asyncFailed;
    if (!player->source.Open([asyncFailed](OperationResult result) { if (result != OperationResult::kSuccess) *asyncFailed = true; })) { player->failed = true; return -1; }
    if (!player->Wait([] { return player->sourceReady.load(); })) { player->failed = true; Status("A tévé streamforrása nem indult el."); return -1; }
    if (!player->element.Play([asyncFailed](OperationResult result) { if (result != OperationResult::kSuccess) *asyncFailed = true; })) { player->failed = true; return -1; }
    if (!player->Wait([] { return player->videoListener.open.load(); })) {
        player->failed = true;
        Status("A tévé kép/hang dekódere nem indult el öt másodpercen belül."); return -1;
    }
    /* Which decoder the TV actually selected decides what resolutions are possible at all: a
     * software fallback cannot do 1440p60. GetActiveDecodingMode() is a synchronous proxy call, so it
     * is only safe here — Prepare() runs on the main loop, never on a library thread. */
    {
        auto mode = player->video.GetActiveDecodingMode();
        char text[256];
        bool software = mode && *mode == ElementaryMediaTrack::ActiveDecodingMode::kSoftware;
        if (mode) snprintf(text, sizeof(text), "Videó-dekóder mód: %s (%s, %ux%u, %u PPS bájt, %s időzítés).",
                           TVDecodingModeName(*mode),
                           codecString, videoConfig.width, videoConfig.height, (unsigned) videoExtra.size(),
                           LatencyModeName(LatencyMode()));
        else snprintf(text, sizeof(text), "Videó-dekóder mód: ismeretlen (hiba %d, %s).",
                      (int) mode.operation_result, codecString);
        Status(text);
        if (software) {
            Status("A tévé szoftveres videódekódert használ. Ha akadozik, csökkentsd a felbontást.");
        }
    }
    /* Not MAIN_THREAD_EM_ASM: this runs on the session's receive thread (see Status above) and the
     * proxied call would never return, killing the whole inbound path right after the first media
     * callback. The main loop performs the call on the browser thread instead. */
    TVRequestAwaitPicture();
    return 0;
}

/* Runs on the main loop, never on a library thread: opening the TV's media pipeline waits up to
 * five seconds for the platform (Prepare's videoListener wait), and doing that on the session's
 * receive thread blocked every inbound control message and every ACK for the whole wait — after
 * which the host's reliable window was full and it stopped sending anything at all. */
extern "C" int TVMediaPrepare(void) {
    std::lock_guard<std::mutex> lock(configMutex);
    return Prepare();
}

static int VideoStart(IHS_Session *, const IHS_StreamVideoConfig *config, void *) {
    if (!config || (config->codec != IHS_StreamVideoCodecH264 && config->codec != IHS_StreamVideoCodecHEVC)) return -1;
    if (!config->width || config->width > 3840 || !config->height || config->height > 2160 || config->codecDataLen > 65536 || (config->codecDataLen && !config->codecData)) return -1;
    std::lock_guard<std::mutex> lock(configMutex);
    videoConfig = *config;
    videoExtra.clear();
    if (config->codecDataLen) videoExtra.assign(config->codecData, config->codecData + config->codecDataLen);
    haveVideo = true;
    {
        char text[128];
        snprintf(text, sizeof(text), "Steam %s videócsatorna: %ux%u.",
                 config->codec == IHS_StreamVideoCodecHEVC ? "HEVC" : "H.264", config->width, config->height);
        Status(text);
    }
    /* Deferred: this callback runs on the session's receive thread (see TVMediaPrepare). */
    TVRequestMediaPrepare();
    return 0;
}
static int AudioStart(IHS_Session *, const IHS_StreamAudioConfig *config, void *) {
    std::lock_guard<std::mutex> lock(configMutex);
    audioMissingFrames = 0;
    if (player) player->pcmBatch.pending = 0;
    if (decoder) opus_decoder_destroy(decoder);
    decoder = nullptr; haveAudio = false;
    int error = OPUS_BAD_ARG;
    if (config && config->codec == IHS_StreamAudioCodecOpus && config->channels == 2 && config->frequency == 48000) {
        audioConfig = *config;
        decoder = opus_decoder_create(48000, 2, &error);
    }
    haveAudio = decoder && error == OPUS_OK;
    if (!haveAudio) Status("A hangdekóder nem indult el; a kép hang nélkül indul.");
    /* Deferred: this callback runs on the session's receive thread (see TVMediaPrepare). */
    TVRequestMediaPrepare();
    return 0;
}
static IHS_StreamVideoSubmitResult VideoSubmit(IHS_Session *session, uint16_t frameId, IHS_Buffer *buffer, IHS_StreamVideoFrameFlag flags, void *) {
    uint32_t callbackAt = IHS_SessionPacketTimestamp();
    uint32_t readyAt = IHS_SessionVideoFrameReadyTimestamp(session);
    videoEntries++; videoStage = 1;
    std::unique_lock<std::mutex> lock(configMutex);
    uint32_t lockedAt = IHS_SessionPacketTimestamp();
    videoStage = 2;
    if (mediaStopping || !player || !player->videoListener.open || player->failed) { videoDrops[TVVideoDropNoTrack]++; return IHS_StreamVideoSubmitReportLost; }
    TVVideoTiming &timing = player->videoTiming;
    TVVideoTimingWindow &window = timing.window;
    window.callbacks++;
    TVTimingAdd(&window.configWait, TVTimestampDeltaMs(callbackAt, lockedAt));
    TVTimingAdd(&window.readyToCallback, TVTimestampDeltaMs(readyAt, callbackAt));
    if (timing.haveCallback) TVTimingAdd(&window.callbackInterval, TVTimestampDeltaMs(timing.lastCallback, callbackAt));
    timing.lastCallback = callbackAt; timing.haveCallback = true;
    if (timing.haveFrameId) {
        int16_t delta = (int16_t)(frameId - timing.lastFrameId);
        if (delta <= 0) window.idNonmonotonic++;
        else if (delta > 1) window.idGaps += (uint16_t)(delta - 1);
    }
    timing.lastFrameId = frameId; timing.haveFrameId = true;
    if (!buffer || !buffer->size) { videoDrops[TVVideoDropEmpty]++; return IHS_StreamVideoSubmitReportLost; }
    if (buffer->size > 16 * 1024 * 1024) {
        // Too large to be decodable; report it as lost so the stream resyncs on the next keyframe.
        // Returning Error here would tear down the entire session for one bad frame.
        Status("A videókeret túl nagy; a következő kulcskockára várunk.");
        videoDrops[TVVideoDropTooLarge]++; return IHS_StreamVideoSubmitReportLost;
    }
    if (player->needKeyframe && !(flags & IHS_StreamVideoFrameKeyFrame)) {
        IHS_SessionReportVideoFrameComplete(session, frameId, IHS_VideoFrameResultDroppedReset);
        videoDrops[TVVideoDropNeedKeyframe]++; return IHS_StreamVideoSubmitReportLost;
    }
    // Both tracks use the same sender clock, including gaps caused by lost packets.
    double duration = player->frameDuration;
    uint32_t timestamp = IHS_SessionVideoFrameTimestamp(session);
    if (!player->haveMediaOrigin) { player->mediaOrigin = timestamp; player->haveMediaOrigin = true; }
    double pts = MediaPacketTime(player->mediaOrigin, timestamp);
    if (player->haveVideoTimestamp) TVTimingAdd(&window.ptsInterval, (pts - player->videoPts) * 1000.0);
    if (pts < 0 || (player->haveVideoTimestamp && pts <= player->videoPts)) {
        window.ptsNonmonotonic++;
        videoDrops[TVVideoDropPts]++; return IHS_StreamVideoSubmitReportLost;
    }
    player->videoPts = pts;
    player->haveVideoTimestamp = true;
    ElementaryMediaPacket packet{Seconds(player->videoPts), Seconds(player->videoPts), Seconds(duration), !!(flags & IHS_StreamVideoFrameKeyFrame), buffer->size, IHS_BufferPointer(buffer), player->width, player->height, fpsNum, fpsDen, player->videoListener.id.load()};
    // Avoid waiting for the platform while the other media track needs this same lock.
    auto startedAt = std::chrono::steady_clock::now();
    videoAppends++; videoStage = 3;
    auto current = player;
    uint32_t queuedAt = IHS_SessionPacketTimestamp(), sdkAt = 0, sdkEnd = 0;
    auto appendResult = AppendWithoutConfigLock(lock, current->videoAppendMutex,
        [&] {
            sdkAt = IHS_SessionPacketTimestamp();
            auto result = current->video.AppendPacketAsync(packet);
            sdkEnd = IHS_SessionPacketTimestamp();
            return result;
        });
    uint32_t relockedAt = IHS_SessionPacketTimestamp();
    videoReturns++; videoStage = 4;
    if (mediaStopping || player != current) return IHS_StreamVideoSubmitReportLost;
    window.appends++;
    TVTimingAdd(&window.preSdk, TVTimestampDeltaMs(queuedAt, sdkAt));
    TVTimingAdd(&window.sdkCall, TVTimestampDeltaMs(sdkAt, sdkEnd));
    TVTimingAdd(&window.relock, TVTimestampDeltaMs(sdkEnd, relockedAt));
    TVTimingAdd(&window.readyToSdk, TVTimestampDeltaMs(readyAt, sdkAt));
    TVTimingAdd(&window.duration, packet.duration.count() * 1000.0);
    if (timing.haveAppend) TVTimingAdd(&window.appendInterval, TVTimestampDeltaMs(timing.lastAppend, sdkAt));
    timing.lastAppend = sdkAt; timing.haveAppend = true;
    window.lastPts = packet.pts.count(); window.lastDts = packet.dts.count(); window.lastDuration = packet.duration.count();
    unsigned slices = current->codec == IHS_StreamVideoCodecH264 ? TVH264FirstSliceType(IHS_BufferPointer(buffer), buffer->size) : TVSliceUnknown;
    window.sliceP += !!(slices & TVSliceP); window.sliceB += !!(slices & TVSliceB);
    window.sliceI += !!(slices & TVSliceI); window.sliceUnknown += !!(slices & TVSliceUnknown);
    bool appended = appendResult ? true : false;
    uint64_t took = (uint64_t) std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt).count();
    if (!appended) {
        /* One refused frame used to latch a permanent failure, which freezes the picture and then
         * tears the session down. kAppendExpectsKeyframe is a recoverable state: ask the host for a
         * keyframe and keep going, like the audio path does. The platform's error code was thrown
         * away before, so it is logged now. */
        uint32_t refusals = ++videoRefused;
        videoDrops[TVVideoDropAppend]++;
        char text[160];
        snprintf(text, sizeof(text), "A tévé videó-dekódere visszautasította a kockát (%u. eset, hibakód %d).",
                 refusals, (int) appendResult.operation_result);
        TVStatus(text);
        if (refusals >= 120) { player->failed = true; return IHS_StreamVideoSubmitReportLost; }
        IHS_SessionReportVideoFrameComplete(session, frameId, IHS_VideoFrameResultDroppedDecodeSlow);
        return IHS_StreamVideoSubmitReportLost;
    }
    videoSubmitted++;
    static uint64_t frames, totalMs, lastReportMs;
    static size_t keyBytes, deltaBytes;
    static uint32_t keys, deltas;
    frames++; totalMs += took;
    if (flags & IHS_StreamVideoFrameKeyFrame) { keyBytes += buffer->size; keys++; } else { deltaBytes += buffer->size; deltas++; }
    uint64_t nowMs = (uint64_t) std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (nowMs - lastReportMs >= 1000 && frames) {
        char text[352];
        double seconds = (nowMs - lastReportMs) / 1000.0;
        static unsigned lastPresented, lastAudioFrames;
        unsigned presented = TVVideoClockPresented();
        double presentedFps = (presented - lastPresented) / (seconds > 0 ? seconds : 1);
        lastPresented = presented;
        double audioPtsSeconds = player->audioPtsUs / 1000000.0;
        /* Per-second values: the counters are cumulative, and printing them under a "/s" label made a
         * correct 100 packets/s feed look like a runaway one. "frame" is what the host sent and we
         * decoded; "írás" is how many platform appends those frames needed. */
        unsigned audioFrameCount = audioFrames.load();
        double audioFrameRate = (audioFrameCount - lastAudioFrames) / (seconds > 0 ? seconds : 1);
        lastAudioFrames = audioFrameCount;
        uint64_t audioCalls = audioAppendCalls;
        uint64_t audioUs = audioAppendUs;
        double audioMaxMs = audioAppendMaxUs / 1000.0;
        audioAppendCalls = audioAppendUs = audioAppendMaxUs = 0;
        snprintf(text, sizeof(text),
                 "Videó: %.0f kocka/s be, beadás átlag %.1f ms, most %llu ms; kulcs %u db átlag %u KB, delta %u db átlag %u KB; TV megjelenített %.0f kocka/s, óra %.1f s, kép-PTS %.1f s, readyState %d, paused %d; hang %.0f frame/s, %u hiba, %u vissza, %u hiányzó, PTS %.1f s, %llu írás/s, %.1f ms (max %.1f).",
                 frames / (seconds > 0 ? seconds : 1), (double) totalMs / frames, (unsigned long long) took,
                 keys, keys ? (unsigned) (keyBytes / keys / 1024) : 0,
                 deltas, deltas ? (unsigned) (deltaBytes / deltas / 1024) : 0,
                 presentedFps, TVVideoClockTime(), player->videoPts, TVVideoClockReady(), TVVideoClockPaused(),
                 audioFrameRate, (unsigned) audioErrors, (unsigned) audioRefused, audioMissingFrames,
                 audioPtsSeconds, (unsigned long long) audioCalls,
                 audioCalls ? (double) audioUs / audioCalls / 1000.0 : 0.0, audioMaxMs);
        TVStatus(text);
        lastReportMs = nowMs; frames = 0; totalMs = 0; keyBytes = 0; deltaBytes = 0; keys = 0; deltas = 0;
    }
    player->needKeyframe = false;
    /* Frame-stage feedback to the host. Measured evidence (host streaming_log.txt, 2026-10-15
     * 13:28-13:31): "Slow framerate: game 1.00, capture 0.00, convert 0.00, encode 0.00,
     * network 37.83 / 57.50 / 70.42, decode 0.00, display 0.00 (network)" and the session summary
     * "AvgNetworkMS 15.8, AvgDecodeMS 0, AvgDisplayMS 0, AvgFrameMS 18.7" - the host's per-frame
     * accounting never receives our decode/upload milestones, so every frame it cannot close is
     * attributed to the network and its framerate limiter throttles the stream (game 1-10 fps) while
     * the client's own append cost in those very windows is 0.2-0.4 ms. Reporting the two milestones we
     * can actually measure gives the host real data.
     *
     * What we report is what we measure, nothing invented: decode begins when the frame is complete and
     * handed to us (readyAt), ends when the platform append returned. The platform exposes no
     * presentation callback in kMediaElement mode, so the upload/display pair carries the same
     * timestamp as the decode end and display therefore stays 0.00 - that limit is documented in
     * outputs/steam-tizen-measurements.md.
     *
     * Revert: set kReportFrameStages to false (the three calls below are the only change). */
    static const bool kReportFrameStages = true;
    if (kReportFrameStages) {
        const uint32_t decodeEndTs = IHS_SessionPacketTimestamp();
        IHS_SessionReportVideoFrameStage(session, frameId, IHS_VideoFrameStageDecodeBegin, readyAt);
        IHS_SessionReportVideoFrameStage(session, frameId, IHS_VideoFrameStageDecodeEnd, decodeEndTs);
        IHS_SessionReportVideoFrameStage(session, frameId, IHS_VideoFrameStageUploadBegin, decodeEndTs);
        IHS_SessionReportVideoFrameStage(session, frameId, IHS_VideoFrameStageUploadEnd, decodeEndTs);
    }
    // shortcut: platform acceptance approximates completion; use per-frame presentation feedback when available.
    IHS_SessionReportVideoFrameComplete(session, frameId, IHS_VideoFrameResultDisplayed);
    return IHS_StreamVideoSubmitOK;
}
/* The caller owns configMutex; only the platform append releases it. */
static bool AppendAudioPcm(const opus_int16 *pcm, int samples, double pts, std::unique_lock<std::mutex>& lock) {
    if (samples <= 0 || samples > 5760) return false;
    if (!decoder || !player->audioListener.open) return false;
    double duration = samples / 48000.0;
    ElementaryMediaPacket packet{Seconds(pts), Seconds(pts), Seconds(duration), true,
                                 size_t(samples) * 2 * sizeof(opus_int16),
                                 reinterpret_cast<const uint8_t *>(pcm), 0, 0, 0, 0,
                                 player->audioListener.id.load()};
    auto startedAt = std::chrono::steady_clock::now();
    audioAppends++; audioStage = 3;
    auto current = player;
    auto appendResult = AppendWithoutConfigLock(lock, current->audioAppendMutex,
        [&] { return current->audio.AppendPacketAsync(packet); });
    audioReturns++; audioStage = 4;
    if (mediaStopping || player != current || !decoder) return false;
    bool appended = appendResult ? true : false;
    uint64_t tookUs = (uint64_t) std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - startedAt).count();
    audioAppendUs += tookUs;
    audioAppendCalls++;
    audioAppendMaxUs = std::max(audioAppendMaxUs, tookUs);
    if (!appended) {
        /* One refused packet used to destroy the decoder, so a single hiccup in the TV's audio sink
         * silenced the stream for the rest of the session (measured on 2026-10-14 12:52:53: "A hang
         * leállt; a kép folytatódik." after 3 minutes of working audio). Drop the packet and try
         * again; only give up after a long unbroken run of refusals. */
        uint32_t refusals = ++audioRefused;
        if (refusals == 1 || refusals % 100 == 0) {
            char text[160];
            snprintf(text, sizeof(text), "A tévé hangpuffere visszautasította a csomagot (%u. eset, hibakód %d); folytatom.",
                     refusals, (int) appendResult.operation_result);
            Status(text);
        }
        if (refusals >= 2000) {
            Status("A hang végleg leállt; a kép hang nélkül folytatódik.");
            opus_decoder_destroy(decoder); decoder = nullptr; haveAudio = false;
        }
        return true;
    }
    player->audioPtsUs = (uint64_t) ((pts + duration) * 1000000.0);
    audioPositionMs = (unsigned)(player->audioPtsUs / 1000);
    audioStage = 5;
    return true;
}
static int AudioSubmit(IHS_Session *session, IHS_Buffer *buffer, void *) {
    audioEntries++; audioStage = 1;
    // Never wait for a playout deadline in a protocol receive callback.
    std::unique_lock<std::mutex> lock(configMutex);
    audioMissingFrames += IHS_SessionAudioFrameGap(session);
    audioStage = 2;
    if (mediaStopping || !decoder || !player || !player->audioListener.open || player->failed) return 0;
    if (!buffer || !buffer->size) return 0; /* Nothing was received here; never invent audio. */
    if (buffer->size > 65536) return -1;
    uint32_t timestamp = IHS_SessionAudioFrameTimestamp(session);
    double senderPts = MediaPacketTime(player->haveMediaOrigin ? player->mediaOrigin : timestamp, timestamp);
    opus_int16 pcm[5760 * 2];
    int samples = opus_decode(decoder, IHS_BufferPointer(buffer), (opus_int32) buffer->size, pcm, 5760, 0);
    if (samples < 0) {
        /* Counted, not logged: a corrupt packet is the one thing the stutter investigation needs to
         * see, but a log line per packet (with the lock held) is itself a stutter source. The 1 Hz
         * video stats line reports the counter. */
        audioErrors++;
        return 0;
    }
    /* A packet that decodes to nothing must not be appended: a zero-duration packet makes the
     * platform raise an append error, and a burst of those is what silenced audio earlier. */
    if (samples == 0) return 0;
    audioFrames++;
    double duration = samples / 48000.0;
    if (!player->haveMediaOrigin) { player->mediaOrigin = timestamp; player->haveMediaOrigin = true; }
    // Frame IDs reject duplicates upstream; consecutive PCM follows its sample clock despite timestamp jitter.
    double pts = AudioPacketTime(player->nextAudioPts, senderPts, IHS_SessionAudioFrameGap(session), player->haveAudioTimestamp);
    player->nextAudioPts = pts + duration;
    player->haveAudioTimestamp = true;
    if (player->pcmBatch.pending && (IHS_SessionAudioFrameGap(session) || !AudioPcmBatchCanJoin(&player->pcmBatch, samples, pts))) {
        if (!AppendAudioPcm(player->pcmBatch.data, 480, player->pcmBatch.pts, lock)) return 0;
        player->pcmBatch.pending = 0;
        if (!decoder) return 0;
    }
    if (samples == 480) {
        if (AudioPcmBatchPush(&player->pcmBatch, pcm, pts))
            if (!AppendAudioPcm(player->pcmBatch.data, 960, player->pcmBatch.pts, lock)) return 0;
    } else if (!AppendAudioPcm(pcm, samples, pts, lock)) return 0;
    audioStage = 5;
    return 0;
}
static void VideoStop(IHS_Session *session, void *) {
    TVMediaBeginStop();
    TVStatus("VideoStop belepes.");
    /* Run summary: totals and maxima for the whole stream, in the same field order as the periodic
     * MERES line so one run can be compared with another at a glance (see
     * outputs/steam-tizen-measurements.md and work/compare-runs.mjs). Emitted here because this is the
     * last point where the session (and its counters) still exists. */
    if (session != nullptr) {
        IHS_SessionStreamMetrics metrics{};
        IHS_SessionGetStreamMetrics(session, &metrics);
        char line[512];
        snprintf(line, sizeof(line),
                 "SUMMARY v=%s frames=%u keys=%u bytes=%u vasmMax=%.1f varrMax=%.1f vhostMax=%.1f "
                 "audFrames=%u audGaps=%u audMissing=%u audReordered=%u aarrMax=%.1f "
                 "loss=%u rst=%u part=%u win=%u/%u ref=%u/%u drop=%u",
                 TVVersion(), (unsigned) metrics.videoFrames, (unsigned) metrics.videoKeyFrames,
                 (unsigned) metrics.videoBytes, metrics.videoAssemblyUsMax / 1000.0,
                 metrics.videoArrivalUsMax / 1000.0, metrics.videoHostIntervalUsMax / 1000.0,
                 (unsigned) metrics.audioFrames, (unsigned) metrics.audioGaps,
                 (unsigned) metrics.audioMissingFrames, (unsigned) metrics.audioReordered,
                 metrics.audioArrivalUsMax / 1000.0,
                 (unsigned) metrics.videoLossRequests, (unsigned) metrics.videoDecoderResets,
                 (unsigned) metrics.videoPartialCleared, (unsigned) metrics.windowDiscards,
                 (unsigned) metrics.windowOverflows, (unsigned) videoRefused.load(),
                 (unsigned) audioRefused.load(), (unsigned) IHS_BaseFailedSendCount());
        TVStatus(line);
    }
    std::lock_guard<std::mutex> lock(configMutex);
    haveVideo = false; videoExtra.clear();
    // The data worker must return before session teardown joins it; platform close belongs to the main loop.
    TVRequestMediaPrepare();
    TVStatus("VideoStop kilepes.");
}
static void AudioStop(IHS_Session *, void *) {
    std::lock_guard<std::mutex> lock(configMutex);
    if (player) player->pcmBatch.pending = 0;
    haveAudio = false;
    if (decoder) opus_decoder_destroy(decoder);
    decoder = nullptr;
}
static int CaptureSize(IHS_Session *, int width, int height, void *) {
    TVHostStreamStarted();
    if (width <= 0 || width > 3840 || height <= 0 || height > 2160) return -1;
    std::unique_lock<std::mutex> lock(configMutex, std::try_to_lock);
    if (!lock.owns_lock()) { TVStatus("CaptureSize kihagyva: a media-zar foglalt."); return 0; }
    videoConfig.width = width; videoConfig.height = height;
    if (player) { player->width = width; player->height = height; }
    return 0;
}
static void FrameRate(IHS_Session *, uint32_t numerator, uint32_t denominator, uint32_t, void *) {
    /* See CaptureSize: called on the socket receive thread, so never block on configMutex. */
    std::unique_lock<std::mutex> lock(configMutex, std::try_to_lock);
    if (!lock.owns_lock()) { TVStatus("FrameRate kihagyva: a media-zar foglalt."); return; }
    if (numerator && denominator) { fpsNum = numerator; fpsDen = denominator; if (player) player->frameDuration = double(denominator) / numerator; }
}
extern "C" const IHS_StreamVideoCallbacks *TVVideoCallbacks(void) {
    static IHS_StreamVideoCallbacks callbacks{};
    callbacks.start = VideoStart; callbacks.submit = VideoSubmit; callbacks.stop = VideoStop;
    callbacks.setCaptureSize = CaptureSize; callbacks.setTargetFramerate = FrameRate;
    return &callbacks;
}
extern "C" const IHS_StreamAudioCallbacks *TVAudioCallbacks(void) {
    static IHS_StreamAudioCallbacks callbacks{};
    callbacks.start = AudioStart; callbacks.submit = AudioSubmit; callbacks.stop = AudioStop;
    return &callbacks;
}
extern "C" void TVMediaStop(void) {
    TVMediaBeginStop();
    VideoStop(nullptr, nullptr); AudioStop(nullptr, nullptr);
    std::lock_guard<std::mutex> lock(configMutex); ClosePlayer(); fpsNum = 60; fpsDen = 1;
}
extern "C" void TVMediaPublish(IHS_Session *session) {
    TVMediaCounters(videoSubmitted.load(), videoRefused.load(), audioFrames.load(), audioErrors.load(), audioRefused.load(), audioPositionMs.load());
    char text[240];
    snprintf(text, sizeof(text), "Beadás: kép állapot %u, belépés %u, beadás %u/%u visszatért; hang állapot %u, belépés %u, beadás %u/%u visszatért.", videoStage.load(), videoEntries.load(), videoAppends.load(), videoReturns.load(), audioStage.load(), audioEntries.load(), audioAppends.load(), audioReturns.load());
    TVStatus(text);
    TVVideoTimingWindow timing{};
    IHS_SessionStreamMetrics metrics{};
    if (session != nullptr) IHS_SessionGetStreamMetrics(session, &metrics);
    bool havePlayer = false;
    double videoPtsSeconds = 0, audioPtsSeconds = 0, audioAppendAvgMs = 0, audioAppendMaxMs = 0;
    unsigned audioAppendRate = 0;
    {
        std::unique_lock<std::mutex> lock(configMutex, std::try_to_lock);
        if (lock.owns_lock() && player) {
            havePlayer = player->videoTiming.window.callbacks != 0;
            timing = player->videoTiming.window;
            player->videoTiming.window = {};
            videoPtsSeconds = timing.lastPts;
            audioPtsSeconds = player->audioPtsUs / 1000000.0;
        }
    }
    /* Audio feed cost of the last window. Kept outside the config lock so it is reported even when the
     * media lock is contended - that contention is one of the things this line has to show. */
    {
        uint64_t calls = audioAppendCalls;
        uint64_t us = audioAppendUs;
        audioAppendMaxMs = audioAppendMaxUs / 1000.0;
        audioAppendRate = (unsigned) calls;
        audioAppendAvgMs = calls ? (double) us / calls / 1000.0 : 0.0;
        audioAppendCalls = audioAppendUs = audioAppendMaxUs = 0;
    }
    /* One fixed-order, machine-comparable line per two seconds - the primary artefact for comparing a
     * smooth run with a stuttering one. Every field is documented in
     * outputs/steam-tv-meres-jegyzet.md; the names are stable so a log can be parsed automatically by
     * work/compare-runs.mjs. ASCII only, so no encoding can corrupt it on the way to the host. */
    if (havePlayer) {
        /* Rates are differences of the cumulative counters over the real elapsed time between two
         * publish calls. Dividing the cumulative values was a bug: the very first line of a run then
         * reported thousands of frames per second (measured live: fed=1946/s), which would have made
         * the first window of every log useless. */
        static uint64_t lastPublishMs = 0, firstPublishMs = 0;
        static uint32_t prevVideoFrames = 0, prevPresented = 0, prevRvfC = 0;
        static unsigned prevAudioFrames = 0;
        uint64_t nowMs = (uint64_t) std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (!firstPublishMs) firstPublishMs = nowMs;
        double windowSeconds = lastPublishMs && nowMs > lastPublishMs ? (nowMs - lastPublishMs) / 1000.0 : 0.0;
        lastPublishMs = nowMs;
        if (windowSeconds < 0.2) windowSeconds = 2.0; /* the first call has no previous window */
        uint32_t videoFrames = (uint32_t) metrics.videoFrames;
        double fedRate = (videoFrames - prevVideoFrames) / windowSeconds;
        prevVideoFrames = videoFrames;
        unsigned presentedFrames = TVVideoClockPresentedFrames();
        double presentedRate = (presentedFrames - prevPresented) / windowSeconds;
        prevPresented = presentedFrames;
        unsigned rvfC = TVVideoClockPresented();
        double rvfCRate = (rvfC - prevRvfC) / windowSeconds;
        prevRvfC = rvfC;
        unsigned audioFrameCount = audioFrames.load();
        double audioRate = (audioFrameCount - prevAudioFrames) / windowSeconds;
        prevAudioFrames = audioFrameCount;
        double vHostAvg = metrics.videoFrames > 1 && metrics.videoHostIntervalUsSum
                          ? (double) metrics.videoHostIntervalUsSum / (metrics.videoFrames - 1) / 1000.0 : 0.0;
        double vArrAvg = metrics.videoFrames > 1 && metrics.videoArrivalUsSum
                         ? (double) metrics.videoArrivalUsSum / (metrics.videoFrames - 1) / 1000.0 : 0.0;
        double vAsmAvg = metrics.videoFrames ? (double) metrics.videoAssemblyUsSum / metrics.videoFrames / 1000.0 : 0.0;
        double aArrAvg = metrics.audioFrames > 1 && metrics.audioArrivalUsSum
                         ? (double) metrics.audioArrivalUsSum / (metrics.audioFrames - 1) / 1000.0 : 0.0;
        double aHostAvg = metrics.audioFrames > 1 && metrics.audioHostIntervalUsSum
                          ? (double) metrics.audioHostIntervalUsSum / (metrics.audioFrames - 1) / 1000.0 : 0.0;
        char line[600];
        snprintf(line, sizeof(line),
                 "MERES v=%s t=%.0f fed=%.1f prez=%.1f rvfc=%.1f clock=%.2f media=%.2f vpts=%.2f apts=%.2f "
                 "vhost=%.1f/%.1f varr=%.1f/%.1f vasm=%.1f/%.1f vdec=%.1f/%.1f vwait=%.1f/%.1f vint=%.1f/%.1f "
                 "aud=%.1f/%u/%u/%u ahost=%.1f/%.1f aarr=%.1f/%.1f aapp=%.1f/%.1f(%u) "
                 "loss=%u rst=%u part=%u win=%u/%u ref=%u/%u drop=%u loop=%u/%u pad=%u/%u proxy=%u/%u "
                 "vdrop=%u/%u/%u/%u/%u/%u lossr=%u/%u/%u/%u/%u/%u",
                 TVVersion(), (double) (nowMs - firstPublishMs) / 1000.0,
                 fedRate, presentedRate,
                 rvfCRate,
                 TVVideoClockTime(), TVVideoClockMediaTime(), videoPtsSeconds, audioPtsSeconds,
                 vHostAvg, metrics.videoHostIntervalUsWindowMax / 1000.0,
                 vArrAvg, metrics.videoArrivalUsWindowMax / 1000.0,
                 vAsmAvg, metrics.videoAssemblyUsWindowMax / 1000.0,
                 TVTimingAverage(&timing.sdkCall), timing.sdkCall.max,
                 TVTimingAverage(&timing.configWait), timing.configWait.max,
                 TVTimingAverage(&timing.callbackInterval), timing.callbackInterval.max,
                 audioRate, (unsigned) audioErrors.load(),
                 (unsigned) audioRefused.load(), (unsigned) metrics.audioMissingFrames,
                 aHostAvg, metrics.audioHostIntervalUsMax / 1000.0,
                 aArrAvg, metrics.audioArrivalUsWindowMax / 1000.0,
                 audioAppendAvgMs, audioAppendMaxMs, audioAppendRate,
                 (unsigned) metrics.videoLossRequests, (unsigned) metrics.videoDecoderResets,
                 (unsigned) metrics.videoPartialCleared, (unsigned) metrics.windowDiscards,
                 (unsigned) metrics.windowOverflows, (unsigned) videoRefused.load(),
                 (unsigned) audioRefused.load(), (unsigned) IHS_BaseFailedSendCount(),
                 timing.callbacks, timing.appends, TVVideoClockPadPolls(), TVVideoClockPadMaxGap(),
                 TVVideoClockProxyUs(), TVVideoClockProxyMaxUs(),
                 (unsigned) videoDrops[TVVideoDropNoTrack].load(), (unsigned) videoDrops[TVVideoDropEmpty].load(),
                 (unsigned) videoDrops[TVVideoDropTooLarge].load(), (unsigned) videoDrops[TVVideoDropNeedKeyframe].load(),
                 (unsigned) videoDrops[TVVideoDropPts].load(), (unsigned) videoDrops[TVVideoDropAppend].load(),
                 (unsigned) metrics.videoLossByReason[IHS_VideoLossReasonSequence],
                 (unsigned) metrics.videoLossByReason[IHS_VideoLossReasonWaitTimeout],
                 (unsigned) metrics.videoLossByReason[IHS_VideoLossReasonDecrypt],
                 (unsigned) metrics.videoLossByReason[IHS_VideoLossReasonPartialStall],
                 (unsigned) metrics.videoLossByReason[IHS_VideoLossReasonAssembleMismatch],
                 (unsigned) metrics.videoLossByReason[IHS_VideoLossReasonSubmitLost]);
        TVStatus(line);
    }
    char line[360];
#define TV_TIMING_VALUES(h) TVTimingAverage(&(h)), TVTimingP95Upper(&(h)), (h).max
    snprintf(line, sizeof(line), "Idozites: ms avg/p95<=/max; cb %.2f/%.2f/%.2f; cfg %.2f/%.2f/%.2f; READY-cb %.2f/%.2f/%.2f; READY-SDK %.2f/%.2f/%.2f.",
             TV_TIMING_VALUES(timing.callbackInterval), TV_TIMING_VALUES(timing.configWait),
             TV_TIMING_VALUES(timing.readyToCallback), TV_TIMING_VALUES(timing.readyToSdk));
    TVStatus(line);
    snprintf(line, sizeof(line), "Idozites: ms avg/p95<=/max; SDK %.2f/%.2f/%.2f; SDKinterval %.2f/%.2f/%.2f; preSDK %.2f/%.2f/%.2f; relock %.2f/%.2f/%.2f.",
             TV_TIMING_VALUES(timing.sdkCall), TV_TIMING_VALUES(timing.appendInterval),
             TV_TIMING_VALUES(timing.preSdk), TV_TIMING_VALUES(timing.relock));
    TVStatus(line);
    uint32_t invalid = timing.callbackInterval.invalid + timing.configWait.invalid + timing.readyToCallback.invalid + timing.readyToSdk.invalid +
                       timing.sdkCall.invalid + timing.appendInterval.invalid + timing.preSdk.invalid + timing.relock.invalid;
    snprintf(line, sizeof(line), "Idozites: PTSdelta ms %.2f/%.2f/%.2f; dur ms %.2f/%.2f/%.2f; last PTS/DTS %.6f/%.6f dur %.6f; cb/append %u/%u PTSbad %u IDbad/gap %u/%u clockbad %u; firstVCL B/P/I/? %u/%u/%u/%u; DTS=PTS synthetic.",
             TV_TIMING_VALUES(timing.ptsInterval), TV_TIMING_VALUES(timing.duration),
             timing.lastPts, timing.lastDts, timing.lastDuration, timing.callbacks, timing.appends,
             timing.ptsNonmonotonic, timing.idNonmonotonic, timing.idGaps, invalid,
             timing.sliceB, timing.sliceP, timing.sliceI, timing.sliceUnknown);
    TVStatus(line);
#undef TV_TIMING_VALUES
}
extern "C" bool TVMediaFailed(void) {
    /* Never block the main loop here. A video append holds this lock for as long as the TV's platform
     * call takes, and this check runs on every main-loop iteration: waiting for the lock turned one
     * slow append into a client that stops logging, stops answering the remote and stops reading its
     * own command queue — a frozen picture with an app that looks dead (measured 2026-10-14). The
     * flag is only read for a failure report, so skipping a poll is harmless. */
    std::unique_lock<std::mutex> lock(configMutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    return player && (player->failed || *player->asyncFailed);
}
