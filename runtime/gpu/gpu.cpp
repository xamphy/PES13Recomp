#include "gpu.h"

#include <guest_memory.h>
#include <kernel/thread.h>
#include <kernel/interrupts.h>
#include <kernel/host_qos.h>
#include <debug/kick_trace.h>
#include <debug/experiments.h>
#include "xenos.h"
#include "shader_translate.h"
#include "renderer.h"
#include "precise_sleep.h"

#include <filesystem>
#include <set>
#include <string>
#include <debug/write_watch.h>
#include <video/presenter.h>

#include <algorithm>
#include <cstring>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
    // ------------------------------------------------------------------ registers
    constexpr uint32_t MmioBase = 0x7FC80000;
    constexpr uint32_t RegisterCount = 0x8000; // covers ALU/fetch constants (0x4000+) too

    // ------------------------------------------------------------------ ring buffer
    std::atomic<uint32_t> g_ringBase{ 0 };      // guest address
    std::atomic<uint32_t> g_ringSizeDwords{ 0 };
    std::atomic<uint32_t> g_rptrWriteBack{ 0 }; // guest address, 0 = disabled
    std::atomic<uint32_t> g_ringGeneration{ 0 }; // bumped on every ring (re)initialization
    std::atomic<bool> g_cpStarted{ false };
    std::thread::id g_cpThreadId;

    // ------------------------------------------------------------------ interrupts
    std::atomic<uint32_t> g_interruptCallback{ 0 };
    std::atomic<uint32_t> g_interruptUserData{ 0 };
    std::mutex g_interruptMutex;
    std::condition_variable g_interruptCv;
    std::deque<uint32_t> g_pendingInterrupts; // sources (1 = command processor)
    std::atomic<bool> g_interruptStarted{ false };

    // PES_CP_SPIN=1: CP waits (WAIT_REG_MEM, empty ring) spin with yield as before, instead of
    // spin-then-precise-sleep.
    // Control "cp spin|sleep" switches it at run time (same-scene A/B).
    std::atomic<bool> g_cpSpin{ [] { const char* v = std::getenv("PES_CP_SPIN"); return v && *v && *v != '0'; }() };

    // ------------------------------------------------------------------ stats
    std::atomic<uint64_t> g_frames{ 0 };
    // CP busy time per frame (wall time between swaps minus WAIT_REG_MEM and idle time), stats.
    uint64_t g_waitUsTotal = 0, g_idleUsTotal = 0; // CP thread only
    uint64_t g_busySumUs = 0, g_busyMaxUs = 0, g_busyFrames = 0, g_busyOver = 0;
    std::atomic<uint64_t> g_draws{ 0 };
    std::atomic<uint64_t> g_packets{ 0 };
    std::atomic<uint32_t> g_lastFenceAddr{ 0 };
    std::atomic<uint32_t> g_lastFenceValue{ 0 };
    std::atomic<uint64_t> g_waitUs{ 0 }, g_waitCount{ 0 }, g_waitMaxUs{ 0 }; // WAIT_REG_MEM time
    std::atomic<uint64_t> g_predicatedSkips{ 0 };

    enum Pm4Opcode : uint32_t
    {
        PM4_ME_INIT            = 0x48,
        PM4_NOP                = 0x10,
        PM4_INDIRECT_BUFFER_PFD = 0x37,
        PM4_INDIRECT_BUFFER    = 0x3F,
        PM4_WAIT_REG_MEM       = 0x3C,
        PM4_REG_RMW            = 0x21,
        PM4_COND_WRITE         = 0x45,
        PM4_EVENT_WRITE        = 0x46,
        PM4_EVENT_WRITE_SHD    = 0x58,
        PM4_EVENT_WRITE_CFL    = 0x59,
        PM4_EVENT_WRITE_EXT    = 0x5A,
        PM4_EVENT_WRITE_ZPD    = 0x5B,
        PM4_DRAW_INDX          = 0x22,
        PM4_DRAW_INDX_2        = 0x36,
        PM4_MEM_WRITE          = 0x3D,
        PM4_INTERRUPT          = 0x54,
        PM4_IM_LOAD            = 0x27,
        PM4_IM_LOAD_IMMEDIATE  = 0x2B,
        PM4_SET_CONSTANT       = 0x2D,
        PM4_LOAD_ALU_CONSTANT  = 0x2F,
        PM4_INVALIDATE_STATE   = 0x3B,
        PM4_SET_CONSTANT2      = 0x55,
        PM4_SET_SHADER_CONSTANTS = 0x56,
        PM4_SET_BIN_MASK       = 0x50,
        PM4_SET_BIN_SELECT     = 0x51,
        PM4_SET_BIN_MASK_LO    = 0x60,
        PM4_SET_BIN_MASK_HI    = 0x61,
        PM4_SET_BIN_SELECT_LO  = 0x62,
        PM4_SET_BIN_SELECT_HI  = 0x63,
        PM4_XE_SWAP            = 0x64, // written by VdSwap (runtime convention, as in Xenia)
    };

    // Predicated tiling: Direct3D replays a frame's command buffers once per screen tile,
    // and type-3 packets with header bit 0 set only execute when (bin select & bin mask)
    // != 0. Fences, swaps and interrupts are predicated so they run in one pass only.
    // Command processor thread only.
    uint64_t g_binMask = ~0ull;
    uint64_t g_binSelect = ~0ull;

    // Endian swap modes encoded in the low 2 bits of GPU addresses.
    uint32_t GpuSwap(uint32_t v, uint32_t endian)
    {
        switch (endian & 3)
        {
        case 1: return ((v >> 8) & 0x00FF00FF) | ((v << 8) & 0xFF00FF00); // 8-in-16
        case 2: return __builtin_bswap32(v);                               // 8-in-32
        case 3: return (v >> 16) | (v << 16);                              // 16-in-32
        default: return v;
        }
    }

    // GPU memory accesses: raw (host-order) dword at a physical address, then GpuSwap.
    uint32_t ReadGpuMemory(uint32_t addressWithEndian)
    {
        const uint32_t addr = Gpu::PhysicalToGuest(addressWithEndian & ~3u);
        return GpuSwap(*Memory::Translate<uint32_t>(addr), addressWithEndian & 3);
    }

    void WriteGpuMemory(uint32_t addressWithEndian, uint32_t value)
    {
        const uint32_t addr = Gpu::PhysicalToGuest(addressWithEndian & ~3u);
        *Memory::Translate<uint32_t>(addr) = GpuSwap(value, addressWithEndian & 3);
    }

    // ------------------------------------------------------------------ front buffer
    // Xenos 2D tiling (32x32-texel macro tiles, swizzled micro tiles). Offsets are in
    // bytes << log2Bpp, exactly as the hardware addresses them.
    uint32_t TiledOffset2DOuter(uint32_t y, uint32_t width, uint32_t log2Bpp)
    {
        const uint32_t macro = ((y / 32) * (width / 32)) << (log2Bpp + 7);
        const uint32_t micro = ((y & 6) << 2) << log2Bpp;
        return macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2Bpp)) + ((y & 1) << 4);
    }

    uint32_t TiledOffset2DInner(uint32_t x, uint32_t y, uint32_t log2Bpp, uint32_t baseOffset)
    {
        const uint32_t macro = (x / 32) << (log2Bpp + 7);
        const uint32_t micro = (x & 7) << log2Bpp;
        const uint32_t offset = baseOffset + (macro + ((micro & ~0xFu) << 1) + (micro & 0xF));
        return ((offset & ~0x1FFu) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F) + ((y & 16) << 7) +
            (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
    }

    std::vector<uint32_t> g_frontBuffer;
    uint32_t g_frontWidth = 0, g_frontHeight = 0;
    bool g_loggedFrontBuffer = false;

    // Converts the front buffer described by a texture fetch constant to RGBA8.
    void CaptureFrontBuffer(const uint32_t* fetch)
    {
        const uint32_t d0 = fetch[0], d1 = fetch[1], d2 = fetch[2];
        const bool tiled = (d0 >> 31) != 0;
        const uint32_t pitch = ((d0 >> 22) & 0x1FF) * 32;
        const uint32_t format = d1 & 0x3F;
        const uint32_t endian = (d1 >> 6) & 3;
        const uint32_t base = d1 & 0xFFFFF000;
        const uint32_t width = (d2 & 0x1FFF) + 1;
        const uint32_t height = ((d2 >> 13) & 0x1FFF) + 1;

        if (!g_loggedFrontBuffer)
        {
            g_loggedFrontBuffer = true;
            std::printf("[gpu] front buffer: %ux%u (pitch %u), format %u, %s, endian %u, at 0x%08X\n",
                width, height, pitch, format, tiled ? "tiled" : "linear", endian, base);
        }
        if (base == 0 || width > 4096 || height > 4096 || (format != 6 && format != 7))
            return; // only 8888 and 2_10_10_10 for now

        g_frontBuffer.resize(size_t(width) * height);
        g_frontWidth = width;
        g_frontHeight = height;
        const uint32_t rowPitch = std::max(pitch, width);
        const uint8_t* src = Memory::Translate(Gpu::PhysicalToGuest(base));

        for (uint32_t y = 0; y < height; ++y)
        {
            const uint32_t rowOffset = tiled ? TiledOffset2DOuter(y, rowPitch, 2) : 0;
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint32_t offset = tiled ? (TiledOffset2DInner(x, y, 2, rowOffset) >> 2) * 4 : (y * rowPitch + x) * 4;
                uint32_t raw;
                std::memcpy(&raw, src + offset, 4);
                const uint32_t v = GpuSwap(raw, endian); // logical 0xAARRGGBB / 2:10:10:10
                uint32_t r, g, b;
                if (format == 6)
                {
                    r = (v >> 16) & 0xFF; g = (v >> 8) & 0xFF; b = v & 0xFF;
                }
                else
                {
                    r = ((v >> 20) & 0x3FF) >> 2; g = ((v >> 10) & 0x3FF) >> 2; b = (v & 0x3FF) >> 2;
                }
                g_frontBuffer[size_t(y) * width + x] = r | (g << 8) | (b << 16) | 0xFF000000u; // RGBA bytes
            }
        }
    }

    void PostInterrupt(uint32_t source)
    {
        {
            std::lock_guard lock(g_interruptMutex);
            g_pendingInterrupts.push_back(source);
        }
        g_interruptCv.notify_one();
    }

    // ------------------------------------------------------------------ PM4 reader
    struct Reader
    {
        uint32_t base;       // guest address
        uint32_t sizeDwords; // ring size (wraps) or buffer size
        uint32_t pos;        // dword index
        uint32_t remaining;  // dwords left to read
        bool ring;

        uint32_t Read()
        {
            const uint32_t index = ring ? pos % sizeDwords : pos;
            ++pos;
            --remaining;
            return Memory::Load32(base + index * 4);
        }
    };

    void ExecuteBuffer(Reader& r, int depth);

    bool CompareFunc(uint32_t func, uint32_t value, uint32_t ref)
    {
        switch (func & 7)
        {
        case 0: return true;
        case 1: return value < ref;
        case 2: return value <= ref;
        case 3: return value == ref;
        case 4: return value != ref;
        case 5: return value >= ref;
        case 6: return value > ref;
        default: return false;
        }
    }

    // Recent type-3 packets, for the watchdog report.
    struct PacketRecord { uint32_t opcode, count, d0, d1, d2; };
    std::mutex g_historyMutex;
    PacketRecord g_history[24];
    uint32_t g_historyPos = 0;

    std::atomic<uint64_t> g_opcodeCounts[128];
    thread_local uint32_t t_currentIb = 0, t_currentIbSize = 0;

    // ------------------------------------------------------------------ shaders + draws
    // Shader microcode currently loaded (IM_LOAD / IM_LOAD_IMMEDIATE), as big-endian
    // instruction dwords in logical (host integer) order. Command processor thread only.
    struct ShaderSlot
    {
        uint32_t address = 0; // physical, 0 = immediate
        std::vector<uint32_t> code;
        uint64_t hash = 0;
    };
    ShaderSlot g_shaders[2]; // 0 = vertex, 1 = pixel

    uint64_t HashDwords(const std::vector<uint32_t>& v)
    {
        uint64_t h = 0xCBF29CE484222325ull; // FNV-1a
        for (uint32_t x : v)
            for (int b = 0; b < 4; ++b) { h ^= (x >> (24 - 8 * b)) & 0xFF; h *= 0x100000001B3ull; }
        return h;
    }

    // Constant-type offsets used by SET_CONSTANT / LOAD_ALU_CONSTANT.
    uint32_t ConstantBase(uint32_t type)
    {
        switch (type)
        {
        case 0: return XeReg::ALU_CONSTANTS;
        case 1: return XeReg::FETCH_CONSTANTS;
        case 2: return XeReg::BOOL_CONSTANTS;
        case 3: return XeReg::LOOP_CONSTANTS;
        case 4: return 0x2000; // context registers
        default: return 0;
        }
    }

    std::atomic<uint64_t> g_dumpFrame{ ~0ull };
    uint32_t g_drawsThisFrame = 0;

    // --dump-frame: translate each distinct shader of the frame once (microcode -> SPIR-V) and
    // save microcode disassembly + SPIR-V under userdata/shader_dump/.
    void TranslateForDump()
    {
        static std::set<uint64_t> done;
        for (int stage = 0; stage < 2; ++stage)
        {
            const ShaderSlot& s = g_shaders[stage];
            if (s.code.empty() || !done.insert(s.hash).second)
                continue;
            const auto result = ShaderTranslate::Translate(stage ? ShaderTranslate::Stage::Pixel : ShaderTranslate::Stage::Vertex,
                s.code.data(), s.code.size());
            std::printf("            [shader] %s %016llX: %s (%zu SPIR-V words)\n", stage ? "ps" : "vs",
                (unsigned long long)s.hash, result.ok ? "translated" : result.error.c_str(), result.spirv.size());
            std::error_code ec;
            const std::filesystem::path dir = "userdata/shader_dump";
            std::filesystem::create_directories(dir, ec);
            char name[64];
            std::snprintf(name, sizeof(name), "%016llX.%s", (unsigned long long)s.hash, stage ? "ps" : "vs");
            if (FILE* f = std::fopen((dir / (std::string(name) + ".ucode.txt")).string().c_str(), "wb"))
            {
                std::fwrite(result.ucodeDisassembly.data(), 1, result.ucodeDisassembly.size(), f);
                std::fclose(f);
            }
            if (!result.ok)
                continue;
            if (FILE* f = std::fopen((dir / (std::string(name) + ".spv")).string().c_str(), "wb"))
            {
                std::fwrite(result.spirv.data(), 4, result.spirv.size(), f);
                std::fclose(f);
            }
            const std::string text = ShaderTranslate::DisassembleSpirv(result.spirv);
            if (FILE* f = std::fopen((dir / (std::string(name) + ".spv.txt")).string().c_str(), "wb"))
            {
                std::fwrite(text.data(), 1, text.size(), f);
                std::fclose(f);
            }
        }
    }

    void DumpDraw(Xenos::DrawInitiator init, uint32_t indexBase, uint32_t indexSize)
    {
        using namespace XeReg;
        const auto R = [](uint32_t r) { return Gpu::ReadRegister(r); };
        const uint32_t mode = R(RB_MODECONTROL) & 7;
        const uint32_t surface = R(RB_SURFACE_INFO), color = R(RB_COLOR_INFO), depth = R(RB_DEPTH_INFO);
        std::printf("  draw %3u: %-10s %5u idx (%s%s) mode %u | surf pitch %u msaa %u | rt0 base %u fmt %u | depth base %u fmt %u | scissor %08X-%08X\n",
            g_drawsThisFrame, Xenos::PrimitiveName(init.Primitive()), init.IndexCount(),
            init.Source() == Xenos::SourceSelect::Dma ? "dma" : init.Source() == Xenos::SourceSelect::AutoIndex ? "auto" : "imm",
            init.Source() == Xenos::SourceSelect::Dma ? (init.Index32() ? "32" : "16") : "",
            mode, surface & 0x3FFF, (surface >> 16) & 3, color & 0xFFF, (color >> 16) & 0xF, depth & 0xFFF, (depth >> 16) & 1,
            R(PA_SC_WINDOW_SCISSOR_TL), R(PA_SC_WINDOW_SCISSOR_BR));
        if (init.Source() == Xenos::SourceSelect::Dma)
            std::printf("            indices at 0x%08X (%u)\n", indexBase, indexSize & 0xFFFFFF);
        std::printf("            window offset %08X  sc_mode_cntl %08X  vte %08X  depth info %08X  surface %08X\n",
            R(PA_SC_WINDOW_OFFSET), R(PA_SU_SC_MODE_CNTL), R(PA_CL_VTE_CNTL), R(RB_DEPTH_INFO), R(RB_SURFACE_INFO));
        std::printf("            vs %016llX (%zu dw @%08X)  ps %016llX (%zu dw @%08X)  prog_cntl %08X  depthctl %08X blend0 %08X colorctl %08X\n",
            (unsigned long long)g_shaders[0].hash, g_shaders[0].code.size(), g_shaders[0].address,
            (unsigned long long)g_shaders[1].hash, g_shaders[1].code.size(), g_shaders[1].address,
            R(SQ_PROGRAM_CNTL), R(RB_DEPTHCONTROL), R(RB_BLENDCONTROL0), R(RB_COLORCONTROL));
        TranslateForDump();
        if (mode == uint32_t(Xenos::EdramMode::Copy))
        {
            const uint32_t cc = R(RB_COPY_CONTROL), pitch = R(RB_COPY_DEST_PITCH), info = R(RB_COPY_DEST_INFO);
            std::printf("            RESOLVE src %u%s%s cmd %u -> 0x%08X pitch %u height %u fmt %u endian %u | clear color %08X depth %08X\n",
                cc & 7, (cc >> 8) & 1 ? " +clear-color" : "", (cc >> 9) & 1 ? " +clear-depth" : "", (cc >> 20) & 3,
                R(RB_COPY_DEST_BASE), pitch & 0x3FFF, (pitch >> 16) & 0x3FFF, (info >> 7) & 0x3F, info & 7,
                R(RB_COLOR_CLEAR), R(RB_DEPTH_CLEAR));
        }
        // Vertex fetch constants (type 3) referenced by vfetch: address (dwords) + size.
        for (uint32_t slot = 0; slot < 96; ++slot)
        {
            const uint32_t d0 = R(FETCH_CONSTANTS + slot * 2), d1 = R(FETCH_CONSTANTS + slot * 2 + 1);
            if ((d0 & 3) != 3 || !(d0 & ~3u)) continue;
            const uint32_t addr = d0 & ~3u, size = (d1 >> 2) & 0xFFFFFF;
            std::printf("            vfetch %2u: 0x%08X, %u dwords, endian %u | data:", slot, addr, size, d1 & 3);
            for (uint32_t i = 0; i < 8 && i < size; ++i)
                std::printf(" %08X", Memory::Load32(Gpu::PhysicalToGuest(addr) + i * 4));
            std::printf("\n");
        }
        // Texture fetch constants that look populated (type 2 = texture in dword 0 bits 0:1).
        for (uint32_t slot = 0; slot < 32; ++slot)
        {
            const uint32_t base = FETCH_CONSTANTS + slot * 6;
            const uint32_t d0 = R(base), d1 = R(base + 1), d2 = R(base + 2);
            if ((d0 & 3) == 2 && (d1 & 0xFFFFF000))
                std::printf("            tex %2u: base 0x%08X fmt %2u %ux%u %s\n", slot, d1 & 0xFFFFF000, d1 & 0x3F,
                    (d2 & 0x1FFF) + 1, ((d2 >> 13) & 0x1FFF) + 1, (d0 >> 31) ? "tiled" : "linear");
        }
    }

    void OnDraw(Xenos::DrawInitiator init, uint32_t indexBase, uint32_t indexSize)
    {
        ++g_draws;
        Gpu::WriteRegister(XeReg::VGT_DRAW_INITIATOR, init.value);
        if (g_frames.load() == g_dumpFrame.load())
            DumpDraw(init, indexBase, indexSize);
        ++g_drawsThisFrame;
        Renderer::Draw(indexBase, indexSize);
    }

    void ExecuteType3(uint32_t opcode, const std::vector<uint32_t>& d, int depth)
    {
        g_opcodeCounts[opcode & 0x7F].fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard lock(g_historyMutex);
            g_history[g_historyPos++ % 24] = { opcode, uint32_t(d.size()),
                d.size() > 0 ? d[0] : 0, d.size() > 1 ? d[1] : 0, d.size() > 2 ? d[2] : 0 };
        }
        switch (opcode)
        {
        case PM4_INDIRECT_BUFFER:
        case PM4_INDIRECT_BUFFER_PFD:
        {
            if (d.size() < 2 || depth > 4) break;
            Reader ib{ Gpu::PhysicalToGuest(d[0] & ~3u), d[1] & 0xFFFFF, 0, d[1] & 0xFFFFF, false };
            const uint32_t outerIb = t_currentIb;
            t_currentIb = d[0];
            t_currentIbSize = d[1];
            ExecuteBuffer(ib, depth + 1);
            t_currentIb = outerIb;
            break;
        }

        case PM4_MEM_WRITE:
            for (size_t i = 1; i < d.size(); ++i)
                WriteGpuMemory((d[0] & ~3u) + uint32_t(i - 1) * 4 | (d[0] & 3), d[i]);
            break;

        case PM4_EVENT_WRITE_SHD:
        {
            // { initiator, address, value }: fence write. Bit 31 of the initiator
            // selects the swap counter instead of the literal value.
            if (d.size() < 3) break;
            Gpu::WriteRegister(GpuReg::VGT_EVENT_INITIATOR, d[0] & 0x3F);
            const uint32_t value = (d[0] >> 31) ? uint32_t(g_frames.load()) : d[2];
            WriteGpuMemory(d[1], value);
            g_lastFenceAddr = d[1];
            g_lastFenceValue = value;

            break;
        }

        case PM4_EVENT_WRITE_EXT:
        {
            // { initiator, address }: screen extents for hi-z/occlusion; report the full surface.
            if (d.size() < 2) break;
            Gpu::WriteRegister(GpuReg::VGT_EVENT_INITIATOR, d[0] & 0x3F);
            const uint32_t addr = Gpu::PhysicalToGuest(d[1] & ~3u);
            const uint16_t extents[6] = { 0, 1280 >> 3, 0, 720 >> 3, 0, 1 };
            for (int i = 0; i < 6; ++i)
                Memory::Store16(addr + i * 2, extents[i]);
            break;
        }

        case PM4_EVENT_WRITE:
            if (!d.empty()) Gpu::WriteRegister(GpuReg::VGT_EVENT_INITIATOR, d[0] & 0x3F);
            break;

        case PM4_WAIT_REG_MEM:
        {
            // { wait_info, poll address/register, reference, mask, wait interval }
            if (d.size() < 5) break;
            const bool memory = (d[0] & 0x10) != 0;
            const auto start = std::chrono::steady_clock::now();
            // Experiment: Direct3D's GPU-side vsync wait polls scratch slot 1 (cleared by the
            // CPU's vblank handler). --no-vsync-wait skips it.
            if (Experiments::g_noVsyncWait && memory && (d[1] & ~3u) == Gpu::ReadRegister(GpuReg::SCRATCH_ADDR) + 4)
                break;
            for (;;)
            {
                const uint32_t value = memory ? ReadGpuMemory(d[1]) : Gpu::ReadRegister(d[1] & 0x7FFF);
                if (CompareFunc(d[0], value & d[3], d[2]))
                    break;
                const auto waited = std::chrono::steady_clock::now() - start;
                if (waited > std::chrono::seconds(2))
                {
                    std::fprintf(stderr, "[gpu] WAIT_REG_MEM on %s 0x%X (want func %u ref 0x%X mask 0x%X, have 0x%X) timed out - continuing\n",
                        memory ? "memory" : "register", d[1], d[0] & 7, d[2], d[3], value);
                    break;
                }
                // Short waits (fences) spin; long ones (D3D's vsync slot, up to a frame) poll every
                // 200 us instead of burning a core. PES_CP_SPIN=1 = always spin (old behaviour).
                if (g_cpSpin || waited < std::chrono::microseconds(150))
                    std::this_thread::yield();
                else
                    PreciseSleepUs(200);
            }
            const uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
            g_waitUs += us;
            g_waitUsTotal += us;
            ++g_waitCount;
            if (us > g_waitMaxUs) g_waitMaxUs = us;
            if (us > 3000)
            {
                static uint32_t longLogs = 0;
                if (longLogs++ < 30)
                    std::fprintf(stderr, "[gpu] long WAIT_REG_MEM %llu us: info 0x%X %s 0x%X ref 0x%X mask 0x%X\n", (unsigned long long)us,
                        d[0], memory ? "mem" : "reg", d[1], d[2], d[3]);
            }
            break;
        }

        case PM4_COND_WRITE:
        {
            // { wait_info, poll addr/reg, reference, mask, write addr/reg, write data }
            if (d.size() < 6) break;
            const bool pollMemory = (d[0] & 0x10) != 0;
            const uint32_t value = pollMemory ? ReadGpuMemory(d[1]) : Gpu::ReadRegister(d[1] & 0x7FFF);
            if (CompareFunc(d[0], value & d[3], d[2]))
            {
                if (d[0] & 0x100) WriteGpuMemory(d[4], d[5]);
                else Gpu::WriteRegister(d[4] & 0x7FFF, d[5]);
            }
            break;
        }

        case PM4_REG_RMW:
        {
            if (d.size() < 3) break;
            const uint32_t reg = d[0] & 0x1FFF;
            uint32_t value = Gpu::ReadRegister(reg);
            value &= (d[0] >> 31) ? Gpu::ReadRegister(d[1] & 0x1FFF) : d[1];
            value |= ((d[0] >> 30) & 1) ? Gpu::ReadRegister(d[2] & 0x1FFF) : d[2];
            Gpu::WriteRegister(reg, value);
            break;
        }

        case PM4_INTERRUPT:
            PostInterrupt(1);
            break;

        case PM4_SET_BIN_MASK:
            if (d.size() >= 2) g_binMask = (uint64_t(d[0]) << 32) | d[1];
            break;
        case PM4_SET_BIN_SELECT:
            if (d.size() >= 2) g_binSelect = (uint64_t(d[0]) << 32) | d[1];
            break;
        case PM4_SET_BIN_MASK_LO:
            if (!d.empty()) g_binMask = (g_binMask & 0xFFFFFFFF00000000ull) | d[0];
            break;
        case PM4_SET_BIN_MASK_HI:
            if (!d.empty()) g_binMask = (g_binMask & 0xFFFFFFFFull) | (uint64_t(d[0]) << 32);
            break;
        case PM4_SET_BIN_SELECT_LO:
            if (!d.empty()) g_binSelect = (g_binSelect & 0xFFFFFFFF00000000ull) | d[0];
            break;
        case PM4_SET_BIN_SELECT_HI:
            if (!d.empty()) g_binSelect = (g_binSelect & 0xFFFFFFFFull) | (uint64_t(d[0]) << 32);
            break;

        case PM4_SET_CONSTANT:
        {
            // { offset_type, values... }: index 0:10, type 16:23
            if (d.empty()) break;
            const uint32_t index = ConstantBase((d[0] >> 16) & 0xFF) + (d[0] & 0x7FF);
            for (size_t i = 1; i < d.size(); ++i)
                Gpu::WriteRegister(index + uint32_t(i - 1), d[i]);
            break;
        }

        case PM4_SET_CONSTANT2:
        case PM4_SET_SHADER_CONSTANTS:
        {
            // { register index (0:15), values... }
            if (d.empty()) break;
            const uint32_t index = d[0] & 0xFFFF;
            for (size_t i = 1; i < d.size(); ++i)
                Gpu::WriteRegister(index + uint32_t(i - 1), d[i]);
            break;
        }

        case PM4_LOAD_ALU_CONSTANT:
        {
            // { physical address, offset_type, size in dwords }
            if (d.size() < 3) break;
            const uint32_t src = Gpu::PhysicalToGuest(d[0] & 0x3FFFFFFF);
            const uint32_t index = ConstantBase((d[1] >> 16) & 0xFF) + (d[1] & 0x7FF);
            const uint32_t count = d[2] & 0xFFF;
            for (uint32_t i = 0; i < count; ++i)
                Gpu::WriteRegister(index + i, Memory::Load32(src + i * 4));
            break;
        }

        case PM4_IM_LOAD:
        {
            // { address | shader type (0 vertex, 1 pixel), start << 16 | size in dwords }
            if (d.size() < 2) break;
            ShaderSlot& s = g_shaders[d[0] & 1];
            s.address = d[0] & ~3u;
            const uint32_t size = d[1] & 0xFFFF;
            s.code.resize(size);
            const uint32_t src = Gpu::PhysicalToGuest(s.address);
            for (uint32_t i = 0; i < size; ++i)
                s.code[i] = Memory::Load32(src + i * 4);
            s.hash = HashDwords(s.code);
            Renderer::SetShader(int(d[0] & 1), s.code.data(), uint32_t(s.code.size()), s.hash);
            break;
        }

        case PM4_IM_LOAD_IMMEDIATE:
        {
            // { shader type, start << 16 | size in dwords, microcode... }
            if (d.size() < 2) break;
            ShaderSlot& s = g_shaders[d[0] & 1];
            s.address = 0;
            const uint32_t size = std::min<uint32_t>(d[1] & 0xFFFF, uint32_t(d.size() - 2));
            s.code.assign(d.begin() + 2, d.begin() + 2 + size);
            s.hash = HashDwords(s.code);
            Renderer::SetShader(int(d[0] & 1), s.code.data(), uint32_t(s.code.size()), s.hash);
            break;
        }

        case PM4_INVALIDATE_STATE:
            break;

        case PM4_DRAW_INDX:
        {
            // { viz query, VGT_DRAW_INITIATOR, [index base, index size (DMA only)] }
            if (d.size() < 2) break;
            const Xenos::DrawInitiator init{ d[1] };
            const bool dma = init.Source() == Xenos::SourceSelect::Dma && d.size() >= 4;
            OnDraw(init, dma ? d[2] : 0, dma ? d[3] : 0);
            break;
        }

        case PM4_DRAW_INDX_2:
            // { VGT_DRAW_INITIATOR, [immediate indices...] }
            if (!d.empty()) OnDraw(Xenos::DrawInitiator{ d[0] }, 0, 0);
            break;

        case PM4_XE_SWAP:
            // [0] 'SWAP', [1] front buffer address, [2] width, [3] height, [4..9] fetch constant
            if (g_frames.load() == g_dumpFrame.load())
            {
                std::printf("---- end of frame %llu: %u draws, swap of 0x%08X ----\n", (unsigned long long)g_frames.load(), g_drawsThisFrame,
                    d.size() > 1 ? d[1] : 0);
                std::fflush(stdout);
            }
            else if (g_frames.load() + 1 == g_dumpFrame.load())
                std::printf("---- frame %llu draws ----\n", (unsigned long long)g_dumpFrame.load());
            g_drawsThisFrame = 0;
            {
                static auto lastSwap = std::chrono::steady_clock::now();
                static uint64_t lastWait = 0, lastIdle = 0;
                const auto now = std::chrono::steady_clock::now();
                const int64_t wall = std::chrono::duration_cast<std::chrono::microseconds>(now - lastSwap).count();
                const int64_t busy = std::max<int64_t>(0, wall - int64_t(g_waitUsTotal - lastWait) - int64_t(g_idleUsTotal - lastIdle));
                lastSwap = now; lastWait = g_waitUsTotal; lastIdle = g_idleUsTotal;
                g_busySumUs += uint64_t(busy); ++g_busyFrames;
                if (uint64_t(busy) > g_busyMaxUs) g_busyMaxUs = uint64_t(busy);
                if (busy > 16667) ++g_busyOver;
            }
            ++g_frames;
            // PES_DUMP_AT=<seconds>: dump the first frame after that time (like --dump-frame).
            {
                static const double dumpAt = [] { const char* e = std::getenv("PES_DUMP_AT"); return e ? std::atof(e) : -1.0; }();
                static const auto start = std::chrono::steady_clock::now();
                static bool armed = dumpAt >= 0;
                if (armed && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= dumpAt)
                {
                    armed = false;
                    g_dumpFrame = g_frames.load() + 1;
                }
            }
            if (d.size() >= 10)
            {
                // Front buffer from the fetch constant: base in dword 1, size in dword 2.
                const uint32_t frontBase = d[5] & 0xFFFFF000u;
                const uint32_t frontW = (d[6] & 0x1FFF) + 1, frontH = ((d[6] >> 13) & 0x1FFF) + 1;
                if (!Renderer::Swap(frontBase, frontW, frontH, (d[7] >> 1) & 0xFFF))
                {
                    CaptureFrontBuffer(&d[4]);
                    if (g_frontWidth)
                        Video::Present(g_frontBuffer.data(), g_frontWidth, g_frontHeight);
                }
            }
            break;

        default:
            // State/constant/shader packets: ignored until the renderer exists.
            break;
        }
    }

    void ExecuteBuffer(Reader& r, int depth)
    {
        while (r.remaining > 0)
        {
            const uint32_t header = r.Read();
            ++g_packets;
            if (header == 0 || header == 0x0BADF00D)
                continue;

            switch (header >> 30)
            {
            case 0: // register writes: base index, count
            {
                const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
                const uint32_t index = header & 0x7FFF;
                const bool oneRegister = (header >> 15) & 1;
                if (count > r.remaining) { r.remaining = 0; return; }
                for (uint32_t i = 0; i < count; ++i)
                    Gpu::WriteRegister(oneRegister ? index : index + i, r.Read());
                break;
            }
            case 1: // two register writes
                if (r.remaining < 2) { r.remaining = 0; return; }
                Gpu::WriteRegister(header & 0x7FF, r.Read());
                Gpu::WriteRegister((header >> 11) & 0x7FF, r.Read());
                break;
            case 2: // nop
                break;
            case 3:
            {
                const uint32_t opcode = (header >> 8) & 0x7F;
                const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
                if (count > r.remaining)
                {
                    std::fprintf(stderr, "[gpu] truncated type-3 packet (opcode 0x%02X, %u dwords, %u left)\n", opcode, count, r.remaining);
                    r.remaining = 0;
                    return;
                }
                std::vector<uint32_t> data(count);
                for (uint32_t i = 0; i < count; ++i)
                    data[i] = r.Read();
                // Experiment PES_NO_DRAW_PREDICATION: draws run in every tile pass (fences/swaps stay predicated).
                static const bool drawsUnpredicated = std::getenv("PES_NO_DRAW_PREDICATION") != nullptr;
                const bool isDraw = opcode == PM4_DRAW_INDX || opcode == PM4_DRAW_INDX_2;
                if ((header & 1) && (g_binSelect & g_binMask) == 0 && !(drawsUnpredicated && isDraw))
                {
                    ++g_predicatedSkips;
                    break; // predicated off for this tile pass
                }
                ExecuteType3(opcode, data, depth);
                break;
            }
            }
        }
    }

    // ------------------------------------------------------------------ threads
