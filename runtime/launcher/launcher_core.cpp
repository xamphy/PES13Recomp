// Launch menu, platform-independent part (see launcher_core.h).
#include "launcher_core.h"
#include "video/presenter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifndef PES_BUILD_COMMIT
#define PES_BUILD_COMMIT "unknown"
#endif

namespace Launcher
{
    namespace
    {
        constexpr const char* kSettingsFile = "userdata/launcher.ini";
        Settings g_current;
        bool g_loaded = false;
    }

    Settings LoadSettings()
    {
        Settings s;
        std::ifstream in(kSettingsFile);
        std::string line;
        while (std::getline(in, line))
        {
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = line.substr(0, eq);
            const int value = std::atoi(line.c_str() + eq + 1);
            if (key == "upscaler") s.upscaler = std::clamp(value, 0, kUpscalerCount - 1);
            else if (key == "upscale_height") s.upscaleHeight = std::clamp(value, 0, kUpscaleHeightCount - 1);
            else if (key == "perf_overlay") s.perfOverlay = value != 0;
            else if (key == "touch_controls") s.touchControls = value != 0;
        }
        if (!g_loaded) { g_current = s; g_loaded = true; }
        return s;
    }

    void SaveSettings(const Settings& s)
    {
        std::error_code ec;
        std::filesystem::create_directories("userdata", ec);
        std::ofstream out(kSettingsFile, std::ios::trunc);
        out << "# PES 2013 launch menu settings\n"
            << "upscaler=" << s.upscaler << "\n"
            << "upscale_height=" << s.upscaleHeight << "\n"
            << "perf_overlay=" << (s.perfOverlay ? 1 : 0) << "\n"
            << "touch_controls=" << (s.touchControls ? 1 : 0) << "\n";
    }

    void ApplySettings(const Settings& s)
    {
        Video::SetUpscaler(Video::Upscaler(std::clamp(s.upscaler, 0, kUpscalerCount - 1)));
        Video::SetUpscaleHeight(kUpscaleHeights[std::clamp(s.upscaleHeight, 0, kUpscaleHeightCount - 1)]);
        std::printf("[launcher] settings: upscaling %s at %s, performance overlay %s, touch controls %s\n",
            kUpscalerNames[s.upscaler], kUpscaleHeightNames[s.upscaleHeight], s.perfOverlay ? "on" : "off", s.touchControls ? "on" : "off");
    }

    const Settings& Current()
    {
        if (!g_loaded) LoadSettings();
        return g_current;
    }

    void SetCurrent(const Settings& s) { g_current = s; g_loaded = true; }

    bool GameFilesPresent()
    {
        std::error_code ec;
        return std::filesystem::is_regular_file("game/xex/default.xex", ec) && std::filesystem::is_directory("game/files", ec);
    }

