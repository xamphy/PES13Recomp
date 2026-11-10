// Diagnostic override (PES_TRACE_LEVELFILL=0x<physical base> or "all"): the game's texture
// "fill mip level from memory" method (vtable slot 24 of the texture wrapper created by
// sub_821BD048): Lock (vfunc +100), memcpy (sub_82A39D28), Unlock (vfunc +104).
// Logs the wrapper, level, source pointer/size, the D3D texture's base address and whether
// the source and the texture hold non-zero data before/after.
#include <ppc_context.h>
#include <guest_memory.h>
#include <kernel/thread.h>
#include "write_watch.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <filesystem>
#include <atomic>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

extern "C" PPC_FUNC(__imp__sub_822A80F0);
extern "C" PPC_FUNC(__imp__sub_822454D8);
extern "C" PPC_FUNC(__imp__sub_8224A2B8);
extern "C" PPC_FUNC(__imp__sub_82244440);
extern "C" PPC_FUNC(__imp__sub_82231E38);

PPC_FUNC(sub_82231E38)
{
    const uint32_t task = ctx.r3.u32;
    const bool trace = std::getenv("PES_TRACE_IMAGE_CONVERT") != nullptr;
    if (trace)
        std::printf("[kit-task] task 0x%08X id 0x%08X colors %04X %04X %04X %04X %04X %04X\n", task,
            Memory::Load32(task + 104), Memory::Load16(task + 108), Memory::Load16(task + 110),
            Memory::Load16(task + 112), Memory::Load16(task + 114), Memory::Load16(task + 116), Memory::Load16(task + 118));
    __imp__sub_82231E38(ctx, base);
    if (trace)
    {
        const uint32_t image = Memory::Load32(task + 88);
        const uint32_t palette = image ? Memory::Load32(image + 4) : 0;
        uint32_t nz = 0;
        for (uint32_t i = 0; palette && i < 1024; i += 4) nz += Memory::Load32(palette + i) != 0;
        std::printf("[kit-task] task 0x%08X image 0x%08X palette 0x%08X entries %u\n", task, image, palette, nz);
    }
}

PPC_FUNC(sub_82244440)
{
    const uint32_t obj = ctx.r3.u32, input = ctx.r4.u32;
    const bool trace = std::getenv("PES_TRACE_IMAGE_CONVERT") != nullptr;
    __imp__sub_82244440(ctx, base);
    if (trace && Memory::Load16(obj + 26) == 512 && Memory::Load16(obj + 28) == 256)
    {
        const uint32_t palette = Memory::Load32(obj + 4);
        const uint32_t offset = Memory::Load16(input + 12);
        uint32_t nz = 0, originalNz = 0;
        for (uint32_t i = 0; palette && i < 1024; i += 4) nz += Memory::Load32(palette + i) != 0;
        for (uint32_t i = 0; offset && i < 1024; i += 4) originalNz += Memory::Load32(input + offset + i) != 0;
        std::printf("[image-load] obj 0x%08X input 0x%08X palette 0x%08X offset 0x%X original %u copied %u format %u\n",
            obj, input, palette, offset, originalNz, nz, Memory::Load32(obj + 20));
        if (palette && !nz)
        {
            uint32_t frame = ctx.r1.u32;
            const auto* thread = Kernel::CurrentGuestThread();
            std::printf("[image-load] zero palette stack:");
            for (int depth = 0; thread && depth < 16; ++depth)
            {
                if (frame < thread->stackLimit || frame >= thread->stackBase) break;
                const uint32_t back = Memory::Load32(frame);
                if (back <= frame || back >= thread->stackBase) break;
                std::printf(" %08X", Memory::Load32(back - 8));
                frame = back;
            }
            std::printf("\n");
            static bool armed = false;
            if (!armed && std::getenv("PES_WATCH_PALETTE"))
            {
                armed = true;
                WriteWatch::WatchStores(palette, 6);
            }
        }
    }
}

