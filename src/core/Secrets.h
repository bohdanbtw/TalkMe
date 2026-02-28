#pragma once
#include <string>
#include <map>

namespace TalkMe {

/// Loads key=value pairs from secret/secrets file.
/// Path: %LOCALAPPDATA%\\TalkMe\\secret\\secrets, or exe_dir\\secret\\secrets if that exists.
/// Lines starting with # are ignored. Keys are case-sensitive.
namespace Secrets {
    /// Call once at startup (e.g. from Application::Initialize). Returns true if file was found.
    bool Load();
    /// Get a value by key. Empty if not found or Load() was not called / failed.
    std::string Get(const std::string& key);
    /// Convenience: get Klipy API key (key "klipy_api_key").
    std::string GetKlipyApiKey();
    /// Convenience: get server IP (key "server_ip").
    std::string GetServerIp();
} // namespace Secrets

} // namespace TalkMe
