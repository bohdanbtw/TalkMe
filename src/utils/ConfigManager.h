#pragma once
#include <string>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <ShlObj.h> // For APPDATA path

namespace TalkMe {

    struct UserSession {
        std::string username;
        std::string password; // In real prod, store a session token, never a raw password!
        bool isLoggedIn = false;
    };

    class ConfigManager {
    public:
        static ConfigManager& Get() {
            static ConfigManager instance;
            return instance;
        }

        UserSession LoadSession() {
            UserSession session;
            std::filesystem::path path = GetConfigPath();

            if (std::filesystem::exists(path)) {
                try {
                    std::ifstream file(path);
                    nlohmann::json j;
                    file >> j;

                    if (j.contains("username")) session.username = j["username"];
                    if (j.contains("password")) session.password = j["password"];
                    // Simple logic: if file exists and has data, we assume they are logged in
                    if (!session.username.empty() && !session.password.empty()) {
                        session.isLoggedIn = true;
                    }
                }
                catch (...) {}
            }
            return session;
        }

        void SaveSession(const std::string& username, const std::string& password) {
            nlohmann::json j;
            j["username"] = username;
            j["password"] = password;

            std::filesystem::path path = GetConfigPath();
            std::filesystem::create_directories(path.parent_path()); // Ensure folder exists

            std::ofstream file(path);
            file << j.dump(4);
        }

        void ClearSession() {
            std::filesystem::remove(GetConfigPath());
        }

    private:
        std::filesystem::path GetConfigPath() {
            char path[MAX_PATH];
            if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, path))) {
                return std::filesystem::path(path) / "TalkMe" / "config.json";
            }
            return "config.json"; // Fallback
        }
    };
}