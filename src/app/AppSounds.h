#pragma once

#include <vector>
#include <cstdint>

namespace TalkMe {

/// In-app join/leave voice channel sounds (WAV, generated at startup).
/// Discord-like: very soft, short tones so they are almost unnoticeable.
class AppSounds {
public:
    /// Generate join and leave WAV buffers. Call once at startup.
    void Generate();

    void PlayJoin() const;
    void PlayLeave() const;
    void PlayMessage() const;

private:
    std::vector<uint8_t> m_JoinSound;
    std::vector<uint8_t> m_LeaveSound;
    std::vector<uint8_t> m_MessageSound;
};

} // namespace TalkMe
