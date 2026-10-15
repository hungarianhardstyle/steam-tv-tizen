#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <opus.h>
#include "steam-tv/gamepad.c"
#include "steam-tv/video_codec.h"
#include "steam-tv/video_timing.h"
#include "steam-tv/video_metrics.h"
#include "steam-tv/audio_batch.h"
#include "steam-tv/diagnostic_filter.h"
#include "session/session_pri.h"
#include "session/frame_stats.h"
#include "steam-tv/teardown.h"
#include "session/channels/ch_control.h"
#include "session/channels/ch_data_audio.h"
#include "ihs_buffer_ext.h"

/* session.c defines struct IHS_QueueItem privately (packet + reliable). Completing it here lets
 * the test inspect the queued client handshake, which is otherwise only observable on the wire.
 * The send worker only starts in IHS_SessionConnect, so the queue is stable while we look at it. */
struct IHS_QueueItem {
    IHS_SessionPacket packet;
    bool reliable;
};

static void test_gamepad(void) {
    padLock = IHS_MutexCreate();
    shared.pads[0].present = 7; shared.pads[0].kind = 45;
    atomic_store(&shared.sequence, 2);
    assert(TVGamepadPoll());
    atomic_store(&shared.sequence, 3);
    shared.pads[0].present = 0;
    assert(!TVGamepadPoll());
    assert(State(0).present == 7); /* An unfinished JS snapshot cannot disconnect a live pad. */
    shared.pads[0].present = 7;
    atomic_store(&shared.sequence, 4);
    assert(!TVGamepadPoll());
    atomic_store(&shared.sequence, 5);
    shared.pads[0].present = 0;
    atomic_store(&shared.sequence, 6);
    assert(TVGamepadPoll());
    assert(State(0).present == 0); /* A committed real disconnect still propagates. */
    pads[0].present = 7; pads[0].kind = 45; pads[0].wire[27] = 3;
    PadDevice device = { .index = 0, .generation = 7 };
    IHS_HIDManagedDevice managed = { .device = &device.base };
    device.base.managed = &managed;
    IHS_HIDReportHolderInit(&managed.reportHolder, 1);
    IHS_HIDReportHolderSetReportLength(&managed.reportHolder, 128);
    assert(Start(&device.base, 128) == 0);
    IHS_HIDDeviceReportMessage *message = IHS_HIDReportHolderGetMessage(&managed.reportHolder);
    assert(message && message->n_reports == 1);
    assert(message->reports[0]->has_full_report && message->reports[0]->full_report.len == 72);
    assert(message->reports[0]->full_report.data[27] == 3);
    IHS_HIDReportHolderResetMessage(&managed.reportHolder);
    assert(Poll(&device.base) == 0);
    pads[0].wire[16] = 1;
    assert(Poll(&device.base) == 1);
    assert(managed.reportActiveInput);
    IHS_Buffer feature = IHS_BUFFER_INIT(64, 64); uint8_t command = 4;
    assert(Feature(&device.base, &command, 1, &feature, 20) == -1);
    assert(Feature(&device.base, &command, 1, &feature, 21) == 0);
    uint32_t kind; IHS_ReadUInt32LE(IHS_BufferPointer(&feature) + 3, &kind); assert(kind == 45);
    pads[0].present = 8;
    assert(Poll(&device.base) == -1);
    pads[0].present = 0;
    assert(Poll(&device.base) == -1);
    IHS_BufferClear(&feature, true); IHS_HIDReportHolderDeinit(&managed.reportHolder); IHS_MutexDestroy(padLock);
}
static void test_network_frame_stats(void) {
    IHS_FrameStatsAggregator *stats = IHS_FrameStatsAggregatorCreate();
    assert(stats);
    /* A clock correction of -1 tick must not become a 65-million-ms network sample. */
    IHS_FrameStatsRecordReceived(stats, 1, 100, 100, 99, 1, 0);
    IHS_FrameStatsRecordComplete(stats, 1, IHS_VideoFrameResultDisplayed);
    IHS_FrameStatsRecordReceived(stats, 2, 100, 100, 116, 1, 0);
    IHS_FrameStatsRecordComplete(stats, 2, IHS_VideoFrameResultDisplayed);
    /* Legitimate uint32 clock wrap still has a positive 16-tick duration. */
    IHS_FrameStatsRecordReceived(stats, 3, 0xfffffff8u, 0xfffffff8u, 8, 1, 0);
    IHS_FrameStatsRecordComplete(stats, 3, IHS_VideoFrameResultDisplayed);
    const IHS_FrameStatsReport *report = IHS_FrameStatsReportBegin(stats);
    assert(report && report->count == 3);
    assert(report->accumulator.slots[6].count == 2);
    assert(report->accumulator.slots[6].sum == 32.0 * 1000.0 / 65536.0);
    IHS_FrameStatsAggregatorDestroy(stats);
}
static atomic_bool teardown_media_entered, teardown_media_release;
static void blocked_media_close(void) {
    atomic_store(&teardown_media_entered, true);
    while (!atomic_load(&teardown_media_release)) usleep(2000);
}
static void test_async_teardown(void) {
    IHS_Init();
    uint8_t secret[32] = {0}; IHS_ClientConfig config = {1, secret, "offline teardown"};
    IHS_SessionInfo info = { .steamId = 1, .sessionKeyLen = 16 };
    TVTeardown job = { .session = IHS_SessionCreate(&config, &info), .provider = TVGamepadProvider(), .stopMedia = blocked_media_close };
    IHS_SessionHIDAddProvider(job.session, job.provider);
    atomic_store(&teardown_media_entered, false); atomic_store(&teardown_media_release, false);
    assert(TVTeardownStart(&job));
    for (unsigned i = 0; i < 1000 && !atomic_load(&teardown_media_entered); i++) usleep(2000);
    assert(atomic_load(&teardown_media_entered));
    /* Actual session destruction finished, but a blocked platform close must not join the caller. */
    assert(atomic_load(&job.stage) == 4);
    for (unsigned i = 0; i < 20; i++) assert(!TVTeardownFinish(&job));
    assert(job.thread != NULL && !atomic_load(&job.done));
    atomic_store(&teardown_media_release, true);
    for (unsigned i = 0; i < 1000 && !atomic_load(&job.done); i++) usleep(2000);
    assert(TVTeardownFinish(&job));
    assert(job.session == NULL && job.provider == NULL && job.thread == NULL);
    assert(!TVTeardownFinish(&job));
    IHS_Quit();
}
static void test_opus(void) {
    int error; OpusDecoder *decoder = opus_decoder_create(48000, 2, &error);
    assert(decoder && error == OPUS_OK);
    const uint8_t silence[] = {0xf8, 0xff, 0xfe}, bad[] = {3}; opus_int16 pcm[11520];
    assert(opus_decode(decoder, silence, sizeof(silence), pcm, 5760, 0) == 960);
    assert(opus_decode(decoder, bad, sizeof(bad), pcm, 5760, 0) < 0);
    opus_decoder_destroy(decoder);
}
static void test_handshake(void) {
    IHS_Init();
    uint8_t secret[32] = {0}; IHS_ClientConfig config = {1, secret, "offline test"};
    IHS_SessionInfo info = { .steamId = 1, .sessionKeyLen = 16 };
    IHS_Session *session = IHS_SessionCreate(&config, &info);
    session->state.connectionState = IHS_SessionConnectionStateConnecting; session->state.connectionId = 189;
    IHS_SessionPacket ack = {0};
    ack.header.type = IHS_SessionPacketTypeConnectACK; ack.header.srcConnectionId = 7; ack.header.dstConnectionId = 189;
    ack.header.hasCrc = true;
    IHS_SessionPacketBodyInitialize(&ack.body, true); IHS_BufferAppendUInt32LE(&ack.body, 0);
    IHS_SessionPacketPopulateBuffer(&ack);
    IHS_Buffer wire = IHS_BUFFER_INIT(2048, 2048);
    IHS_BufferAppendMem(&wire, ack.body.data, IHS_SessionPacketSize(&ack));
    session->base.callbacks.received(&session->base, &info.address, &wire);
    assert(session->state.connectionState == IHS_SessionConnectionStateHandshaking);
    /* The client handshake must be queued for the send worker; its addressing and plaintext body
     * are verified byte by byte in test_queued_handshake. */
    assert(!IHS_QueueIsEmpty(session->sendQueue));
    // A replayed ConnectACK must not restart the handshake or queue a second client handshake.
    IHS_Buffer duplicate = IHS_BUFFER_INIT(2048, 2048);
    IHS_BufferAppendMem(&duplicate, ack.body.data, IHS_SessionPacketSize(&ack));
    session->base.callbacks.received(&session->base, &info.address, &duplicate);
    assert(session->state.connectionState == IHS_SessionConnectionStateHandshaking);
    assert(session->state.hostConnectionId == 7);
    // The server handshake is the step the TV never reached; it must complete the handshake
    // phase and carry the negotiated MTU into authentication.
    CServerHandshakeMsg handshake = CSERVER_HANDSHAKE_MSG__INIT;
    CStreamingServerHandshakeInfo handshakeInfo = CSTREAMING_SERVER_HANDSHAKE_INFO__INIT;
    handshakeInfo.has_mtu = true; handshakeInfo.mtu = 1400;
    handshake.info = &handshakeInfo;
    IHS_Buffer handshakePayload = IHS_BUFFER_INIT(64, 64);
    IHS_BufferAppendMessage(&handshakePayload, (ProtobufCMessage *)&handshake);
    IHS_SessionChannelControlOnMessageReceived(session->channels[1], k_EStreamControlServerHandshake, &handshakePayload, NULL);
    assert(session->state.connectionState == IHS_SessionConnectionStateAuthenticating);
    assert(session->state.mtu == 1400);
    IHS_BufferClear(&handshakePayload, true);
    IHS_BufferClear(&duplicate, true);
    CAuthenticationResponseMsg response = CAUTHENTICATION_RESPONSE_MSG__INIT;
    response.has_result = true; response.result = CAUTHENTICATION_RESPONSE_MSG__AUTHENTICATION_RESULT__FAILED;
    IHS_Buffer payload = IHS_BUFFER_INIT(64, 64); IHS_BufferAppendMessage(&payload, (ProtobufCMessage *)&response);
    IHS_SessionChannelControlOnAuthentication(session->channels[1], k_EStreamControlAuthenticationResponse, &payload, NULL);
    for (int i = 0; i < 40 && !atomic_load(&session->base.interrupted); i++) usleep(50000);
    assert(atomic_load(&session->base.interrupted));
    IHS_BufferClear(&payload, true);
    IHS_BufferClear(&wire, true); IHS_SessionPacketClear(&ack, true);
    IHS_SessionDestroy(session); IHS_Quit();
}
static void test_codec(void) {
    char buffer[64];
    assert(strcmp(VideoCodecString(IHS_StreamVideoCodecH264, 1024, 576, 30, buffer, sizeof(buffer)), "video/mp4; codecs=\"avc1.64001F\"") == 0);
    assert(strcmp(VideoCodecString(IHS_StreamVideoCodecH264, 1280, 720, 60, buffer, sizeof(buffer)), "video/mp4; codecs=\"avc1.64002A\"") == 0);
    assert(strcmp(VideoCodecString(IHS_StreamVideoCodecH264, 1920, 1080, 60, buffer, sizeof(buffer)), "video/mp4; codecs=\"avc1.64002A\"") == 0);
    assert(strcmp(VideoCodecString(IHS_StreamVideoCodecH264, 2560, 1440, 60, buffer, sizeof(buffer)), "video/mp4; codecs=\"avc1.640033\"") == 0);
    assert(strcmp(VideoCodecString(IHS_StreamVideoCodecH264, 3840, 2160, 30, buffer, sizeof(buffer)), "video/mp4; codecs=\"avc1.640033\"") == 0);
    assert(strcmp(VideoCodecString(IHS_StreamVideoCodecH264, 3840, 2160, 60, buffer, sizeof(buffer)), "video/mp4; codecs=\"avc1.640034\"") == 0);
    // This is the bug that stalled the startup on the TV: 4K60 is 497664000 luma samples per
    // second, which still fits HEVC level 5.1, so L153 is required rather than L150.
    assert(strcmp(VideoCodecString(IHS_StreamVideoCodecHEVC, 2560, 1440, 60, buffer, sizeof(buffer)), "video/mp4; codecs=\"hvc1.2.4.L150.B0\"") == 0);
    assert(strcmp(VideoCodecString(IHS_StreamVideoCodecHEVC, 3840, 2160, 30, buffer, sizeof(buffer)), "video/mp4; codecs=\"hvc1.2.4.L150.B0\"") == 0);
    assert(strcmp(VideoCodecString(IHS_StreamVideoCodecHEVC, 3840, 2160, 60, buffer, sizeof(buffer)), "video/mp4; codecs=\"hvc1.2.4.L153.B0\"") == 0);
}
/**
 * Drives a real ServerHandshake packet through the WHOLE receive path: packet parse, peer and
 * connection-id validation, the reliable channel window and finally the control dispatcher. The
 * direct dispatcher call in test_handshake() cannot catch a packet that is dropped on the way in,
 * which is exactly what the TV cannot distinguish.
 */
