#pragma once
#include <stdint.h>

static inline double MediaPacketTime(uint32_t origin, uint32_t timestamp) {
    // shortcut: signed elapsed time supports 9-hour sessions; rebase the origin for longer streams.
    return (int32_t)(timestamp - origin) / 65536.0;
}

static inline double AudioPacketTime(double next, double sender, uint16_t gap, int started) {
    if (!started) return sender > 0 ? sender : 0;
    return gap && sender > next ? sender : next;
}

