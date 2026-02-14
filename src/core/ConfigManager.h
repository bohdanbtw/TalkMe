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