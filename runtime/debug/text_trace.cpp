// Debug: logs the strings passed to the game's text tokenizer sub_8244A3F8(layout, utf8).
// Its loop never advances on an invalid UTF-8 sequence, so a bad string hangs the main thread.
#include <ppc_context.h>
#include <guest_memory.h>
#include <debug/write_watch.h>

#include <atomic>
#include <cstdio>
#include <string>

extern "C" PPC_FUNC(__imp__sub_8244A3F8);

namespace
{
    // Mirrors the decoder sub_82460740: a lead byte announces 1-3 extra bytes; anything
    // else above 0x7F (lone continuation byte, 0xF8+) makes the tokenizer spin.
    bool IsValidForTokenizer(uint32_t s, uint32_t& badOffset)
    {
        for (uint32_t i = 0; i < 4096; )
        {
            const uint8_t c = Memory::Load8(s + i);
            if (c == 0) return true;
            uint32_t extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 99;
            if (extra == 99) { badOffset = i; return false; }
            for (uint32_t k = 1; k <= extra; ++k)
                if ((Memory::Load8(s + i + k) & 0xC0) != 0x80) { badOffset = i; return false; }
            i += 1 + extra;
        }
        return true;
    }
}

PPC_FUNC(sub_8244A3F8)
{
    static std::atomic<uint32_t> calls{ 0 }, badLogs{ 0 };
    const uint32_t s = ctx.r4.u32;
    const uint32_t n = ++calls;
    uint32_t bad = 0;
    const bool valid = s && IsValidForTokenizer(s, bad);
    if (!valid ? badLogs++ < 10 : n <= 30)
    {
        std::string text, hex;
        for (uint32_t i = 0; s && i < 48; ++i)
        {
            const uint8_t c = Memory::Load8(s + i);
            if (c == 0) break;
            text += (c >= 32 && c < 127) ? char(c) : '.';
            char b[4];
            std::snprintf(b, sizeof(b), "%02X ", c);
            hex += b;
        }
        std::string stack;
        if (!valid)
        {
            // Return addresses saved at (caller frame - 8), frames linked through 0(r1).
            uint32_t frame = ctx.r1.u32;
            for (int depth = 0; depth < 12 && frame >= 0x70000000 && frame < 0x7F000000; ++depth)
            {
                const uint32_t back = Memory::Load32(frame);
                if (back <= frame) break;
                char b[16];
                std::snprintf(b, sizeof(b), " <- 0x%08X", Memory::Load32(back - 8));
                stack += b;
                frame = back;
            }
        }
        std::fprintf(stderr, "[text] #%u 0x%08X %s\"%s\"  %s (lr 0x%08X%s)\n", n, s, valid ? "" : "INVALID UTF-8 ", text.c_str(),
            valid ? "" : hex.c_str(), uint32_t(ctx.lr), stack.c_str());
    }
    __imp__sub_8244A3F8(ctx, base);
    if (n == 1 && valid)
        WriteWatch::WatchFirstPercent(s); // who overwrites this buffer with a raw format string?
}
