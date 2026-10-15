#pragma once
#include <stdint.h>
#include <string.h>

typedef struct AudioPcmBatch {
    int16_t data[960 * 2];
    double pts;
    int pending;
} AudioPcmBatch;

static inline int AudioPcmBatchCanJoin(const AudioPcmBatch *batch, int samples, double pts) {
    double delta = pts - batch->pts;
    return batch->pending && samples == 480 && delta > 0.009999 && delta < 0.010001;
}

// shortcut: pair 10 ms stereo packets; other durations are appended directly by the caller.
static inline int AudioPcmBatchPush(AudioPcmBatch *batch, const int16_t *pcm, double pts) {
    int joining = batch->pending;
    memcpy(batch->data + (joining ? 480 * 2 : 0), pcm, 480 * 2 * sizeof(int16_t));
    if (!joining) batch->pts = pts;
    batch->pending = !joining;
    return joining ? 960 : 0;
}
