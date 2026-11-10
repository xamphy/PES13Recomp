// PES 2013 recompiled runtime - entry point.
//
// Loads the XEX into guest memory, registers the recompiled functions and sets up
// the kernel-side module state. `--run` creates the main guest thread and jumps to
// the game's entry point; unimplemented kernel imports log a [stub] line.
#include <ppc_config.h>
#include <ppc_context.h>

#include <file.h>
#include <image.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <chrono>
#include <thread>

#include "guest_memory.h"
#include "kernel/thread.h"
#include "kernel/kernel_vars.h"
#include "kernel/xex_module.h"
#include "gpu/gpu.h"
#include "gpu/renderer.h"
#include "fs/vfs.h"
#include "video/presenter.h"
#include "debug/call_counter.h"
#include "debug/write_watch.h"
#include "debug/experiments.h"

#ifdef _WIN32
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   include <windows.h>
#endif

namespace
{
#ifdef _WIN32
    // Last-chance crash report. Resolve "rva" with: tools\addr2func.ps1 <rva>
    LONG WINAPI CrashHandler(EXCEPTION_POINTERS* info)
    {
        const auto* rec = info->ExceptionRecord;
        const auto module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto rip = reinterpret_cast<uintptr_t>(rec->ExceptionAddress);

        std::fprintf(stderr, "\n[crash] exception 0x%08lX at rva 0x%llX", rec->ExceptionCode,
            (unsigned long long)(rip - module));

        if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
        {
            const auto addr = static_cast<uintptr_t>(rec->ExceptionInformation[1]);
            const char* kind = rec->ExceptionInformation[0] == 1 ? "write" : rec->ExceptionInformation[0] == 8 ? "execute" : "read";
            std::fprintf(stderr, " - %s of 0x%llX", kind, (unsigned long long)addr);

            const auto guestBase = reinterpret_cast<uintptr_t>(Memory::Base());
            if (guestBase && addr >= guestBase && addr - guestBase < 0x100000000ull)
                std::fprintf(stderr, " (guest address 0x%08llX - not committed)", (unsigned long long)(addr - guestBase));
        }
        std::fprintf(stderr, "\n");
        return EXCEPTION_CONTINUE_SEARCH;
    }
#endif

    // Target of every lookup-table slot without a recompiled function: an indirect
    // call (bctrl) to an address that is not a known function start.
    void UnresolvedIndirectCall(PPCContext& __restrict ctx, uint8_t* base)
    {
        std::fprintf(stderr, "\n[crash] indirect call to guest 0x%08X - no recompiled function there (lr=0x%08llX)\n",
            ctx.ctr.u32, (unsigned long long)ctx.lr);
        std::fflush(stderr);
        std::abort();
    }

    size_t RegisterFunctions(uint8_t* base)
    {
        auto** table = reinterpret_cast<PPCFunc**>(base + PPC_IMAGE_BASE + PPC_IMAGE_SIZE);
        for (size_t i = 0; i < PPC_CODE_SIZE / 4; ++i)
            table[i] = UnresolvedIndirectCall;

        size_t count = 0;
        for (const PPCFuncMapping* m = PPCFuncMappings; m->guest != 0; ++m, ++count)
            PPC_LOOKUP_FUNC(base, m->guest) = m->host;
        return count;
    }
}

