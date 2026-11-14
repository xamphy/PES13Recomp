// Launch menu for Windows: a custom-drawn window (GDI+) shown before the game window - the
// generated stadium background, the title and Play Game / Settings / About. Mouse, keyboard
// (arrows, Enter, Esc) and an Xbox controller (D-pad, A, B). Same pages as the iPad menu
// (video/launcher_ios.mm); settings, background and About contents: launcher_core.cpp.
#include "launcher_core.h"
#include "launcher_win32.h"

#include <windows.h>
#include <windowsx.h>
#include <objidl.h> // IStream / PROPID for gdiplus.h (WIN32_LEAN_AND_MEAN is defined project-wide)
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; } // gdiplus.h needs them (NOMINMAX)
#include <gdiplus.h>
#include <xinput.h>
#pragma comment(lib, "gdiplus.lib")

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{
    using namespace Gdiplus;

    enum class Page { Main, Settings, About };
    enum class Action { Play, OpenSettings, OpenAbout, Back, Upscaler, UpscaleHeight };
    struct Item { Action action; RectF rect; bool enabled = true; };

    struct State
    {
        HWND hwnd = nullptr;
        Page page = Page::Main;
        Launcher::Settings settings;
        Launcher::About about;
        std::vector<Item> items;      // the current page's buttons (rebuilt at every paint)
        int focus = 0;                // keyboard / controller selection
        int hover = -1;               // mouse
        bool play = false;
        std::unique_ptr<Bitmap> background;
        WORD padButtons = 0;
    } g;

    std::wstring Wide(const std::string& s)
    {
        std::wstring w(s.size(), L'\0');
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), int(w.size()));
        w.resize(std::max(n, 0));
        return w;
    }

    GraphicsPath* RoundRect(const RectF& r, float radius)
    {
        auto* p = new GraphicsPath();
        const float d = radius * 2;
        p->AddArc(r.X, r.Y, d, d, 180, 90);
        p->AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
        p->AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
        p->AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
        p->CloseFigure();
        return p;
    }

    void FillRound(Graphics& gr, const RectF& r, float radius, Color fill, Color border = Color(0, 0, 0, 0))
    {
        std::unique_ptr<GraphicsPath> p(RoundRect(r, radius));
        SolidBrush b(fill);
        gr.FillPath(&b, p.get());
        if (border.GetA())
        {
            Pen pen(border, 1.5f);
            gr.DrawPath(&pen, p.get());
        }
    }

    void Text(Graphics& gr, const std::wstring& s, float x, float y, float size, int style, Color color, float width = 0,
        StringAlignment align = StringAlignmentNear)
    {
        FontFamily family(L"Segoe UI");
        Font font(&family, size, style, UnitPixel);
        SolidBrush brush(color);
        StringFormat fmt;
        fmt.SetAlignment(align);
        if (width > 0)
            gr.DrawString(s.c_str(), -1, &font, RectF(x, y, width, size * 3), &fmt, &brush);
        else
            gr.DrawString(s.c_str(), -1, &font, PointF(x, y), &brush);
    }

    // A menu button: primary (blue) or translucent; highlighted when focused or hovered.
    void Button(Graphics& gr, int index, const std::wstring& label, const std::wstring& icon, bool primary)
    {
        const Item& it = g.items[index];
        const bool hot = index == g.focus || index == g.hover;
        const RectF& r = it.rect;
        Color fill = primary ? Color(hot ? 255 : 235, 42, 115, 242) : Color(hot ? 215 : 175, 14, 20, 34);
        FillRound(gr, r, 12, fill, hot ? Color(160, 255, 255, 255) : Color(50, 255, 255, 255));
        if (hot)
        {
            SolidBrush accent(Color(255, 255, 255, 255));
            gr.FillRectangle(&accent, r.X + 1, r.Y + 14, 4.0f, r.Height - 28);
        }
        Text(gr, icon, r.X + 24, r.Y + (r.Height - 26) / 2 - 2, 22, FontStyleRegular, Color(255, 255, 255, 255));
        Text(gr, label, r.X + 62, r.Y + (r.Height - 26) / 2 - 3, 24, FontStyleBold, Color(255, 255, 255, 255));
    }

    // A settings row: label, description and the value with < > arrows.
    void SettingRow(Graphics& gr, int index, const std::wstring& label, const std::wstring& detail, const std::wstring& value)
    {
        const Item& it = g.items[index];
        const bool hot = (index == g.focus || index == g.hover) && it.enabled;
        const RectF& r = it.rect;
        FillRound(gr, r, 12, Color(hot ? 215 : 180, 14, 20, 34), hot ? Color(150, 255, 255, 255) : Color(45, 255, 255, 255));
        const Color main = it.enabled ? Color(255, 255, 255, 255) : Color(110, 255, 255, 255);
        Text(gr, label, r.X + 24, r.Y + 14, 22, FontStyleBold, main);
        Text(gr, detail, r.X + 24, r.Y + 46, 15, FontStyleRegular, it.enabled ? Color(175, 220, 228, 240) : Color(90, 220, 228, 240), r.Width - 300);
        const RectF pill(r.GetRight() - 236, r.Y + (r.Height - 44) / 2, 212, 44);
        FillRound(gr, pill, 22, it.enabled ? Color(235, 42, 115, 242) : Color(60, 255, 255, 255));
        Text(gr, L"‹   " + value + L"   ›", pill.X, pill.Y + 9, 20, FontStyleBold, main, pill.Width, StringAlignmentCenter);
    }

    void LayoutAndPaint(Graphics& gr, float w, float h)
    {
        gr.SetSmoothingMode(SmoothingModeAntiAlias);
        gr.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        gr.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        if (g.background) gr.DrawImage(g.background.get(), RectF(0, 0, w, h));
        {
            // Darker left side behind the menu.
            LinearGradientBrush shade(PointF(0, 0), PointF(w * 0.6f, 0), Color(170, 2, 4, 10), Color(0, 2, 4, 10));
            gr.FillRectangle(&shade, 0.0f, 0.0f, w * 0.6f + 1, h);
        }
        const float s = std::min(w / 1280.0f, h / 720.0f); // layout designed for 1280x720
        gr.ScaleTransform(s, s);
        const float vw = w / s, vh = h / s;
        g.items.clear();
        const float x = 80;

        if (g.page == Page::Main)
        {
            Text(gr, L"PES 2013", x - 4, 92, 84, FontStyleBold, Color(255, 255, 255, 255));
            Text(gr, Wide(g.about.subtitle), x, 196, 22, FontStyleRegular, Color(200, 210, 225, 245));
            SolidBrush accent(Color(255, 42, 115, 242));
            gr.FillRectangle(&accent, x, 236.0f, 64.0f, 4.0f);
            const wchar_t* labels[] = { L"Play Game", L"Settings", L"About" };
            const wchar_t* icons[] = { L"▶", L"⚙", L"ⓘ" };
            const Action actions[] = { Action::Play, Action::OpenSettings, Action::OpenAbout };
            for (int i = 0; i < 3; ++i)
                g.items.push_back({ actions[i], RectF(x, 290 + i * 84.0f, 380, 68) });
            for (int i = 0; i < 3; ++i)
                Button(gr, i, labels[i], icons[i], i == 0);
            const bool ok = g.about.xexFound && g.about.filesFound;
            Text(gr, (ok ? L"✓  Game files found" : L"⚠  " + Wide(g.about.gameFiles)), x, vh - 92, 17, FontStyleRegular,
                ok ? Color(220, 120, 220, 140) : Color(255, 255, 190, 90));
            Text(gr, L"Build " + Wide(g.about.build[0].value), x, vh - 64, 15, FontStyleRegular, Color(140, 220, 228, 240));
        }
        else if (g.page == Page::Settings)
        {
            Text(gr, L"Settings", x - 2, 70, 52, FontStyleBold, Color(255, 255, 255, 255));
            Text(gr, L"DISPLAY", x, 160, 14, FontStyleBold, Color(220, 140, 175, 255));
            const float rw = std::min(vw - 2 * x, 900.0f);
            g.items.push_back({ Action::Upscaler, RectF(x, 190, rw, 96) });
            g.items.push_back({ Action::UpscaleHeight, RectF(x, 300, rw, 96), g.settings.upscaler != 0 });
            g.items.push_back({ Action::Back, RectF(vw - x - 200, 80, 200, 56) });
            SettingRow(gr, 0, L"Upscaling", L"Sharper picture when the window is larger than 1280x720. FSR 1 is the sharpest, "
                L"SGSR 1 is lighter on the GPU.", Wide(Launcher::kUpscalerNames[g.settings.upscaler]));
            SettingRow(gr, 1, L"Upscaling resolution", L"Size the upscaler renders at before the picture is fitted to the "
                L"window. Lower sizes cost less GPU time.", Wide(Launcher::kUpscaleHeightNames[g.settings.upscaleHeight]));
            Button(gr, 2, L"Back", L"←", false);
        }
        else
        {
            Text(gr, L"About", x - 2, 56, 52, FontStyleBold, Color(255, 255, 255, 255));
            g.items.push_back({ Action::Back, RectF(vw - x - 200, 66, 200, 56) });
            const float pw = std::min(vw - 2 * x, 1120.0f);
            const float col2 = x + 300;
            const float lineBuild = 25, lineCredit = 21;
            const float panelH = 20 + 24 + lineBuild * (g.about.build.size() + 1) + 18 + 24 + lineCredit * g.about.credits.size() + 16 + 44;
            FillRound(gr, RectF(x, 140, pw, panelH), 16, Color(205, 8, 12, 22), Color(50, 255, 255, 255));
            float y = 160;
            Text(gr, L"BUILD", x + 24, y, 14, FontStyleBold, Color(200, 140, 175, 255));
            y += 24;
            for (const auto& l : g.about.build)
            {
                Text(gr, Wide(l.label), x + 24, y, 16, FontStyleRegular, Color(170, 220, 228, 240));
                Text(gr, Wide(l.value), col2, y, 16, FontStyleRegular, Color(255, 255, 255, 255));
                y += lineBuild;
            }
            const bool ok = g.about.xexFound && g.about.filesFound;
            Text(gr, L"Game files", x + 24, y, 16, FontStyleRegular, Color(170, 220, 228, 240));
            Text(gr, (ok ? L"✓  " : L"✗  ") + Wide(g.about.gameFiles), col2, y, 16, FontStyleRegular,
                ok ? Color(255, 120, 220, 140) : Color(255, 255, 120, 110));
            y += lineBuild + 18;
            Text(gr, L"CREDITS", x + 24, y, 14, FontStyleBold, Color(200, 140, 175, 255));
            y += 24;
            for (const auto& l : g.about.credits)
            {
                Text(gr, Wide(l.label), x + 24, y, 15, FontStyleRegular, Color(255, 255, 255, 255));
                Text(gr, Wide(l.value), col2, y, 15, FontStyleRegular, Color(170, 220, 228, 240));
                y += lineCredit;
            }
            y += 16;
            Text(gr, Wide(g.about.disclaimer), x + 24, y, 14, FontStyleItalic, Color(170, 220, 228, 240), pw - 48);
            Button(gr, 0, L"Back", L"←", false);
        }
        // Item rectangles are in layout units; store them in window pixels for hit testing.
        for (Item& it : g.items) it.rect = RectF(it.rect.X * s, it.rect.Y * s, it.rect.Width * s, it.rect.Height * s);
    }

    void Redraw() { InvalidateRect(g.hwnd, nullptr, FALSE); }

    void Open(Page p)
    {
        g.page = p;
        g.focus = 0;
        g.hover = -1;
        Redraw();
    }

    void Activate(int index, int direction = 1)
    {
        if (index < 0 || index >= int(g.items.size()) || !g.items[index].enabled) return;
        switch (g.items[index].action)
        {
        case Action::Play:
            g.play = true;
            DestroyWindow(g.hwnd);
            return;
        case Action::OpenSettings: Open(Page::Settings); return;
        case Action::OpenAbout: Open(Page::About); return;
        case Action::Back: Launcher::SaveSettings(g.settings); Open(Page::Main); return;
        case Action::Upscaler:
            g.settings.upscaler = (g.settings.upscaler + direction + Launcher::kUpscalerCount) % Launcher::kUpscalerCount;
            break;
        case Action::UpscaleHeight:
            g.settings.upscaleHeight = (g.settings.upscaleHeight + direction + Launcher::kUpscaleHeightCount) % Launcher::kUpscaleHeightCount;
            break;
        }
        Launcher::SaveSettings(g.settings);
        Redraw();
    }

    void Key(WPARAM key)
    {
        const int n = int(g.items.size());
        if (!n) return;
        auto move = [&](int d) {
            for (int i = 0; i < n; ++i)
            {
                g.focus = (g.focus + d + n) % n;
                if (g.items[g.focus].enabled) break;
            }
            g.hover = -1;
            Redraw();
        };
        switch (key)
        {
        case VK_UP: move(-1); break;
        case VK_DOWN: case VK_TAB: move(1); break;
        case VK_LEFT: if (g.items[g.focus].action == Action::Upscaler || g.items[g.focus].action == Action::UpscaleHeight) Activate(g.focus, -1); break;
        case VK_RIGHT: if (g.items[g.focus].action == Action::Upscaler || g.items[g.focus].action == Action::UpscaleHeight) Activate(g.focus, 1); break;
        case VK_RETURN: case VK_SPACE: Activate(g.focus); break;
        case VK_ESCAPE: case VK_BACK:
            if (g.page != Page::Main) { Launcher::SaveSettings(g.settings); Open(Page::Main); }
            break;
        }
    }

    // Xbox controller: D-pad / left stick, A, B (edge-triggered), through XInput if present.
    void PollPad()
    {
        static auto getState = [] {
            for (const wchar_t* dll : { L"xinput1_4.dll", L"xinput9_1_0.dll" })
                if (HMODULE m = LoadLibraryW(dll))
                    return reinterpret_cast<DWORD(WINAPI*)(DWORD, XINPUT_STATE*)>(GetProcAddress(m, "XInputGetState"));
            return static_cast<DWORD(WINAPI*)(DWORD, XINPUT_STATE*)>(nullptr);
        }();
        if (!getState) return;
        XINPUT_STATE st{};
        if (getState(0, &st) != ERROR_SUCCESS) return;
        WORD b = st.Gamepad.wButtons;
        if (st.Gamepad.sThumbLY > 16000) b |= XINPUT_GAMEPAD_DPAD_UP;
        if (st.Gamepad.sThumbLY < -16000) b |= XINPUT_GAMEPAD_DPAD_DOWN;
        if (st.Gamepad.sThumbLX > 16000) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
        if (st.Gamepad.sThumbLX < -16000) b |= XINPUT_GAMEPAD_DPAD_LEFT;
        const WORD pressed = b & ~g.padButtons;
        g.padButtons = b;
        if (pressed & XINPUT_GAMEPAD_DPAD_UP) Key(VK_UP);
        if (pressed & XINPUT_GAMEPAD_DPAD_DOWN) Key(VK_DOWN);
        if (pressed & XINPUT_GAMEPAD_DPAD_LEFT) Key(VK_LEFT);
        if (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) Key(VK_RIGHT);
        if (pressed & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START)) Key(VK_RETURN);
        if (pressed & XINPUT_GAMEPAD_B) Key(VK_ESCAPE);
    }

    int HitTest(int x, int y)
    {
        for (size_t i = 0; i < g.items.size(); ++i)
            if (g.items[i].rect.Contains(float(x), float(y)) && g.items[i].enabled) return int(i);
        return -1;
    }

    LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            const int w = std::max<int>(rc.right, 1), h = std::max<int>(rc.bottom, 1);
            Bitmap back(w, h, PixelFormat32bppPARGB);
            {
                Graphics gr(&back);
                LayoutAndPaint(gr, float(w), float(h));
            }
            Graphics screen(dc);
            screen.DrawImage(&back, 0, 0);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE: Redraw(); return 0;
        case WM_MOUSEMOVE:
        {
            const int hit = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (hit != g.hover) { g.hover = hit; if (hit >= 0) g.focus = hit; Redraw(); }
            SetCursor(LoadCursor(nullptr, hit >= 0 ? IDC_HAND : IDC_ARROW));
            return 0;
        }
        case WM_LBUTTONUP: Activate(HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp))); return 0;
        case WM_RBUTTONUP:
        {
            const int hit = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (hit >= 0 && (g.items[hit].action == Action::Upscaler || g.items[hit].action == Action::UpscaleHeight)) Activate(hit, -1);
            return 0;
        }
        case WM_KEYDOWN: Key(wp); return 0;
        case WM_TIMER: PollPad(); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

