// Windows window for the presenter: Win32 window on its own message-pump thread.
// (Moved unchanged out of presenter.cpp.)
#include "window.h"
#include "presenter.h"

#ifndef WIN32_LEAN_AND_MEAN
#   define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#   define NOMINMAX
#endif
#include <windows.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <volk.h>

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace
{
    HWND g_hwnd = nullptr;

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_CLOSE)
        {
            std::printf("[video] window closed - exiting\n");
            std::fflush(stdout);
            std::_Exit(0);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void WindowThread(uint32_t width, uint32_t height, std::wstring title, std::mutex* m, std::condition_variable* cv)
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"PES13Window";
        RegisterClassW(&wc);

        RECT r{ 0, 0, LONG(width), LONG(height) };
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        HWND hwnd = CreateWindowW(wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
        {
            std::lock_guard lock(*m);
            g_hwnd = hwnd;
        }
        cv->notify_all();

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

namespace Video::Window
{
    bool Create(uint32_t width, uint32_t height, const char* title)
    {
        // Window on its own thread (message pump).
        std::mutex m;
        std::condition_variable cv;
        std::wstring wtitle(title, title + std::strlen(title));
        std::thread(WindowThread, width, height, wtitle, &m, &cv).detach();
        {
            std::unique_lock lock(m);
            cv.wait(lock, [] { return g_hwnd != nullptr; });
        }
        return true;
    }

    std::vector<const char*> InstanceExtensions()
    {
        return { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    }

    VkSurfaceKHR CreateSurface(VkInstance instance)
    {
        VkWin32SurfaceCreateInfoKHR sci{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
        sci.hinstance = GetModuleHandleW(nullptr);
        sci.hwnd = g_hwnd;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        const VkResult r = vkCreateWin32SurfaceKHR(instance, &sci, nullptr, &surface);
        if (r != VK_SUCCESS)
            std::fprintf(stderr, "[video] vkCreateWin32SurfaceKHR failed: %d\n", int(r));
        return surface;
    }

    bool HasFocus() { return g_hwnd && GetForegroundWindow() == g_hwnd; }

    void SetTitle(const std::string& title)
    {
        if (g_hwnd)
            SetWindowTextA(g_hwnd, title.c_str());
    }
}

namespace
{
    // Text prompt (Video::PromptText): a small window with a label, an edit box, OK and Cancel,
    // run with its own message loop on the calling thread.
    struct PromptState
    {
        bool done = false, ok = false;
    };

    LRESULT CALLBACK PromptProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto* state = reinterpret_cast<PromptState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg)
        {
        case WM_COMMAND:
            if (state && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL))
            {
                state->ok = LOWORD(wp) == IDOK;
                state->done = true;
                return 0;
            }
            break;
        case WM_CLOSE:
            if (state) state->done = true;
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

namespace Video
{
    // Windows: the game runs on the process main thread, as before.
    int RunMain(int (*appMain)(int, char**), int argc, char** argv) { return appMain(argc, argv); }

    bool PromptText(const std::u16string& title, const std::u16string& description, std::u16string& text, uint32_t maxChars)
    {
        static const bool registered = [] {
            WNDCLASSW wc{};
            wc.lpfnWndProc = PromptProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hbrBackground = GetSysColorBrush(COLOR_3DFACE);
            wc.lpszClassName = L"PES13Prompt";
            return RegisterClassW(&wc) != 0;
        }();
        if (!registered) return true; // keep the default text

        const std::wstring wtitle(title.begin(), title.end());
        const std::wstring wlabel = description.empty() ? wtitle : std::wstring(description.begin(), description.end());
        const std::wstring wtext(text.begin(), text.end());

        NONCLIENTMETRICSW ncm{ sizeof(ncm) };
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);

        // Client area 420x130, centred over the game window (or the screen).
        const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU, exStyle = WS_EX_DLGMODALFRAME | WS_EX_TOPMOST;
        RECT r{ 0, 0, 420, 130 };
        AdjustWindowRectEx(&r, style, FALSE, exStyle);
        const int w = r.right - r.left, h = r.bottom - r.top;
        RECT area;
        if (!g_hwnd || !GetWindowRect(g_hwnd, &area)) SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
        const int x = area.left + (area.right - area.left - w) / 2, y = area.top + (area.bottom - area.top - h) / 2;

        const HINSTANCE inst = GetModuleHandleW(nullptr);
        HWND dlg = CreateWindowExW(exStyle, L"PES13Prompt", wtitle.empty() ? L"Enter text" : wtitle.c_str(), style,
            x, y, w, h, nullptr, nullptr, inst, nullptr);
        if (!dlg) { DeleteObject(font); return true; }
        PromptState state;
        SetWindowLongPtrW(dlg, GWLP_USERDATA, LONG_PTR(&state));

        auto child = [&](const wchar_t* cls, const wchar_t* caption, DWORD s, int cx, int cy, int cw, int ch, int id, DWORD ex = 0) {
            HWND c = CreateWindowExW(ex, cls, caption, WS_CHILD | WS_VISIBLE | s, cx, cy, cw, ch, dlg, HMENU(INT_PTR(id)), inst, nullptr);
            SendMessageW(c, WM_SETFONT, WPARAM(font), TRUE);
            return c;
        };
        child(L"STATIC", wlabel.c_str(), SS_LEFT, 14, 12, 392, 20, -1);
        HWND edit = child(L"EDIT", wtext.c_str(), WS_TABSTOP | ES_AUTOHSCROLL, 14, 38, 392, 26, 100, WS_EX_CLIENTEDGE);
        child(L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, 222, 86, 88, 30, IDOK);
        child(L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, 318, 86, 88, 30, IDCANCEL);
        if (maxChars) SendMessageW(edit, EM_LIMITTEXT, maxChars, 0);
        SendMessageW(edit, EM_SETSEL, 0, -1);

        ShowWindow(dlg, SW_SHOW);
        SetForegroundWindow(dlg);
        SetFocus(edit);

        MSG msg;
        while (!state.done && GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            if (!IsDialogMessageW(dlg, &msg)) // Tab, Enter = OK, Esc = Cancel
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (state.ok)
        {
            const int len = GetWindowTextLengthW(edit);
            std::wstring answer(size_t(len) + 1, L'\0');
            GetWindowTextW(edit, answer.data(), len + 1);
            answer.resize(size_t(len));
            text.assign(answer.begin(), answer.end());
        }
        DestroyWindow(dlg);
        DeleteObject(font);
        if (g_hwnd) SetForegroundWindow(g_hwnd); // give the keyboard back to the game
        return state.ok;
    }
}
