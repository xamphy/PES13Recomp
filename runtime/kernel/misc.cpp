// Small kernel/XAM imports: time, exceptions, system configuration, launch data,
// notifications, kernel pool.
#include "kernel.h"
#include "objects.h"
#include "thread.h"

#include <guest_memory.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

// __rdtsc(): the same counter recompiled `mftb` reads. On arm64 ppc_context.h (via kernel.h)
// defines it as the generic timer (cntvct_el0).
#ifdef _WIN32
#   include <intrin.h>
#elif defined(__x86_64__)
#   include <x86intrin.h>
#endif

namespace
{
    uint64_t HostFileTime()
    {
        using namespace std::chrono;
        constexpr uint64_t UnixEpochAsFileTime = 116444736000000000ull;
        return UnixEpochAsFileTime + duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() / 100;
    }

    // Recompiled `mftb` reads the host TSC, so the performance-counter frequency the
    // title sees must be the TSC frequency (not the console's 50 MHz time base).
    uint64_t MeasureTscFrequency()
    {
        using clock = std::chrono::steady_clock;
        const auto t0 = clock::now();
        const uint64_t c0 = __rdtsc();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const uint64_t c1 = __rdtsc();
        const auto t1 = clock::now();
        const double seconds = std::chrono::duration<double>(t1 - t0).count();
        return uint64_t(double(c1 - c0) / seconds);
    }

    uint64_t TscFrequency()
    {
        static const uint64_t frequency = MeasureTscFrequency();
        return frequency;
    }

    std::string GuestString(uint32_t addr, size_t max = 256)
    {
        std::string s;
        for (char c; addr && s.size() < max && (c = char(Memory::Load8(addr))) != 0; ++addr)
            s.push_back(c);
        return s;
    }

    constexpr uint32_t X_ERROR_SUCCESS   = 0;
    constexpr uint32_t X_ERROR_NOT_FOUND = 0x490;
}

// ------------------------------------------------------------------- time
// u64 KeQueryPerformanceFrequency()
GUEST_IMPORT(KeQueryPerformanceFrequency)
{
    ctx.r3.u64 = TscFrequency();
}

// void KeQuerySystemTime(u64* CurrentTime)
GUEST_IMPORT(KeQuerySystemTime)
{
    if (ctx.r3.u32) Memory::Store64(ctx.r3.u32, HostFileTime());
}

// TIME_FIELDS: u16 Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday
namespace
{
    constexpr int64_t TicksPerMs = 10000;
    constexpr int64_t TicksPerDay = 864000000000ll;

    // Days since 1601-01-01 <-> civil date (proleptic Gregorian).
    int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d)
    {
        y -= m <= 2;
        const int64_t era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = unsigned(y - era * 400);
        const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + int64_t(doe) - 719468 + 134774; // 134774 = days 1601-01-01 .. 1970-01-01
    }

    void CivilFromDays(int64_t z, int64_t& y, unsigned& m, unsigned& d)
    {
        z += 719468 - 134774;
        const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        const unsigned doe = unsigned(z - era * 146097);
        const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const unsigned mp = (5 * doy + 2) / 153;
        d = doy - (153 * mp + 2) / 5 + 1;
        m = mp < 10 ? mp + 3 : mp - 9;
        y = int64_t(yoe) + era * 400 + (m <= 2);
    }
}

// void RtlTimeToTimeFields(u64* Time, TIME_FIELDS* Fields)
GUEST_IMPORT(RtlTimeToTimeFields)
{
    const int64_t t = int64_t(Memory::Load64(ctx.r3.u32));
    const int64_t days = t / TicksPerDay;
    int64_t rem = t % TicksPerDay;
    int64_t year; unsigned month, day;
    CivilFromDays(days, year, month, day);
    const uint32_t f = ctx.r4.u32;
    Memory::Store16(f + 0x0, uint16_t(year));
    Memory::Store16(f + 0x2, uint16_t(month));
    Memory::Store16(f + 0x4, uint16_t(day));
    Memory::Store16(f + 0x6, uint16_t(rem / (3600 * 1000 * TicksPerMs))); rem %= 3600 * 1000 * TicksPerMs;
    Memory::Store16(f + 0x8, uint16_t(rem / (60 * 1000 * TicksPerMs)));   rem %= 60 * 1000 * TicksPerMs;
    Memory::Store16(f + 0xA, uint16_t(rem / (1000 * TicksPerMs)));        rem %= 1000 * TicksPerMs;
    Memory::Store16(f + 0xC, uint16_t(rem / TicksPerMs));
    Memory::Store16(f + 0xE, uint16_t((days + 1) % 7)); // 1601-01-01 was a Monday
}

