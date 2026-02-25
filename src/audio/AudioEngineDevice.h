#pragma once

#include "AudioEngine.h"
#include "../../vendor/miniaudio.h"
#include <vector>

namespace TalkMe {

// Fills cached input/output device lists using miniaudio context. Sets *deviceListDirty = false.
void RefreshDeviceList(std::vector<AudioDeviceInfo>* cachedInput,
    std::vector<AudioDeviceInfo>* cachedOutput,
    bool* deviceListDirty);

// Stops current device if started, enumerates devices, builds config, inits and starts device.
// Fills *config (sampleRate, channels, format, periodSizeInFrames, dataCallback, pUserData, device IDs).
// Returns true on success and sets *deviceStarted = true, *deviceListDirty = true.
bool ReinitDeviceImpl(ma_device* device,
    ma_device_config* config,
    bool* deviceStarted,
    bool* deviceListDirty,
    int inputIdx,
    int outputIdx,
    int sampleRate,
    int frameSize,
    ma_device_data_proc dataCallback,
    void* userData);

} // namespace TalkMe
