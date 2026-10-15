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

#include <stdio.h>
#include <unistd.h>

#include "ch_data.h"
#include "endianness.h"
#include "session/session_pri.h"

#include "ihs_buffer_ext.h"

/* Frames older than this are dropped when the consumer falls behind. 200 ms was too eager for
 * VIDEO: a single hiccup (a keyframe burst, a slow decode) then discarded frames, which forces a
 * keyframe request and costs far more than the latency it saves — measured as repeated "Unexpected
 * video frame sequence ... request keyframe" at 1440p. Audio uses its own, shorter value (see the
 * worker): for audio a delayed packet must be skipped quickly, not waited for. */
#define DISCARD_DIFF IHS_SESSION_PACKET_TIMESTAMP_FROM_MILLIS(450)

static void DataThreadWorker(IHS_SessionChannelData *channel);

static void DataThreadInterrupt(IHS_SessionChannelData *channel);

static void ReceivedFrame(IHS_SessionChannelData *channel, IHS_SessionFrame *frame);

static const char *DataChannelName(IHS_SessionChannelType type);

IHS_SessionChannel *IHS_SessionChannelDataCreate(const IHS_SessionChannelDataClass *cls,
                                                 IHS_Session *session, IHS_SessionChannelType type,
                                                 IHS_SessionChannelId id, const void *config) {
    assert(cls->base.instanceSize >= sizeof(IHS_SessionChannelData));
    return IHS_SessionChannelCreate(&cls->base, session, type, id, config);
}

bool IHS_SessionChannelDataInit(IHS_SessionChannel *channel, uint16_t windowCapacity) {
    IHS_SessionChannelData *dataCh = (IHS_SessionChannelData *)channel;
    dataCh->lock = IHS_MutexCreate();
    dataCh->windowLock = IHS_MutexCreate();
    dataCh->windowCond = IHS_CondCreate();
    dataCh->window = IHS_SessionPacketsWindowCreate(windowCapacity);
    atomic_init(&dataCh->interrupted, false);
    if (!dataCh->lock || !dataCh->windowLock || !dataCh->windowCond || !dataCh->window) {
        channel->initFailed = true;
        return false;
    }
    char threadName[16];
    snprintf(threadName, 16, "IHS%s", DataChannelName(channel->type));
    dataCh->worker = IHS_ThreadCreate((IHS_ThreadFunction *)DataThreadWorker, threadName, dataCh);
    channel->initFailed = dataCh->worker == NULL;
    return !channel->initFailed;
}

void IHS_SessionChannelDataDeinit(IHS_SessionChannel *channel) {
    IHS_SessionChannelData *dataCh = (IHS_SessionChannelData *)channel;

    assert(!dataCh->worker || dataCh->interrupted);

    IHS_ThreadJoin(dataCh->worker);
    if (dataCh->window)
        IHS_SessionPacketsWindowDestroy(dataCh->window);
    IHS_MutexDestroy(dataCh->windowLock);
    IHS_CondDestroy(dataCh->windowCond);
    dataCh->window = NULL;
    IHS_MutexDestroy(dataCh->lock);
}

void IHS_SessionChannelDataReceived(IHS_SessionChannel *channel, IHS_SessionPacket *packet) {
    IHS_SessionChannelData *dataCh = (IHS_SessionChannelData *)channel;
    assert(dataCh->window != NULL);
    IHS_SessionPacketType type = packet->header.type;
    if (type != IHS_SessionPacketTypeUnreliable && type != IHS_SessionPacketTypeUnreliableFrag)
        return;
    IHS_MutexLock(dataCh->windowLock);
    if (!IHS_SessionPacketsWindowAdd(dataCh->window, packet)) {
        channel->session->metrics.windowOverflows++;
        IHS_SessionLog(channel->session, IHS_LogLevelWarn, "Data",
                       "%s channel items overflow! Available: %u", DataChannelName(channel->type),
                       IHS_SessionPacketsWindowAvailable(dataCh->window));
        /* Drop everything, not just whole frames: Discard() walks forward to the
         * next frame-head packet, so once the head of the oldest frame is lost the
         * window is stuck full and every later packet overflows too. */
        IHS_SessionPacketsWindowReleaseAll(dataCh->window);
        IHS_SessionChannelDataLost(channel);
        IHS_SessionPacketsWindowAdd(dataCh->window,
                                    packet); /* now fits; caller clears on failure */
    }
    /* No IHS_CondSignal here: SDL condition variables do not work on this TV (the send worker had
     * to be changed to a 5 ms poll for the same reason), and signalling a waiter that is parked in a
     * broken futex wait wedges the caller. This is the thread that owns the UDP socket, so wedging
     * it stops every inbound packet for the rest of the session while sends keep working — exactly
     * what was measured on 2026-10-14 11:31 (last control packet 2687.627, 0 packets for the next
     * 28 s while the host logged ten further sends). The worker polls instead. */
    IHS_MutexUnlock(dataCh->windowLock);
    dataCh->lastPacketTimestamp = packet->header.sendTimestamp;
}
void IHS_SessionChannelDataLost(IHS_SessionChannel *channel) {
    CStreamDataLostMsg message = CSTREAM_DATA_LOST_MSG__INIT;
    IHS_SessionPacket packet;
    IHS_SessionChannelInitializePacket(channel, &packet, IHS_SessionPacketTypeUnreliable, true,
                                       IHS_PACKET_ID_NEXT);
    IHS_BufferAppendUInt8(&packet.body, k_EStreamDataLost);
    IHS_BufferAppendMessage(&packet.body, (const ProtobufCMessage *)&message);
    IHS_SessionChannelQueuePacket(channel, &packet, false);
    IHS_SessionPacketClear(&packet, true);
}

