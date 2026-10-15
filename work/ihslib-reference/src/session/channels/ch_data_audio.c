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

#include <memory.h>
#include <stdio.h>
#include <stdlib.h>

#include "ch_data.h"
#include "ch_data_audio.h"

#include "session/session_pri.h"

typedef struct ChannelAudio {
    IHS_SessionChannelData base;
    IHS_StreamAudioConfig config;
} ChannelAudio;

static void ChannelAudioInit(IHS_SessionChannel *channel, const void *config);

static void ChannelAudioDeinit(IHS_SessionChannel *channel);

static bool DataStart(IHS_SessionChannel *channel);

static void DataReceived(IHS_SessionChannel *channel, const IHS_SessionDataFrameHeader *header,
                         IHS_Buffer *body);

static void DataStop(IHS_SessionChannel *channel);

static const IHS_SessionChannelDataClass ChannelClass = {
    {.init = ChannelAudioInit,
     .deinit = ChannelAudioDeinit,
     .received = IHS_SessionChannelDataReceived,
     .stopped = IHS_SessionChannelDataStopped,
     .instanceSize = sizeof(ChannelAudio)},
    .start = DataStart,
    .dataFrame = DataReceived,
    .stop = DataStop,
};

IHS_SessionChannel *IHS_SessionChannelDataAudioCreate(IHS_Session *session,
                                                      const CStartAudioDataMsg *message) {
    return IHS_SessionChannelDataCreate(&ChannelClass, session, IHS_SessionChannelTypeDataAudio,
                                        message->channel, message);
}

static void ChannelAudioInit(IHS_SessionChannel *channel, const void *config) {
    ChannelAudio *audioCh = (ChannelAudio *)channel;
    const CStartAudioDataMsg *message = config;
    audioCh->config.channels = message->channels;
    audioCh->config.frequency = message->frequency;
    audioCh->config.codec = (IHS_StreamAudioCodec)message->codec;
    if (message->has_codec_data && message->codec_data.len) {
        if (message->codec_data.len > 2u * 1024u * 1024u || !message->codec_data.data) {
            channel->initFailed = true;
            return;
        }
        audioCh->config.codecDataLen = message->codec_data.len;
        audioCh->config.codecData = malloc(message->codec_data.len);
        if (!audioCh->config.codecData) {
            channel->initFailed = true;
            return;
        }
        memcpy(audioCh->config.codecData, message->codec_data.data, message->codec_data.len);
    }
    IHS_SessionChannelDataInit(channel, 256);
}

static void ChannelAudioDeinit(IHS_SessionChannel *channel) {
    IHS_SessionChannelDataDeinit(channel);
    ChannelAudio *audioCh = (ChannelAudio *)channel;
    if (audioCh->config.codecData) {
        free(audioCh->config.codecData);
    }
}

/* Id-gap statistics for the audio stream. Reset in DataStart: a reconnect must not compare the first
 * id of the new stream with the last id of the previous one (that reported a bogus 65535-frame gap). */
static uint16_t audioExpectedId;
static bool audioHaveExpected;
static uint32_t audioGaps, audioMissingFrames;

static bool DataStart(struct IHS_SessionChannel *channel) {
    ChannelAudio *audioCh = (ChannelAudio *)channel;
    IHS_Session *session = channel->session;
    const IHS_StreamAudioCallbacks *callbacks = session->callbacks.audio;
    audioHaveExpected = false;
    audioGaps = audioMissingFrames = 0;
    if (!callbacks || !callbacks->start)
        return true;
    return callbacks->start(session, &audioCh->config, session->callbackContexts.audio) == 0;
}

