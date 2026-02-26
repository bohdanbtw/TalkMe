#include "AudioEngineInternal.h"
#include <cstring>

namespace TalkMe {

    void EncodeThreadFunc(AudioInternal* internal) {
        if (!internal) return;
        try {
            float pcmBuffer[OPUS_FRAME_SIZE];

            internal->m_EncodeWorkBuffer.reserve(kOpusMaxPacket);
            internal->m_EncodeWorkBuffer.resize(kOpusMaxPacket);

            while (!internal->m_EncodeThreadShutdown.load(std::memory_order_relaxed)) {
                std::unique_lock<std::mutex> lock(internal->m_EncodeMutex);
                internal->m_EncodeCv.wait(lock, [internal] {
                    return internal->m_EncodeThreadShutdown.load(std::memory_order_relaxed) ||
                        ma_pcm_rb_available_read(&internal->captureRb) >= OPUS_FRAME_SIZE;
                    });
                if (internal->m_EncodeThreadShutdown.load(std::memory_order_relaxed)) break;
                lock.unlock();

                if (!internal->m_Engine || !internal->m_Engine->IsCaptureEnabled()) {
                    ma_uint32 available = ma_pcm_rb_available_read(&internal->captureRb);
                    while (available >= OPUS_FRAME_SIZE) {
                        void* pRead;
                        ma_uint32 chunk = OPUS_FRAME_SIZE;
                        if (ma_pcm_rb_acquire_read(&internal->captureRb, &chunk, &pRead) != MA_SUCCESS)
                            break;
                        // Bug fix: guard against a zero-length successful acquire (ring buffer
                        // wrap edge case), which would cause an infinite loop.
                        if (chunk == 0) break;
                        ma_pcm_rb_commit_read(&internal->captureRb, chunk);
                        // chunk may be less than OPUS_FRAME_SIZE at a wrap boundary; guard
                        // against unsigned underflow.
                        available = (chunk < available) ? (available - chunk) : 0;
                    }
                    continue;
                }

                void* pRead;
                ma_uint32 framesRead = OPUS_FRAME_SIZE;
                if (ma_pcm_rb_acquire_read(&internal->captureRb, &framesRead, &pRead) != MA_SUCCESS)
                    continue;
                // Bug fix: ma_pcm_rb_acquire_read may return fewer frames than requested at a
                // ring-buffer wrap boundary. Feeding a partial (and partially uninitialised)
                // buffer to Opus produces corrupted audio. Discard and wait for the next full
                // frame instead.
                if (framesRead < OPUS_FRAME_SIZE) {
                    ma_pcm_rb_commit_read(&internal->captureRb, framesRead);
                    continue;
                }
                std::memcpy(pcmBuffer, pRead, framesRead * sizeof(float));
                ma_pcm_rb_commit_read(&internal->captureRb, framesRead);

                // Mix system audio (loopback) into the mic buffer
                if (internal->systemAudioRbInit && ma_pcm_rb_available_read(&internal->systemAudioRb) >= OPUS_FRAME_SIZE) {
                    void* pSysRead = nullptr;
                    ma_uint32 sysFrames = OPUS_FRAME_SIZE;
                    if (ma_pcm_rb_acquire_read(&internal->systemAudioRb, &sysFrames, &pSysRead) == MA_SUCCESS && sysFrames > 0) {
                        const float vol = internal->systemAudioVolume.load(std::memory_order_relaxed);
                        const float* sysData = static_cast<const float*>(pSysRead);
                        for (ma_uint32 i = 0; i < sysFrames && i < OPUS_FRAME_SIZE; i++)
                            pcmBuffer[i] += sysData[i] * vol;
                        ma_pcm_rb_commit_read(&internal->systemAudioRb, sysFrames);
                    }
                }

                if (!internal->m_Engine->IsSelfMuted() && internal->encoder) {
                    int encodedBytes;
                    {
                        // Bug fix: was incorrectly locking m_EncodeMutex (the CV mutex), which
                        // blocked the audio callback from waking the encode thread during the
                        // entire encode call. m_EncoderMutex is the correct guard for encoder
                        // access.
                        std::lock_guard<std::mutex> lock(internal->m_EncoderMutex);
                        encodedBytes =
                            internal->encoder->EncodeInto(pcmBuffer, internal->m_EncodeWorkBuffer);
                    }
                    if (encodedBytes > 0 && internal->onMicData) {
                        try {
                            internal->onMicData(internal->m_EncodeWorkBuffer, internal->outgoingSeqNum++);
                        }
                        catch (...) {
                            // Never let a bad callback kill the encode thread.
                        }
                    }
                }
            }
        }
        catch (...) {
            // Do not let any exception escape the encode thread — would call std::terminate().
        }
    }

} // namespace TalkMe