void IHS_SessionChannelDataStopped(IHS_SessionChannel *channel) {
    DataThreadInterrupt((IHS_SessionChannelData *)channel);
}

size_t IHS_SessionChannelDataFrameHeaderParse(IHS_SessionDataFrameHeader *header,
                                              const IHS_Buffer *data) {
    if (data->size < IHS_SESSION_DATA_FRAME_HEADER_SIZE)
        return 0;
    size_t offset = 0;
    offset += IHS_ReadUInt16LE(IHS_BufferPointerAt(data, offset), &header->id);
    offset += IHS_ReadUInt32LE(IHS_BufferPointerAt(data, offset), &header->timestamp);
    offset += IHS_ReadUInt16LE(IHS_BufferPointerAt(data, offset), &header->inputMark);
    offset += IHS_ReadUInt32LE(IHS_BufferPointerAt(data, offset), &header->inputRecvTimestamp);
    return offset;
}

static void DataThreadWorker(IHS_SessionChannelData *channel) {
    const IHS_SessionChannelDataClass *cls = (const IHS_SessionChannelDataClass *)channel->base.cls;
    IHS_SessionFrame frame;
    IHS_BufferInit(&frame.body, 1024, 1024 * 1024);
    const char *channelName = DataChannelName(channel->base.type);
    IHS_SessionLog(channel->base.session, IHS_LogLevelInfo, "Data", "Starting %s channel",
                   channelName);
    if (!cls->start((IHS_SessionChannel *)channel)) {
        IHS_SessionLog(channel->base.session, IHS_LogLevelError, "Data",
                       "Failed to start %s channel", channelName);
        IHS_SessionDisconnect(channel->base.session);
        IHS_BufferClear(&frame.body, true);
        return;
    }
    IHS_SessionLog(channel->base.session, IHS_LogLevelInfo, "Data", "%s channel started",
                   channelName);
    /* Diagnostics for the age-based discard: when the last frame was delivered, and when the discard
     * cost was last reported (rate-limited to one Warn line per second per channel). */
    uint64_t lastDeliveryMs = 0, lastDiscardLogMs = 0;
    while (!channel->interrupted) {
        IHS_MutexLock(channel->windowLock);
        bool hasFrame;
        for (;;) {
            /* Retry the age-based discard on every wakeup, not just once per
             * delivered frame: a packet lost at the head makes Poll fail forever,
             * and the discard that would step over it is the only way out.
             * Audio keeps a SHORT tolerance: a long one does not help a straggler, it only makes the
             * worker sit idle behind a lost packet and then deliver a burst, which is heard as a
             * stutter. Video keeps the longer one, where dropping an old frame is the right answer. */
            uint32_t discardDiff = channel->base.type == IHS_SessionChannelTypeDataAudio
                                   ? IHS_SESSION_PACKET_TIMESTAMP_FROM_MILLIS(200)
                                   : DISCARD_DIFF;
            uint16_t discarded = IHS_SessionPacketsWindowDiscard(channel->window, discardDiff);
            channel->base.session->metrics.windowDiscards += discarded;
            if (discarded > 0) {
                /* Verbose, not Debug: a busy stream discards packets on almost every poll, and each
                 * logged line costs a ring write plus a JavaScript proxy call on the main loop. That
                 * churn was measurable as stutter on 1080p. */
                IHS_SessionLog(channel->base.session, IHS_LogLevelVerbose, channelName,
                               "Discarded %u packets", discarded);
                /* One Warn line per second per channel, because this is the only place that shows the
                 * cost of the age-based discard: a frame head that never completes blocks Poll until the
                 * tolerance expires (video 450 ms, audio 200 ms), and for that whole time the worker
                 * delivers NOTHING on that channel - ~27 frames of video at 60 fps. "stall" is how long
                 * the channel has gone without a completed frame, so a matching picture stutter can be
                 * attributed to it instead of guessed at. */
                uint64_t discardNow = IHS_TimerNow();
                if (discardNow - lastDiscardLogMs >= 1000) {
                    lastDiscardLogMs = discardNow;
                    uint64_t stallMs = lastDeliveryMs != 0 ? discardNow - lastDeliveryMs : 0;
                    IHS_SessionLog(channel->base.session, IHS_LogLevelWarn, channelName,
                                   "Discarded %u packets, no complete frame for %llu ms (tolerance %u ms)",
                                   discarded, (unsigned long long) stallMs,
                                   (unsigned) (discardDiff * 1000u / 65536u));
                }
            }
            if ((hasFrame = IHS_SessionPacketsWindowPoll(channel->window, &frame))) {
                break;
            }
            /* Stop may have signalled before this worker acquired windowLock. */
            if (channel->interrupted)
                break;
            /* Poll instead of IHS_CondWait: SDL/pthread condition variables do not wake on this TV
             * (the same defect froze the session send worker until it was changed to a 5 ms poll).
             * Waiting here also means the receive thread's signal can park in a broken futex. */
            IHS_MutexUnlock(channel->windowLock);
            usleep(2000);
            IHS_MutexLock(channel->windowLock);
            if (!channel->worker || channel->interrupted) {
                hasFrame = false;
                break;
            }
        }
        IHS_MutexUnlock(channel->windowLock);
        if (hasFrame) {
            ReceivedFrame(channel, &frame);
            IHS_SessionPacketsWindowReleaseFrame(&frame);
            lastDeliveryMs = IHS_TimerNow();
        }
    }
    IHS_SessionLog(channel->base.session, IHS_LogLevelInfo, "Data", "Stopping %s channel",
                   channelName);
    IHS_BufferClear(&frame.body, true);
    cls->stop((IHS_SessionChannel *)channel);
}

