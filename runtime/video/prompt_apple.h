#pragma once
// Native text prompt on Apple (prompt_apple.mm): UIAlertController on iPadOS, NSAlert on macOS.
#include <cstdint>
#include <functional>
#include <string>

namespace Video::Apple
{
    // Main thread only. nativeWindow: UIWindow* (iOS) / NSWindow* (macOS). Returns at once;
    // done(accepted, text) runs later on the main thread. False if nothing could be shown.
    bool ShowTextPrompt(void* nativeWindow, const std::u16string& title, const std::u16string& message,
        const std::u16string& text, uint32_t maxChars, std::function<void(bool, std::u16string)> done);
}
