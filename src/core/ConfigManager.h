#pragma once
#include <string>
#include <fstream>
#include <sstream>
#include <windows.h>
#include <shlobj.h>
#include <nlohmann/json.hpp>
#include <wincrypt.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "crypt32.lib")

namespace TalkMe {
    struct UserSession {
        bool isLoggedIn = false;
        std::string email;
        std::string username;
        std::string password;
    };

    class ConfigManager {
    public:
        static ConfigManager& Get() { static ConfigManager instance; return instance; }

        UserSession LoadSession() {
            UserSession session;
            std::string path = GetConfigPath();
            std::ifstream file(path, std::ios::binary);
            if (file.is_open()) {
                std::stringstream buffer;
                buffer << file.rdbuf();
                std::string data;
                if (!UnprotectData(buffer.str(), data)) {
                    // Fallback to legacy obfuscation
                    data = Obfuscate(buffer.str());
                }
                try {
                    auto j = nlohmann::json::parse(data);
                    session.email = j.value("e", "");
                    session.password = j.value("p", "");
                    session.isLoggedIn = true;
                }
                catch (...) {}
            }
            return session;
        }

        void SaveSession(const std::string& email, const std::string& password) {
            nlohmann::json j;
            j["e"] = email;
            j["p"] = password;
            std::string plain = j.dump();

            std::string out;
            if (!ProtectData(plain, out)) {
                out = Obfuscate(plain); // fallback
            }

            std::string dir = GetConfigDir();
            CreateDirectoryA(dir.c_str(), NULL);

            std::ofstream file(GetConfigPath(), std::ios::binary | std::ios::trunc);
            if (file.is_open()) {
                file.write(out.data(), out.size());
            }
        }

        void ClearSession() {
            DeleteFileA(GetConfigPath().c_str());
        }

        void SaveTheme(int themeId) {
            std::string dir = GetConfigDir();
            CreateDirectoryA(dir.c_str(), NULL);
            std::ofstream f(GetConfigDir() + "\\theme.cfg", std::ios::trunc);
            if (f.is_open()) f << themeId;
        }

        int LoadTheme(int defaultId = 0) {
            std::ifstream f(GetConfigDir() + "\\theme.cfg");
            int id = defaultId;
            if (f.is_open()) f >> id;
            return id;
        }

        void SaveKeybinds(const std::vector<int>& muteMicKeys, const std::vector<int>& deafenKeys) {
            std::string dir = GetConfigDir();
            CreateDirectoryA(dir.c_str(), NULL);
            nlohmann::json j;
            j["mute_mic"] = muteMicKeys;
            j["deafen"] = deafenKeys;
            std::ofstream f(dir + "\\keybinds.cfg", std::ios::trunc);
            if (f.is_open()) f << j.dump();
        }

        void LoadKeybinds(std::vector<int>& muteMicKeys, std::vector<int>& deafenKeys) {
            std::ifstream f(GetConfigDir() + "\\keybinds.cfg");
            if (f.is_open()) {
                try {
                    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                    auto j = nlohmann::json::parse(s);
                    if (j.contains("mute_mic")) {
                        if (j["mute_mic"].is_array())
                            muteMicKeys = j["mute_mic"].get<std::vector<int>>();
                        else if (j["mute_mic"].is_number()) {
                            int k = j["mute_mic"].get<int>();
                            if (k != 0) muteMicKeys = { k };
                        }
                    }
                    if (j.contains("deafen")) {
                        if (j["deafen"].is_array())
                            deafenKeys = j["deafen"].get<std::vector<int>>();
                        else if (j["deafen"].is_number()) {
                            int k = j["deafen"].get<int>();
                            if (k != 0) deafenKeys = { k };
                        }
                    }
                } catch (...) {}
            }
        }

        void SaveOverlay(bool enabled, int corner, float opacity) {
            std::string dir = GetConfigDir();
            CreateDirectoryA(dir.c_str(), NULL);
            nlohmann::json j;
            j["enabled"] = enabled;
            j["corner"] = corner;
            j["opacity"] = opacity;
            std::ofstream f(dir + "\\overlay.cfg", std::ios::trunc);
            if (f.is_open()) f << j.dump();
        }

