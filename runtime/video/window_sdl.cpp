// Apple window for the presenter: SDL3 (zlib license, thirdparty/SDL).
// macOS / iPadOS require the window and its event loop on the process main thread (UIKit / AppKit
// own it), so Video::RunMain keeps the main thread in SDL's event loop and runs the game - which
// otherwise runs its guest main thread on the host main thread - on a separate large-stack thread.
// Window work requested from the game side (create, surface, title) is marshalled to the main thread
// with SDL_RunOnMainThread. Presenting (vkQueuePresentKHR) stays on the GPU thread: MoltenVK
// allows that.
#include "window.h"
#include "presenter.h"
#include "prompt_apple.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <TargetConditionals.h>
#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    SDL_Window* g_window = nullptr;
    std::atomic<bool> g_focus{ false };

    // Controllers (SDL gamepads: Xbox, PlayStation, MFi, ...), opened and read on the main thread
    // (event loop); the game thread reads the snapshots.
    struct PadSlot
    {
        SDL_JoystickID id = 0;
        SDL_Gamepad* pad = nullptr;
        Video::HostPad state;
    };
    std::mutex g_padMutex;
    PadSlot g_pads[4];

    void UpdatePad(PadSlot& s)
    {
        using namespace std;
        SDL_Gamepad* g = s.pad;
        Video::HostPad p;
        const struct { SDL_GamepadButton b; uint16_t bit; } map[] = {
            { SDL_GAMEPAD_BUTTON_DPAD_UP, 0x0001 }, { SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x0002 }, { SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x0004 },
            { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0008 }, { SDL_GAMEPAD_BUTTON_START, 0x0010 }, { SDL_GAMEPAD_BUTTON_BACK, 0x0020 },
            { SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x0040 }, { SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x0080 },
            { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x0100 }, { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0200 },
            { SDL_GAMEPAD_BUTTON_SOUTH, 0x1000 }, { SDL_GAMEPAD_BUTTON_EAST, 0x2000 }, { SDL_GAMEPAD_BUTTON_WEST, 0x4000 },
            { SDL_GAMEPAD_BUTTON_NORTH, 0x8000 } };
        for (const auto& m : map)
            if (SDL_GetGamepadButton(g, m.b)) p.buttons |= m.bit;
        // SDL: y grows downwards, triggers 0..32767. Xbox: y grows upwards, triggers 0..255.
        auto axis = [&](SDL_GamepadAxis a) { return int32_t(SDL_GetGamepadAxis(g, a)); };
        auto flip = [](int32_t v) { return int16_t(std::clamp(-v, -32768, 32767)); };
        p.lx = int16_t(axis(SDL_GAMEPAD_AXIS_LEFTX));
        p.ly = flip(axis(SDL_GAMEPAD_AXIS_LEFTY));
        p.rx = int16_t(axis(SDL_GAMEPAD_AXIS_RIGHTX));
        p.ry = flip(axis(SDL_GAMEPAD_AXIS_RIGHTY));
        p.lt = uint8_t(std::clamp(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 128, 0, 255));
        p.rt = uint8_t(std::clamp(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 128, 0, 255));
        std::lock_guard lock(g_padMutex);
        s.state = p;
    }

    void HandleGamepadEvent(const SDL_Event& e)
    {
        switch (e.type)
        {
        case SDL_EVENT_GAMEPAD_ADDED:
            for (PadSlot& s : g_pads)
                if (!s.pad)
                {
                    s.pad = SDL_OpenGamepad(e.gdevice.which);
                    s.id = e.gdevice.which;
                    if (s.pad)
                    {
                        std::printf("[input] controller connected: %s\n", SDL_GetGamepadName(s.pad));
                        UpdatePad(s);
                    }
                    break;
                }
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            for (PadSlot& s : g_pads)
                if (s.pad && s.id == e.gdevice.which)
                {
                    SDL_CloseGamepad(s.pad);
                    std::lock_guard lock(g_padMutex);
                    s = {};
                }
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            for (PadSlot& s : g_pads)
                if (s.pad && s.id == e.gdevice.which) UpdatePad(s);
            break;
        default:
            break;
        }
    }

    template <typename F>
    void OnMainThread(F&& f)
    {
        // wait_complete = true: f may reference the caller's locals.
        SDL_RunOnMainThread([](void* p) { (*static_cast<F*>(p))(); }, &f, true);
    }

    struct AppArgs
    {
        int (*appMain)(int, char**);
        int argc;
        char** argv;
    };

    void* AppThread(void* p)
    {
        const AppArgs* a = static_cast<const AppArgs*>(p);
        const int code = a->appMain(a->argc, a->argv);
        std::fflush(stdout);
        std::fflush(stderr);
        std::_Exit(code); // the guest never returns cleanly; don't run static destructors under it
        return nullptr;
    }
}

