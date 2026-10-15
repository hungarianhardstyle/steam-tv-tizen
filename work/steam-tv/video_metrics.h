#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct TVTimingHistogram {
    uint32_t bins[128], count, invalid;
    double sum, max;
} TVTimingHistogram;

static inline void TVTimingAdd(TVTimingHistogram *h, double ms) {
    if (ms < 0 || ms != ms || ms > 1000000) { h->invalid++; return; }
    unsigned bin = ms >= 127 ? 127 : (unsigned)ms;
    h->bins[bin]++; h->count++; h->sum += ms;
    if (ms > h->max) h->max = ms;
}
static inline double TVTimingAverage(const TVTimingHistogram *h) { return h->count ? h->sum / h->count : 0; }
/* One-ms buckets yield an upper bound; the overflow bucket uses the observed maximum. */
static inline double TVTimingP95Upper(const TVTimingHistogram *h) {
    uint32_t rank = (h->count * 95u + 99u) / 100u, sum = 0;
    if (!rank) return 0;
    for (unsigned i = 0; i < 128; i++) {
        sum += h->bins[i];
        if (sum >= rank) return i == 127 || h->max < i + 1.0 ? h->max : i + 1.0;
    }
    return h->max;
}
static inline double TVTimestampDeltaMs(uint32_t before, uint32_t after) {
    return (int32_t)(after - before) * (1000.0 / 65536.0);
}

enum { TVSliceP = 1, TVSliceB = 2, TVSliceI = 4, TVSliceUnknown = 8 };

static inline unsigned TVAnnexBPrefix(const uint8_t *p, size_t n, size_t i) {
    if (i + 3 > n || p[i] || p[i + 1]) return 0;
    if (p[i + 2] == 1) return 3;
    return i + 4 <= n && !p[i + 2] && p[i + 3] == 1 ? 4 : 0;
}
static inline int TVReadUnsignedGolomb(const uint8_t *p, size_t n, size_t *bit, uint32_t *value) {
    unsigned zeros = 0;
    while (*bit < n * 8 && !((p[*bit / 8] >> (7 - *bit % 8)) & 1)) {
        ++*bit;
        if (++zeros >= 31) return 0;
    }
    if (*bit >= n * 8) return 0;
    ++*bit;
    uint32_t suffix = 0;
    for (unsigned i = 0; i < zeros; i++) {
        if (*bit >= n * 8) return 0;
        suffix = (suffix << 1) | ((p[*bit / 8] >> (7 - *bit % 8)) & 1);
        ++*bit;
    }
    *value = ((1u << zeros) - 1u) + suffix;
    return 1;
}
/* Inspect only the first VCL slice header, not POC or decoder output order.
 * At most 4 KiB of leading metadata and 32 header bytes are examined. Anything
 * truncated, unsupported, or beyond that budget is explicitly unknown. */
static inline unsigned TVH264FirstSliceType(const uint8_t *data, size_t size) {
    if (!data || !size) return TVSliceUnknown;
    size_t limit = size > 4096 ? 4096 : size;
    for (size_t i = 0; i + 3 <= limit; i++) {
        unsigned prefix = TVAnnexBPrefix(data, limit, i);
        if (!prefix) continue;
        size_t nal = i + prefix;
        if (nal >= size) return TVSliceUnknown;
        unsigned type = data[nal] & 31;
        if (data[nal] & 0x80) return TVSliceUnknown;
        if (type >= 2 && type <= 4) return TVSliceUnknown;
        if (type != 1 && type != 5) { i = nal; continue; }
        uint8_t rbsp[16]; size_t bytes = 0, bit = 0; unsigned zeros = 0;
        size_t end = size - nal > 33 ? nal + 33 : size;
        for (size_t j = nal + 1; j < end && bytes < sizeof(rbsp); j++) {
            if (TVAnnexBPrefix(data, size, j)) break;
            uint8_t byte = data[j];
            if (zeros >= 2 && byte == 3) { zeros = 0; continue; }
            rbsp[bytes++] = byte;
            zeros = byte == 0 ? zeros + 1 : 0;
        }
        uint32_t firstMb, slice;
        if (!TVReadUnsignedGolomb(rbsp, bytes, &bit, &firstMb) ||
            !TVReadUnsignedGolomb(rbsp, bytes, &bit, &slice) || slice > 9)
            return TVSliceUnknown;
        return slice % 5 == 0 ? TVSliceP : slice % 5 == 1 ? TVSliceB : slice % 5 == 2 ? TVSliceI : TVSliceUnknown;
    }
    return TVSliceUnknown;
}

typedef struct TVVideoTimingWindow {
    TVTimingHistogram callbackInterval, configWait, readyToCallback, readyToSdk;
    TVTimingHistogram sdkCall, appendInterval, preSdk, relock, ptsInterval, duration;
    uint32_t callbacks, appends, ptsNonmonotonic, idNonmonotonic, idGaps;
    uint32_t sliceP, sliceB, sliceI, sliceUnknown;
    double lastPts, lastDts, lastDuration;
} TVVideoTimingWindow;

typedef struct TVVideoTiming {
    TVVideoTimingWindow window;
    uint32_t lastCallback, lastAppend;
    uint16_t lastFrameId;
    int haveCallback, haveAppend, haveFrameId;
} TVVideoTiming;