        void LoadOverlay(bool& enabled, int& corner, float& opacity) {
            std::ifstream f(GetConfigDir() + "\\overlay.cfg");
            if (f.is_open()) {
                try {
                    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                    auto j = nlohmann::json::parse(s);
                    enabled = j.value("enabled", false);
                    corner = j.value("corner", 1);
                    opacity = j.value("opacity", 0.85f);
                } catch (...) {}
            }
        }

        void SaveNoiseSuppressionMode(int mode) {
            std::string dir = GetConfigDir();
            CreateDirectoryA(dir.c_str(), NULL);
            std::ofstream f(dir + "\\noise_suppression.cfg", std::ios::trunc);
            if (f.is_open()) f << mode;
        }

        int LoadNoiseSuppressionMode(int defaultMode = 1) {
            std::ifstream f(GetConfigDir() + "\\noise_suppression.cfg");
            int mode = defaultMode;
            if (f.is_open()) {
                f >> mode;
                if (mode < 0 || mode > 3) mode = defaultMode;
            }
            return mode;
        }

        /// Returns persistent device ID (16-char hex). Generates and saves to config if missing.
        std::string GetDeviceId() {
            std::string path = GetConfigDirectory() + "\\device_id.cfg";
            std::ifstream f(path);
            std::string id;
            if (f.is_open()) {
                id.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                while (!id.empty() && (id.back() == '\r' || id.back() == '\n' || id.back() == ' ')) id.pop_back();
            }
            if (id.size() != 16) {
                uint8_t buf[8];
                HCRYPTPROV prov = 0;
                if (CryptAcquireContextA(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) && prov) {
                    CryptGenRandom(prov, 8, buf);
                    CryptReleaseContext(prov, 0);
                } else {
                    for (int i = 0; i < 8; ++i) buf[i] = (uint8_t)((GetTickCount() + i * 31) & 0xFF);
                }
                static const char hex[] = "0123456789abcdef";
                id.resize(16);
                for (int i = 0; i < 8; ++i) { id[i*2] = hex[buf[i]>>4]; id[i*2+1] = hex[buf[i]&0xF]; }
                CreateDirectoryA(GetConfigDirectory().c_str(), NULL);
                std::ofstream out(path, std::ios::trunc);
                if (out.is_open()) out << id;
            }
            return id;
        }

        /// Returns AppData\\Local\\TalkMe (same as session.dat folder). Use for imgui.ini etc.
        static std::string GetConfigDirectory() {
            char path[MAX_PATH];
            if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, path)))
                return std::string(path) + "\\TalkMe";
            return "TalkMe";
        }

    private:
        std::string GetConfigDir() {
            char path[MAX_PATH];
            if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, path))) {
                return std::string(path) + "\\TalkMe";
            }
            return "TalkMe"; // Fallback to local dir if AppData fails
        }

        std::string GetConfigPath() {
            return GetConfigDir() + "\\session.dat";
        }

        // Basic XOR Obfuscation (legacy fallback)
        std::string Obfuscate(const std::string& input) {
            std::string output = input;
            char key = 0x5A;
            for (size_t i = 0; i < output.size(); ++i) {
                output[i] ^= key;
            }
            return output;
        }

        bool ProtectData(const std::string& plain, std::string& out) {
            DATA_BLOB in; in.cbData = (DWORD)plain.size(); in.pbData = (BYTE*)plain.data();
            DATA_BLOB outBlob;
            if (CryptProtectData(&in, L"TalkMeSession", nullptr, nullptr, nullptr, 0, &outBlob)) {
                out.assign((char*)outBlob.pbData, outBlob.cbData);
                LocalFree(outBlob.pbData);
                return true;
            }
            return false;
        }

        bool UnprotectData(const std::string& in, std::string& plain) {
            DATA_BLOB inBlob; inBlob.cbData = (DWORD)in.size(); inBlob.pbData = (BYTE*)const_cast<char*>(in.data());
            DATA_BLOB outBlob;
            if (CryptUnprotectData(&inBlob, nullptr, nullptr, nullptr, nullptr, 0, &outBlob)) {
                plain.assign((char*)outBlob.pbData, outBlob.cbData);
                LocalFree(outBlob.pbData);
                return true;
            }
            return false;
        }
    };
}