// Trace the CPU image conversion itself, before its temporary allocation is freed.
// Allocation addresses are reused, so matching a later image pointer to an older
// decompression output is not sufficient to establish that they are the same asset.
PPC_FUNC(sub_8224A2B8)
{
    static const bool enabled = std::getenv("PES_TRACE_IMAGE_CONVERT") != nullptr;
    if (!enabled) { __imp__sub_8224A2B8(ctx, base); return; }
    const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, palette = ctx.r5.u32;
    const uint32_t format = ctx.r6.u32, w = ctx.r7.u32 & 0xFFFF, h = ctx.r8.u32 & 0xFFFF;
    std::printf("[image-convert] dst 0x%08X src 0x%08X palette 0x%08X format %u %ux%u lr 0x%08X\n",
        dst, src, palette, format, w, h, uint32_t(ctx.lr));
    if (format == 0 && w == 512 && h == 256)
    {
        uint32_t pnz = 0, snz = 0;
        for (uint32_t i = 0; i < 256; ++i) pnz += Memory::Load32(palette + i * 4) != 0;
        for (uint32_t i = 0; i < w * h; ++i) snz += Memory::Load8(src + i) != 0;
        std::printf("[image-convert] source: %u non-zero indices, %u non-zero palette entries; palette:", snz, pnz);
        for (uint32_t i = 0; i < 8; ++i) std::printf(" %08X", Memory::Load32(palette + i * 4));
        std::printf("\n");
    }
    __imp__sub_8224A2B8(ctx, base);
    uint32_t nz = 0;
    const uint32_t bytes = uint32_t((uint64_t(w) * h * 4 < 0x80000) ? uint64_t(w) * h * 4 : 0x80000);
    for (uint32_t i = 0; dst && i < bytes; i += 4) nz += Memory::Load32(dst + i) != 0;
    std::printf("[image-convert] dst 0x%08X result %u/%u non-zero dwords\n", dst, nz, bytes / 4);
}

namespace
{
    int64_t Filter()
    {
        static const int64_t f = [] {
            const char* e = std::getenv("PES_TRACE_LEVELFILL");
            if (!e) return int64_t(-2);
            if (std::strcmp(e, "all") == 0) return int64_t(-1);
            return int64_t(std::strtoul(e, nullptr, 0));
        }();
        return f;
    }

    // Non-zero dwords in [guest, guest + bytes), stopping at uncommitted memory.
    uint32_t NonZero(uint32_t guest, uint32_t bytes)
    {
        uint32_t n = 0;
        uint32_t i = 0;
        while (i < bytes)
        {
            const uint8_t* p = Memory::Translate(guest + i);
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) break;
            const uint32_t avail = uint32_t(static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize - p);
            const uint32_t end = (bytes - i < avail ? bytes - i : avail) & ~3u;
            for (uint32_t k = 0; k < end; k += 4) n += *reinterpret_cast<const uint32_t*>(p + k) != 0;
            if (!end) break;
            i += end;
        }
        return n;
    }
}