static void DataReceived(IHS_SessionChannel *channel, const IHS_SessionDataFrameHeader *header,
                         IHS_Buffer *body) {
    IHS_Session *session = channel->session;
    const IHS_StreamAudioCallbacks *callbacks = session->callbacks.audio;
    if (!callbacks || !callbacks->submit)
        return;
    /* Only the packets the host actually sent are submitted. Synthesising Opus concealment frames
     * for the gaps (added 15:42, removed again here) multiplied the audio track's load: every
     * missing id produced up to eight extra 20 ms packets, which pushes the audio timeline ahead of
     * real time and floods the shared TV media pipeline. Measured consequence on 2026-10-14: the
     * first video append after the audio track opened took 13-62 ms instead of 1-6 ms and the stream
     * wedged before the first frame was ever presented. Gaps are logged instead - that is the input
     * the stutter investigation needs, and it costs nothing. */
    uint16_t gap = 0;
    if (audioHaveExpected) {
        /* int16_t, not uint16_t: a duplicate or reordered packet makes the difference negative, and
         * as an unsigned value that would count 65535 lost frames. Only forward gaps are losses. */
        int16_t delta = IHS_AudioFrameGap(audioExpectedId, header->id);
        if (delta < 0) {
            session->metrics.audioReordered++;
            return;
        }
        gap = (uint16_t)delta;
        if (delta > 0) {
            unsigned missing = (unsigned) delta;
            audioGaps++;
            audioMissingFrames += missing;
            session->metrics.audioGaps++;
            session->metrics.audioMissingFrames += missing;
            if (audioGaps <= 8 || audioGaps % 50 == 0) {
                char line[128];
                snprintf(line, sizeof(line), "Hang: %u kihagyott id (%u kocka) a %u. csomag elott.",
                         missing, audioMissingFrames, (unsigned) header->id);
                IHS_SessionLog(session, IHS_LogLevelWarn, "Audio", "%s", line);
            }
        }
    }
    audioExpectedId = (uint16_t) (header->id + 1);
    audioHaveExpected = true;
    session->lastAudioFrameTimestamp = header->timestamp;
    session->lastAudioFrameGap = gap;
    session->metrics.audioFrames++;
    /* Audio arrival pacing (local receive clock) and the same interval taken from the host's own
     * timestamps. The client feeds the decoder one frame per arrival, so a bursty arrival pattern is
     * the audio's own stutter signature even when nothing is lost and nothing is refused; comparing
     * the two intervals separates host-side audio pacing from network delivery. Diagnostics only. */
    {
        uint32_t now = IHS_SessionPacketTimestamp();
        if (session->metricsHaveAudioArrival) {
            uint64_t localUs = (uint64_t) (uint32_t) (now - session->metricsLastAudioArrivalTs) * 1000000u / 65536u;
            uint64_t hostUs = (uint64_t) (uint32_t) (header->timestamp - session->metricsLastAudioHostTs) * 1000000u / 65536u;
            if (localUs > 0 && localUs < 5000000u) {
                session->metrics.audioArrivalUsSum += localUs;
                if (localUs > session->metrics.audioArrivalUsMax) session->metrics.audioArrivalUsMax = localUs;
                if (localUs > session->metrics.audioArrivalUsWindowMax) session->metrics.audioArrivalUsWindowMax = localUs;
            }
            if (hostUs > 0 && hostUs < 5000000u) {
                session->metrics.audioHostIntervalUsSum += hostUs;
                if (hostUs > session->metrics.audioHostIntervalUsMax) session->metrics.audioHostIntervalUsMax = hostUs;
            }
        }
        session->metricsLastAudioArrivalTs = now;
        session->metricsLastAudioHostTs = header->timestamp;
        session->metricsHaveAudioArrival = true;
    }
    callbacks->submit(session, body, session->callbackContexts.audio);
}

static void DataStop(IHS_SessionChannel *channel) {
    IHS_Session *session = channel->session;
    const IHS_StreamAudioCallbacks *callbacks = session->callbacks.audio;
    if (!callbacks || !callbacks->stop)
        return;
    callbacks->stop(session, session->callbackContexts.audio);
}
