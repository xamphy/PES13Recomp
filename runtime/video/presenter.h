#pragma once
// Host window + Vulkan swap chain. Shows the guest front buffer at every swap.
// (Windows: Win32 window, window_win32.cpp. Apple: SDL3 window + MoltenVK, window_sdl.cpp.)
#include <cstdint>
#include <string>

namespace Video
{
    // Runs appMain (the runtime's real main). Windows: calls it directly on this thread.
    // Apple: the process main thread must own the window and event loop, so this runs
    // appMain on a separate thread and never returns (the process exits with appMain's code).
    int RunMain(int (*appMain)(int, char**), int argc, char** argv);

    // Creates the window and Vulkan objects. Returns false if Vulkan is unavailable;
    // the runtime then keeps running without a display.
    bool Init(uint32_t width, uint32_t height, const char* title);

    // Displays an RGBA8 image (scaled to the window). Called from the GPU thread at
    // every frame swap; blocks for vsync (FIFO presentation).
    void Present(const uint32_t* rgba, uint32_t width, uint32_t height);

    void SetTitle(const std::string& title);

    // Upscaling of the game picture to the window (presenter.cpp): plain linear filtering, AMD
    // FSR 1 (EASU + RCAS) or Snapdragon GSR 1. Any thread; takes effect at the next frame.
    // Initial value: PES_UPSCALER=off|fsr|sgsr (default off).
    enum class Upscaler { Off = 0, Fsr1 = 1, Sgsr1 = 2 };
    void SetUpscaler(Upscaler upscaler);
    Upscaler GetUpscaler();
    // Height of the upscaler's output (0: the on-screen size). A lower one costs less GPU time; the
    // result is then scaled to the screen with linear filtering. Initial value: PES_UPSCALE_HEIGHT.
    void SetUpscaleHeight(uint32_t height);

    // Performance numbers for the on-screen overlay (iPadOS: drawn in the letterbox bar).
    // The GPU thread publishes them once per second; any thread may read them.
    struct PerfStats
    {
        uint32_t fps = 0;            // guest frames swapped in the last second
        uint32_t drawsPerFrame = 0;
        float cpAvgMs = 0, cpMaxMs = 0; // command-processor busy time per frame
        uint32_t slowFrames = 0;     // frames whose CP time exceeded 16.7 ms
        uint32_t frameWidth = 0, frameHeight = 0; // the game's output size (set by the presenter)
    };
    // iPadOS forbids GPU work while the app is in the background (Metal fails the command buffer
    // and the device is lost for good). Every queue submit / acquire / present runs inside a
    // GpuSubmitScope: it waits while the app is in the background. SetAppInBackground(true) returns
    // only after submits already in flight have finished. Elsewhere it never blocks.
    void SetAppInBackground(bool background);
    struct GpuSubmitScope
    {
        GpuSubmitScope();
        ~GpuSubmitScope();
        GpuSubmitScope(const GpuSubmitScope&) = delete;
        GpuSubmitScope& operator=(const GpuSubmitScope&) = delete;
    };

    void SetPerfStats(const PerfStats& stats); // keeps frameWidth/Height
    PerfStats GetPerfStats();

    // True while the game window is the foreground window (keyboard input goes to the game).
    bool WindowHasFocus();

    // Apple (SDL3): host gamepad / keyboard state in Xbox 360 terms (XInput button bits, sticks
    // -32768..32767 with +y up, triggers 0..255). Windows reads XInput directly instead.
    struct HostPad
    {
        uint16_t buttons = 0;
        uint8_t lt = 0, rt = 0;
        int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    };
    // index: 0-3 (the order controllers were connected). False if no controller has that index.
    bool GetHostGamepad(uint32_t index, HostPad& out);
    // Keyboard mapped like on Windows (arrows, Enter, Space, WASD, ...), while the window has focus.
    HostPad GetHostKeyboard();

    // Host replacement for the console's on-screen keyboard: asks the user for text and blocks
    // until they answer. text holds the default on entry and the answer on return; maxChars
    // excludes the terminator. Returns false if the user cancelled.
    // Windows: a small dialog over the game window. Apple: native pop-up (prompt_apple.mm) -
    // UIAlertController on iPadOS, NSAlert sheet on macOS. Never call it on the Apple main thread.
    bool PromptText(const std::u16string& title, const std::u16string& description, std::u16string& text, uint32_t maxChars);
}
