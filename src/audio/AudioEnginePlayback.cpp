#include "AudioEnginePlayback.h"
#include "AudioEngineInternal.h"
#include "OpusCodec.h"
#include "../core/Logger.h"
#include <cstring>
#include <algorithm>

namespace TalkMe {

    void MixTracks(AudioInternal* internal, float* pOutputFloat, ma_uint32 frameCount) {
        std::memset(pOutputFloat, 0, frameCount * sizeof(float));
        if (internal->selfDeafened.load(std::memory_order_relaxed)) return;

        // Both mixBuffer and m_PerTrackBuffer are pre-allocated to 2x the device period.
        if (frameCount > static_cast<ma_uint32>(internal->mixBuffer.size()) ||
            frameCount > static_cast<ma_uint32>(internal->m_PerTrackBuffer.size()))
        {
            LOG_ERROR_BUF("MixTracks: frameCount exceeds pre-allocated buffer — skipping");
            return;
        }

        std::fill(internal->mixBuffer.begin(),
            internal->mixBuffer.begin() + frameCount, 0.0f);

        int activeCount = 0;

        {
            std::lock_guard<std::mutex> lk(internal->m_TracksMutex);

            for (auto& trackPtr : internal->tracks) {
                VoiceTrack* tr = trackPtr.get();
                if (!tr->active) continue;

                ma_uint32 available = ma_pcm_rb_available_read(&tr->rb);
                const int targetMs = internal->adaptiveBufferLevel;

                if (tr->isBuffering) {
                    int maxJitter = internal->maxJitterSpikeMsAtomic.load(
                        std::memory_order_relaxed);
                    int recoveryMs = (std::max)(40, (std::min)(maxJitter + 20, targetMs));
                    int thresholdMs = tr->coldStart ? targetMs : recoveryMs;

                    ma_uint32 requiredFrames = std::clamp(
                        static_cast<ma_uint32>((thresholdMs * SAMPLE_RATE) / 1000),
                        static_cast<ma_uint32>(OPUS_FRAME_SIZE),
                        static_cast<ma_uint32>(48000));

                    if (available < requiredFrames) continue;

                    tr->isBuffering = false;
                    tr->smoothedBufferLevelMs = (available * 1000.0) / SAMPLE_RATE;
                }
                else {
                    double availMs = (available * 1000.0) / SAMPLE_RATE;
                    tr->smoothedBufferLevelMs =
                        tr->smoothedBufferLevelMs * 0.933 + availMs * 0.067;

                    // Widen hysteresis: only shrink if we are severely over-buffered (prevents micro-drops)
                    if (tr->smoothedBufferLevelMs > targetMs + 100.0 &&
                        available >= static_cast<ma_uint32>(OPUS_FRAME_SIZE * 4))
                    {
                        void* pRead;
                        ma_uint32 chunk = OPUS_FRAME_SIZE;
                        if (ma_pcm_rb_acquire_read(&tr->rb, &chunk, &pRead) == MA_SUCCESS) {
                            ma_pcm_rb_commit_read(&tr->rb, chunk);
                            tr->smoothedBufferLevelMs -= 10.0;
                            available -= OPUS_FRAME_SIZE;
                        }
                    }
                    // Widen hysteresis: only pad silence if we are critically starving
                    else if (tr->smoothedBufferLevelMs < targetMs - 40.0 &&
                        available > 0 &&
                        available < static_cast<ma_uint32>(OPUS_FRAME_SIZE * 2))
                    {
                        tr->smoothedBufferLevelMs += 10.0;
                        continue;
                    }
                }

                if (available < frameCount) {
                    internal->bufferUnderruns.fetch_add(1, std::memory_order_relaxed);
                    tr->isBuffering = true;
                    tr->coldStart = false;
                    internal->adaptiveBufferLevel =
                        (std::min)(internal->adaptiveBufferLevel + 20, internal->maxBufferMs);
                    continue;
                }

                activeCount++;
                float* trackBuffer = internal->m_PerTrackBuffer.data();
                ma_uint32 read = 0;
                while (read < frameCount) {
                    ma_uint32 chunk = frameCount - read;
                    void* pRead;
                    if (ma_pcm_rb_acquire_read(&tr->rb, &chunk, &pRead) != MA_SUCCESS) break;
                    if (chunk == 0) break;
                    std::memcpy(trackBuffer + read, pRead, chunk * sizeof(float));
                    ma_pcm_rb_commit_read(&tr->rb, chunk);
                    read += chunk;
                }

                const float gain = tr->gain.load(std::memory_order_relaxed);
                for (ma_uint32 s = 0; s < read; ++s)
                    internal->mixBuffer[s] += trackBuffer[s] * gain;
            }
        }

        if (activeCount > 0) {
            const float mixRMS = ComputeRms(internal->mixBuffer.data(),
                static_cast<int>(frameCount));
            internal->targetGain =
                (mixRMS > 0.001f)
                ? (std::min)(0.5f / (mixRMS * std::sqrt(static_cast<float>(activeCount))), 1.0f)
                : 1.0f;
            internal->currentGain =
                internal->currentGain * 0.95f + internal->targetGain * 0.05f;
            const float g = internal->currentGain;
            for (ma_uint32 i = 0; i < frameCount; ++i)
                pOutputFloat[i] = SoftClip(internal->mixBuffer[i] * g);
        }
    }

} // namespace TalkMe