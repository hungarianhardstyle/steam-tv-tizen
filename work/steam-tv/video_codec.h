#pragma once
/* MIME codec string selection for the Samsung media track. Kept in its own header so the native
 * test and the probe build exercise the exact same code: a wrong level here either makes the TV
 * refuse the track or silently selects the wrong hardware decoder.
 * C-compatible on purpose: the native test is compiled as C99. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "ihslib/video.h"

/* H.264 levels by macroblocks per second (3.1=108000, 4.2=522240, 5.1=983040, 5.2=2073600) and
 * HEVC levels by luma samples per second (5.0=267386880, 5.1=534773760, 5.2=1069547520). The
 * declared level must be at least what the stream needs, otherwise hardware decoders refuse it. */
static inline const char *VideoCodecString(IHS_StreamVideoCodec codec, uint32_t width,
                                           uint32_t height, uint32_t fps, char *buffer,
                                           size_t size) {
    if (codec == IHS_StreamVideoCodecHEVC) {
        uint64_t samples = (uint64_t)width * height * fps;
        /* Pick the LOWEST level whose own limit still covers the stream: 4K60 has 497664000
         * samples/s, which fits level 5.1 (534773760) and therefore must be reported as L153. */
        const char *level = samples > 534773760 ? "L156" : samples > 267386880 ? "L153" : "L150";
        snprintf(buffer, size, "video/mp4; codecs=\"hvc1.2.4.%s.B0\"", level);
        return buffer;
    }
    uint64_t macroblocks = (uint64_t)((width + 15) / 16) * ((height + 15) / 16) * fps;
    const char *level = macroblocks > 983040 ? "640034" : macroblocks > 522240 ? "640033" :
                        macroblocks > 108000 ? "64002A" : "64001F";
    snprintf(buffer, size, "video/mp4; codecs=\"avc1.%s\"", level);
    return buffer;
}