PPC_FUNC(sub_822A80F0)
{
    const int64_t filter = Filter();
    static const bool traceSources = std::getenv("PES_TRACE_TEXTURE_SOURCES") != nullptr;
    if (filter == -2 && !traceSources)
    {
        __imp__sub_822A80F0(ctx, base);
        return;
    }
    const uint32_t wrapper = ctx.r3.u32, level = ctx.r4.u32, face = ctx.r5.u32, src = ctx.r6.u32, size = ctx.r7.u32;
    const uint32_t lr = uint32_t(ctx.lr);
    // The wrapper keeps the D3D texture at +32; its fetch constant starts at +0x1C (dword 1 holds
    // the base address >> 12 in bits 12-31).
    const uint32_t tex = Memory::Load32(wrapper + 32);
    const uint32_t fetch1 = tex ? Memory::Load32(tex + 0x1C + 4) : 0;
    const uint32_t texBase = fetch1 & 0xFFFFF000u;
    const uint32_t sourceWidth = Memory::Load32(wrapper + 12);
    const uint32_t sourceHeight = Memory::Load32(wrapper + 16);
    const bool dumpSource = traceSources && tex && src && !level && !face &&
        sourceWidth == 128 && sourceHeight == 128 && (fetch1 & 63u) == 6;
    if (dumpSource)
    {
        // Save the whole CPU image before the original method copies it and its caller frees it.
        // Most of a skin atlas can be transparent; inspecting only its first 4 KB is misleading.
        static std::atomic<uint32_t> sequence{0};
        const uint32_t seq = ++sequence;
        const uint32_t mipBase = Memory::Load32(tex + 0x1C + 20) & 0xFFFFF000u;
        std::printf("[texture-source] #%u base 0x%08X mip 0x%08X src 0x%08X 128x128 stride %u: %u/16384 non-zero dwords lr 0x%08X\n",
            seq, texBase, mipBase, src, size, NonZero(src, 128 * 128 * 4), lr);
        std::error_code directoryError;
        const auto directory = std::filesystem::path("userdata/gpt_capture") /
            ("sources_" + std::to_string(GetCurrentProcessId()));
        std::filesystem::create_directories(directory, directoryError);
        if (!directoryError)
        {
            char filename[96];
            std::snprintf(filename, sizeof(filename), "%04u_%08X_%08X.bin", seq, texBase, mipBase);
            if (FILE* file = std::fopen((directory / filename).string().c_str(), "wb"))
            {
                std::fwrite(Memory::Translate(src), 1, 128 * 128 * 4, file);
                std::fclose(file);
            }
        }
    }
    const bool log = filter == -1 || (texBase & 0x1FFFFFFFu) == (uint32_t(filter) & 0x1FFFFFFFu);
    const uint32_t checkBytes = size ? (size < 0x80000 ? size : 0x80000) : 4096;
    if (log)
    {
        std::printf("[levelfill] wrapper 0x%08X tex 0x%08X base 0x%08X level %u face %u src 0x%08X size 0x%X (src non-zero dwords %u/%u) lr 0x%08X\n",
            wrapper, tex, texBase, level, face, src, size, src ? NonZero(src, checkBytes) : 0, checkBytes / 4, lr);
        if (tex)
        {
            std::printf("[levelfill]   header:");
            for (uint32_t i = 0; i < 13; ++i) std::printf(" %08X", Memory::Load32(tex + i * 4));
            std::printf("\n");
        }
    }
    // A fill from an all-zero source: re-check the source later (filled afterwards = load race).
    if (log && src && NonZero(src, 4096) == 0)
        std::thread([src, checkBytes, texBase] {
            for (int seconds : { 2, 10 })
            {
                std::this_thread::sleep_for(std::chrono::seconds(seconds == 2 ? 2 : 8));
                std::printf("[levelfill]   zero source 0x%08X (texture 0x%08X) %d s later: %u/%u non-zero dwords\n",
                    src, texBase, seconds, NonZero(src, checkBytes), checkBytes / 4);
            }
        }).detach();
    __imp__sub_822A80F0(ctx, base);
    if (dumpSource && texBase)
        std::printf("[texture-source] base 0x%08X after copy: %u/16384 non-zero dwords\n", texBase,
            NonZero(0xA0000000u | (texBase & 0x1FFFFFFFu), 128 * 128 * 4));
    if (log && texBase)
        std::printf("[levelfill]   texture memory non-zero dwords after: %u/%u\n",
            NonZero(0xA0000000u | (texBase & 0x1FFFFFFFu), checkBytes), checkBytes / 4);
}

// The caller that creates a texture from a decoded image (r5 = image descriptor: +0 data buffer,
// +144 first level, +148 level count, +152/+154 width/height as u16). PES_TRACE_LEVELFILL enables
// it too: logs descriptors whose data is all zero, with the guest call stack.
PPC_FUNC(sub_822454D8)
{
    if (Filter() == -2)
    {
        __imp__sub_822454D8(ctx, base);
        return;
    }
    const uint32_t desc = ctx.r5.u32;
    const uint32_t data = Memory::Load32(desc);
    const uint32_t first = Memory::Load32(desc + 144), count = Memory::Load32(desc + 148);
    const uint32_t w = Memory::Load16(desc + 152), h = Memory::Load16(desc + 154);
    const uint32_t level0 = Memory::Load32(desc + 4 * first + 4 - 4 + 4); // pointer used for the first level
    const uint32_t nz = data ? NonZero(data, 4096) : 0;
    std::printf("[imgtex] desc 0x%08X data 0x%08X (%u/1024 non-zero) levels %u..%u %ux%u level ptr 0x%08X obj 0x%08X\n",
        desc, data, nz, first, count, w, h, level0, ctx.r3.u32);
    // PES_WATCH_ZERO_IMAGE="512x256": watch the data page of the first such all-zero image.
    static const char* watchDims = std::getenv("PES_WATCH_ZERO_IMAGE");
    if (nz == 0 && data && watchDims)
    {
        char dims[32];
        std::snprintf(dims, sizeof(dims), "%ux%u", w, h);
        if (std::strcmp(dims, watchDims) == 0) WriteWatch::WatchVirtualPage(data);
    }
    if (nz == 0)
        if (Kernel::GuestThread* t = Kernel::CurrentGuestThread())
        {
            uint32_t frame = ctx.r1.u32;
            std::printf("[imgtex]   zero image, guest stack (thread %u): lr 0x%08X", t->threadId, uint32_t(ctx.lr));
            for (int depth = 0; depth < 16; ++depth)
            {
                if (frame < t->stackLimit || frame >= t->stackBase) break;
                const uint32_t back = Memory::Load32(frame);
                if (back <= frame || back >= t->stackBase) break;
                std::printf(" <- 0x%08X", Memory::Load32(back - 8));
                frame = back;
            }
            std::printf("\n");
            std::printf("[imgtex]   desc dwords:");
            for (uint32_t i = 0; i < 40; ++i) std::printf(" %08X", Memory::Load32(desc + i * 4));
            std::printf("\n");
        }
    __imp__sub_822454D8(ctx, base);
}

