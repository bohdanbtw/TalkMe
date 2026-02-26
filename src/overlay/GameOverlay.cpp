#ifndef NOMINMAX
#define NOMINMAX
#endif
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
        m_Corner = (std::min)((std::max)(corner, 0), 3);
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

        int headerH = 28;
        int totalH = headerH + m_Padding + (int)m_Members.size() * m_LineHeight + m_Padding;
        if (totalH < 50) totalH = 50;

        RECT workArea;
        SystemParametersInfo(SPI_GETWORKAREA, 0, &workArea, 0);
        int screenW = workArea.right - workArea.left;
        int screenH = workArea.bottom - workArea.top;
        int margin = 16;
        int x = 0, y = 0;

        switch (m_Corner) {
            case 0: x = workArea.left + margin;                      y = workArea.top + margin; break;
            case 1: x = workArea.left + screenW - m_Width - margin;  y = workArea.top + margin; break;
            case 2: x = workArea.left + margin;                      y = workArea.top + screenH - totalH - margin; break;
            case 3: x = workArea.left + screenW - m_Width - margin;  y = workArea.top + screenH - totalH - margin; break;
        }

        ::SetWindowPos(m_Hwnd, HWND_TOPMOST, x, y, m_Width, totalH,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    void GameOverlay::Repaint() {
        if (!m_Hwnd || m_Members.empty()) return;

        int headerH = 28;
        int totalH = headerH + m_Padding + (int)m_Members.size() * m_LineHeight + m_Padding;
        if (totalH < 50) totalH = 50;

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

            BYTE bgAlpha = (BYTE)(m_Opacity * 210.0f);

            // Rounded background
            Gdiplus::GraphicsPath bgPath;
            float r = 10.0f;
            float w = (float)m_Width;
            float h = (float)totalH;
            bgPath.AddArc(0.0f, 0.0f, r * 2, r * 2, 180, 90);
            bgPath.AddArc(w - r * 2, 0.0f, r * 2, r * 2, 270, 90);
            bgPath.AddArc(w - r * 2, h - r * 2, r * 2, r * 2, 0, 90);
            bgPath.AddArc(0.0f, h - r * 2, r * 2, r * 2, 90, 90);
            bgPath.CloseFigure();

            Gdiplus::SolidBrush bgBrush(Gdiplus::Color(bgAlpha, 18, 18, 22));
            gfx.FillPath(&bgBrush, &bgPath);

            // Subtle border
            Gdiplus::Pen borderPen(Gdiplus::Color((BYTE)(bgAlpha * 0.5f), 80, 80, 90), 1.0f);
            gfx.DrawPath(&borderPen, &bgPath);

            // Header bar with accent
            Gdiplus::SolidBrush headerBrush(Gdiplus::Color((BYTE)(bgAlpha * 0.6f), 40, 42, 48));
            Gdiplus::GraphicsPath headerPath;
            headerPath.AddArc(0.0f, 0.0f, r * 2, r * 2, 180, 90);
            headerPath.AddArc(w - r * 2, 0.0f, r * 2, r * 2, 270, 90);
            headerPath.AddLine(w, (float)headerH, 0.0f, (float)headerH);
            headerPath.CloseFigure();
            gfx.FillPath(&headerBrush, &headerPath);

            // Header text
            Gdiplus::Font headerFont(L"Segoe UI Semibold", 10.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::SolidBrush headerTextBrush(Gdiplus::Color(200, 130, 140, 255));
            wchar_t headerText[64];
            _snwprintf_s(headerText, _countof(headerText), _TRUNCATE,
                L"TALKME  \x2022  %d user%s",
                (int)m_Members.size(),
                m_Members.size() == 1 ? L"" : L"s");
            Gdiplus::RectF headerRect(10.0f, 6.0f, w - 20.0f, (float)headerH - 6.0f);
            gfx.DrawString(headerText, -1, &headerFont, headerRect, nullptr, &headerTextBrush);

            // Member list
            Gdiplus::Font nameFont(L"Segoe UI", 12.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::Font iconFont(L"Segoe UI Symbol", 11.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::StringFormat fmt;
            fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
            fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);

            for (int i = 0; i < (int)m_Members.size(); i++) {
                const auto& m = m_Members[i];
                float yPos = (float)(headerH + m_Padding + i * m_LineHeight);

                // Speaking indicator: bright green circle with glow; idle: dim gray dot
                float dotX = (float)m_Padding;
                float dotCY = yPos + (float)m_LineHeight * 0.5f;
                float dotR = m.isSpeaking ? 5.0f : 3.5f;

                if (m.isSpeaking) {
                    // Green glow behind the dot
                    Gdiplus::SolidBrush glowBrush(Gdiplus::Color(60, 80, 220, 100));
                    gfx.FillEllipse(&glowBrush, dotX - 3.0f, dotCY - dotR - 3.0f,
                        (dotR + 3.0f) * 2.0f, (dotR + 3.0f) * 2.0f);
                    Gdiplus::SolidBrush dotBrush(Gdiplus::Color(255, 80, 220, 100));
                    gfx.FillEllipse(&dotBrush, dotX, dotCY - dotR, dotR * 2.0f, dotR * 2.0f);
                } else if (m.isMuted || m.isDeafened) {
                    Gdiplus::SolidBrush dotBrush(Gdiplus::Color(180, 220, 70, 70));
                    gfx.FillEllipse(&dotBrush, dotX, dotCY - dotR, dotR * 2.0f, dotR * 2.0f);
                } else {
                    Gdiplus::SolidBrush dotBrush(Gdiplus::Color(120, 140, 140, 145));
                    gfx.FillEllipse(&dotBrush, dotX, dotCY - dotR, dotR * 2.0f, dotR * 2.0f);
                }

                // Name with proper UTF-8 conversion
                float textX = dotX + 16.0f;
                float maxNameW = w - textX - 44.0f;

                Gdiplus::Color nameColor(220, 220, 220);
                if (m.isSpeaking) nameColor = Gdiplus::Color(130, 240, 150);
                else if (m.isMuted || m.isDeafened) nameColor = Gdiplus::Color(160, 160, 165);

                Gdiplus::SolidBrush nameBrush(nameColor);
                Gdiplus::RectF nameRect(textX, yPos, maxNameW, (float)m_LineHeight);

                int wlen = MultiByteToWideChar(CP_UTF8, 0, m.name.c_str(), -1, nullptr, 0);
                std::wstring wname(wlen > 0 ? wlen - 1 : 0, L'\0');
                if (wlen > 1)
                    MultiByteToWideChar(CP_UTF8, 0, m.name.c_str(), -1, &wname[0], wlen);

                gfx.DrawString(wname.c_str(), -1, &nameFont, nameRect, &fmt, &nameBrush);

                // Status icons using Unicode symbols (right-aligned)
                float iconX = w - (float)m_Padding - 32.0f;
                if (m.isDeafened) {
                    Gdiplus::SolidBrush redBrush(Gdiplus::Color(230, 220, 70, 70));
                    gfx.DrawString(L"D", -1, &iconFont,
                        Gdiplus::RectF(iconX + 14.0f, yPos, 18.0f, (float)m_LineHeight),
                        &fmt, &redBrush);
                }
                if (m.isMuted) {
                    Gdiplus::SolidBrush orangeBrush(Gdiplus::Color(230, 240, 160, 60));
                    gfx.DrawString(L"M", -1, &iconFont,
                        Gdiplus::RectF(iconX, yPos, 18.0f, (float)m_LineHeight),
                        &fmt, &orangeBrush);
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
