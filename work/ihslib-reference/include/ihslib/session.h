/*
 *  _____  _   _  _____  _  _  _
 * |_   _|| | | |/  ___|| |(_)| |     Steam
 *   | |  | |_| |\ `--. | | _ | |__     In-Home
 *   | |  |  _  | `--. \| || || '_ \      Streaming
 *  _| |_ | | | |/\__/ /| || || |_) |       Library
 *  \___/ \_| |_/\____/ |_||_||_.__/
 *
 * Copyright (c) 2022 Mariotaku <https://github.com/mariotaku>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#pragma once

#include "common.h"
#include "audio.h"
#include "video.h"
#include "input.h"
#include "hid.h"

typedef struct IHS_Session IHS_Session;

typedef struct IHS_SessionInfo {
    IHS_SocketAddress address;
    uint8_t sessionKey[32];
    size_t sessionKeyLen;
    uint64_t steamId;
} IHS_SessionInfo;

/** Encoding preference, independent of bitrate/resolution limits. Zero uses Balanced. */
typedef enum IHS_StreamQuality {
    IHS_StreamQualityDefault = 0,
    IHS_StreamQualityFast = 1,
    IHS_StreamQualityBalanced = 2,
    IHS_StreamQualityBeautiful = 3,
} IHS_StreamQuality;

typedef struct IHS_SessionConfig {
    bool enableAudio;
    bool enableHevc;
    /** Cap on the host's encode resolution, framerate and bitrate. 0 = library default. */
    uint32_t maxWidth;
    uint32_t maxHeight;
    uint32_t maxFps;
    uint32_t maxBitrateKbps;
    IHS_StreamQuality quality;
} IHS_SessionConfig;

/** Optional platform/decoder facts. Unknown fields are omitted from the wire.
 * Bitrates are capability limits, separate from the requested encode bitrate. */
typedef enum IHS_StreamDeviceFormFactor {
    IHS_StreamDeviceFormFactorUnknown = 0,
    IHS_StreamDeviceFormFactorPhone = 1,
    IHS_StreamDeviceFormFactorTablet = 2,
    IHS_StreamDeviceFormFactorComputer = 3,
    IHS_StreamDeviceFormFactorTV = 4,
    IHS_StreamDeviceFormFactorVRHeadset = 5,
} IHS_StreamDeviceFormFactor;

typedef struct IHS_StreamClientCapabilities {
    const char *systemInfo; /* VDF SystemInfo, NULL = unknown */
    const char *decoderInfo; /* Descriptive decoder name, NULL = unknown */
    uint32_t decoderThreads; /* 0 = unknown */
    uint32_t maximumDecodeBitrateKbps; /* 0 = unknown */
    uint32_t maximumBurstBitrateKbps; /* 0 = unknown */
    IHS_StreamDeviceFormFactor formFactor;
    bool hasSystemCanSuspend;
    bool systemCanSuspend;
} IHS_StreamClientCapabilities;

/** Set before Connect or creation of data channels. Strings are copied; the
 * caller may release them immediately. NULL resets to unknown capabilities.
 * Returns false for invalid fields, allocation failure, or a started session;
 * on failure the previous capabilities remain intact. */
bool IHS_SessionSetClientCapabilities(IHS_Session *session,
                                      const IHS_StreamClientCapabilities *capabilities);

/** Monotonic counters and current occupancy for the reliable transport and the
 * HID transport packets (confirmation does not imply host input application). Safe to sample from a diagnostics thread. */
typedef struct IHS_SessionReliabilityStats {
    uint64_t reliableTracked;
    uint64_t reliableAcknowledged;
    uint64_t reliableSuperseded;
    uint64_t reliableGiveUps;
    uint64_t reliableNacks;
    uint64_t reliableRetries;
    uint64_t reliableSendFailures;
    uint32_t reliableOutstanding;
    uint64_t reliableOldestOutstandingMs;
    uint32_t reliableOldestChannelId;
    uint32_t reliableOldestPacketId;
    int32_t reliableOldestFragmentId;
    uint32_t reliableOldestRetryCount;
    uint64_t reliableMaxAckLatencyMs;
    uint64_t hidSubmitted;
    uint64_t hidCoalesced;
    uint64_t hidSent; /* successfully queued RemoteHID messages */
    uint64_t hidAcknowledged; /* confirmed HID transport packets, including fragments */
    uint64_t hidSuperseded;
    uint32_t hidPending; /* queued HID packets before their first send attempt */
    uint32_t hidInFlight; /* attempted HID packets still awaiting confirmation */
    int32_t hidOldestInFlightPacketId;
} IHS_SessionReliabilityStats;

