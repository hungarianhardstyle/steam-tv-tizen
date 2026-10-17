#include <emscripten.h>
#include <emscripten/threading.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "diagnostic_filter.h"
#include <unistd.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "ihslib.h"
#include "protobuf/remoteplay.pb-c.h"
#include "protobuf/pb_utils.h"
#include "ihs_thread.h"

/* Internal session helper (defined in session.c, not part of the public headers). */
bool IHS_SessionSendControlMessage(IHS_Session *session, EStreamControlMessage type,
                                   const ProtobufCMessage *message);
#include "ihs_timer.h"
#include "crypto.h"
#include "client/pubkeys.h"
#include "media.h"
#include "gamepad.h"
#include "hid/provider.h"
#include "teardown.h"

static IHS_HostInfo target;
static IHS_Mutex *target_lock;
static int found;
static IHS_Session *session;
static IHS_HIDProvider *gamepad_provider;
static TVTeardown teardown;
static bool teardown_active;
static char teardown_text[192];
static bool teardown_repair;
static uint64_t teardown_started;
static int teardown_reported_stage = -1;
static IHS_ClientConfig stream_config;
static atomic_bool session_requested;
static atomic_bool picture_ready;
static uint64_t request_started;
static int failure_pending, repair_pairing;
static char failure_text[192];
static int pending_session;
static IHS_SessionInfo pending_info;
static atomic_int phase;
// Stream settings chosen on the TV (see TVLink.presets / TVLink.frameRates in app.js).
static uint32_t stream_width = 1920, stream_height = 1080, stream_fps = 60;
static uint32_t stream_bitrate_choice; /* 0 = automatic */
static uint32_t stream_codec_choice;   /* 0 = automatic, 1 = H.264, 2 = HEVC */
/* Latency-mode override for A/B measurements: 0 = automatic (TV capability decides), 1 = normal,
 * 2 = low, 3 = ultra-low. Read by media.cpp through TVLatencyOverride(). */
static uint32_t latency_override;
static bool stream_audio = true;
static bool stream_hevc = false;

/**
 * Read the resolution, frame rate, bitrate and audio choice selected in the app. Only runs on the
 * main loop thread: MAIN_THREAD_EM_ASM blocks until the browser thread services it, so calling it
 * from the session/timer threads would deadlock.
 */
static void load_stream_settings(void) {
    uint32_t values[7] = { stream_width, stream_height, stream_fps, stream_bitrate_choice,
                           stream_audio ? 1u : 0u, stream_codec_choice, latency_override };
    MAIN_THREAD_EM_ASM({ TVLink.settings($0); }, values);
    if (values[0] < 1024 || values[0] > 3840 || values[1] < 576 || values[1] > 2160) return;
    if (values[2] != 30 && values[2] != 60) return;
    stream_width = values[0]; stream_height = values[1]; stream_fps = values[2];
    stream_bitrate_choice = values[3] <= 80000 ? values[3] : 0;
    stream_audio = values[4] != 0;
    stream_codec_choice = values[5] <= 2 ? values[5] : 0;
    /* A/B test switch (temporary, see outputs/steam-tizen-measurements.md): 0 = automatic, 1 = normal,
     * 2 = low, 3 = ultra-low. It only changes which latency mode the media source is created in, and
     * the mode actually used is logged, so a measurement can always be tied to it. */
    if (values[6] <= 3) latency_override = values[6];
    /* HEVC is opt-in: forcing it above 1080p is the most likely reason 2K/4K did not play on this
     * TV, since H.264 is the path that has been proven here. Automatic keeps H.264 up to 1440p and
     * falls back to HEVC only for 4K, where H.264 at 60 FPS needs level 5.2. */
    if (stream_codec_choice == 1) stream_hevc = false;
    else if (stream_codec_choice == 2) stream_hevc = true;
    else stream_hevc = stream_height > 1440;
}

/**
 * The bitrate chosen on the TV, or a resolution-based default when left automatic. The automatic
 * value is capped at 60 Mbit/s: 4K60 would otherwise ask for ~80 Mbit/s, which the TV could not
 * keep up with (its decoder reported an overflow).
 */
static uint32_t stream_bitrate_kbps(void) {
    if (stream_bitrate_choice) return stream_bitrate_choice;
    uint64_t rate = (uint64_t)stream_width * stream_height * stream_fps * 16 / 100000;
    if (rate < 8000) rate = 8000;
    if (rate > 60000) rate = 60000;
    return (uint32_t)rate;
}

EMSCRIPTEN_KEEPALIVE int TVStage(void) { return atomic_load(&phase); }
EMSCRIPTEN_KEEPALIVE void TVPicture(void) { atomic_store(&picture_ready, true); atomic_store(&phase, 12); }

/* Single source of truth for the build version: the app shows it (app.js asks for it with
 * Module._TVVersion()), every measurement line carries it (MET/SUMMARY/RUN), so a log can always be
 * tied to the exact build that produced it. Bump it with every installed build. */
#define TV_BUILD_VERSION "1.4.4"
EMSCRIPTEN_KEEPALIVE const char *TVVersion(void) { return TV_BUILD_VERSION; }
/* A/B test switch for the latency mode; 0 keeps the automatic behaviour. See media.cpp's LatencyMode. */
int TVLatencyOverride(void) { return (int) latency_override; }

/* Written by JavaScript (see TVLink.startVideoClock in app.js): the video element's own clock and
 * the number of frames it actually PRESENTED. Feeding the decoder 60 frames per second says nothing
 * about what reaches the screen, and a frozen picture with 60 fps of input is exactly that case.
 *
 * The second half is written by C and READ by JavaScript's watchdog (see TVLink.watchdog): the client
 * log can only be produced by the threads that are still running, so when the picture freezes the log
 * usually goes silent exactly where the answer is. The browser thread keeps running regardless, so the
 * watchdog keeps reporting these counters — a frozen main-loop tick count next to a rising audio or
 * send counter names the thread that is stuck. Keep the offsets in sync with app.js. */