    // ------------------------------------------------------------------ background
    namespace
    {
        struct C { float r, g, b; };
        C Mix(C a, C b, float t) { return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t }; }
        float Smooth(float e0, float e1, float x) { const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f); return t * t * (3 - 2 * t); }
        float Hash(int x, int y) { uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u; h = (h ^ (h >> 13)) * 1274126177u; return float(h & 0xFFFF) / 65535.0f; }

        // One sample at normalized (u, v), v down; aspect = width / height.
        C Sample(float u, float v, float aspect)
        {
            constexpr float horizon = 0.40f;  // where the pitch meets the stands
            constexpr float standTop = 0.20f;
            C c;
            // Night sky.
            c = Mix(C{ 0.004f, 0.007f, 0.020f }, C{ 0.018f, 0.035f, 0.080f }, Smooth(0.0f, standTop + 0.05f, v));
            // Floodlights: four towers above the stands, warm-white glow.
            const float lights[4][2] = { { 0.08f, 0.10f }, { 0.34f, 0.06f }, { 0.66f, 0.06f }, { 0.92f, 0.10f } };
            float glow = 0;
            for (const auto& l : lights)
            {
                const float dx = (u - l[0]) * aspect, dy = v - l[1];
                const float d2 = dx * dx + dy * dy;
                glow += 1.2f * std::exp(-d2 * 2500.0f) + 0.10f * std::exp(-d2 * 60.0f) + 0.010f * std::exp(-d2 * 6.0f);
            }
            // Stands: a dark bowl between the roof line and the pitch, faint crowd speckle in the light.
            const float roof = standTop + 0.025f * std::sin(u * 3.14159f);
            if (v > roof && v < horizon)
            {
                const float t = (v - roof) / (horizon - roof);
                c = Mix(C{ 0.012f, 0.016f, 0.030f }, C{ 0.030f, 0.036f, 0.055f }, t);
                c = { c.r * (0.85f + 0.15f * std::sin(t * 40.0f)), c.g * (0.85f + 0.15f * std::sin(t * 40.0f)), c.b * (0.85f + 0.15f * std::sin(t * 40.0f)) }; // tiers
                const float speck = Hash(int(u * 900), int(v * 900));
                const float crowd = (speck > 0.72f ? (speck - 0.72f) * 0.12f : 0.0f) * Smooth(roof, roof + 0.03f, v);
                c = { c.r + crowd * 0.9f, c.g + crowd * 0.8f, c.b + crowd * 0.7f };
                // Advertising boards along the front of the stands.
                if (v > horizon - 0.018f)
                    c = Mix(c, C{ 0.015f, 0.055f, 0.170f }, 0.85f);
            }
            // Pitch in perspective below the horizon.
            if (v >= horizon)
            {
                const float dv = std::max(v - horizon, 0.0015f);
                const float z = 0.06f / dv;                 // depth (world units, 1 = near)
                const float x = (u - 0.5f) * aspect * z;    // lateral world position
                const bool onPitch = std::abs(x) < 0.62f;
                const float stripe = std::fmod(z * 14.0f, 2.0f) < 1.0f ? 1.0f : 0.0f;
                C grass = Mix(C{ 0.055f, 0.230f, 0.085f }, C{ 0.090f, 0.330f, 0.125f }, stripe);
                grass = Mix(C{ 0.020f, 0.090f, 0.040f }, grass, Smooth(0.0f, 0.12f, dv)); // darker far away
                if (!onPitch) grass = Mix(grass, C{ 0.030f, 0.070f, 0.040f }, 0.6f);
                c = grass;
                // White lines: halfway line, centre circle, touchlines. Width in world units grows with
                // depth, so lines keep a similar on-screen width.
                const float w = 0.0035f * z;
                const float zc = 0.36f; // centre spot depth
                float line = 0;
                line = std::max(line, 1.0f - Smooth(w * 0.5f, w, std::abs(x)));                         // halfway line
                const float r = std::sqrt(x * x + (z - zc) * (z - zc));
                line = std::max(line, 1.0f - Smooth(w * 0.5f, w, std::abs(r - 0.10f)));              // centre circle
                line = std::max(line, 1.0f - Smooth(w * 0.5f, w, std::abs(std::abs(x) - 0.62f)));      // touchlines
                line *= Smooth(0.0f, 0.03f, dv);
                c = Mix(c, C{ 0.80f, 0.85f, 0.82f }, line * 0.75f);
                // Light pool from the floodlights.
                const float pool = std::exp(-(x * x) * 4.0f - (z - zc) * (z - zc) * 1.5f);
                c = { c.r * (0.75f + 0.45f * pool), c.g * (0.75f + 0.45f * pool), c.b * (0.75f + 0.45f * pool) };
            }
            c = { c.r + glow * 1.00f, c.g + glow * 0.97f, c.b + glow * 0.88f };
            // Haze over the horizon, vignette, and a darker left side behind the menu text.
            const float haze = std::exp(-(v - horizon) * (v - horizon) * 500.0f) * 0.06f;
            c = { c.r + haze, c.g + haze * 1.1f, c.b + haze * 1.4f };
            const float vx = u - 0.5f, vy = v - 0.5f;
            const float vignette = 1.0f - 0.55f * Smooth(0.25f, 0.85f, std::sqrt(vx * vx * 1.3f + vy * vy * 1.6f));
            const float side = 1.0f - 0.70f * (1.0f - Smooth(0.05f, 0.62f, u));
            const float k = vignette * side;
            return { c.r * k, c.g * k, c.b * k };
        }
    }

    std::vector<uint32_t> RenderBackground(uint32_t width, uint32_t height)
    {
        std::vector<uint32_t> px(size_t(width) * height);
        if (!width || !height) return px;
        const float aspect = float(width) / float(height);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                // 2x2 supersampling (smooth lines).
                C sum{ 0, 0, 0 };
                for (int sy = 0; sy < 2; ++sy)
                    for (int sx = 0; sx < 2; ++sx)
                    {
                        const C s = Sample((float(x) + 0.25f + 0.5f * sx) / float(width), (float(y) + 0.25f + 0.5f * sy) / float(height), aspect);
                        sum = { sum.r + s.r, sum.g + s.g, sum.b + s.b };
                    }
                auto to8 = [](float v) { return uint32_t(std::clamp(std::sqrt(std::max(v * 0.25f, 0.0f)) * 255.0f + 0.5f, 0.0f, 255.0f)); };
                px[size_t(y) * width + x] = to8(sum.r) | (to8(sum.g) << 8) | (to8(sum.b) << 16) | 0xFF000000u;
            }
        return px;
    }

    // ------------------------------------------------------------------ about
    About GetAbout(const std::string& platform, const std::string& gpu)
    {
        About a;
        a.build = {
            { "Build", std::string(PES_BUILD_COMMIT) + " (" + __DATE__ + ")" },
            { "Platform", platform },
        };
        if (!gpu.empty()) a.build.push_back({ "GPU", gpu });
        std::error_code ec;
        a.xexFound = std::filesystem::is_regular_file("game/xex/default.xex", ec);
        a.filesFound = std::filesystem::is_directory("game/files", ec);
        a.gameFiles = a.xexFound && a.filesFound ? "Found (game/xex/default.xex and game/files)"
            : !a.xexFound && !a.filesFound ? "Missing: copy the game folder (game/xex and game/files)"
            : !a.xexFound ? "Missing game/xex/default.xex" : "Missing the game/files folder";
        a.credits = {
            { "PES 2013 recompilation", "xamphy" },
            { "XenonRecomp / XenosRecomp", "hedge-dev, MIT" },
            { "Xenia (GPU emulation code)", "Ben Vanik and contributors, BSD-3-Clause" },
            { "AMD FidelityFX FSR 1", "Advanced Micro Devices, MIT" },
            { "Snapdragon Game Super Resolution 1", "Qualcomm, BSD-3-Clause" },
            { "SDL 3", "Sam Lantinga, zlib" },
            { "MoltenVK", "The Brenwill Workshop / Khronos, Apache-2.0" },
            { "Vulkan-Headers, glslang", "The Khronos Group, Apache-2.0 / BSD" },
            { "volk", "Arseny Kapoulkine, MIT" },
            { "fmt", "Victor Zverovich, MIT" },
            { "miniaudio", "David Reid, public domain / MIT-0" },
        };
        a.disclaimer = "Requires your own legally obtained copy of Pro Evolution Soccer 2013 (Xbox 360). "
                       "This project is not affiliated with or endorsed by Konami.";
        return a;
    }
}