namespace Video::Window
{
    bool Create(uint32_t width, uint32_t height, const char* title)
    {
        bool ok = false;
        OnMainThread([&]
        {
#if TARGET_OS_IOS
            const char* vulkanLib = "@executable_path/Frameworks/MoltenVK.framework/MoltenVK"; // embedded in the app
#else
            const char* vulkanLib = nullptr;
#endif
            if (!SDL_Vulkan_LoadLibrary(vulkanLib))
            {
                std::fprintf(stderr, "[video] SDL_Vulkan_LoadLibrary: %s\n", SDL_GetError());
                return;
            }
            g_window = SDL_CreateWindow(title, int(width), int(height),
                SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY); // full-resolution drawable on Retina / iPad, not points
            if (!g_window)
            {
                std::fprintf(stderr, "[video] SDL_CreateWindow: %s\n", SDL_GetError());
                return;
            }
            {
                int pw = 0, ph = 0, ww = 0, wh = 0;
                SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
                SDL_GetWindowSize(g_window, &ww, &wh);
                std::printf("[video] window %dx%d points, %dx%d pixels\n", ww, wh, pw, ph);
            }
            g_focus = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS) != 0;
            ok = true;
        });
        return ok;
    }

    std::vector<const char*> InstanceExtensions()
    {
        Uint32 count = 0;
        const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
        return names ? std::vector<const char*>(names, names + count) : std::vector<const char*>{};
    }

    VkSurfaceKHR CreateSurface(VkInstance instance)
    {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        OnMainThread([&]
        {
            if (!SDL_Vulkan_CreateSurface(g_window, instance, nullptr, &surface))
                std::fprintf(stderr, "[video] SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
        });
        return surface;
    }

    bool HasFocus() { return g_focus; }

    void SetTitle(const std::string& title)
    {
        if (!g_window)
            return;
        // Don't wait: the GPU thread calls this; the copy is freed on the main thread.
        auto* copy = new std::string(title);
        SDL_RunOnMainThread([](void* p)
        {
            auto* s = static_cast<std::string*>(p);
            SDL_SetWindowTitle(g_window, s->c_str());
            delete s;
        }, copy, false);
    }
}

namespace Video
{
    bool GetHostGamepad(uint32_t index, HostPad& out)
    {
        std::lock_guard lock(g_padMutex);
        if (index >= 4 || !g_pads[index].pad) return false;
        out = g_pads[index].state;
        return true;
    }

    // Same mapping as the Windows keyboard: arrows = D-pad, Enter = Start, Backspace = Back,
    // Space = A, Esc = B, C = X, V = Y, Q/E = LB/RB, WASD / IJKL = sticks, Shift = LT, R = RT.
    HostPad GetHostKeyboard()
    {
        HostPad p;
        if (!g_focus) return p;
        int count = 0;
        const bool* k = SDL_GetKeyboardState(&count);
        auto key = [&](SDL_Scancode s) { return int(s) < count && k[s]; };
        const struct { SDL_Scancode s; uint16_t bit; } map[] = {
            { SDL_SCANCODE_UP, 0x0001 }, { SDL_SCANCODE_DOWN, 0x0002 }, { SDL_SCANCODE_LEFT, 0x0004 }, { SDL_SCANCODE_RIGHT, 0x0008 },
            { SDL_SCANCODE_RETURN, 0x0010 }, { SDL_SCANCODE_BACKSPACE, 0x0020 }, { SDL_SCANCODE_SPACE, 0x1000 },
            { SDL_SCANCODE_ESCAPE, 0x2000 }, { SDL_SCANCODE_C, 0x4000 }, { SDL_SCANCODE_V, 0x8000 },
            { SDL_SCANCODE_Q, 0x0100 }, { SDL_SCANCODE_E, 0x0200 } };
        for (const auto& m : map)
            if (key(m.s)) p.buttons |= m.bit;
        p.lx = int16_t((key(SDL_SCANCODE_D) ? 32767 : 0) - (key(SDL_SCANCODE_A) ? 32767 : 0));
        p.ly = int16_t((key(SDL_SCANCODE_W) ? 32767 : 0) - (key(SDL_SCANCODE_S) ? 32767 : 0));
        p.rx = int16_t((key(SDL_SCANCODE_L) ? 32767 : 0) - (key(SDL_SCANCODE_J) ? 32767 : 0));
        p.ry = int16_t((key(SDL_SCANCODE_I) ? 32767 : 0) - (key(SDL_SCANCODE_K) ? 32767 : 0));
        p.lt = key(SDL_SCANCODE_LSHIFT) || key(SDL_SCANCODE_RSHIFT) ? 255 : 0;
        p.rt = key(SDL_SCANCODE_R) ? 255 : 0;
        return p;
    }

