#pragma once
// Launch menu for Windows (launcher_win32.cpp).

namespace Launcher
{
    // False for automated runs (PES_INPUT_SCRIPT / PES_CONTROL / PES_SHOTS / PES_NO_LAUNCHER set,
    // or --no-launcher on the command line).
    bool ShouldShowWin32(int argc, char** argv);
    // Shows the menu until Play Game (true: settings saved and applied) or the window is closed (false).
    bool RunWin32();
}