typedef struct IHS_StreamSessionCallbacks {
    void (*initialized)(IHS_Session *session, void *context);

    void (*connecting)(IHS_Session *session, void *context);

    void (*configuring)(IHS_Session *session, IHS_SessionConfig *config, void *context);

    void (*connected)(IHS_Session *session, void *context);

    void (*disconnected)(IHS_Session *session, void *context);

    /**
     * All the resources and states has been destroyed. Nothing can be used beyond this call.
     * @param session Session pointer
     * @param context Callback context
     */
    void (*finalized)(IHS_Session *session, void *context);
} IHS_StreamSessionCallbacks;

/*
 * Lifecycle functions
 */

/**
 * Create session instance
 * @param clientConfig Client info
 * @param sessionInfo Session info
 * @return Session instance
 */
IHS_Session *IHS_SessionCreate(const IHS_ClientConfig *clientConfig, const IHS_SessionInfo *sessionInfo);

/**
 * Start receive and send thread, and send connect request
 * @param session Session instance
 * @return
 */
bool IHS_SessionConnect(IHS_Session *session);

/** Wake and stop the session worker thread. Safe to call before
 * IHS_SessionThreadedJoin to guarantee the join completes. */
void IHS_SessionInterrupt(IHS_Session *session);

/**
 * Send disconnect request
 * @param session Session instance
 */
void IHS_SessionDisconnect(IHS_Session *session);

/* Explicit user action only. Queues reliable StopRequest(129) while connected;
 * success means queued, not that the host game has finished. */
bool IHS_SessionStopGame(IHS_Session *session);

/* True only after an explicit host StopRequest or transport Disconnect.
 * Local teardown, authentication failures and silence do not set this flag. */
bool IHS_SessionHostRequestedStop(IHS_Session *session);
/* Explicit StopVideoData, cleared by StartVideoData; independent of audio/session. */
bool IHS_SessionHostVideoStopped(IHS_Session *session);

/**
 * Wait for all threads to finish
 * @param session Session instance
 */
void IHS_SessionThreadedJoin(IHS_Session *session);

/**
 * Release all resources of the session and free the pointer
 * @param session Session instance
 */
void IHS_SessionDestroy(IHS_Session *session);

/*
 * Callback related functions
 */

void IHS_SessionSetSessionCallbacks(IHS_Session *session, const IHS_StreamSessionCallbacks *callbacks, void *context);

void IHS_SessionSetAudioCallbacks(IHS_Session *session, const IHS_StreamAudioCallbacks *callbacks, void *context);

void IHS_SessionSetVideoCallbacks(IHS_Session *session, const IHS_StreamVideoCallbacks *callbacks, void *context);

void IHS_SessionSetInputCallbacks(IHS_Session *session, const IHS_StreamInputCallbacks *callbacks, void *context);

/**
 * Register microphone (client → server) callbacks. Setting non-NULL callbacks here
 * advertises the `enable_microphone_streaming` capability to the server during
 * negotiation; the server may then issue Start/Stop microphone control messages.
 *
 * Must be called before IHS_SessionConnect — capability negotiation happens once and
 * cannot be re-advertised mid-session.
 */
void IHS_SessionSetMicrophoneCallbacks(IHS_Session *session, const IHS_StreamMicrophoneCallbacks *callbacks,
                                       void *context);

/**
 * Submit one encoded microphone frame for transmission. Call from the capture / encode
 * thread, once per Opus packet (or once per PCM chunk when codec=Raw). The payload is
 * wrapped in a data-frame header and queued for unreliable send on the mic data channel.
 *
 * Returns false if no microphone channel is currently active (the server hasn't issued
 * Start, or has issued Stop, or the session isn't connected); the caller can use this
 * as the cue to drop the frame.
 */
bool IHS_SessionSendMicrophoneData(IHS_Session *session, const uint8_t *data, size_t len);

/**
 * Forward one line of the client's own log to the host over the stats channel
 * (`k_EStreamStatsLogMessage`). Steam's host writes these lines into its own
 * streaming_log.txt prefixed with "CLIENT: ", which is the only way to read a
 * client's log on a device whose stdout is unreachable. Real Steam Link clients
 * do this from the moment the session is connected.
 *
 * Returns false when the session has no stats channel yet.
 */
