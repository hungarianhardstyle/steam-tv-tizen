#pragma once
#include "samsung/wasm/elementary_media_track.h"
inline const char *TVDecodingModeName(samsung::wasm::ElementaryMediaTrack::ActiveDecodingMode mode) {
    using Mode = samsung::wasm::ElementaryMediaTrack::ActiveDecodingMode;
    return mode == Mode::kHardware ? "hardveres" : mode == Mode::kSoftware ? "szoftveres" : "ismeretlen";
}
