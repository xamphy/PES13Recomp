#pragma once
// Performance overlay on iPadOS (perf_overlay_ios.mm): FPS, CP time, memory, thermal state and
// battery in the letterbox bar below the game picture.

namespace Video::Apple
{
    // Main thread only. nativeWindow: the SDL window's UIWindow*. Install before the touch
    // controls so it stays underneath them.
    void InstallPerfOverlay(void* nativeWindow);
    // Main thread only. Brings the overlay back above views added later (SDL's Metal view).
    void RaisePerfOverlay();
}