static void test_server_handshake_packet(void) {
    IHS_Init();
    uint8_t secret[32] = {0}; IHS_ClientConfig config = {1, secret, "offline test"};
    IHS_SessionInfo info = { .steamId = 1, .sessionKeyLen = 16 };
    IHS_Session *session = IHS_SessionCreate(&config, &info);
    // The state the TV reaches right after the ConnectACK: handshaking, our id 189, host id 7.
    session->state.connectionState = IHS_SessionConnectionStateHandshaking;
    session->state.connectionId = 189;
    session->state.hostConnectionId = 7;

    IHS_SessionPacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.header.type = IHS_SessionPacketTypeReliable;
    packet.header.channelId = IHS_SessionChannelIdControl;
    packet.header.packetId = 0;
    packet.header.fragmentId = 0;
    packet.header.srcConnectionId = 7;
    packet.header.dstConnectionId = 189;
    packet.header.hasCrc = true;
    IHS_SessionPacketBodyInitialize(&packet.body, true);
    IHS_BufferAppendUInt8(&packet.body, k_EStreamControlServerHandshake);
    CServerHandshakeMsg message = CSERVER_HANDSHAKE_MSG__INIT;
    CStreamingServerHandshakeInfo messageInfo = CSTREAMING_SERVER_HANDSHAKE_INFO__INIT;
    messageInfo.has_mtu = true; messageInfo.mtu = 1400;
    message.info = &messageInfo;
    IHS_BufferAppendMessage(&packet.body, (ProtobufCMessage *)&message);
    IHS_SessionPacketPopulateBuffer(&packet);

    IHS_Buffer wire = IHS_BUFFER_INIT(2048, 2048);
    IHS_BufferAppendMem(&wire, packet.body.data, IHS_SessionPacketSize(&packet));
    session->base.callbacks.received(&session->base, &session->info.address, &wire);
    assert(session->state.connectionState == IHS_SessionConnectionStateAuthenticating);
    assert(session->state.mtu == 1400);

    IHS_BufferClear(&wire, true);
    IHS_SessionPacketClear(&packet, true);
    IHS_SessionDestroy(session);
    IHS_Quit();
}
/**
 * Inspects the client handshake the ConnectACK produces. The ConnectACK is fed straight to the
 * discovery channel on purpose: the session-level receive path drains the send queue as a safety
 * net, which would empty it before it could be inspected.
 */
