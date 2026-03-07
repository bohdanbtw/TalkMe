#pragma once
#include <string>

namespace TalkMe {

/// Run the screen capture process (child): connect to the named pipe and run
/// DXGI capture loop, writing each frame as 4-byte length + payload. Exits when
/// pipe is closed or stopFlag is set. Returns exit code (0 = normal, non-zero = error).
int RunScreenCaptureProcess(const std::string& pipeName, int fps, int quality, int maxWidth, int maxHeight);

} // namespace TalkMe
