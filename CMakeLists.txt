cmake_minimum_required(VERSION 3.20)
project(PES13 C CXX)

if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")   # Clang or AppleClang
    message(FATAL_ERROR "The recompiled code requires Clang (clang-cl on Windows).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Must match XenonRecomp's setting, since we link its XenonUtils library.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# XenonUtils: XEX loading (decrypt/decompress) and PE image parsing.
add_subdirectory(tools/XenonRecomp EXCLUDE_FROM_ALL)

if (CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
    if (MSVC)
        add_compile_options(/arch:AVX)
    else()
        add_compile_options(-mavx)
    endif()
endif()

# ---------------------------------------------------------------------------
# Recompiled game code (generated/ - output of XenonRecomp, never committed)
# ---------------------------------------------------------------------------
file(GLOB PES13_RECOMP_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/generated/ppc_recomp.*.cpp")
if (NOT PES13_RECOMP_SOURCES)
    message(FATAL_ERROR "generated/ is empty - run XenonRecomp first (see docs/PLAN.md).")
endif()

add_library(PES13Recomp STATIC
    ${PES13_RECOMP_SOURCES}
    "${CMAKE_SOURCE_DIR}/generated/ppc_func_mapping.cpp"
)
target_include_directories(PES13Recomp PUBLIC
    "${CMAKE_SOURCE_DIR}/generated"
    "${CMAKE_SOURCE_DIR}/tools/XenonRecomp/thirdparty/simde"
)
# Generated code is huge and warning-heavy; silence warnings there only.
target_compile_options(PES13Recomp PRIVATE -w)

# The generated translation units are large: cap parallel compiles so 16 GB RAM is enough.
set(PES13_RECOMP_JOBS 6 CACHE STRING "Max parallel compile jobs for generated code")
set_property(GLOBAL APPEND PROPERTY JOB_POOLS recomp=${PES13_RECOMP_JOBS})
set_target_properties(PES13Recomp PROPERTIES JOB_POOL_COMPILE recomp)

# ---------------------------------------------------------------------------
# Runtime (hand-written: kernel/XAM, memory, later GPU/audio/input/fs)
# ---------------------------------------------------------------------------
add_executable(pes13
    runtime/main.cpp
    runtime/audio/xaudio_imports.cpp
    runtime/debug/call_counter.cpp
    runtime/debug/call_counter.gen.cpp
    runtime/debug/experiments.cpp
    runtime/debug/kick_trace.cpp
    runtime/debug/text_trace.cpp
    runtime/fs/file_imports.cpp
    runtime/fs/vfs.cpp
    runtime/guest_memory.cpp
    runtime/gpu/gpu.cpp
    runtime/gpu/shader_translate.cpp
    runtime/gpu/renderer.cpp
    runtime/gpu/memory_watch.cpp
    runtime/gpu/precise_sleep.cpp
    runtime/gpu/d3d_wait.cpp
    runtime/debug/process_memory.cpp
    runtime/gpu/xg_hooks.cpp
    runtime/gpu/video_imports.cpp
    runtime/input/input_imports.cpp
    runtime/kernel/dpc.cpp
    runtime/kernel/interrupts.cpp
    runtime/kernel/kernel_vars.cpp
    runtime/kernel/misc.cpp
    runtime/kernel/objects.cpp
    runtime/kernel/stubs.cpp
    runtime/kernel/sync.cpp
    runtime/kernel/thread.cpp
    runtime/kernel/threading.cpp
    runtime/kernel/tls.cpp
    runtime/kernel/virtual_memory.cpp
    runtime/kernel/xex_module.cpp
    runtime/kernel/xam_user.cpp
    runtime/video/presenter.cpp
)
# Window behind the presenter: Win32 on Windows; SDL3 on Apple (thirdparty/SDL, zlib license),
# where the window and event loop must own the main thread. SDL is not used on Windows.
if (WIN32)
    target_sources(pes13 PRIVATE runtime/video/window_win32.cpp)
else()
    target_sources(pes13 PRIVATE runtime/video/window_sdl.cpp runtime/video/prompt_apple.mm)
    # Native text prompt (UIKit / AppKit): Objective-C++ with ARC, built by the C++ compiler.
    set_source_files_properties(runtime/video/prompt_apple.mm PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-x;objective-c++;-fobjc-arc")
    if (IOS)
        target_link_libraries(pes13 PRIVATE "-framework UIKit")
    else()
        target_link_libraries(pes13 PRIVATE "-framework AppKit")
    endif()
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(thirdparty/SDL EXCLUDE_FROM_ALL)
    target_link_libraries(pes13 PRIVATE SDL3::SDL3-static)
endif()
# Windows-only debug tools (page-guard watches, texture-fill trace hooks); stubs elsewhere.
if (WIN32)
    target_sources(pes13 PRIVATE runtime/debug/write_watch.cpp runtime/debug/levelfill_trace.cpp)
else()
    target_sources(pes13 PRIVATE runtime/debug/write_watch_stub.cpp)
endif()
target_include_directories(pes13 PRIVATE runtime runtime/kernel)
target_compile_definitions(pes13 PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
include(cmake/xenia_shader.cmake)

# Vulkan: headers + volk (loads vulkan-1 at runtime, no SDK or import library needed)
add_library(volk STATIC thirdparty/volk/volk.c)
target_include_directories(volk PUBLIC thirdparty/volk thirdparty/Vulkan-Headers/include)
if (WIN32)
    target_compile_definitions(volk PUBLIC VK_USE_PLATFORM_WIN32_KHR)
endif()

# Audio output: miniaudio (single header, public domain / MIT-0; WASAPI / Core Audio backends)
add_library(miniaudio STATIC runtime/audio/miniaudio_impl.c)
target_include_directories(miniaudio PUBLIC thirdparty/miniaudio)

target_link_libraries(pes13 PRIVATE PES13Recomp XenonUtils volk XeniaShader miniaudio)
if (MSVC)
    # Map file: lets tools/addr2func.ps1 turn a crash offset into a sub_XXXXXXXX name.
    target_link_options(pes13 PRIVATE "/MAP:${CMAKE_BINARY_DIR}/bin/pes13.map" /errorlimit:0)
endif()

set_target_properties(pes13 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

# ---------------------------------------------------------------------------
# iPadOS app (cmake -DCMAKE_SYSTEM_NAME=iOS ...; built by .github/workflows/ios.yml). The bundle is
# unsigned here: CI ad-hoc signs it with cmake/ios/pes13.entitlements and adds MoltenVK.framework,
# then AltStore/SideStore re-sign it on install. Nothing here affects Windows or macOS builds.
# ---------------------------------------------------------------------------
if (IOS)
    set_target_properties(pes13 PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/cmake/ios/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.xamphy.pes13"
        MACOSX_BUNDLE_BUNDLE_VERSION "1"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1"
        BUILD_RPATH "@executable_path/Frameworks"
        INSTALL_RPATH "@executable_path/Frameworks"
    )
    # miniaudio includes AVFoundation on iOS: compile its implementation as Objective-C.
    set_source_files_properties(runtime/audio/miniaudio_impl.c PROPERTIES COMPILE_OPTIONS "-x;objective-c")
    target_link_libraries(pes13 PRIVATE "-framework AVFoundation" "-framework CoreAudio" "-framework AudioToolbox" "-framework Foundation")
endif()
