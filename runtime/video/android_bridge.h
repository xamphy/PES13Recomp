#pragma once
// Android glue between the native game and the Java side (android/app/src/main/java/com/xamphy/pes13):
// LauncherActivity (launch menu), PesActivity (touch controls, performance overlay, text prompt).
// The Android counterparts of the UIKit files *_ios.mm.
#include "presenter.h"

#include <cstdint>
#include <functional>
#include <string>

namespace Video::Android
{
    // Any thread: the on-screen controls in Xbox 360 terms (merged into player 1).
    HostPad GetTouchPad();

    // SDL main thread: shows an AlertDialog with a text field; done(accepted, text) runs later on
    // the Android UI thread. False if it could not be shown.
    bool ShowTextPrompt(const std::u16string& title, const std::u16string& message, const std::u16string& text,
        uint32_t maxChars, std::function<void(bool, std::u16string)> done);
}