#ifdef _WIN32
    extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentThreadId();
#endif
    void CommandProcessorThread()
    {
        g_cpThreadId = std::this_thread::get_id();
        Kernel::SetHostThreadQos(Kernel::HostQos::Interactive); // Apple: keep it on a performance core
#ifdef _WIN32
        std::printf("[gpu] command processor: host thread %lu\n", GetCurrentThreadId());
#endif
        uint32_t rptr = 0;
        uint32_t idleSpins = 0;
        auto lastReport = std::chrono::steady_clock::now();
        uint64_t lastFrames = 0;

        uint32_t generation = g_ringGeneration.load();
        for (;;)
        {
            if (const uint32_t g = g_ringGeneration.load(); g != generation)
            {
                generation = g;
                rptr = 0; // ring was reinitialized
            }
            const uint32_t size = g_ringSizeDwords.load();
            const uint32_t wptr = size ? Gpu::ReadRegister(GpuReg::CP_RB_WPTR) % size : 0;
            if (size && wptr != rptr)
            {
                // Diagnostics: a write pointer that moves "backwards" would make us replay
                // nearly the whole ring. Log it (rate-limited).
                const uint32_t pending = (wptr + size - rptr) % size;
                static uint32_t backwardsLogs = 0;
                if (pending > size - 64 && backwardsLogs < 20) // small step back (a big jump is a legitimate wrap)
                {
                    ++backwardsLogs;
                    std::fprintf(stderr, "[gpu] WPTR moved backwards? rptr %u -> wptr %u (%u dwords would be replayed)\n",
                        rptr, wptr, pending);
                    if (backwardsLogs <= 3)
                    {
                        WriteWatch::PrintRecent();
                        KickTrace::Dump();
                    }
                }
                KickTrace::RecordCpRead(rptr, wptr);
                Reader ring{ g_ringBase.load(), size, rptr, (wptr + size - rptr) % size, true };
                const auto batchStart = std::chrono::steady_clock::now();
                ExecuteBuffer(ring, 0);
                if (const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - batchStart).count(); ms > 100)
                {
                    static uint32_t slowLogs = 0;
                    if (slowLogs++ < 20)
                        std::fprintf(stderr, "[gpu] slow batch: %u -> %u took %lld ms\n", rptr, wptr, (long long)ms);
                }
                rptr = wptr;
                // (CP_RB_RPTR register not updated: Direct3D reads the read pointer from the
                // write-back slot, and the register shares a page with WPTR - see write_watch.cpp.)
                if (const uint32_t wb = g_rptrWriteBack.load())
                    Memory::Store32(wb, rptr);
                idleSpins = 0;
            }
            else
            {
                const auto idleStart = std::chrono::steady_clock::now();
                if (++idleSpins < (g_cpSpin ? 2000u : 200u))
                    std::this_thread::yield();
                else if (g_cpSpin)
                    std::this_thread::sleep_for(std::chrono::microseconds(500));
                else
                    PreciseSleepUs(250);
                g_idleUsTotal += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - idleStart).count();
            }

            const auto now = std::chrono::steady_clock::now();
            if (now - lastReport >= std::chrono::seconds(1))
            {
                const uint64_t frames = g_frames.load();
                const uint32_t globalDevice = Memory::Load32(0x820006BC);
                const uint32_t device = globalDevice ? Memory::Load32(globalDevice) : 0;
                const uint32_t fencePtr = device ? Memory::Load32(device + 11024) : 0;
                std::printf("[gpu] %llu fps, %llu frames, %llu draws, %llu packets | d3d fence cpu 0x%X gpu 0x%X\n",
                    (unsigned long long)(frames - lastFrames), (unsigned long long)frames,
                    (unsigned long long)g_draws.load(), (unsigned long long)g_packets.load(),
                    device ? Memory::Load32(device + 11036) : 0, fencePtr ? Memory::Load32(fencePtr) : 0);
                if (g_busyFrames)
                    std::printf("[gpu] CP busy per frame: avg %.1f ms, max %.1f ms, %llu of %llu frames over 16.7 ms\n",
                        g_busySumUs / 1000.0 / g_busyFrames, g_busyMaxUs / 1000.0, (unsigned long long)g_busyOver, (unsigned long long)g_busyFrames);
                {
                    static uint64_t perfLastDraws = 0;
                    const uint64_t perfDraws = g_draws.load();
                    Video::PerfStats ps;
                    ps.fps = uint32_t(frames - lastFrames);
                    ps.drawsPerFrame = uint32_t(frames > lastFrames ? (perfDraws - perfLastDraws) / (frames - lastFrames) : 0);
                    ps.cpAvgMs = g_busyFrames ? float(g_busySumUs / 1000.0 / g_busyFrames) : 0.0f;
                    ps.cpMaxMs = float(g_busyMaxUs / 1000.0);
                    ps.slowFrames = uint32_t(g_busyOver);
                    Video::SetPerfStats(ps);
                    perfLastDraws = perfDraws;
                }
                g_busySumUs = g_busyMaxUs = g_busyFrames = g_busyOver = 0;
                std::printf("[gpu] WAIT_REG_MEM: %llu waits, %llu ms total, max %llu ms\n",
                    (unsigned long long)g_waitCount.exchange(0), (unsigned long long)(g_waitUs.exchange(0) / 1000),
                    (unsigned long long)(g_waitMaxUs.exchange(0) / 1000));
                {
                    std::printf("[gpu] predicated packets skipped: %llu\n", (unsigned long long)g_predicatedSkips.exchange(0));
                    std::printf("[gpu] opcodes:");
                    for (uint32_t op = 0; op < 128; ++op)
                        if (const uint64_t c = g_opcodeCounts[op].exchange(0)) std::printf(" %02X:%llu", op, (unsigned long long)c);
                    std::printf("\n");
                }
                static uint64_t lastDraws = 0;
                const uint64_t draws = g_draws.load();
                char title[128];
                std::snprintf(title, sizeof(title), "PES 2013 (recompiled) - %llu fps, %llu draws/frame",
                    (unsigned long long)(frames - lastFrames),
                    (unsigned long long)(frames > lastFrames ? (draws - lastDraws) / (frames - lastFrames) : 0));
                Video::SetTitle(title);
                lastDraws = draws;
                lastFrames = frames;
                lastReport = now;
            }
        }
    }

    void InterruptThread()
    {
        Kernel::GuestThread* thread = Kernel::CreateGuestThread(0x10000);
        if (!thread)
            return;
        std::snprintf(thread->name, sizeof(thread->name), "[gpu interrupts]");
        // Graphics interrupts are delivered on hardware thread 2 (as in Xenia). Direct3D's
        // handler checks the current processor, and PM4_INTERRUPT packets target CPU 2.
        Memory::Store8(thread->pcr + 0x10C, 2);

        using clock = std::chrono::steady_clock;
        const auto vblankPeriod = std::chrono::microseconds(16667);
        auto nextVblank = clock::now() + vblankPeriod;

        for (;;)
        {
            uint32_t source;
            {
                std::unique_lock lock(g_interruptMutex);
                g_interruptCv.wait_until(lock, nextVblank, [] { return !g_pendingInterrupts.empty(); });
                if (!g_pendingInterrupts.empty())
                {
                    source = g_pendingInterrupts.front();
                    g_pendingInterrupts.pop_front();
                }
                else
                {
                    source = 0; // vblank
                    nextVblank += vblankPeriod;
                    if (clock::now() > nextVblank) // fell behind: don't burst
                        nextVblank = clock::now() + vblankPeriod;
                }
            }

            if (const uint32_t callback = g_interruptCallback.load())
            {
                // Like a real interrupt on CPU 2: never overlaps guest code running on CPU 2 with
                // interrupts disabled (Direct3D's worker thread relies on this).
                std::lock_guard cpuLock(Kernel::CpuInterruptLock(2));
                Kernel::CallGuestFunction(*thread, callback, source, g_interruptUserData.load(), 0, 0, true);
            }
        }
    }
}

