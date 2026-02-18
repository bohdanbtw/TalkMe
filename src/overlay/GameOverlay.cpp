#include "GameOverlay.h"
#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

namespace TalkMe {

    static const wchar_t* OVERLAY_CLASS = L"TalkMeOverlay";
    static ULONG_PTR s_GdipToken = 0;
    static int s_GdipRefCount = 0;

    static void InitGdiPlus() {
        if (s_GdipRefCount++ == 0) {
            Gdiplus::GdiplusStartupInput input;
            Gdiplus::GdiplusStartup(&s_GdipToken, &input, nullptr);
        }
    }

    static void ShutdownGdiPlus() {
        if (--s_GdipRefCount <= 0) {
            Gdiplus::GdiplusShutdown(s_GdipToken);
            s_GdipRefCount = 0;
        }
    }

    GameOverlay::GameOverlay() {}

    GameOverlay::~GameOverlay() {
        Destroy();
    }

    void GameOverlay::RegisterWindowClass(HINSTANCE hInstance) {
        if (m_ClassRegistered) return;
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hInstance;
        wc.lpszClassName = OVERLAY_CLASS;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        ::RegisterClassExW(&wc);
        m_ClassRegistered = true;
    }

    void GameOverlay::Create(HINSTANCE hInstance) {
        m_hInstance = hInstance;
        InitGdiPlus();
        RegisterWindowClass(hInstance);

        m_Hwnd = ::CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            OVERLAY_CLASS,
            L"TalkMe Overlay",
            WS_POPUP,
            0, 0, m_Width, 100,
            nullptr, nullptr, hInstance, nullptr
        );

