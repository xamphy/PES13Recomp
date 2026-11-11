#pragma once
// On-screen touch controls on iPadOS (touch_controls_ios.mm): a UIKit overlay over the game view.
#include "presenter.h"

namespace Video::Apple
{
    // Main thread only. nativeWindow: the SDL window's UIWindow*.
    void InstallTouchControls(void* nativeWindow);
    // Main thread only. Brings the controls back above views added later (SDL's Metal view).
    void RaiseTouchControls();
    // Main thread only. Hidden while a real controller is connected.
    void SetTouchControlsVisible(bool visible);
    // Any thread: the current touch state in Xbox 360 terms (merged into player 1).
    HostPad GetTouchPad();
}