bool IHS_SessionSendLogMessage(IHS_Session *session, int type, const char *message);

/**
 * One of the four client-owned stages of a video frame's lifecycle that the
 * application reports back to ihslib via IHS_SessionReportVideoFrameStage.
 * Values match Steam's EStreamFrameEvent so the wire encoding is a direct
 * cast — do not renumber.
 */
typedef enum IHS_VideoFrameStage {
    IHS_VideoFrameStageDecodeBegin = 14,
    IHS_VideoFrameStageDecodeEnd = 15,
    IHS_VideoFrameStageUploadBegin = 16,
    IHS_VideoFrameStageUploadEnd = 17,
} IHS_VideoFrameStage;

/**
 * Per-frame outcome reported by the application. Values match Steam's
 * EStreamFrameResult — do not renumber.
 */
typedef enum IHS_VideoFrameResult {
    IHS_VideoFrameResultPending = 0,
    IHS_VideoFrameResultDisplayed = 1,
    IHS_VideoFrameResultDroppedNetworkSlow = 2,
    IHS_VideoFrameResultDroppedNetworkLost = 3,
    IHS_VideoFrameResultDroppedDecodeSlow = 4,
    IHS_VideoFrameResultDroppedDecodeCorrupt = 5,
    IHS_VideoFrameResultDroppedLate = 6,
    IHS_VideoFrameResultDroppedReset = 7,
} IHS_VideoFrameResult;

/**
 * Report a per-frame decode/upload milestone. ihslib accumulates these into a
 * 1 Hz CFrameStatsListMsg that drives the host's adaptive-bitrate / framerate
 * loop. `timestamp` is in IHS_SessionPacketTimestamp units (1/65536 second);
 * pass `0` to use the current session clock.
 *
 * Safe to call from any thread.
 */
void IHS_SessionReportVideoFrameStage(IHS_Session *session, uint16_t frameId,
                                      IHS_VideoFrameStage stage, uint32_t timestamp);

/**
 * Report the final outcome of a video frame (displayed or one of the drop
 * categories). Must be called exactly once per frame the application
 * processed, AFTER any IHS_SessionReportVideoFrameStage calls for it. This is
 * what advances the "last displayed" cursor that the 1 Hz flush walks; if
 * skipped, the host never gets stats for that frame.
 *
 * Safe to call from any thread.
 */
void IHS_SessionReportVideoFrameComplete(IHS_Session *session, uint16_t frameId,
                                         IHS_VideoFrameResult result);

/**
 * Debug toggle: when true, each flush packet carries per-frame CFrameStats
 * rows in addition to the aggregated stats. Default false (aggregated only,
 * matching Steam's normal operating mode). Useful for capturing wire traces.
 */
void IHS_SessionStatsSetFullReporting(IHS_Session *session, bool enabled);

void IHS_SessionSetLogFunction(IHS_Session *session, IHS_LogFunction *logFunction);

const IHS_SessionInfo *IHS_SessionGetInfo(const IHS_Session *session);

/**
 * Sender timestamp (in 1/65536 second units, see IHS_SESSION_PACKET_TIMESTAMP_TO_MILLIS) of the video
 * frame the decoder callback is currently receiving. A live video timeline must advance by these
 * deltas rather than by a fixed 1/fps per delivered frame: the host delivers fewer frames than the
 * target rate when the content allows it, and a timeline built from the frame count then runs slower
 * than real time and drifts away from the audio timeline.
 */
uint32_t IHS_SessionVideoFrameTimestamp(const IHS_Session *session);
/* Local 16.16 clock at complete assembly, for the current video submit callback; excludes receive queueing. */
uint32_t IHS_SessionVideoFrameReadyTimestamp(const IHS_Session *session);
/** Sender timestamp of the packet currently passed to the audio submit callback, in the same clock. */
uint32_t IHS_SessionAudioFrameTimestamp(const IHS_Session *session);
/** Missing frame IDs before the current audio callback; zero for its first/consecutive packet. */
uint16_t IHS_SessionAudioFrameGap(const IHS_Session *session);

/**
 * Why the video path gave up on a frame and asked the host for a keyframe. Every request is counted
 * both in total and by reason: a missing frame (sequence), an assembly that stalled, a failed decrypt,
 * a discarded mismatch and a decoder that refused the frame all look identical in a plain total, but
 * they point at different fixes (network, assembly window, crypto, decoder hand-off).
 */
