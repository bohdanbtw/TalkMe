#pragma once

#include "../../vendor/miniaudio.h"
#include <cstdint>

namespace TalkMe {

struct AudioInternal;

// Mix all active voice tracks into pOutputFloat (frameCount samples).
// Caller must hold no locks; MixTracks acquires m_TracksMutex internally.
// Zeros pOutputFloat, then writes mixed+gain+soft-clipped output.
// Mic-test overlay (adding capture buffer) is done by the caller after MixTracks.
void MixTracks(AudioInternal* internal, float* pOutputFloat, ma_uint32 frameCount);

} // namespace TalkMe