typedef struct {
    double currentTime;      /* +0  JS -> C */
    uint32_t readyState;     /* +8  JS -> C */
    uint32_t paused;         /* +12 JS -> C */
    uint32_t presented;      /* +16 JS -> C, requestVideoFrameCallback callbacks */
    uint32_t loopTicks;      /* +20 C  -> JS, main loop iterations */
    uint32_t videoSubmitted; /* +24 C  -> JS, frames handed to the TV decoder */
    uint32_t videoRefused;   /* +28 C  -> JS, frames the TV decoder refused */
    uint32_t audioPackets;   /* +32 C  -> JS, Opus packets decoded */
    uint32_t audioErrors;    /* +36 C  -> JS, Opus packets that failed to decode */
    uint32_t audioRefused;   /* +40 C  -> JS, PCM packets the TV refused */
    uint32_t sendFailures;   /* +44 C  -> JS, upstream packets the kernel dropped */
    uint32_t audioPtsMs;     /* +48 C  -> JS, audio timeline position */
    uint32_t presentedFrames;/* +52 JS -> C, browser-reported presented frames (rVFC metadata) */
    uint32_t padPolls;       /* +56 JS -> C, gamepad samples in the last watchdog window */
    uint32_t padMaxGap;      /* +60 JS -> C, worst sampling gap in that window, in ms */
    uint32_t proxyUs;        /* +64 C  -> JS, microseconds spent in JS proxy calls per second */
    uint32_t proxyMaxUs;     /* +68 C  -> JS, worst single proxy call in that second */
    double mediaTime;        /* +72 JS -> C, mediaTime of the last presented frame */
} TVVideoClockState;
static TVVideoClockState videoClockState;
EMSCRIPTEN_KEEPALIVE void *TVVideoClockBuffer(void) { return &videoClockState; }
double TVVideoClockTime(void) { return videoClockState.currentTime; }
unsigned TVVideoClockPresented(void) { return videoClockState.presented; }
int TVVideoClockReady(void) { return (int) videoClockState.readyState; }
int TVVideoClockPaused(void) { return (int) videoClockState.paused; }
unsigned TVVideoClockPresentedFrames(void) { return videoClockState.presentedFrames; }
double TVVideoClockMediaTime(void) { return videoClockState.mediaTime; }
unsigned TVVideoClockPadPolls(void) { return videoClockState.padPolls; }
unsigned TVVideoClockPadMaxGap(void) { return videoClockState.padMaxGap; }
unsigned TVVideoClockProxyUs(void) { return videoClockState.proxyUs; }
unsigned TVVideoClockProxyMaxUs(void) { return videoClockState.proxyMaxUs; }
/* Called by the media layer (media.cpp) once per second, from the video worker. */
void TVMediaCounters(unsigned videoSubmitted, unsigned videoRefused, unsigned audioPackets,
                     unsigned audioErrors, unsigned audioRefused, unsigned audioPtsMs) {
    videoClockState.videoSubmitted = videoSubmitted;
    videoClockState.videoRefused = videoRefused;
    videoClockState.audioPackets = audioPackets;
    videoClockState.audioErrors = audioErrors;
    videoClockState.audioRefused = audioRefused;
    videoClockState.audioPtsMs = audioPtsMs;
}
/* Lets JavaScript report through the same channel the C side uses. The browser thread survives a
 * wedged session, so this is the only reporter left when the picture freezes (see TVLink.watchdog).
 * Defined below the push_diagnostic prototype. */


/* The client's video decoder report. The host's own log shows it logging ">>> Client video decoder
 * set to <info>" whenever the info string CHANGES (its handler compares with the stored string and
 * returns early when they match), so this message is bookkeeping, not the video-start trigger: the
 * 2026-10-14 run sent 56 of them, the host logged every one of them, and never sent StartVideoData.
 * It is still reported (every real client does), with the same stable text a real client uses, and
 * repeated a bounded number of times in case the first one is lost. */
static CVideoDecoderInfoMsg decoder_report = CVIDEO_DECODER_INFO_MSG__INIT;
static char decoder_info[96];
static int decoder_attempts;
static atomic_int host_stream_seen;
static uint64_t host_stream_seen_at;
static uint64_t decoder_fallback_at;
static uint64_t last_liveness_at;
static unsigned long last_datagram_count;
static unsigned long last_send_failures;
/* Browser-thread cost accumulator for the JS proxy calls the main loop makes (see the main loop). */
static uint32_t proxyUsWindow, proxyMaxWindow;
/* Defined in base.c: every datagram the session socket delivered, whatever the channel. */
unsigned long IHS_BaseReceivedDatagramCount(void);
/* Defined in base.c: upstream packets the kernel refused instead of parking the sender. */
unsigned long IHS_BaseFailedSendCount(void);
#define DECODER_ANNOUNCE_MAX 8

/* Forwarding is re-entrant by nature: sending the log makes the library log the send. */
static atomic_int host_log_sending;

static void push_diagnostic(const char *tag, const char *message);
static void forward_to_host(IHS_Session *active, const char *line, bool evidence);

/* Called from the library's video control path (SetCaptureSize), i.e. from a session thread:
 * only touch atomics/plain values here, never JavaScript. */
void TVHostStreamStarted(void) {
    if (atomic_exchange(&host_stream_seen, 1) == 0) host_stream_seen_at = IHS_TimerNow();
}

static atomic_int await_picture_requested;
static atomic_int media_prepare_requested;

/* Media callbacks run on the session's receive thread. MAIN_THREAD_EM_ASM from such a thread never
 * returns on this TV (it froze the whole inbound path 170 ms after the audio channel started on
 * 2026-10-14 10:56), so these only buffer; the main loop performs the JavaScript calls. */
void TVStatus(const char *text) {
    push_diagnostic("Média", text);
}

void TVRequestAwaitPicture(void) {
    atomic_store(&await_picture_requested, 1);
}

void TVRequestMediaPrepare(void) {
    atomic_store(&media_prepare_requested, 1);
}

/* Called from the browser thread (JavaScript, see TVLink.watchdog). Every other reporter in this file
 * runs on a library thread or on the main loop, and both can be parked by a wedged media pipeline;
 * the page keeps running, so its lines are the only ones that still arrive while the picture is
 * frozen. JavaScript writes the text into the buffer and calls the flush. */
static char external_log[256];
EMSCRIPTEN_KEEPALIVE char *TVLogBuffer(void) { return external_log; }
EMSCRIPTEN_KEEPALIVE void TVLogFlush(void) {
    if (external_log[0] != '\0') {
        external_log[sizeof(external_log) - 1] = '\0';
        if (strncmp(external_log, "Render: ", 8) == 0) push_diagnostic("Render", external_log + 8);
        else push_diagnostic("Őr", external_log);
        external_log[0] = '\0';
    }
}

static void report_video_decoder(IHS_Session *session) {
    decoder_attempts++;
    snprintf(decoder_info, sizeof(decoder_info), "Samsung Tizen WASM hardware decoder");
    decoder_report.info = decoder_info;
    decoder_report.has_threads = 1;
    decoder_report.threads = 4;
    IHS_SessionSendControlMessage(session, k_EStreamControlVideoDecoderInfo,
                                  (const ProtobufCMessage *) &decoder_report);
    {
        char line[96];
        snprintf(line, sizeof(line), "VideoDecoderInfo kikuldve (proba %d).", decoder_attempts);
        push_diagnostic("Dekoder", line);
    }
}