static void DataThreadInterrupt(IHS_SessionChannelData *channel) {
    if (!channel->worker || channel->interrupted) {
        return;
    }
    IHS_MutexLock(channel->lock);
    channel->interrupted = true;
    IHS_MutexUnlock(channel->lock);
    /* The worker polls every 2 ms, so no IHS_CondSignal is needed here; signalling a parked waiter
     * is exactly the operation that hangs on this platform. */
}

static void ReceivedFrame(IHS_SessionChannelData *channel, IHS_SessionFrame *frame) {
    assert(frame->header.type == IHS_SessionPacketTypeUnreliable);
    if (frame->body.size < 1)
        return;
    EStreamDataMessage type = *IHS_BufferPointer(&frame->body);
    IHS_BufferOffsetBy(&frame->body, 1);
    if (type != k_EStreamDataPacket) {
        return;
    }
    IHS_SessionDataFrameHeader header;
    bool hasHeader = false;
    if (frame->body.size > IHS_SESSION_DATA_FRAME_HEADER_SIZE) {
        hasHeader = true;
        size_t offset = IHS_SessionChannelDataFrameHeaderParse(&header, &frame->body);
        IHS_BufferOffsetBy(&frame->body, (int)offset);
        header.sendTimestamp = frame->header.sendTimestamp;
        header.receiveTimestamp = frame->header.receiveTimestamp;
    }
    const IHS_SessionChannelDataClass *cls = (const IHS_SessionChannelDataClass *)channel->base.cls;
    if (hasHeader)
        cls->dataFrame((IHS_SessionChannel *)channel, &header, &frame->body);
}

static const char *DataChannelName(IHS_SessionChannelType type) {
    switch (type) {
    case IHS_SessionChannelTypeDataAudio:
        return "Audio";
    case IHS_SessionChannelTypeDataVideo:
        return "Video";
    default:
        return "Data";
    }
}
