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

#include <stdlib.h>
#include "ch_stats.h"
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"

static const IHS_SessionChannelClass ChannelClass = {
        .received = IHS_SessionChannelReceivedPacketNoop,
        .instanceSize = sizeof(IHS_SessionChannel)
};

IHS_SessionChannel *IHS_SessionChannelStatsCreate(IHS_Session *session) {
    return IHS_SessionChannelCreate(&ChannelClass, session, IHS_SessionChannelTypeStats, IHS_SessionChannelIdStats,
                                    NULL);
}

bool IHS_SessionChannelStatsSend(IHS_SessionChannel *channel, EStreamStatsMessage type,
                                 const ProtobufCMessage *message, int32_t packetId) {
    IHS_SessionPacket packet;
    IHS_SessionChannelInitializePacket(channel, &packet, IHS_SessionPacketTypeReliable, true, packetId);
    IHS_BufferAppendUInt8(&packet.body, type);
    IHS_BufferAppendMessage(&packet.body, message);
    /* No retransmission for the stats channel. Every packet here is periodic diagnostics (frame
     * stats and the forwarded client log); the host demonstrably does not acknowledge them, and the
     * library retries forever. A single 1440p session logged 5679 retransmissions, which burns the
     * CPU and the upstream bandwidth that the video needs — the measured cause of stutter and of
     * frozen high-resolution streams. */
    bool ret = IHS_SessionChannelQueuePacket(channel, &packet, false);
    IHS_SessionPacketClear(&packet, true);
    return ret;
}