        if (m_Hwnd) {
            ::SetWindowLongPtrW(m_Hwnd, GWLP_USERDATA, (LONG_PTR)this);
        }
    }

    void GameOverlay::Destroy() {
        if (m_Hwnd) {
            ::DestroyWindow(m_Hwnd);
            m_Hwnd = nullptr;
        }
        if (m_ClassRegistered && m_hInstance) {
            ::UnregisterClassW(OVERLAY_CLASS, m_hInstance);
            m_ClassRegistered = false;
        }
        ShutdownGdiPlus();
    }

    void GameOverlay::SetEnabled(bool enabled) {
        m_Enabled = enabled;
        if (!m_Hwnd) return;
        if (m_Enabled && !m_Members.empty()) {
            Repaint();
            ::ShowWindow(m_Hwnd, SW_SHOWNOACTIVATE);
        } else {
            ::ShowWindow(m_Hwnd, SW_HIDE);
        }
    }

    void GameOverlay::SetCorner(int corner) {
        m_Corner = corner;
        if (m_Enabled && m_Hwnd) Reposition();
    }

    void GameOverlay::SetOpacity(float opacity) {
        m_Opacity = opacity;
        if (m_Enabled && m_Hwnd) Repaint();
    }

    void GameOverlay::UpdateMembers(const std::vector<OverlayMember>& members) {
        m_Members = members;
        if (!m_Hwnd) return;
        if (m_Enabled && !m_Members.empty()) {
            Repaint();
            ::ShowWindow(m_Hwnd, SW_SHOWNOACTIVATE);
        } else if (m_Members.empty()) {
            ::ShowWindow(m_Hwnd, SW_HIDE);
        }
    }

    void GameOverlay::Reposition() {
        if (!m_Hwnd) return;

        int totalH = m_Padding * 2 + (int)m_Members.size() * m_LineHeight;
        if (totalH < 40) totalH = 40;

        RECT workArea;
        SystemParametersInfo(SPI_GETWORKAREA, 0, &workArea, 0);
        int screenW = workArea.right - workArea.left;
        int screenH = workArea.bottom - workArea.top;
        int margin = 16;
        int x = 0, y = 0;

        switch (m_Corner) {
            case 0: x = workArea.left + margin;                  y = workArea.top + margin; break;
            case 1: x = workArea.left + screenW - m_Width - margin; y = workArea.top + margin; break;
            case 2: x = workArea.left + margin;                  y = workArea.top + screenH - totalH - margin; break;
            case 3: x = workArea.left + screenW - m_Width - margin; y = workArea.top + screenH - totalH - margin; break;
        }

        ::SetWindowPos(m_Hwnd, HWND_TOPMOST, x, y, m_Width, totalH,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    void GameOverlay::Repaint() {
        if (!m_Hwnd || m_Members.empty()) return;

        int totalH = m_Padding * 2 + (int)m_Members.size() * m_LineHeight;
        if (totalH < 40) totalH = 40;

        Reposition();

        HDC screenDC = ::GetDC(nullptr);
        HDC memDC = ::CreateCompatibleDC(screenDC);

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = m_Width;
        bmi.bmiHeader.biHeight = -totalH;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP hBmp = ::CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HBITMAP oldBmp = (HBITMAP)::SelectObject(memDC, hBmp);

        {
            Gdiplus::Graphics gfx(memDC);
            gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            gfx.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

            BYTE bgAlpha = (BYTE)(m_Opacity * 200.0f);
            Gdiplus::SolidBrush bgBrush(Gdiplus::Color(bgAlpha, 24, 25, 28));
            Gdiplus::Pen borderPen(Gdiplus::Color(bgAlpha, 50, 52, 55), 1.0f);

            Gdiplus::RectF bgRect(0.0f, 0.0f, (float)m_Width, (float)totalH);
            gfx.FillRectangle(&bgBrush, bgRect);
            gfx.DrawRectangle(&borderPen, bgRect);

            Gdiplus::Font font(L"Segoe UI", 11.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::Font iconFont(L"Segoe UI", 10.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::StringFormat fmt;
            fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);

            for (int i = 0; i < (int)m_Members.size(); i++) {
                const auto& m = m_Members[i];
                float yPos = (float)(m_Padding + i * m_LineHeight);

                Gdiplus::Color nameColor(220, 220, 220);
                if (m.isSpeaking)
                    nameColor = Gdiplus::Color(100, 220, 120);

                // Status indicator
                float dotX = (float)m_Padding;
                float dotY = yPos + 6.0f;
                Gdiplus::Color dotColor(100, 220, 120);
                if (m.isMuted || m.isDeafened) dotColor = Gdiplus::Color(220, 80, 80);
                Gdiplus::SolidBrush dotBrush(dotColor);
                gfx.FillEllipse(&dotBrush, dotX, dotY, 8.0f, 8.0f);

                // Name
                float textX = dotX + 14.0f;
                float maxNameW = (float)(m_Width - (int)textX - m_Padding - 40);
                Gdiplus::SolidBrush nameBrush(nameColor);
                Gdiplus::RectF nameRect(textX, yPos + 2.0f, maxNameW, (float)m_LineHeight);

                std::wstring wname(m.name.begin(), m.name.end());
                gfx.DrawString(wname.c_str(), -1, &font, nameRect, &fmt, &nameBrush);

                // Mute/deafen icons
                float iconX = (float)(m_Width - m_Padding - 36);
                if (m.isDeafened) {
                    Gdiplus::SolidBrush redBrush(Gdiplus::Color(220, 80, 80));
                    gfx.DrawString(L"D", -1, &iconFont, Gdiplus::PointF(iconX + 16, yPos + 3.0f), &redBrush);
                }
                if (m.isMuted) {
                    Gdiplus::SolidBrush redBrush(Gdiplus::Color(220, 80, 80));
                    gfx.DrawString(L"M", -1, &iconFont, Gdiplus::PointF(iconX, yPos + 3.0f), &redBrush);
                }
            }
        }

        POINT ptSrc = { 0, 0 };
        SIZE sz = { m_Width, totalH };
        BLENDFUNCTION blend = {};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;

        POINT ptPos;
        RECT wr;
        ::GetWindowRect(m_Hwnd, &wr);
        ptPos.x = wr.left;
        ptPos.y = wr.top;

        ::UpdateLayeredWindow(m_Hwnd, screenDC, &ptPos, &sz, memDC, &ptSrc, 0, &blend, ULW_ALPHA);

        ::SelectObject(memDC, oldBmp);
        ::DeleteObject(hBmp);
        ::DeleteDC(memDC);
        ::ReleaseDC(nullptr, screenDC);
    }

    LRESULT CALLBACK GameOverlay::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        return ::DefWindowProcW(hWnd, msg, wParam, lParam);
    }

}