static int AppMain(int argc, char** argv)
{
    // Unbuffered, so output written before a crash is never lost.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef _WIN32
    SetUnhandledExceptionFilter(CrashHandler);
#endif

    std::filesystem::path xexPath = "game/xex/default.xex";
    bool run = false;
    bool countCalls = false;
    bool watchWptr = false;
    std::filesystem::path dumpImagePath;
    static bool alwaysDiag = false;
    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg = argv[i];
        if (arg == "--run") run = true;
        else if (arg == "--count-calls") countCalls = true;
        else if (arg == "--diag") alwaysDiag = true;
        else if (arg == "--watch-wptr") watchWptr = true;
        else if (arg == "--skip-fence-wait") Experiments::g_skipFenceWait = true;
        else if (arg == "--no-vsync-wait") Experiments::g_noVsyncWait = true;
        else if (arg == "--dump-frame" && i + 1 < argc) Gpu::DumpFrame(std::strtoull(argv[++i], nullptr, 10));
        else if (arg == "--dump-image" && i + 1 < argc) dumpImagePath = argv[++i];
        else if (arg == "--dump-surfaces" && i + 1 < argc) Renderer::DumpSurfacesAtFrame(std::strtoull(argv[++i], nullptr, 10));
        else xexPath = arg;
    }

    const auto file = LoadFile(xexPath);
    if (file.empty())
    {
        std::fprintf(stderr, "could not read %s\n", xexPath.string().c_str());
        return 1;
    }

    Image image = Image::ParseImage(file.data(), file.size());
    if (!image.data || image.base != PPC_IMAGE_BASE)
    {
        std::fprintf(stderr, "XEX did not load at the expected base (got 0x%zX, want 0x%llX)\n",
            image.base, (unsigned long long)PPC_IMAGE_BASE);
        return 1;
    }

    if (!dumpImagePath.empty())
    {
        // Decrypted, decompressed executable image (input for tools that scan it, e.g. XenosRecomp).
        FILE* out = std::fopen(dumpImagePath.string().c_str(), "wb");
        if (!out || std::fwrite(image.data.get(), 1, image.size, out) != image.size)
        {
            std::fprintf(stderr, "could not write %s\n", dumpImagePath.string().c_str());
            return 1;
        }
        std::fclose(out);
        std::printf("wrote image (%u bytes) to %s\n", image.size, dumpImagePath.string().c_str());
        return 0;
    }

    if (!Memory::Init())
        return 1;
    Gpu::Init();
    if (watchWptr)
        WriteWatch::EnableWptrWatch();
    // PES_WATCH_PHYS="0x17000000@170": from that many seconds on, report the first non-zero store
    // to that guest physical page (any view).
    // PES_PEEK="0x82126A40,16@5": after 5 s, print 16 guest dwords from that address.
    if (const char* e = std::getenv("PES_PEEK"))
    {
        const uint32_t addr = uint32_t(std::strtoul(e, nullptr, 0));
        const char* comma = std::strchr(e, ',');
        const uint32_t count = comma ? uint32_t(std::strtoul(comma + 1, nullptr, 0)) : 8;
        const char* at = std::strchr(e, '@');
        const double seconds = at ? std::atof(at + 1) : 5.0;
        std::thread([addr, count, seconds] {
            std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
            for (uint32_t i = 0; i < count; ++i)
                std::printf("[peek] 0x%08X: 0x%08X\n", addr + i * 4, Memory::Load32(addr + i * 4));
        }).detach();
    }
    if (const char* e = std::getenv("PES_WATCH_PHYS"))
    {
        const uint32_t addr = uint32_t(std::strtoul(e, nullptr, 0));
        const char* at = std::strchr(e, '@');
        const double seconds = at ? std::atof(at + 1) : 0.0;
        std::thread([addr, seconds] {
            std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
            WriteWatch::WatchPhysicalPage(addr);
        }).detach();
    }
    // Disc contents are expected next to the XEX: game/xex/default.xex -> game/files/.
    Fs::Init(std::filesystem::absolute(xexPath).parent_path().parent_path() / "files", std::filesystem::absolute("userdata"));
    // XMA audio decoder registers (0x7FEA0000-0x7FEAFFFF): plain memory until the
    // audio system exists, so the title's direct register writes land somewhere.
    Memory::Commit(0x7FEA0000, 0x10000);
    uint8_t* base = Memory::Base();

    // Image + the host-function lookup table that sits right after it (PPC_LOOKUP_FUNC).
    const size_t lookupTableSize = PPC_CODE_SIZE * 2;
    if (!Memory::Commit(PPC_IMAGE_BASE, PPC_IMAGE_SIZE + lookupTableSize))
    {
        std::fprintf(stderr, "failed to commit image memory\n");
        return 1;
    }
    std::memcpy(base + image.base, image.data.get(), image.size);
    const size_t functions = RegisterFunctions(base);

    std::printf("PES13 runtime\n");
    std::printf("  image      : 0x%08zX - 0x%08zX (%u bytes)\n", image.base, image.base + image.size, image.size);
    std::printf("  entry point: 0x%08zX\n", image.entry_point);
    std::printf("  functions  : %zu registered\n", functions);

    // Kernel side of loading: XEX header in guest memory, module object, kernel variables.
    Kernel::ModuleInfo module;
    if (!Kernel::LoadExecutableModule(file, uint32_t(image.base), image.size, uint32_t(image.entry_point), module))
    {
        std::fprintf(stderr, "failed to set up the executable module (import table not found?)\n");
        return 1;
    }

    if (!run)
    {
        std::printf("Loaded OK. Pass --run to jump to the entry point.\n");
        return 0;
    }

    Kernel::StartTimeStampUpdater();
    if (Video::Init(1280, 720, "PES 2013 (recompiled)"))
        Renderer::Init();

    // Main guest thread: its own stack, PCR (r13), KTHREAD and TLS.
    Kernel::GuestThread* mainThread = Kernel::CreateGuestThread(module.stackSize);
    if (!mainThread)
        return 1;
    std::printf("  main thread: id %u, stack 0x%08X-0x%08X, PCR 0x%08X, TLS 0x%08X\n", mainThread->threadId,
        mainThread->stackLimit, mainThread->stackBase, mainThread->pcr, mainThread->tlsStatic);

    // Watchdog: if no frame has been presented for 10 s, report what every guest thread is doing.
    std::thread([]
    {
        uint64_t lastFrames = ~0ull;
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            const uint64_t frames = Gpu::FrameCount();
            if (frames == lastFrames || alwaysDiag)
            {
                Kernel::DumpGuestThreads();
                Gpu::DumpState();
            }
            lastFrames = frames;
        }
    }).detach();

    CallCounter::StartWatch();

    if (countCalls)
    {
        // After the game has been rendering for a while, report calls per frame over a 5 s window.
        std::thread([]
        {
            while (Gpu::FrameCount() < 600) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            CallCounter::Report(0); // no-op; counters keep running
            for (uint32_t i = 0; i < g_callCounterCount; ++i) g_callCounters[i] = 0;
            const uint64_t start = Gpu::FrameCount();
            std::this_thread::sleep_for(std::chrono::seconds(5));
            CallCounter::Report(Gpu::FrameCount() - start);
        }).detach();
    }

    std::printf("Jumping to entry point...\n");
    const uint32_t exitCode = Kernel::RunGuestThread(*mainThread, uint32_t(image.entry_point), 0, 0);
    std::printf("Main thread exited (code 0x%08X)\n", exitCode);
    return 0;
}

#if defined(__APPLE__)
#   include <TargetConditionals.h>
#   if TARGET_OS_IOS
#       include <SDL3/SDL_main.h> // iPadOS: SDL starts UIKit and calls main from its app delegate
#   endif
#endif

// Windows: AppMain runs right here. Apple: the main thread runs the window event loop (SDL) and
// AppMain gets its own thread (see video/window_sdl.cpp).
int main(int argc, char** argv)
{
    return Video::RunMain(AppMain, argc, argv);
}
