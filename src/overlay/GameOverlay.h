#pragma once
#include <string>
#include <vector>
#include <windows.h>

namespace TalkMe {

    struct OverlayMember {
        std::string name;
        bool isMuted = false;
        bool isDeafened = false;
        bool isSpeaking = false;
    };

    class GameOverlay {
    public:
        GameOverlay();
        ~GameOverlay();

        void Create(HINSTANCE hInstance);
        void Destroy();

        void SetEnabled(bool enabled);
        bool IsEnabled() const { return m_Enabled; }

        void SetCorner(int corner);
        int GetCorner() const { return m_Corner; }

        void SetOpacity(float opacity);
        float GetOpacity() const { return m_Opacity; }

        void UpdateMembers(const std::vector<OverlayMember>& members);
        void Reposition();

    private:
        void Repaint();
        void RegisterWindowClass(HINSTANCE hInstance);
        static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

        HWND m_Hwnd = nullptr;
        HINSTANCE m_hInstance = nullptr;
        bool m_Enabled = false;
        bool m_ClassRegistered = false;
        int m_Corner = 1;       // 0=TL, 1=TR, 2=BL, 3=BR
        float m_Opacity = 0.85f;
        int m_Width = 220;
        int m_LineHeight = 24;
        int m_Padding = 10;
        std::vector<OverlayMember> m_Members;
    };

}