typedef enum IHS_VideoLossReason {
    IHS_VideoLossReasonSequence = 0,   /**< frame id jumped: a frame is missing */
    IHS_VideoLossReasonWaitTimeout,    /**< the keyframe we asked for never arrived in time */
    IHS_VideoLossReasonDecrypt,        /**< the frame failed to decrypt */
    IHS_VideoLossReasonPartialStall,   /**< fragments of one frame were spread too far apart */
    IHS_VideoLossReasonAssembleMismatch, /**< fragments of another frame appeared mid-assembly */
    IHS_VideoLossReasonSubmitLost,     /**< the decoder/host reported the submitted frame lost */
    IHS_VideoLossReasonOverflow,       /**< the receive window overflowed */
    IHS_VideoLossReasonCount
} IHS_VideoLossReason;

/**
 * Diagnostic counters for the running stream: cumulative totals and maxima since the session was
 * created. They exist so a single log line can say *why* the picture was uneven - frames arriving
 * late, frames assembling slowly, or the loss/keyframe machinery being busy - instead of guessing.
 *
 * Nothing in the library's control flow reads these counters, so sampling them can never change the
 * stream's behaviour.
 */
typedef struct IHS_SessionStreamMetrics {
    /* Video, measured in the video data channel. */
    uint64_t videoFrames;         /**< frames handed to the submit callback */
    uint64_t videoKeyFrames;      /**< keyframes among them */
    uint64_t videoBytes;          /**< total encoded bytes of those frames */
    uint64_t videoAssemblyUsSum;  /**< sum of "first fragment arrived -> frame complete" */
    uint64_t videoAssemblyUsMax;  /**< worst such time in microseconds */
    uint64_t videoArrivalUsSum;   /**< sum of "frame complete -> next frame complete" */
    uint64_t videoArrivalUsMax;   /**< worst such gap in microseconds */
    /* Host pacing, measured purely from the host's own frame timestamps: no clock synchronisation and
     * no network involvement. Comparing this with videoArrivalUs (local receive side) is what
     * separates "the host/PC sent frames late" from "the network delivered them late". */
    uint64_t videoHostIntervalUsSum;
    uint64_t videoHostIntervalUsMax;
    uint64_t videoLossRequests;   /**< keyframe requests due to loss, reset or overflow */
    uint64_t videoLossByReason[IHS_VideoLossReasonCount]; /**< the same requests, by IHS_VideoLossReason */
    uint64_t videoDecoderResets;  /**< decoder resets requested */
    uint64_t videoPartialCleared; /**< fragments discarded before their frame completed */
    uint64_t windowDiscards;      /**< packets dropped by the age-based discard */
    uint64_t windowOverflows;     /**< packets dropped because the channel window was full */
    /* Audio */
    uint64_t audioFrames;         /**< frames delivered to the audio callback */
    uint64_t audioGaps;           /**< times an audio frame id was missing */
    uint64_t audioMissingFrames;  /**< how many audio frames were missing in those gaps */
    uint64_t audioReordered;      /**< packets dropped because they arrived out of order */
    uint64_t audioArrivalUsSum;   /**< sum of "audio frame -> next audio frame", local receive side */
    uint64_t audioArrivalUsMax;   /**< worst such gap: a gap here is an audible hole or a burst */
    uint64_t audioHostIntervalUsSum; /**< the same interval from the host's own timestamps */
    uint64_t audioHostIntervalUsMax;
    /* The same maxima for the CURRENT reporting window only: they are reset every time
     * IHS_SessionGetStreamMetrics() copies the struct out. The session-wide maxima above pin the
     * start-up spike forever, which made per-second analysis impossible: a single slow first second
     * then looked like a problem in every later window. */
    uint64_t videoArrivalUsWindowMax;
    uint64_t videoAssemblyUsWindowMax;
    uint64_t videoHostIntervalUsWindowMax;
    uint64_t audioArrivalUsWindowMax;
} IHS_SessionStreamMetrics;

void IHS_SessionGetStreamMetrics(IHS_Session *session, IHS_SessionStreamMetrics *metrics);

void IHS_SessionGetReliabilityStats(IHS_Session *session,
                                    IHS_SessionReliabilityStats *stats);
