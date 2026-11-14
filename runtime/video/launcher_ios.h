#pragma once
// Launch menu on iPadOS / iOS (launcher_ios.mm). Settings: launcher/launcher_core.h
// (Launcher::Current() after the menu closes).

namespace Video::Apple
{
    // Game thread: shows the menu on the main thread and returns when the player taps Play Game
    // (settings saved and applied).
    void RunLauncher();
}