namespace Gpu
{
    void SetCpSpin(bool spin) { g_cpSpin = spin; std::printf("[gpu] CP waits: %s\n", spin ? "spin" : "spin+sleep"); }

    void Init()
    {
        Memory::Commit(MmioBase, RegisterCount * 4);
        // Values Direct3D reads back during init (from Xenia).
        WriteRegister(GpuReg::RB_EDRAM_TIMING, 0x08100748);
        WriteRegister(GpuReg::RB_BC_CONTROL, 0x0000200E);
        WriteRegister(GpuReg::D1MODE_V_COUNTER, 0x000002D0);
        WriteRegister(GpuReg::D1MODE_INTERRUPT_STATUS, 1);   // vblank
        WriteRegister(GpuReg::D1MODE_VIEWPORT_SIZE, 0x050002D0); // 1280x720
    }

    uint32_t ReadRegister(uint32_t index)
    {
        return index < RegisterCount ? Memory::Load32(MmioBase + index * 4) : 0;
    }

    void WriteRegister(uint32_t index, uint32_t value)
    {
        if (index >= RegisterCount)
            return;
        // The register file doubles as the CPU's MMIO view, so command-stream register
        // writes must not clobber the ring-buffer control registers the CPU owns
        // (a stray write to CP_RB_WPTR made the CP replay the whole ring).
        if (index == GpuReg::CP_RB_WPTR && std::this_thread::get_id() == g_cpThreadId)
        {
            static int logged = 0;
            if (logged++ < 5)
                std::fprintf(stderr, "[gpu] command stream wrote CP_RB_WPTR = 0x%X - ignored\n", value);
            return;
        }
        Memory::Store32(MmioBase + index * 4, value);
        Renderer::WriteRegister(index, value);

        // Scratch registers: when the matching SCRATCH_UMSK bit is set, the CP also
        // writes the value to SCRATCH_ADDR + n*4. Direct3D uses this for fences.
        if (index >= GpuReg::SCRATCH_REG0 && index <= GpuReg::SCRATCH_REG0 + 7)
        {
            const uint32_t n = index - GpuReg::SCRATCH_REG0;
            if (ReadRegister(GpuReg::SCRATCH_UMSK) & (1u << n))
                Memory::Store32(PhysicalToGuest(ReadRegister(GpuReg::SCRATCH_ADDR) + n * 4), value);
        }
    }