// The game's WESYS decompression (sysFileUncompressThread): r3 = stream (+4 input data, +8
// compressed size, +12 output, +16 uncompressed size, +28 compressed flag). PES_TRACE_UNCOMPRESS
// logs every call whose result is empty or failed ("all" logs every call).
extern "C" PPC_FUNC(__imp__sub_82309C50);
PPC_FUNC(sub_82309C50)
{
    static const char* mode = std::getenv("PES_TRACE_UNCOMPRESS");
    if (!mode)
    {
        __imp__sub_82309C50(ctx, base);
        return;
    }
    const uint32_t stream = ctx.r3.u32;
    __imp__sub_82309C50(ctx, base);
    const uint32_t ok = ctx.r3.u32;
    const uint32_t in = Memory::Load32(stream + 4), inSize = Memory::Load32(stream + 8);
    const uint32_t out = Memory::Load32(stream + 12), outSize = Memory::Load32(stream + 16);
    const uint32_t flag = Memory::Load8(stream + 28);
    const uint32_t nz = out ? NonZero(out, outSize < 4096 ? outSize : 4096) : 0;
    if (out && outSize == 0x20430 && std::getenv("PES_TRACE_IMAGE_CONVERT"))
    {
        std::printf("[kit-uncompress] out 0x%08X palette entries %u header:", out, NonZero(out + 0x30, 1024));
        for (uint32_t i = 0; i < 12; ++i) std::printf(" %08X", Memory::Load32(out + i * 4));
        std::printf("\n");
        if (std::getenv("PES_WATCH_KIT_UNC")) WriteWatch::WatchStores(out, 6);
    }
    static uint32_t calls = 0, failures = 0;
    ++calls;
    const bool bad = !ok || (outSize && nz == 0);
    if (bad) ++failures;
    // PES_WATCH_UNC="lo,hi": after the first decompression with output size in [lo, hi), report the next
    // stores to its output page (who rewrites the decompressed data).
    static const char* watchUnc = std::getenv("PES_WATCH_UNC");
    if (watchUnc && out)
    {
        const uint32_t lo = uint32_t(std::strtoul(watchUnc, nullptr, 0));
        const char* comma = std::strchr(watchUnc, ',');
        const uint32_t hi = comma ? uint32_t(std::strtoul(comma + 1, nullptr, 0)) : 0xFFFFFFFFu;
        static bool armed = false;
        if (!armed && outSize >= lo && outSize < hi)
        {
            armed = true;
            std::printf("[uncompress] watching output 0x%08X (size 0x%X)\n", out, outSize);
            WriteWatch::WatchStores(out, 6);
        }
    }
    if (bad || std::strcmp(mode, "all") == 0)
        std::printf("[uncompress] #%u in 0x%08X size 0x%X -> out 0x%08X size 0x%X flag %u: %s, first 4 KB non-zero dwords %u (failures so far %u)\n",
            calls, in, inSize, out, outSize, flag, ok ? "ok" : "FAILED", nz, failures);
}