// BOOLEAN RtlTimeFieldsToTime(TIME_FIELDS* Fields, u64* Time)
GUEST_IMPORT(RtlTimeFieldsToTime)
{
    const uint32_t f = ctx.r3.u32;
    const int64_t days = DaysFromCivil(Memory::Load16(f), Memory::Load16(f + 2), Memory::Load16(f + 4));
    const int64_t ms = ((int64_t(Memory::Load16(f + 6)) * 60 + Memory::Load16(f + 8)) * 60 + Memory::Load16(f + 0xA)) * 1000
        + Memory::Load16(f + 0xC);
    Memory::Store64(ctx.r4.u32, uint64_t(days * TicksPerDay + ms * TicksPerMs));
    ctx.r3.u64 = 1;
}

// void RtlFillMemoryUlong(void* Dest, u32 Length, u32 Pattern) - pattern is stored big-endian
GUEST_IMPORT(RtlFillMemoryUlong)
{
    for (uint32_t i = 0; i + 4 <= ctx.r4.u32; i += 4)
        Memory::Store32(ctx.r3.u32 + i, ctx.r5.u32);
}

// u32 RtlCompareMemoryUlong(void* Source, u32 Length, u32 Pattern) -> bytes matched
GUEST_IMPORT(RtlCompareMemoryUlong)
{
    uint32_t i = 0;
    while (i + 4 <= ctx.r4.u32 && Memory::Load32(ctx.r3.u32 + i) == ctx.r5.u32)
        i += 4;
    ctx.r3.u64 = i;
}

// -------------------------------------------------------------- exceptions
// void RtlRaiseException(EXCEPTION_RECORD*)
// EXCEPTION_RECORD: Code, Flags, Record, Address, NumberParameters, Information[15]
GUEST_IMPORT(RtlRaiseException)
{
    const uint32_t record = ctx.r3.u32;
    const uint32_t code = Memory::Load32(record + 0x00);
    if (code == 0x406D1388) // MSVC "SetThreadName": Information = { 0x1000, Name, ThreadId, Flags }
    {
        const uint32_t name = Memory::Load32(record + 0x18);
        const uint32_t threadId = Memory::Load32(record + 0x1C);
        const std::string threadName = GuestString(name, 64);
        Kernel::GuestThread* self = Kernel::CurrentGuestThread();
        if (self && (threadId == 0xFFFFFFFF || threadId == self->threadId))
            std::snprintf(self->name, sizeof(self->name), "%s", threadName.c_str());
        std::printf("[thread] thread %u named \"%s\"\n", self ? self->threadId : threadId, threadName.c_str());
        return;
    }
    std::fprintf(stderr, "[kernel] RtlRaiseException code 0x%08X at 0x%08X (ignored)\n", code, Memory::Load32(record + 0x0C));
}

// u32 RtlNtStatusToDosError(NTSTATUS)
GUEST_IMPORT(RtlNtStatusToDosError)
{
    const uint32_t status = ctx.r3.u32;
    uint32_t error;
    switch (status)
    {
    case 0x00000000: error = 0; break;
    case 0x00000102: error = 258; break;  // WAIT_TIMEOUT
    case 0xC0000008: error = 6; break;    // ERROR_INVALID_HANDLE
    case 0xC000000D: error = 87; break;   // ERROR_INVALID_PARAMETER
    case 0xC000000F:
    case 0xC0000034: error = 2; break;    // ERROR_FILE_NOT_FOUND
    case 0xC000003A: error = 3; break;    // ERROR_PATH_NOT_FOUND
    case 0xC0000017: error = 8; break;    // ERROR_NOT_ENOUGH_MEMORY
    case 0xC0000022: error = 5; break;    // ERROR_ACCESS_DENIED
    case 0xC0000035: error = 183; break;  // ERROR_ALREADY_EXISTS
    case 0xC0000135: error = 126; break;  // ERROR_MOD_NOT_FOUND
    case 0xC0000138: error = 182; break;  // ERROR_INVALID_ORDINAL
    case 0x80000006: error = 18; break;   // ERROR_NO_MORE_FILES
    case 0xC0000011: error = 38; break;   // ERROR_HANDLE_EOF
    default:
        error = (status & 0xFFFF0000) == 0x80070000 ? (status & 0xFFFF) : 317; // ERROR_MR_MID_NOT_FOUND
        break;
    }
    ctx.r3.u64 = error;
}

// ------------------------------------------------------- title / system info
GUEST_IMPORT(ExRegisterTitleTerminateNotification) {}

// BOOL XexCheckExecutablePrivilege(u32 Privilege)
GUEST_IMPORT(XexCheckExecutablePrivilege)
{
    ctx.r3.u64 = 0;
}