    void InitializeRingBuffer(uint32_t physicalAddress, uint32_t sizeLog2)
    {
        // (Re)initializing the ring resets the command processor: read and write pointers
        // start over at 0. Direct3D initializes the ring twice during device creation; keeping
        // the old read pointer made the CP replay the whole ring.
        g_ringSizeDwords = 0; // pauses the CP thread while we reset
        WriteRegister(GpuReg::CP_RB_WPTR, 0);
        WriteRegister(GpuReg::CP_RB_RPTR, 0);
        if (const uint32_t wb = g_rptrWriteBack.load())
            Memory::Store32(wb, 0);
        ++g_ringGeneration;
        g_ringBase = PhysicalToGuest(physicalAddress);
        g_ringSizeDwords = (1u << (sizeLog2 + 3)) / 4;
        std::printf("[gpu] ring buffer at 0x%08X, %u bytes\n", g_ringBase.load(), g_ringSizeDwords.load() * 4);
        if (!g_cpStarted.exchange(true))
            std::thread(CommandProcessorThread).detach();
    }

    void EnableReadPointerWriteBack(uint32_t physicalAddress, uint32_t blockSizeLog2)
    {
        g_rptrWriteBack = PhysicalToGuest(physicalAddress);
        std::printf("[gpu] read pointer write-back at 0x%08X\n", g_rptrWriteBack.load());
    }