namespace Launcher
{
    bool ShouldShowWin32(int argc, char** argv)
    {
        for (int i = 1; i < argc; ++i)
            if (!std::strcmp(argv[i], "--no-launcher")) return false;
        // Automated runs (tools/nav): no menu.
        for (const char* v : { "PES_INPUT_SCRIPT", "PES_CONTROL", "PES_SHOTS", "PES_NO_LAUNCHER" })
            if (std::getenv(v)) return false;
        return true;
    }

    bool RunWin32()
    {
        ULONG_PTR token = 0;
        GdiplusStartupInput input;
        GdiplusStartup(&token, &input, nullptr);
        g.settings = LoadSettings();
        // GPU name for the About page: filled in later on the iPad; here the presenter is not up yet.
        g.about = GetAbout("Windows (x64)", "");
        {
            constexpr uint32_t bw = 1280, bh = 720;
            const std::vector<uint32_t> px = RenderBackground(bw, bh);
            g.background = std::make_unique<Bitmap>(int(bw), int(bh), PixelFormat32bppARGB);
            BitmapData data;
            Rect all(0, 0, int(bw), int(bh));
            g.background->LockBits(&all, ImageLockModeWrite, PixelFormat32bppARGB, &data);
            for (uint32_t y = 0; y < bh; ++y)
            {
                auto* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(data.Scan0) + y * data.Stride);
                for (uint32_t x = 0; x < bw; ++x)
                {
                    const uint32_t p = px[size_t(y) * bw + x]; // RGBA -> ARGB (BGRA in memory)
                    row[x] = (p & 0xFF00FF00u) | ((p & 0xFF) << 16) | ((p >> 16) & 0xFF);
                }
            }
            g.background->UnlockBits(&data);
        }
        // PES_LAUNCHER_SHOT=<folder>: render the three pages off-screen to PNGs and quit (tests).
        if (const char* shotDir = std::getenv("PES_LAUNCHER_SHOT"))
        {
            CLSID png{};
            UINT count = 0, size = 0;
            GetImageEncodersSize(&count, &size);
            std::vector<uint8_t> buf(size);
            auto* encoders = reinterpret_cast<ImageCodecInfo*>(buf.data());
            GetImageEncoders(count, size, encoders);
            for (UINT i = 0; i < count; ++i)
                if (!wcscmp(encoders[i].MimeType, L"image/png")) png = encoders[i].Clsid;
            const Page pages[] = { Page::Main, Page::Settings, Page::About };
            const wchar_t* names[] = { L"launcher_main.png", L"launcher_settings.png", L"launcher_about.png" };
            for (int i = 0; i < 3; ++i)
            {
                g.page = pages[i];
                g.focus = 0;
                Bitmap back(1280, 720, PixelFormat32bppARGB);
                {
                    Graphics gr(&back);
                    LayoutAndPaint(gr, 1280, 720);
                }
                back.Save((Wide(shotDir) + L"\\" + names[i]).c_str(), &png, nullptr);
            }
            g.background.reset();
            GdiplusShutdown(token);
            std::printf("[launcher] pages written to %s\n", shotDir);
            return false;
        }
        WNDCLASSW wc{};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        wc.lpszClassName = L"PES13Launcher";
        RegisterClassW(&wc);
        RECT r{ 0, 0, 1280, 720 };
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        const int ww = r.right - r.left, wh = r.bottom - r.top;
        g.hwnd = CreateWindowExW(0, wc.lpszClassName, L"PES 2013", WS_OVERLAPPEDWINDOW,
            (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww, wh,
            nullptr, nullptr, wc.hInstance, nullptr);
        ShowWindow(g.hwnd, SW_SHOW);
        SetTimer(g.hwnd, 1, 16, nullptr);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        g.background.reset();
        GdiplusShutdown(token);
        if (!g.play) return false;
        SaveSettings(g.settings);
        SetCurrent(g.settings);
        ApplySettings(g.settings);
        return true;
    }
}