/*
 * Library threads must not call into JavaScript. MAIN_THREAD_EM_ASM proxies the call to the browser
 * thread and blocks until it is serviced, and on this TV the calling pthread did not resume after
 * its first such call: the send worker stopped right after its first log line, which left the
 * client handshake unsent. Library threads therefore only buffer their messages here, and the main
 * loop (which already talks to the page for status updates) flushes them.
 */
#define DIAGNOSTIC_RING 64
/* 192 was too small for the 1 Hz video/hang statistics line: it was silently truncated on the way to
 * the host, cutting off exactly the readyState/paused and audio fields. */
/* 384 was not enough for the table-style MERES measurement line (up to 511 characters plus the
 * "Média: " prefix); a truncated line silently loses its trailing fields, which is exactly the kind of
 * measurement loss this build is meant to eliminate. One 590-byte diagnostics packet still fits the
 * 1451-byte fragment payload. */
#define DIAGNOSTIC_LINE 640
#define DIAGNOSTIC_LOG_PORT 9999
static char diagnostic_lines[DIAGNOSTIC_RING][DIAGNOSTIC_LINE];
static atomic_int diagnostic_written;
static int diagnostic_read;
static int diagnostic_socket = -1;

/*
 * The TV has no readable device log and the on-screen trace needs a photo, so the same lines are
 * also sent over UDP to the PC that is streaming. That is the only channel that works while the
 * app runs normally, without debug mode.
 */
static void diagnostic_socket_init(void) {
    struct sockaddr_in local;
    diagnostic_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (diagnostic_socket < 0) return;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = 0;
    local.sin_addr.s_addr = INADDR_ANY;
    bind(diagnostic_socket, (struct sockaddr *) &local, sizeof(local));
}

static void diagnostic_send(const char *line) {
    if (diagnostic_socket < 0 || target_lock == NULL) return;
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    /* The UDP mirror follows the host that was actually discovered rather than a compile-time address:
     * the literal used to send this log to the author's PC on every network, and to nobody when that
     * address did not exist. Port 9999 in network byte order, written literally. */
    to.sin_port = (uint16_t) (((DIAGNOSTIC_LOG_PORT & 0xFF) << 8) | (DIAGNOSTIC_LOG_PORT >> 8));
    IHS_MutexLock(target_lock);
    int have_target = found && target.address.ip.family == IHS_IPAddressFamilyIPv4;
    if (have_target) memcpy(&to.sin_addr, target.address.ip.v4.data, sizeof(to.sin_addr));
    IHS_MutexUnlock(target_lock);
    if (!have_target) return;
    sendto(diagnostic_socket, line, strlen(line), 0, (struct sockaddr *) &to, sizeof(to));
}

static void push_diagnostic(const char *tag, const char *message) {
    int slot = atomic_fetch_add(&diagnostic_written, 1);
    snprintf(diagnostic_lines[slot % DIAGNOSTIC_RING], DIAGNOSTIC_LINE, "%s: %s", tag, message);
    /* The ring drops lines when a burst overflows it, which hid exactly the messages that matter
     * (the host's control packets). Every line is therefore also sent straight to the host from
     * whichever thread produced it: the stats-channel send is a socket queue append, not a
     * JavaScript proxy call, so a library thread may do it safely. */
    if (target_lock != NULL && atomic_exchange(&host_log_sending, 1) == 0) {
        char line[DIAGNOSTIC_LINE];
        snprintf(line, sizeof(line), "%s: %s", tag, message);
        IHS_MutexLock(target_lock);
        IHS_Session *active = session;
        IHS_MutexUnlock(target_lock);
        /* The packet-level lines are the evidence; a rate limit that drops them makes a burst of
         * twenty packets look like three. Only they skip the gap. */
        bool evidence = strncmp(message, "Control packet:", 15) == 0 ||
                        strncmp(message, "Received control message", 24) == 0 ||
                        strncmp(message, "Send control message", 20) == 0 ||
                        strncmp(message, "Sending ", 8) == 0 ||
                        strcmp(tag, "Worker") == 0;
        forward_to_host(active, line, evidence);
        atomic_store(&host_log_sending, 0);
    }
}

/* Real Steam Link clients forward their own log to the host over the stats channel and the host
 * writes every line into streaming_log.txt as "CLIENT: <line>". On this TV the app's stdout is
 * unreachable and the PC cannot receive its UDP log, so this is the only way to read the client's
 * log of a normal (non-debug) run — and it is also what the host expects from a client. */
#define HOST_LOG_MIN_GAP_MS 5
#define HOST_LOG_BUDGET 6000
static uint64_t host_log_last_ms;
/* Last non-measurement line: the 30 s heartbeat budget, kept apart from host_log_last_ms so the 1 Hz
 * telemetry cannot starve it (see forward_to_host). */
static uint64_t host_log_last_plain_ms;
/* Last measurement line. This must be its OWN timestamp: the multi-line "Render:"/"Idozites:" reports
 * are deliberately exempt from the 900 ms measurement budget, but they still update host_log_last_ms
 * (which exists for the 5 ms floor), and using that shared value for the budget dropped the 1 Hz
 * "Média: Videó" health line almost every time - measured 2026-10-15: only 14/50 and 29/162 of them
 * reached the host (28% / 18%), while the exempt reports arrived completely. A hidden telemetry loss
 * like that makes every later comparison wrong, so the classes are counted separately and the loss is
 * reported in the liveness line. */
static uint64_t host_log_last_measurement_ms;
static uint32_t host_log_drop_gap, host_log_drop_measurement, host_log_drop_heartbeat, host_log_drop_budget;
static int host_log_budget = HOST_LOG_BUDGET;
/* CLogMsg.type is undocumented; send the first few lines with a different value each so exactly
 * one of them is guaranteed to be the one the host prints, then settle on the first. */
static int host_log_type_probe;
/* Proof that the host's control channel to us is alive: its packets arrive or they do not. */
static atomic_int control_packets_seen;

