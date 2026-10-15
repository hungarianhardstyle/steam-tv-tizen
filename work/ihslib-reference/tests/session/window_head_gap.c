/* Reordered packets survive a short local deadline; genuine loss must still make progress. */

#include <assert.h>
#include <string.h>

#include "ihs_buffer.h"
#include "session/window.h"
#include "session/packet.h"

#define CAPACITY 16
#define DISCARD_DIFF IHS_SESSION_PACKET_TIMESTAMP_FROM_MILLIS(200)

/* A frame head (non-fragmented) packet: Poll() can yield it on its own. */
static void AddHead(IHS_SessionPacketsWindow *window, uint16_t packetId, uint32_t timestamp) {
    IHS_SessionPacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.header.type = IHS_SessionPacketTypeUnreliable;
    packet.header.channelId = IHS_SessionChannelIdDataStart;
    packet.header.packetId = packetId;
    packet.header.fragmentId = 0;
    packet.header.sendTimestamp = timestamp;
    IHS_BufferInit(&packet.body, 16, 16);
    IHS_BufferFillMem(&packet.body, 0, 0, 16);
    assert(IHS_SessionPacketsWindowAdd(window, &packet));
    IHS_SessionPacketClear(&packet, true);
}

static void ReorderScenario(uint16_t firstId, uint32_t origin) {
    IHS_SessionPacketsWindow *window = IHS_SessionPacketsWindowCreate(CAPACITY);
    IHS_SessionFrame frame;
    IHS_BufferInit(&frame.body, 1024, 1024 * 1024);
    /* Place the missing slot at the final ring position, with the later packet wrapping to zero. */
    for (uint16_t before = CAPACITY - 2; before; before--) {
        AddHead(window, (uint16_t)(firstId - before), origin - before * 655);
        assert(IHS_SessionPacketsWindowPoll(window, &frame));
        IHS_SessionPacketsWindowReleaseFrame(&frame);
    }
    AddHead(window, firstId, origin);
    assert(IHS_SessionPacketsWindowPoll(window, &frame));
    IHS_SessionPacketsWindowReleaseFrame(&frame);
    AddHead(window, (uint16_t)(firstId + 2), origin + 1311);
    assert(IHS_SessionPacketsWindowDiscardAt(window, DISCARD_DIFF, 1000) == 0);
    assert(!IHS_SessionPacketsWindowPoll(window, &frame));
    assert(IHS_SessionPacketsWindowDiscardAt(window, DISCARD_DIFF, 1009) == 0);
    AddHead(window, (uint16_t)(firstId + 1), origin + 655);
    assert(IHS_SessionPacketsWindowDiscardAt(window, DISCARD_DIFF, 1010) == 0);
    assert(IHS_SessionPacketsWindowPoll(window, &frame));
    assert(frame.header.packetId == (uint16_t)(firstId + 1));
    IHS_SessionPacketsWindowReleaseFrame(&frame);
    assert(IHS_SessionPacketsWindowPoll(window, &frame));
    assert(frame.header.packetId == (uint16_t)(firstId + 2));
    IHS_SessionPacketsWindowReleaseFrame(&frame);
    /* Packet 3 never arrives. Even without more incoming traffic the local deadline expires. */
    AddHead(window, (uint16_t)(firstId + 4), origin + 2622);
    assert(IHS_SessionPacketsWindowDiscardAt(window, DISCARD_DIFF, 1020) == 0);
    assert(IHS_SessionPacketsWindowDiscardAt(window, DISCARD_DIFF, 1039) == 0);
    assert(!IHS_SessionPacketsWindowPoll(window, &frame));
    assert(IHS_SessionPacketsWindowDiscardAt(window, DISCARD_DIFF, 1040) == 1);
    assert(IHS_SessionPacketsWindowPoll(window, &frame));
    assert(frame.header.packetId == (uint16_t)(firstId + 4));
    IHS_SessionPacketsWindowReleaseFrame(&frame);
    /* A new gap gets its own deadline rather than inheriting the expired one. */
    AddHead(window, (uint16_t)(firstId + 6), origin + 3933);
    assert(IHS_SessionPacketsWindowDiscardAt(window, DISCARD_DIFF, 1041) == 0);
    AddHead(window, (uint16_t)(firstId + 5), origin + 3277);
    assert(IHS_SessionPacketsWindowDiscardAt(window, DISCARD_DIFF, 1050) == 0);
    assert(IHS_SessionPacketsWindowPoll(window, &frame));
    assert(frame.header.packetId == (uint16_t)(firstId + 5));
    IHS_SessionPacketsWindowReleaseFrame(&frame);
    assert(IHS_SessionPacketsWindowPoll(window, &frame));
    assert(frame.header.packetId == (uint16_t)(firstId + 6));
    IHS_SessionPacketsWindowReleaseFrame(&frame);
    IHS_BufferClear(&frame.body, true);
    IHS_SessionPacketsWindowDestroy(window);
}

int main() {
    ReorderScenario(0, 10u * 65536);
    ReorderScenario(65534, 0xfffffff0u);
    return 0;
}