    void SetInterruptCallback(uint32_t callback, uint32_t userData)
    {
        g_interruptCallback = callback;
        g_interruptUserData = userData;
        std::printf("[gpu] interrupt callback 0x%08X (user data 0x%08X)\n", callback, userData);
        if (callback && !g_interruptStarted.exchange(true))
            std::thread(InterruptThread).detach();
    }

    uint64_t FrameCount() { return g_frames.load(); }

    void DumpFrame(uint64_t frame) { g_dumpFrame = frame; }

    void DumpState()
    {
        std::printf("---- gpu ----\n");
        std::printf("  ring 0x%08X (%u dwords)  WPTR %u  RPTR %u  write-back 0x%08X = %u\n",
            g_ringBase.load(), g_ringSizeDwords.load(), ReadRegister(GpuReg::CP_RB_WPTR), ReadRegister(GpuReg::CP_RB_RPTR),
            g_rptrWriteBack.load(), g_rptrWriteBack.load() ? Memory::Load32(g_rptrWriteBack.load()) : 0);
        std::printf("  fence area (phys 0x10000):");
        for (uint32_t i = 0; i < 8; ++i)
            std::printf(" %08X", Memory::Load32(PhysicalToGuest(0x10000 + i * 4)));
        std::printf("\n  last EVENT_WRITE_SHD: addr 0x%08X value 0x%08X\n", g_lastFenceAddr.load(), g_lastFenceValue.load());
        // D3D device (pointer published through the VdGlobalDevice kernel variable, import slot 0x820006BC).
        const uint32_t globalDevice = Memory::Load32(0x820006BC);
        const uint32_t device = globalDevice ? Memory::Load32(globalDevice) : 0;
        if (device)
        {
            const uint32_t fencePtr = Memory::Load32(device + 11024);
            std::printf("  D3D device 0x%08X: fence counter (+11036) 0x%X, GPU fence ptr (+11024) 0x%08X -> 0x%X, +11016 0x%X\n",
                device, Memory::Load32(device + 11036), fencePtr, fencePtr ? Memory::Load32(fencePtr) : 0, Memory::Load32(device + 11016));
        }
        const uint32_t scratchAddr = ReadRegister(GpuReg::SCRATCH_ADDR);
        std::printf("  SCRATCH_UMSK 0x%X  SCRATCH_ADDR 0x%08X  regs:", ReadRegister(GpuReg::SCRATCH_UMSK), scratchAddr);
        for (uint32_t i = 0; i < 8; ++i)
            std::printf(" %X", ReadRegister(GpuReg::SCRATCH_REG0 + i));
        std::printf("\n  last packets (oldest first):\n");
        std::lock_guard lock(g_historyMutex);
        for (uint32_t i = 0; i < 24; ++i)
        {
            const PacketRecord& p = g_history[(g_historyPos + i) % 24];
            if (p.count)
                std::printf("    op 0x%02X x%-4u %08X %08X %08X\n", p.opcode, p.count, p.d0, p.d1, p.d2);
        }
        std::printf("-------------\n");
    }
}