static void forward_to_host(IHS_Session *active, const char *line, bool evidence) {
    if (active == NULL || atomic_load(&phase) < 11 || host_log_budget <= 0) return;
    /* While the picture is flowing, the client log is not worth a single packet: it competes with
     * the video for upstream bandwidth and CPU. Only one heartbeat line every 30 seconds goes out,
     * plus the video/decode measurements — those are the numbers that explain a frozen high
     * resolution stream. */
    if (atomic_load(&phase) >= 12) {
        /* Two classes of lines, each with its own budget. The live telemetry (the 1 Hz video/audio
         * statistics, the 2 s channel liveness report and the JavaScript watchdog) keeps arriving; a
         * single shared timestamp made the 30 s heartbeat rule unreachable, because a measurement
         * slipped in every second and always reset it - which is why no "Él:" and no "Őr:" line ever
         * appeared in a healthy session. ("Média: " is the only tag the media layer uses, so the old
         * "Video:"/"Data: Video" prefixes never matched anything.) */
        /* The media layer tags its lines "Média: ", but the budget classes below were matched against the
         * FULL line - so every "Média: MERES …" and "Média: SUMMARY …" line missed the exempt list and
         * fell into the plain class, i.e. one line per 30 s. Measured in streaming_log.txt, v1.4.0 run of
         * 2026-10-15 13:27 (193 s): exactly 7 MERES lines, 30/32 s apart (t=10, 42, 72, 102, 132, 163,
         * 193) instead of ~96, and the closing SUMMARY was lost completely. That is the primary evidence
         * line for a stutter window, sampled 15x too sparsely - the very tables used to locate the bad
         * seconds were built from 7 points. Classification therefore uses the tag-stripped body. */
        const char *body = line;
        static const char kMediaTag[] = "Média: ";
        if (strncmp(body, kMediaTag, sizeof(kMediaTag) - 1) == 0) body += sizeof(kMediaTag) - 1;
        bool measurement = strncmp(body, "Videó", sizeof("Videó") - 1) == 0 ||
                           strncmp(body, "Beadás:", sizeof("Beadás:") - 1) == 0 ||
                           strncmp(body, "Él: ", sizeof("Él: ") - 1) == 0 ||
                           strncmp(body, "HID: ", sizeof("HID: ") - 1) == 0 ||
                           strncmp(body, "Őr: ", sizeof("Őr: ") - 1) == 0;
        uint64_t nowMs = IHS_TimerNow();
        /* These producers already emit bounded two-second windows, including multi-line reports. */
        if (strncmp(body, "Render: ", sizeof("Render: ") - 1) == 0 ||
            strncmp(body, "Idozites:", sizeof("Idozites:") - 1) == 0 ||
            strncmp(body, "MERES ", sizeof("MERES ") - 1) == 0 ||
            strncmp(body, "SUMMARY ", sizeof("SUMMARY ") - 1) == 0 ||
            /* The two lines that identify HOW the run was configured: the effective latency mode /
             * decoder path and the platform capabilities. Without them a run cannot be attributed to a
             * mode at all, and the capability line was already being lost: "Média: EMSS:" is a plain
             * line, so the 30 s heartbeat budget swallowed it in every measured session (0 of 3 in
             * streaming_log.txt on 2026-10-15). Exempt both. */
            strncmp(body, "EMSS:", sizeof("EMSS:") - 1) == 0 ||
            strncmp(body, "Videó-dekóder mód:", sizeof("Videó-dekóder mód:") - 1) == 0 ||
            /* The run header itself. It is tagged "MERES", so the assembled line is "MERES: RUN v=…"
             * with a colon - which never matched the exempt "MERES " prefix and therefore fell into the
             * plain (30 s) class: measured 2026-10-15, the RUN line of the 15:59 and 16:01 sessions is
             * missing from the host log, so those runs cannot be tied to their bitrate/fps settings from
             * the log at all. Both spellings are exempt now. */
            strncmp(body, "RUN v=", sizeof("RUN v=") - 1) == 0 ||
            strncmp(body, "MERES: RUN", sizeof("MERES: RUN") - 1) == 0) {
            /* Keep the whole window; a shared 900 ms gap would erase its second/third line. */
        } else if (measurement) {
            if (nowMs - host_log_last_measurement_ms < 900) { host_log_drop_measurement++; return; }
            host_log_last_measurement_ms = nowMs;
        } else {
            if (nowMs - host_log_last_plain_ms < 30000) { host_log_drop_heartbeat++; return; }
            host_log_last_plain_ms = nowMs;
        }
    } else {
        if (strstr(line, "Discarded") != NULL) { host_log_drop_heartbeat++; return; }
        /* Before the first picture the forwarding used to be unlimited except for the 5 ms gap, which
         * made a busy handshake emit 150+ log packets per second upstream — on a TV whose Wi-Fi is at
         * the same time receiving a 20-30 Mbit/s video downlink. Dropped sends are survivable (the
         * socket drops them now), a STALLED send is not: it holds the session lock and through
         * IHS_BaseLog freezes every thread of the client at once. The cap keeps the forensic lines
         * while removing the flood. */
        uint64_t nowMs = IHS_TimerNow();
        if (nowMs - host_log_last_ms < (evidence ? 10u : 40u)) return;
    }
    uint64_t now = IHS_TimerNow();
    if (!evidence && now - host_log_last_ms < HOST_LOG_MIN_GAP_MS) { host_log_drop_gap++; return; }
    if (host_log_budget <= 0) { host_log_drop_budget++; return; }
    host_log_last_ms = now;
    host_log_budget--;
    char text[DIAGNOSTIC_LINE + 16];
    if (host_log_type_probe < 4) {
        snprintf(text, sizeof(text), "TV[type=%d]: %s", host_log_type_probe, line);
    } else {
        snprintf(text, sizeof(text), "TV: %s", line);
    }
    for (char *p = text; *p != '\0'; p++) {
        if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
    }
    IHS_SessionSendLogMessage(active, host_log_type_probe < 4 ? host_log_type_probe : 1, text);
    if (host_log_type_probe < 4) host_log_type_probe++;
}

static void flush_diagnostics(void) {
    static uint64_t last_mirror_ms;
    int written = atomic_load(&diagnostic_written);
    if (written - diagnostic_read > DIAGNOSTIC_RING) diagnostic_read = written - DIAGNOSTIC_RING;
    /* While the picture is flowing only the newest line is mirrored: the on-screen window shows the
     * last eight lines anyway, and one JavaScript proxy call per line was itself a stutter source on
     * this TV (the proxy parks the calling thread until the browser thread services it). */
    if (atomic_load(&phase) >= 12 && written - diagnostic_read > 1) diagnostic_read = written - 1;
    while (diagnostic_read < written) {
        char line[DIAGNOSTIC_LINE];
        memcpy(line, diagnostic_lines[diagnostic_read % DIAGNOSTIC_RING], sizeof(line));
        line[DIAGNOSTIC_LINE - 1] = '\0';
        /* Before the first picture the handshake can emit hundreds of lines per second, and each one
         * costs a synchronous proxy call from this loop — the same loop that must open the TV's media
         * pipeline. Cap the mirroring, not the draining: the host still gets every line that passes its
         * own rate limit. */
        uint64_t now = IHS_TimerNow();
        if (last_mirror_ms == 0 || now - last_mirror_ms >= (atomic_load(&phase) >= 12 ? 500u : 40u)) {
            MAIN_THREAD_EM_ASM({ TVLink.push(UTF8ToString($0)); }, line);
            last_mirror_ms = now;
        }
        diagnostic_send(line);
        diagnostic_read++;
    }
}

