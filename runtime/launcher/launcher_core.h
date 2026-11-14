#pragma once
// Launch menu, platform-independent part: settings (userdata/launcher.ini), the generated
// background picture, the About page contents and applying the settings to the presenter.
// The pages themselves are native: launcher_win32.cpp (Windows), video/launcher_ios.mm (iPadOS).
#include <cstdint>
#include <string>
#include <vector>

namespace Launcher
{
    // Upscaling output heights offered (index stored in the settings): the on-screen size, then fixed.
    constexpr uint32_t kUpscaleHeights[] = { 0, 1440, 1080, 900 };
    constexpr const char* kUpscaleHeightNames[] = { "Screen", "1440p", "1080p", "900p" };
    constexpr int kUpscaleHeightCount = 4;
    constexpr const char* kUpscalerNames[] = { "Off", "FSR 1", "SGSR 1" };
    constexpr int kUpscalerCount = 3;

    struct Settings
    {
        int upscaler = 0;          // Video::Upscaler: 0 off, 1 FSR 1, 2 SGSR 1
        int upscaleHeight = 0;     // index into kUpscaleHeights
        bool perfOverlay = true;   // iPad: performance overlay in the letterbox bar
        bool touchControls = true; // iPad: on-screen controls (hidden while a controller is connected)
    };
    Settings LoadSettings();       // userdata/launcher.ini (relative to the working directory)
    void SaveSettings(const Settings& s);
    void ApplySettings(const Settings& s); // upscaler + output height to the presenter
    const Settings& Current();     // the settings chosen (LoadSettings until a menu ran)
    void SetCurrent(const Settings& s);

    // The background: a stadium at night (sky, floodlights, stands, pitch in perspective),
    // drawn in code. width x height RGBA8 pixels, opaque.
    std::vector<uint32_t> RenderBackground(uint32_t width, uint32_t height);

    struct AboutLine { std::string label, value; };
    struct About
    {
        std::string title = "PES 2013";
        std::string subtitle = "Xbox 360 static recompilation";
        std::vector<AboutLine> build;    // version, commit, date, platform
        bool xexFound = false, filesFound = false;
        std::string gameFiles;           // one-line status
        std::vector<AboutLine> credits;  // component, author / license
        std::string disclaimer;
    };
    // platform / gpu: filled in by the caller (device and GPU names).
    About GetAbout(const std::string& platform, const std::string& gpu);
    bool GameFilesPresent();
}
