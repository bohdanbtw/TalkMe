#define MINIAUDIO_IMPLEMENTATION
#include "../../vendor/miniaudio.h"

#include "AudioEngineDevice.h"

namespace TalkMe {

    void RefreshDeviceList(std::vector<AudioDeviceInfo>* cachedInput,
        std::vector<AudioDeviceInfo>* cachedOutput,
        bool* deviceListDirty)
    {
        if (!cachedInput || !cachedOutput || !deviceListDirty || !*deviceListDirty)
            return;

        ma_context ctx;
        if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS)
            return;

        ma_device_info* pCapture = nullptr;
        ma_uint32 captureCount = 0;
        ma_device_info* pPlayback = nullptr;
        ma_uint32 playbackCount = 0;
        if (ma_context_get_devices(&ctx, &pPlayback, &playbackCount,
            &pCapture, &captureCount) == MA_SUCCESS)
        {
            cachedInput->clear();
            for (ma_uint32 i = 0; i < captureCount; i++) {
                AudioDeviceInfo d;
                d.name = pCapture[i].name;
                d.index = static_cast<int>(i);
                d.isDefault = (pCapture[i].isDefault != 0);
                cachedInput->push_back(d);
            }
            cachedOutput->clear();
            for (ma_uint32 i = 0; i < playbackCount; i++) {
                AudioDeviceInfo d;
                d.name = pPlayback[i].name;
                d.index = static_cast<int>(i);
                d.isDefault = (pPlayback[i].isDefault != 0);
                cachedOutput->push_back(d);
            }
        }
        ma_context_uninit(&ctx);
        *deviceListDirty = false;
    }

    bool ReinitDeviceImpl(ma_device* device,
        ma_device_config* config,
        bool* deviceStarted,
        bool* deviceListDirty,
        int inputIdx,
        int outputIdx,
        int sampleRate,
        int frameSize,
        ma_device_data_proc dataCallback,
        void* userData)
    {
        if (!device || !config || !deviceStarted || !deviceListDirty)
            return false;

        if (*deviceStarted) {
            ma_device_stop(device);
            ma_device_uninit(device);
            *deviceStarted = false;
        }

        ma_context ctx;
        if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS)
            return false;

        ma_device_info* pCapture = nullptr;
        ma_uint32 captureCount = 0;
        ma_device_info* pPlayback = nullptr;
        ma_uint32 playbackCount = 0;
        bool gotDevices = ma_context_get_devices(&ctx, &pPlayback, &playbackCount,
            &pCapture, &captureCount) == MA_SUCCESS;

        ma_device_id captureId{};
        ma_device_id playbackId{};
        ma_device_id* pCaptureId = nullptr;
        ma_device_id* pPlaybackId = nullptr;

        if (gotDevices) {
            if (inputIdx >= 0 && static_cast<ma_uint32>(inputIdx) < captureCount) {
                captureId = pCapture[inputIdx].id;
                pCaptureId = &captureId;
            }
            if (outputIdx >= 0 && static_cast<ma_uint32>(outputIdx) < playbackCount) {
                playbackId = pPlayback[outputIdx].id;
                pPlaybackId = &playbackId;
            }
        }
        ma_context_uninit(&ctx);

        *config = ma_device_config_init(ma_device_type_duplex);
        config->sampleRate = sampleRate;
        config->capture.channels = 1;
        config->playback.channels = 1;
        config->capture.format = ma_format_f32;
        config->playback.format = ma_format_f32;
        config->periodSizeInFrames = frameSize;
        config->dataCallback = dataCallback;
        config->pUserData = userData;
        config->capture.pDeviceID = pCaptureId;
        config->playback.pDeviceID = pPlaybackId;

        if (ma_device_init(nullptr, config, device) != MA_SUCCESS)
            return false;

        // Bug fix: pCaptureId / pPlaybackId point to stack-local ma_device_id copies that
        // are about to go out of scope. ma_device_init has already copied the IDs
        // internally. Null the pointers in *config so that nothing can later dereference
        // them through the stored config and trigger undefined behaviour.
        config->capture.pDeviceID = nullptr;
        config->playback.pDeviceID = nullptr;

        if (ma_device_start(device) == MA_SUCCESS) {
            *deviceStarted = true;
            *deviceListDirty = true;
            return true;
        }
        return false;
    }

} // namespace TalkMe