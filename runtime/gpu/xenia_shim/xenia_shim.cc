// Implementations of the few Xenia base-library symbols the shader translator uses,
// replacing Xenia's logging, filesystem, cvar and memory modules.
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <utility>

// Normally defined in xenia/gpu/gpu_flags.cc / shader.cc's users.
DEFINE_path(dump_shaders, "", "", "GPU");
DEFINE_bool(half_pixel_offset, true, "", "GPU"); // Xenia default (gpu_flags.cc)

namespace xe
{
    namespace logging
    {
        // Translator messages: errors and warnings only.
        bool ShouldLog(LogLevel log_level) { return log_level <= LogLevel::Warning; }

        namespace internal
        {
            std::pair<char*, size_t> GetThreadBuffer()
            {
                thread_local char buffer[16384];
                return { buffer, sizeof(buffer) - 1 };
            }

            void AppendLogLine(LogLevel log_level, const char prefix_char, size_t written)
            {
                auto [buffer, size] = GetThreadBuffer();
                std::fprintf(stderr, "[xenia %c] %.*s\n", prefix_char, int(written < size ? written : size), buffer);
            }
        }
    }

    namespace filesystem
    {
        FILE* OpenFile(const std::filesystem::path& path, std::string_view mode)
        {
#ifdef _WIN32
            const std::wstring wmode(mode.begin(), mode.end());
            return _wfopen(path.c_str(), wmode.c_str());
#else
            return std::fopen(path.c_str(), std::string(mode).c_str());
#endif
        }
    }

    void copy_and_swap_32_aligned(void* dest, const void* src, size_t count)
    {
        auto* d = static_cast<uint32_t*>(dest);
        auto* s = static_cast<const uint32_t*>(src);
        for (size_t i = 0; i < count; ++i)
            d[i] = __builtin_bswap32(s[i]);
    }

    void copy_and_swap_32_unaligned(void* dest, const void* src, size_t count)
    {
        auto* d = static_cast<uint8_t*>(dest);
        auto* s = static_cast<const uint8_t*>(src);
        for (size_t i = 0; i < count; ++i)
        {
            uint32_t v;
            std::memcpy(&v, s + i * 4, 4);
            v = __builtin_bswap32(v);
            std::memcpy(d + i * 4, &v, 4);
        }
    }

}