static void fail_request(const char *text, bool repair) {
    if (!atomic_load(&session_requested)) return;
    IHS_MutexLock(target_lock);
    snprintf(failure_text, sizeof(failure_text), "%s", text);
    repair_pairing |= repair; failure_pending = 1;
    IHS_MutexUnlock(target_lock);
}
/* Every line reaches the host through push_diagnostic; this hook only counts the host's control
 * packets so the two-second liveness report can say whether the host is still talking to us. */
static void diagnostic_log(IHS_LogLevel level, const char *tag, const char *message) {
    if (strncmp(message, "Control packet:", 15) == 0) atomic_fetch_add(&control_packets_seen, 1);
    if (TVDiagnosticEnabled(level, atomic_load(&phase), tag, message)) {
        push_diagnostic(tag, message);
    }
    if (level == IHS_LogLevelError && strcmp(message, "Failed to authenticate") == 0)
        fail_request("A Steam elutasította a hitelesítést. Párosítsd újra az eszközt.", true);
}

static void configuring(IHS_Session *active, IHS_SessionConfig *config, void *context) {
    atomic_store(&phase, 10);
    config->enableAudio = stream_audio;
    config->enableHevc = stream_hevc;
    config->maxWidth = stream_width; config->maxHeight = stream_height; config->maxFps = stream_fps;
    /* Every session in the host's own streaming_log.txt that received video used Beautiful; ours
     * was the only one asking for Fast. Nothing points at quality as a gate, but there is no
     * reason to be the odd one out. */
    config->maxBitrateKbps = stream_bitrate_kbps(); config->quality = IHS_StreamQualityBeautiful;
}
static void session_connected(IHS_Session *active, void *context) {
    atomic_store(&phase, 11);
    MAIN_THREAD_EM_ASM({ TVLink.streaming = true; TVLink.status('Steam stream kapcsolódott. A tévé dekódere indul…'); });
    atomic_store(&host_stream_seen, 0);
    decoder_fallback_at = IHS_TimerNow() + 2500;
    host_log_budget = HOST_LOG_BUDGET;
    host_log_last_ms = 0;
    host_log_last_plain_ms = 0;
    host_log_type_probe = 0;
    atomic_store(&control_packets_seen, 0);
    last_liveness_at = IHS_TimerNow();
    /* Run header: ties every following measurement line to the exact build and settings that produced
     * it, so two logs (a smooth and a stuttering run) can be compared field by field. */
    {
        char line[256];
        snprintf(line, sizeof(line),
                 "RUN v=%s felbontas=%ux%u fps=%u bitrate=%u codec=%u hang=%u kesleltetes=%u",
                 TV_BUILD_VERSION, (unsigned) stream_width, (unsigned) stream_height,
                 (unsigned) stream_fps, (unsigned) stream_bitrate_kbps(),
                 (unsigned) stream_codec_choice, (unsigned) stream_audio,
                 (unsigned) latency_override);
        push_diagnostic("MERES", line);
    }
    // Tell the host about the input devices; without it no HID device is ever enumerated.
    // (SetStreamingClientConfig is deliberately NOT sent: its handler in ihslib reads the server's
    // configuration, and the host sends it to the client, so sending it the other way is invalid.)
    IHS_SessionHIDNotifyDeviceChange(active);
}
static void session_disconnected(IHS_Session *active, void *context) {
    fail_request("A Steam stream megszakadt. Újraindíthatod a kapcsolatot.", false);
}
static void stream_success(IHS_Client *client, const IHS_HostInfo *host, const IHS_SocketAddress *address,
                           const uint8_t *key, size_t length, void *context) {
    if (!atomic_load(&session_requested)) return;
    if (!key || !length || length > 32 || !host->steamId) { fail_request("Hiányzó Steam felhasználó vagy hibás munkamenetkulcs.", false); return; }
    IHS_SessionInfo info = {0};
    info.address = *address; info.sessionKeyLen = length; info.steamId = host->steamId;
    memcpy(info.sessionKey, key, length);
    // The callback holds library timer locks; create the session on our main worker.
    IHS_MutexLock(target_lock);
    if (atomic_load(&session_requested)) { pending_info = info; pending_session = 1; }
    IHS_MutexUnlock(target_lock);
}
static void start_session(const IHS_SessionInfo *info) {
    atomic_store(&phase, 5);
    static const IHS_StreamSessionCallbacks callbacks = { .configuring = configuring, .connected = session_connected, .disconnected = session_disconnected };
    IHS_Session *active = IHS_SessionCreate(&stream_config, info);
    if (!active) { fail_request("Nem sikerült létrehozni a Steam munkamenetet.", false); return; }
    IHS_SessionSetLogFunction(active, diagnostic_log);
    atomic_store(&phase, 6);
    {
        // The host parses system_info as a VDF KeyValues document ("KeyValues Error:
        // LoadFromBuffer: missing { ... in file ClientSystemInfo" otherwise), so send a real block.
        const IHS_StreamClientCapabilities capabilities = {
            .systemInfo = "\"1.0\"\n{\n\t\"SystemInfo\"\n\t{\n\t\t\"OsName\"\t\t\"Tizen\"\n\t\t\"OsVersion\"\t\"9.0\"\n\t\t\"OsArch\"\t\"armv7\"\n\t\t\"CPUCount\"\t\"4\"\n\t\t\"GPUName\"\t\"Samsung\"\n\t\t\"VideoDecode\"\t\"hardware\"\n\t}\n}\n",
            .decoderInfo = "Samsung Tizen WASM hardware decoder",
            .decoderThreads = 4,
            .maximumDecodeBitrateKbps = 80000,
            .maximumBurstBitrateKbps = 100000,
            .formFactor = IHS_StreamDeviceFormFactorTV,
        };
        IHS_SessionSetClientCapabilities(active, &capabilities);
    }
    IHS_SessionSetSessionCallbacks(active, &callbacks, NULL);
    IHS_SessionSetVideoCallbacks(active, TVVideoCallbacks(), NULL);
    IHS_SessionSetAudioCallbacks(active, TVAudioCallbacks(), NULL);
    gamepad_provider = TVGamepadProvider();
    IHS_SessionHIDAddProvider(active, gamepad_provider);
    IHS_MutexLock(target_lock); session = active; IHS_MutexUnlock(target_lock);
    atomic_store(&phase, 8);
    if (!IHS_SessionConnect(active)) fail_request("Nem indult el a Steam munkamenet.", false);
}
static void stream_failed(IHS_Client *client, const IHS_HostInfo *host, IHS_StreamingResult result, void *context) {
    char text[100]; snprintf(text, sizeof(text), "A Big Picture stream nem indult el. Steam hibakód: %d", result);
    fail_request(text, result == IHS_StreamingUnauthorized || result == IHS_StreamingPINRequired);
}