static void test_queued_handshake(void) {
    IHS_Init();
    uint8_t secret[32] = {0}; IHS_ClientConfig config = {1, secret, "offline test"};
    IHS_SessionInfo info = { .steamId = 1, .sessionKeyLen = 16 };
    IHS_Session *session = IHS_SessionCreate(&config, &info);
    session->state.connectionState = IHS_SessionConnectionStateConnecting;
    session->state.connectionId = 189;

    IHS_SessionChannel *discovery = session->channels[IHS_SessionChannelIdDiscovery];
    IHS_SessionPacket ack;
    memset(&ack, 0, sizeof(ack));
    ack.header.type = IHS_SessionPacketTypeConnectACK;
    ack.header.channelId = IHS_SessionChannelIdDiscovery;
    ack.header.srcConnectionId = 7;
    ack.header.dstConnectionId = 189;
    IHS_SessionPacketBodyInitialize(&ack.body, false);
    IHS_BufferAppendUInt32LE(&ack.body, 0);
    discovery->cls->received(discovery, &ack);

    assert(session->state.connectionState == IHS_SessionConnectionStateHandshaking);
    assert(session->state.hostConnectionId == 7);
    /* This is the packet the client must put on the wire right after the ConnectACK: verify its
     * addressing and that it is the plaintext control handshake. Nothing further downstream can
     * tell us, so it is checked here. */
    IHS_QueueItem *queued = IHS_QueuePoll(session->sendQueue);
    assert(queued != NULL);
    assert(queued->reliable);
    assert(queued->packet.header.type == IHS_SessionPacketTypeReliable);
    assert(queued->packet.header.channelId == IHS_SessionChannelIdControl);
    assert(queued->packet.header.srcConnectionId == 189);
    assert(queued->packet.header.dstConnectionId == 7);
    assert(queued->packet.body.size > 1);
    assert(*IHS_BufferPointer(&queued->packet.body) == k_EStreamControlClientHandshake);
    IHS_SessionPacketClear(&queued->packet, true);
    IHS_QueueItemFree(queued);

    IHS_SessionPacketClear(&ack, true);
    IHS_SessionDestroy(session);
    IHS_Quit();
}
#define main test_orphan_window
#include "ihslib-reference/tests/session/window_join_midmessage.c"
#undef main
#undef CAPACITY
#define main test_reordered_window
#include "ihslib-reference/tests/session/window_head_gap.c"
#undef main
#undef CAPACITY
#undef DISCARD_DIFF
void test_media_append(void);
int main(void) {
    TVTimingHistogram histogram = {0};
    for (unsigned i = 0; i < 100; i++) TVTimingAdd(&histogram, i);
    assert(histogram.count == 100 && TVTimingAverage(&histogram) == 49.5);
    assert(TVTimingP95Upper(&histogram) == 95 && histogram.max == 99);
    TVTimingAdd(&histogram, -1);
    assert(histogram.invalid == 1 && histogram.count == 100);
    TVTimingHistogram overflow = {0}; TVTimingAdd(&overflow, 250);
    assert(TVTimingP95Upper(&overflow) == 250);
    assert(TVTimestampDeltaMs(0xffff0000u, 0) == 1000);
    assert(TVTimestampDeltaMs(100, 99) < 0);
    const uint8_t pSlice[] = {0, 0, 1, 0x41, 0xc0};
    const uint8_t bSlice[] = {0, 0, 0, 1, 0x41, 0xa0};
    const uint8_t iSlice[] = {0, 0, 1, 0x65, 0xb0};
    const uint8_t badSlice[] = {0, 0, 1, 0x41, 0};
    const uint8_t avcc[] = {0, 0, 0, 2, 0x41, 0xa0};
    assert(TVH264FirstSliceType(pSlice, sizeof(pSlice)) == TVSliceP);
    assert(TVH264FirstSliceType(bSlice, sizeof(bSlice)) == TVSliceB);
    assert(TVH264FirstSliceType(iSlice, sizeof(iSlice)) == TVSliceI);
    assert(TVH264FirstSliceType(badSlice, sizeof(badSlice)) == TVSliceUnknown);
    assert(TVH264FirstSliceType(avcc, sizeof(avcc)) == TVSliceUnknown);
    assert(TVH264FirstSliceType(NULL, 0) == TVSliceUnknown);
    uint8_t mixed[sizeof(pSlice) + sizeof(bSlice)];
    memcpy(mixed, pSlice, sizeof(pSlice)); memcpy(mixed + sizeof(pSlice), bSlice, sizeof(bSlice));
    assert(TVH264FirstSliceType(mixed, sizeof(mixed)) == TVSliceP);
    memcpy(mixed, bSlice, sizeof(bSlice)); memcpy(mixed + sizeof(bSlice), pSlice, sizeof(pSlice));
    assert(TVH264FirstSliceType(mixed, sizeof(mixed)) == TVSliceB);
    uint8_t largeFrame[8192] = {0};
    memcpy(largeFrame, pSlice, sizeof(pSlice));
    assert(TVH264FirstSliceType(largeFrame, sizeof(largeFrame)) == TVSliceP);
    memset(largeFrame, 0, sizeof(largeFrame));
    memcpy(largeFrame + 4096, bSlice, sizeof(bSlice));
    assert(TVH264FirstSliceType(largeFrame, sizeof(largeFrame)) == TVSliceUnknown);
    test_media_append();
    AudioPcmBatch batch = {0};
    int16_t first[480 * 2], second[480 * 2];
    for (unsigned i = 0; i < 480 * 2; i++) { first[i] = (int16_t)i; second[i] = (int16_t)(-1 - (int)i); }
    assert(AudioPcmBatchPush(&batch, first, 0.5) == 0);
    assert(batch.pending && !AudioPcmBatchCanJoin(&batch, 480, 0.52));
    assert(!AudioPcmBatchCanJoin(&batch, 960, 0.51));
    assert(AudioPcmBatchCanJoin(&batch, 480, 0.51));
    assert(AudioPcmBatchPush(&batch, second, 0.51) == 960);
    assert(!batch.pending && batch.pts == 0.5 && sizeof(batch.data) == 3840);
    assert(memcmp(batch.data, first, sizeof(first)) == 0);
    assert(memcmp(batch.data + 480 * 2, second, sizeof(second)) == 0);
    unsigned batchCalls = 0;
    for (unsigned i = 0; i < 100; i++) {
        if (batch.pending) assert(AudioPcmBatchCanJoin(&batch, 480, i * 0.01));
        if (AudioPcmBatchPush(&batch, first, i * 0.01)) batchCalls++;
    }
    assert(batchCalls == 50 && !batch.pending);
    assert(TVDiagnosticEnabled(IHS_LogLevelDebug, 11, "Control", "Handshake"));
    assert(!TVDiagnosticEnabled(IHS_LogLevelDebug, 12, "Control", "SetCursor"));
    assert(!TVDiagnosticEnabled(IHS_LogLevelInfo, 12, "Control", "Control packet: id=42"));
    assert(!TVDiagnosticEnabled(IHS_LogLevelInfo, 12, "HID", "Message 42: Read"));
    assert(TVDiagnosticEnabled(IHS_LogLevelWarn, 12, "Control", "Control packet: invalid"));
    assert(TVDiagnosticEnabled(IHS_LogLevelInfo, 12, "Média", "Videó: 60 FPS"));
    assert(IHS_AudioFrameGap(100, 100) == 0);
    assert(IHS_AudioFrameGap(100, 99) < 0);
    assert(IHS_AudioFrameGap(100, 98) < 0);
    assert(IHS_AudioFrameGap(65535, 0) == 1);
    assert(IHS_AudioFrameGap(0, 0) == 0);
    assert(AudioPacketTime(0, -0.01, 0, 0) == 0);
    assert(AudioPacketTime(0, 0.5, 0, 0) == 0.5);
    double audioNext = 0;
    for (unsigned i = 0; i < 100; i++) {
        // Unique consecutive packets survive identical and backward-jittered sender timestamps.
        audioNext = AudioPacketTime(audioNext, i % 2 ? -0.01 : 0, 0, i != 0) + 0.01;
    }
    assert(audioNext > 0.999999 && audioNext < 1.000001);
    assert(AudioPacketTime(audioNext, 1.5, 4, 1) == 1.5);
    assert(AudioPacketTime(audioNext, 0.5, 4, 1) == audioNext);
    // Audio loss and sparse video preserve elapsed time on the same clock, including wrap.
    assert(MediaPacketTime(1000, 1000 + 32768) == 0.5);
    assert(MediaPacketTime(0xffff8000u, 0x00008000u) == 1.0);
    assert(MediaPacketTime(1000, 1000) == 0);
    assert(MediaPacketTime(1000, 999) < 0);
    assert(MediaPacketTime(1000, 1000 + 65536) - MediaPacketTime(1000, 1000 + 32768) == 0.5);
    assert(test_orphan_window() == 0);
    assert(test_reordered_window() == 0);
    test_network_frame_stats();
    test_gamepad(); test_opus(); test_codec(); test_handshake(); test_queued_handshake();
    test_server_handshake_packet();
    test_async_teardown();
    puts("Native WASM: 128-byte report, full/delta input, hotplug, Opus PCM, codec level ladder, ConnectACK handshake with queue drain, queued plaintext ClientHandshake addressing, duplicate-ACK replay, ServerHandshake dispatch and full receive path, failed-auth teardown passed.");
    return 0;
}
