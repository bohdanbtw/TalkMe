#include "Secrets.h"
#include "ConfigManager.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

#ifdef _WIN32
#include <windows.h>
#endif

namespace TalkMe {

namespace Secrets {

namespace {
    std::map<std::string, std::string> s_Secrets;
    bool s_Loaded = false;

    std::string GetExeDirectory() {
        char path[MAX_PATH] = {};
        if (::GetModuleFileNameA(nullptr, path, MAX_PATH) == 0) return {};
        std::string p(path);
        size_t last = p.find_last_of("\\/");
        return (last != std::string::npos) ? p.substr(0, last) : p;
    }

    void ParseLine(const std::string& line) {
        size_t eq = line.find('=');
        if (eq == 0 || eq == std::string::npos) return;
        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        // Trim key (leading and trailing)
        while (!key.empty() && (std::isspace(static_cast<unsigned char>(key.front())))) key.erase(0, 1);
        while (!key.empty() && (std::isspace(static_cast<unsigned char>(key.back())))) key.pop_back();
        // Trim value (leading/trailing)
        while (!value.empty() && (std::isspace(static_cast<unsigned char>(value.front())))) value.erase(0, 1);
        while (!value.empty() && (std::isspace(static_cast<unsigned char>(value.back())))) value.pop_back();
        if (!key.empty())
            s_Secrets[key] = value;
    }

    void CopySecretsToLocalAppData(const std::string& content) {
        std::string configDir = ConfigManager::GetConfigDirectory();
        std::string secretDir = configDir + "\\secret";
        std::string destPath = secretDir + "\\secrets";
#ifdef _WIN32
        CreateDirectoryA(configDir.c_str(), NULL);
        CreateDirectoryA(secretDir.c_str(), NULL);
#endif
        std::ofstream out(destPath, std::ios::binary | std::ios::trunc);
        if (out.is_open())
            out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
} // namespace

bool Load() {
    if (s_Loaded) return !s_Secrets.empty();
    s_Loaded = true;
    const std::string localAppDataPath = ConfigManager::GetConfigDirectory() + "\\secret\\secrets";
    std::string path = localAppDataPath;
    std::ifstream f(path);
    if (!f.is_open()) {
        std::string exeDir = GetExeDirectory();
        if (!exeDir.empty()) {
            path = exeDir + "\\secret\\secrets";
            f.open(path);
            if (!f.is_open()) {
                path = exeDir + "\\..\\secret\\secrets";
                f.open(path);
            }
            if (!f.is_open()) {
                path = exeDir + "\\..\\..\\secret\\secrets";
                f.open(path);
            }
        }
    }
    if (!f.is_open()) return false;
    std::stringstream raw;
    raw << f.rdbuf();
    std::string content = raw.str();
    f.close();

    std::string line;
    std::istringstream iss(content);
    while (std::getline(iss, line)) {
        size_t start = 0;
        while (start < line.size() && (std::isspace(static_cast<unsigned char>(line[start])) || line[start] == '\r')) ++start;
        if (start >= line.size() || line[start] == '#') continue;
        ParseLine(line.substr(start));
    }

    if (s_Secrets.empty()) return false;

    if (path != localAppDataPath)
        CopySecretsToLocalAppData(content);

    return true;
}

std::string Get(const std::string& key) {
    auto it = s_Secrets.find(key);
    return (it != s_Secrets.end()) ? it->second : std::string();
}

std::string GetKlipyApiKey() {
    return Get("klipy_api_key");
}

std::string GetServerIp() {
    return Get("server_ip");
}

} // namespace Secrets
} // namespace TalkMe