static void event(const char *message) {
    MAIN_THREAD_EM_ASM({ TVLink.status(UTF8ToString($0)); }, message);
}

static void discovered(IHS_Client *client, const IHS_HostInfo *host, void *context) {
    char *ip = IHS_IPAddressToString(&host->address.ip);
    /* Any Steam host that advertises itself on the network is usable. This used to be pinned to one
     * address with strcmp(), so the app could only ever find that single machine and every other
     * network sat at "searching…" forever. The first host that answers is kept: discovery repeats
     * every few seconds and a second PC on the same LAN must not steal a session that is already
     * pairing or streaming. */
    int take = 0;
    IHS_MutexLock(target_lock);
    if (!found) { target = *host; found = 1; take = 1; }
    IHS_MutexUnlock(target_lock);
    if (take) {
        char text[160];
        /* push_diagnostic, not the JavaScript status: this runs on the library's discovery thread, and
         * a synchronous proxy call from a library thread does not return on this TV. */
        snprintf(text, sizeof(text), "Steam PC: %s (%s), jatek fut=%d", host->hostname[0] ? host->hostname : "?", ip, host->gamesRunning ? 1 : 0);
        push_diagnostic("Discovery", text);
        MAIN_THREAD_EM_ASM({ TVLink.host(UTF8ToString($0)); }, ip);
    }
    free(ip);
}

static void authorized(IHS_Client *client, const IHS_HostInfo *host, uint64_t steam_id, void *context) {
    uint8_t *identity = context;
    IHS_ClientGetSecretKey(client, identity + 8);
    MAIN_THREAD_EM_ASM({ TVLink.saveSecret($0); }, identity + 8);
    MAIN_THREAD_EM_ASM({ TVLink.done('A Steam elfogadta a párosítást. Big Picture indítása…'); document.getElementById('start').onclick(); });
}

static void failed(IHS_Client *client, const IHS_HostInfo *host, IHS_AuthorizationResult result, void *context) {
    char message[96];
    snprintf(message, sizeof(message), "A Steam-párosítás sikertelen. Hibakód: %d", result);
    MAIN_THREAD_EM_ASM({ TVLink.done(UTF8ToString($0)); }, message);
}

static void progress(IHS_Client *client, const IHS_HostInfo *host, void *context) {
    event("Írd be a tévén megjelenő PIN-t a PC Steam-párosítási ablakába.");
}

/* The host answers k_ERemoteDeviceStreamingInProgress while it closes a previous session (it did so
 * for seven seconds on 2026-10-14 10:48). Without this the TV kept showing the pairing text and it
 * looked as if the request never left the app. */
static void stream_progress(IHS_Client *client, const IHS_HostInfo *host, void *context) {
    event("A PC még az előző munkamenetet zárja le; újrapróbálom…");
}