    int RunMain(int (*appMain)(int, char**), int argc, char** argv)
    {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD))
        {
            std::fprintf(stderr, "[video] SDL_Init: %s - running without a window\n", SDL_GetError());
            return appMain(argc, argv);
        }

#if TARGET_OS_IOS
        // iPadOS: no command line and no console. Work in the app's Documents folder (visible in the
        // Files app: copy game/xex and game/files there), log to Documents/pes13.log, always run.
        if (const char* docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS))
            chdir(docs);
        std::freopen("pes13.log", "w", stdout);
        std::freopen("pes13.err.log", "w", stderr);
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        static std::vector<char*> iosArgv;
        static char runArg[] = "--run";
        iosArgv.assign(argv, argv + argc);
        iosArgv.push_back(runArg);
        iosArgv.push_back(nullptr);
        argc = int(iosArgv.size()) - 1;
        argv = iosArgv.data();
#endif
        // The game: 64 MB host stack (the Windows main thread has 1 MB; recompiled code can nest deeply).
        static AppArgs args;
        args = { appMain, argc, argv };
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 64u << 20);
        pthread_t thread;
        if (pthread_create(&thread, &attr, AppThread, &args) != 0)
        {
            std::fprintf(stderr, "[video] could not start the game thread\n");
            return 1;
        }
        pthread_attr_destroy(&attr);
        pthread_detach(thread);

        // Main thread: SDL event loop (also runs SDL_RunOnMainThread callbacks).
        for (;;)
        {
            SDL_Event e;
            if (!SDL_WaitEvent(&e))
                continue;
            switch (e.type)
            {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                std::printf("[video] window closed - exiting\n");
                std::fflush(stdout);
                std::_Exit(0);
            case SDL_EVENT_WINDOW_FOCUS_GAINED: g_focus = true; break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:   g_focus = false; break;
            case SDL_EVENT_GAMEPAD_ADDED:
            case SDL_EVENT_GAMEPAD_REMOVED:
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:   HandleGamepadEvent(e); break;
            default: break;
            }
        }
    }
}

namespace Video
{
    // Native pop-up (prompt_apple.mm): shown on the main thread, the calling game thread waits for
    // the answer. Without a window (headless) the default text is kept, or "Player" if it is empty.
    bool PromptText(const std::u16string& title, const std::u16string& description, std::u16string& text, uint32_t maxChars)
    {
        std::mutex m;
        std::condition_variable cv;
        bool answered = false, accepted = false, shown = false;
        std::u16string answer;
        OnMainThread([&]
        {
            void* native = nullptr;
            if (g_window)
            {
                const SDL_PropertiesID props = SDL_GetWindowProperties(g_window);
#if TARGET_OS_IOS
                native = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
#else
                native = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
#endif
            }
            if (!native) return;
            shown = Apple::ShowTextPrompt(native, title, description, text, maxChars, [&](bool ok, std::u16string result)
            {
                std::lock_guard lock(m);
                accepted = ok;
                answer = std::move(result);
                answered = true;
                cv.notify_all();
            });
        });
        if (!shown)
        {
            std::fprintf(stderr, "[video] PromptText: no native window - keeping the default text\n");
            if (text.empty()) text = u"Player";
            if (maxChars && text.size() > maxChars) text.resize(maxChars);
            return true;
        }
        std::unique_lock lock(m);
        cv.wait(lock, [&] { return answered; });
        if (accepted) text = answer;
        return accepted;
    }
}