// void XGetVideoMode(X_VIDEO_MODE*) - 1280x720 widescreen HD, 60 Hz
GUEST_IMPORT(XGetVideoMode)
{
    const uint32_t m = ctx.r3.u32;
    std::memset(Memory::Translate(m), 0, 0x30);
    Memory::Store32(m + 0x00, 1280); // DisplayWidth
    Memory::Store32(m + 0x04, 720);  // DisplayHeight
    Memory::Store32(m + 0x08, 0);    // IsInterlaced
    Memory::Store32(m + 0x0C, 1);    // IsWidescreen
    Memory::Store32(m + 0x10, 1);    // IsHiDef
    float refresh = 60.0f;
    uint32_t refreshBits;
    std::memcpy(&refreshBits, &refresh, 4);
    Memory::Store32(m + 0x14, refreshBits); // RefreshRate
    Memory::Store32(m + 0x18, 1);    // VideoStandard (NTSC)
    Memory::Store32(m + 0x1C, 0x4A); // ReservedA (pixel rate class, as on hardware)
    Memory::Store32(m + 0x20, 0x01); // ReservedB
}

GUEST_IMPORT(XGetGameRegion) { ctx.r3.u64 = 0x02FE; } // PAL (the disc is the European release)
GUEST_IMPORT(XGetLanguage)   { ctx.r3.u64 = 1; }      // English
GUEST_IMPORT(XGetAVPack)     { ctx.r3.u64 = 6; }      // HDMI

// NTSTATUS ExGetXConfigSetting(u16 Category, u16 Setting, void* Buffer, u16 BufferSize, u16* RequiredSize)
GUEST_IMPORT(ExGetXConfigSetting)
{
    const uint32_t category = ctx.r3.u32 & 0xFFFF, setting = ctx.r4.u32 & 0xFFFF;
    const uint32_t buffer = ctx.r5.u32, bufferSize = ctx.r6.u32 & 0xFFFF, requiredPtr = ctx.r7.u32;

    uint32_t size = 0, value = 0;
    if (category == 0x0003) // XCONFIG_USER_CATEGORY
    {
        switch (setting)
        {
        case 0x0001: case 0x0006: case 0x0007:  // time zone biases
        case 0x0002: case 0x0003:                // time zone standard/daylight names (4 chars, empty)
        case 0x0004: case 0x0005:                // time zone standard/daylight dates
            size = 4; value = 0; break;
        case 0x0009: size = 4; value = 1; break;          // LANGUAGE: English
        case 0x000A: size = 4; value = 0x00040000; break; // VIDEO_FLAGS: widescreen
        case 0x000B: size = 4; value = 0x00010001; break; // AUDIO_FLAGS: Dolby Digital / stereo
        case 0x000C: size = 4; value = 0x40; break;       // RETAIL_FLAGS
        case 0x000E: size = 1; value = 0x67; break;       // COUNTRY: United Kingdom
        }
    }
    else if (category == 0x0002 && setting == 0x0002) // XCONFIG_CONSOLE_CATEGORY / AV region
    {
        size = 4; value = 0x00001000; // PAL-I
    }

    if (size == 0)
    {
        std::fprintf(stderr, "[kernel] ExGetXConfigSetting(category %u, setting 0x%X) - unknown\n", category, setting);
        ctx.r3.u64 = Kernel::STATUS_INVALID_PARAMETER;
        return;
    }

    if (requiredPtr) Memory::Store16(requiredPtr, uint16_t(size));
    if (buffer && bufferSize >= size)
    {
        if (size == 1) Memory::Store8(buffer, uint8_t(value));
        else Memory::Store32(buffer, value);
    }
    ctx.r3.u64 = Kernel::STATUS_SUCCESS;
}

// ----------------------------------------------------------- launch data
// DWORD XamLoaderGetLaunchDataSize(u32* Size)
GUEST_IMPORT(XamLoaderGetLaunchDataSize)
{
    if (ctx.r3.u32) Memory::Store32(ctx.r3.u32, 0);
    ctx.r3.u64 = X_ERROR_NOT_FOUND;
}

// DWORD XamLoaderGetLaunchData(void* Buffer, u32 Size)
GUEST_IMPORT(XamLoaderGetLaunchData)
{
    ctx.r3.u64 = X_ERROR_NOT_FOUND;
}

// (notifications: see xam_user.cpp)


// ------------------------------------------------------------- kernel pool
// Pool allocations come from the kernel region and are never reused yet.
GUEST_IMPORT(ExAllocatePoolTypeWithTag) { ctx.r3.u64 = Memory::AllocKernel(ctx.r3.u32 ? ctx.r3.u32 : 4, 16); }
GUEST_IMPORT(ExAllocatePoolWithTag)     { ctx.r3.u64 = Memory::AllocKernel(ctx.r3.u32 ? ctx.r3.u32 : 4, 16); }
GUEST_IMPORT(ExFreePool) {}