int main(void) {
    event("Indulás: identitás betöltése…");
    uint8_t identity[40];
    int ready = MAIN_THREAD_EM_ASM_INT({ return TVLink.identity($0); }, identity);
    if (!ready) { event("Nem érhető el a biztonságos véletlengenerátor vagy az identitástároló."); return 1; }
    uint64_t device_id;
    memcpy(&device_id, identity, sizeof(device_id));
    IHS_ClientConfig config = { device_id, identity + 8, "Samsung Steam TV" };
    stream_config = config;
    TVMediaSelfCheck();
    {
        // Printed on screen and in the log so the device ID can be matched against Steam's own
        // remote_connections.txt: an unchanged ID proves the pairing was remembered.
        char id_text[64];
        snprintf(id_text, sizeof(id_text), "Eszközazonosító: %llu", (unsigned long long)device_id);
        push_diagnostic("Eszköz", id_text);
    }
    diagnostic_socket_init();
    push_diagnostic("Eszköz", "Naplócsatorna megnyitva a PC felé.");
    event("Identitás és média rendben. Titkosítási önellenőrzés…");
    uint8_t test_cipher[384];
    size_t test_length = sizeof(test_cipher);
    int crypto_result = IHS_CryptoRSAEncrypt(identity, 40, IHS_AuthorizationPubKeyPublic,
                                           sizeof(IHS_AuthorizationPubKeyPublic), test_cipher, &test_length);
    char crypto_status[160];
    snprintf(crypto_status, sizeof(crypto_status), "Eszköz: %llu · RSA: eredmény=%d, kimenet=%zu bájt", (unsigned long long)device_id, crypto_result, test_length);
    MAIN_THREAD_EM_ASM({ document.getElementById('crypto').textContent = UTF8ToString($0); }, crypto_status);
    if (crypto_result != 0 || test_length != 256) { event("A párosítás titkosítási önellenőrzése hibát talált. Nem küldünk hibás kérést."); return 1; }
    event("Steam indítása…");
    IHS_Init();
    target_lock = IHS_MutexCreate();
    IHS_Client *client = IHS_ClientCreate(&config);
    IHS_ClientSetLogFunction(client, diagnostic_log);
    const IHS_ClientDiscoveryCallbacks discovery = { .discovered = discovered };
    const IHS_ClientAuthorizationCallbacks authorization = { .progress = progress, .success = authorized, .failed = failed };
    const IHS_ClientStreamingCallbacks streaming = { .progress = stream_progress, .success = stream_success, .failed = stream_failed };
    IHS_ClientSetDiscoveryCallbacks(client, &discovery, NULL);
    IHS_ClientSetAuthorizationCallbacks(client, &authorization, identity);
    IHS_ClientSetStreamingCallbacks(client, &streaming, NULL);
    if (!IHS_ClientStartDiscovery(client, 3000)) { event("A Steam felderítése nem indult el."); return 1; }
    event("Steam keresése a hálózaton… A PC-n legyen bekapcsolva a Remote Play.");
    for (;;) {
        /* Heartbeat the JavaScript watchdog reads: if this stops while the picture is supposed to be
         * flowing, the main loop (or a lock it takes) is the thing that is stuck. */
        videoClockState.loopTicks++;
        /* One-shot step markers: the last one recorded tells exactly which main-loop step stopped,
         * since this loop makes JavaScript proxy calls that have proved unreliable on this TV. */
        static int loop_step = 0;
#define LOOP_STEP(n, text) do { if (loop_step < (n)) { loop_step = (n); push_diagnostic("Loop", text); } } while (0)
        LOOP_STEP(1, "gamepad poll");
        if (TVGamepadPoll() && session && atomic_load(&phase) >= 11) IHS_SessionHIDNotifyDeviceChange(session);
        LOOP_STEP(2, "timeout and media checks");
        if (atomic_load(&session_requested) && !atomic_load(&picture_ready) && IHS_TimerNow() - request_started >= 30000) {
            int step = atomic_load(&phase);
            const char *stage = step >= 11 ? "az első videóképre várva" : step >= 10 ? "a videóbeállítás közben" : step >= 5 ? "a Steam kézfogása közben" : "a Steam indítására várva";
            char text[192]; snprintf(text, sizeof(text), "A kapcsolódás 30 másodperc után lejárt %s. Újraindíthatod.", stage);
            fail_request(text, false);
        }
        if (atomic_load(&session_requested) && TVMediaFailed())
            fail_request("A tévé médialejátszója nem indult el vagy hibát jelzett. Újraindíthatod.", false);
        LOOP_STEP(3, "failure handling");
        IHS_MutexLock(target_lock);
        int failed_now = failure_pending, repair_now = repair_pairing;
        char text[sizeof(failure_text)]; memcpy(text, failure_text, sizeof(text));
        IHS_MutexUnlock(target_lock);
        if (failed_now && !teardown_active) {
            atomic_store(&session_requested, false);
            atomic_store(&phase, 0); atomic_store(&picture_ready, false);
            TVMediaBeginStop();
            /* Unpublish under the lock the readers use BEFORE destroying: push_diagnostic() snapshots
             * `session` under target_lock and sends through it, and since the JavaScript watchdog
             * reports every two seconds that reader now runs on the browser thread too - which
             * IHS_SessionDestroy neither joins nor synchronises. Destroying first left a window in
             * which the watchdog could walk freed channels. */
            IHS_MutexLock(target_lock);
            IHS_Session *victim = session;
            session = NULL;
            pending_session = failure_pending = repair_pairing = 0;
            IHS_MutexUnlock(target_lock);
            memcpy(teardown_text, text, sizeof(teardown_text)); teardown_repair = repair_now;
            teardown.client = client; teardown.session = victim; teardown.provider = gamepad_provider;
            gamepad_provider = NULL;
            teardown.logSending = &host_log_sending; teardown.stopMedia = TVMediaStop;
            teardown_active = true; teardown_started = IHS_TimerNow(); teardown_reported_stage = -1;
            MAIN_THREAD_EM_ASM({ TVLink.beginStopping('A kapcsolat leállítása… Vissza: kilépés.'); });
            if (!TVTeardownStart(&teardown))
                MAIN_THREAD_EM_ASM({ TVLink.status('Nem indult el a leállító szál. Vissza: kilépés; a kapcsolat nem indítható újra.'); });
        }
        if (teardown_active) {
            if (TVTeardownFinish(&teardown)) {
                teardown_active = false;
                atomic_store(&phase, 0); atomic_store(&picture_ready, false);
                atomic_store(&media_prepare_requested, 0); atomic_store(&await_picture_requested, 0);
                MAIN_THREAD_EM_ASM({ TVLink.endStopping(UTF8ToString($0), !!$1); }, teardown_text, teardown_repair);
            } else if (IHS_TimerNow() - teardown_started >= 5000) {
                int stage = atomic_load(&teardown.stage);
                if (stage != teardown_reported_stage) {
                    const char *steps[] = {"szálindítás", "Steam lemondás / napló lezárás", "Steam szálak bevárása", "kontroller lezárás", "tévé lejátszó lezárás", "befejezés"};
                    char line[192]; snprintf(line, sizeof(line), "Leállítás folyamatban: %s. Vissza: kilépés.", steps[stage]);
                    MAIN_THREAD_EM_ASM({ TVLink.status(UTF8ToString($0)); }, line);
                    teardown_reported_stage = stage;
                }
            }
        }
        IHS_MutexLock(target_lock);
        int should_start = pending_session;
        IHS_SessionInfo info = pending_info;
        pending_session = 0;
        IHS_MutexUnlock(target_lock);
        if (should_start && !teardown_active && atomic_load(&session_requested) && !session) start_session(&info);
        LOOP_STEP(4, "session start");
        /* Browser-thread contention measurement: these proxy calls are serviced by the browser thread,
         * the same thread that runs the media pipeline's callbacks and the app's timers. How long they
         * take is therefore a direct measure of how busy that thread is - a candidate cause of uneven
         * motion that no media-side counter would show. emscripten_get_now() is a high-resolution
         * monotonic timer, so sub-millisecond proxy costs are visible. */
        double proxyStart = emscripten_get_now();
        int command = MAIN_THREAD_EM_ASM_INT({ var value = TVLink.command; TVLink.command = 0; return value || TVLink.keys.shift() || 0; });
        {
            double costUs = (emscripten_get_now() - proxyStart) * 1000.0;
            proxyUsWindow += (uint32_t) costUs;
            if ((uint32_t) costUs > proxyMaxWindow) proxyMaxWindow = (uint32_t) costUs;
        }
        LOOP_STEP(5, "command read");
        if (command == 1 && !teardown_active) {
            IHS_MutexLock(target_lock);
            IHS_HostInfo host = target;
            int can_pair = found;
            IHS_MutexUnlock(target_lock);
            if (!can_pair) { MAIN_THREAD_EM_ASM({ TVLink.done('Még nincs felfedezett Steam host.'); }); continue; }
            char pin[5];
            int pin_ready = MAIN_THREAD_EM_ASM_INT({ return TVLink.pin($0); }, pin);
            if (!pin_ready || !IHS_ClientAuthorizationRequest(client, &host, pin)) {
                MAIN_THREAD_EM_ASM({ TVLink.done('A párosítási kérés nem indult el.'); });
            }
        }
        if (command == 2 && !teardown_active && !session_requested) {
            atomic_store(&phase, 1);
            load_stream_settings();
            IHS_MutexLock(target_lock); IHS_HostInfo host = target; int available = found; IHS_MutexUnlock(target_lock);
            if (!available) { MAIN_THREAD_EM_ASM({ TVLink.failed('Még nincs Steam host.'); }); continue; }
            IHS_StreamingRequest request = { .streamingEnable = {true, true, true}, .maxResolution = {(int32_t)stream_width, (int32_t)stream_height}, .maxFramerate = {(int32_t)stream_fps, 1}, .audioChannelCount = 2, .streamingInterface = IHS_StreamInterfaceBigPicture, .gamepadCount = TVGamepadCount() };
            request_started = IHS_TimerNow(); atomic_store(&picture_ready, false);
            atomic_store(&session_requested, true);
            if (!IHS_ClientStreamingRequest(client, &host, &request)) fail_request("Nem indult el a Big Picture kérés.", false);
            else {
                char text[128];
                snprintf(text, sizeof(text), "Big Picture indítása: %ux%u, %u FPS…", stream_width, stream_height, stream_fps);
                event(text);
            }
        }
        IHS_MutexLock(target_lock); IHS_Session *active = session; IHS_MutexUnlock(target_lock);
        /* The media layer asked for the picture watcher from a library thread; this is the thread
         * that may actually call into the page. */
        if (!teardown_active && atomic_exchange(&await_picture_requested, 0)) {
            MAIN_THREAD_EM_ASM({ TVLink.awaitPicture(); });
        }
        /* Opening the TV's media pipeline waits for the platform, so it happens here (on a thread
         * that may safely block) instead of on the session's receive thread. */
        if (!teardown_active && atomic_exchange(&media_prepare_requested, 0)) {
            push_diagnostic("Média", "Streamforrás előkészítése…");
            /* Opening the TV's media pipeline blocks this loop for up to ~15 s (three waits of up to
             * 5 s for the platform). The JavaScript watchdog treats a non-advancing loop tick count as
             * a freeze, so the wait is announced: otherwise a normal slow start shows "the picture
             * froze" on screen and makes the Back key leave the app. */
            MAIN_THREAD_EM_ASM({ TVLink.preparing = true; });
            TVMediaPrepare();
            MAIN_THREAD_EM_ASM({ TVLink.preparing = false; TVLink.watchdogStalled = 0; });
        }
        if (active && atomic_load(&phase) >= 11 && !atomic_load(&picture_ready) &&
            decoder_attempts < DECODER_ANNOUNCE_MAX) {
            uint64_t now = IHS_TimerNow();
            uint64_t due = atomic_load(&host_stream_seen) ? host_stream_seen_at + 400 : decoder_fallback_at;
            due += (uint64_t) decoder_attempts * 800;
            if (now >= due) report_video_decoder(active);
        }
        /* Is the host's control channel still delivering? Its reliable messages (SetCursor and
         * friends) are the only sign of life between our sends; if this drops to zero the host has
         * stopped talking to us and no video message can arrive either. The raw datagram counter
         * next to it tells whether the socket delivered anything at all — audio data is invisible to
         * the control-packet counter. */
        if (active && atomic_load(&phase) >= 11) {
            uint64_t now = IHS_TimerNow();
            if (now - last_liveness_at >= 2000) {
                TVMediaPublish(active);
                int seen = atomic_exchange(&control_packets_seen, 0);
                unsigned long datagrams = IHS_BaseReceivedDatagramCount();
                unsigned long delta = datagrams - last_datagram_count;
                last_datagram_count = datagrams;
                unsigned long sendFailures = IHS_BaseFailedSendCount();
                unsigned long sendDelta = sendFailures - last_send_failures;
                last_send_failures = sendFailures;
                videoClockState.sendFailures = (uint32_t) sendFailures;
                char line[400];
                if (active) {
                    /* Retransmission telemetry: an unbounded reliable queue would show up here as a
                     * growing outstanding count and retry count, which would also explain the host
                     * going quiet (it receives a flood instead of our ACKs). */
                    IHS_SessionReliabilityStats stats;
                    IHS_SessionGetReliabilityStats(active, &stats);
                    snprintf(line, sizeof(line),
                             "Vezérlőcsatorna: %d csomag, datagram +%lu (össz %lu) / 2 s. Küldés: %llu nyugtázva, %llu ismétlés, %u függőben, legrégebbi %llu ms, eldobva +%lu (össz %lu). Bemenet: %llu feladva, %llu elküldve, %llu nyugtázva, %u függőben, %u repülésben.",
                             seen, delta, datagrams,
                             (unsigned long long) stats.reliableAcknowledged,
                             (unsigned long long) stats.reliableRetries,
                             stats.reliableOutstanding,
                             (unsigned long long) stats.reliableOldestOutstandingMs,
                             sendDelta, sendFailures,
                             (unsigned long long) stats.hidSubmitted,
                             (unsigned long long) stats.hidSent,
                             (unsigned long long) stats.hidAcknowledged,
                             stats.hidPending, stats.hidInFlight);
                } else {
                    snprintf(line, sizeof(line),
                             "Vezérlőcsatorna: %d csomag, datagram +%lu (össz %lu) / 2 s, eldobva +%lu.",
                             seen, delta, datagrams, sendDelta);
                }
                push_diagnostic("Él", line);
                /* Log-loss visibility: without these counters a missing measurement line cannot be
                 * told apart from "the client never produced it", which silently invalidates any
                 * comparison between two runs. */
                {
                    char loss[160];
                    snprintf(loss, sizeof(loss), "Naplo: kihagyva meres %u, szivveres %u, 5ms-res %u, keret %u (sorok: %u).",
                             host_log_drop_measurement, host_log_drop_heartbeat, host_log_drop_gap,
                             host_log_drop_budget, (unsigned) host_log_type_probe);
                    push_diagnostic("Él", loss);
                }
                last_liveness_at = now;
                /* Publish the browser-thread cost of the last two seconds and restart the window. */
                videoClockState.proxyUs = (uint32_t) (proxyUsWindow / 2);
                videoClockState.proxyMaxUs = proxyMaxWindow;
                proxyUsWindow = 0;
                proxyMaxWindow = 0;
            }
        }
        if (active && command >= 1000 && command < 2000) IHS_SessionSendKeyDown(active, command - 1000);
        if (active && command >= 2000 && command < 3000) IHS_SessionSendKeyUp(active, command - 2000);
        if (command == 3) fail_request("Stream leállítva. Újraindíthatod a kapcsolatot.", false);
        LOOP_STEP(6, "log flush");
        {
            double flushStart = emscripten_get_now();
            flush_diagnostics();
            double costUs = (emscripten_get_now() - flushStart) * 1000.0;
            proxyUsWindow += (uint32_t) costUs;
            if ((uint32_t) costUs > proxyMaxWindow) proxyMaxWindow = (uint32_t) costUs;
        }
        /* 4 ms, not 8: the gamepad poll is now a memory copy instead of a JavaScript proxy call, so
         * the input snapshot can be refreshed twice as often without touching the browser thread. */
        usleep(4000);
    }
}
