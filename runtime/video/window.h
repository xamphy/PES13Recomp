#pragma once
// Platform window behind the presenter (internal to runtime/video).
//   window_win32.cpp : Windows - Win32 window on its own message-pump thread.
//   window_sdl.cpp   : Apple   - SDL3 window; SDL owns the main thread (Video::RunMain).
#include <volk.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Video::Window
{
    // Creates and shows the window. Called once from Video::Init.
    bool Create(uint32_t width, uint32_t height, const char* title);

    // Instance extensions the surface needs (VK_KHR_surface + the platform one).
    std::vector<const char*> InstanceExtensions();

    // Vulkan surface for the window (after Create, with a volk-loaded instance).
    VkSurfaceKHR CreateSurface(VkInstance instance);

    bool HasFocus();
    void SetTitle(const std::string& title);
}
