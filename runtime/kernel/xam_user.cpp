// XAM user profile, notifications, content (saves), enumerators and system UI.
// Behaviour follows Xenia's xam_user/xam_content/xam_enum/xam_ui/xam_notify (BSD-3):
//   - one local profile, signed in for player 1 (index 0)
//   - content packages are directories under <user root>/content/<title id>/<type>/<name>/,
//     mounted as "<root>:" while open
//   - system UIs complete without being shown (Xenia's headless mode): the device selector
//     picks the hard drive, message boxes return their default button
#include "kernel.h"
#include "objects.h"
#include "xex_module.h"

#include <guest_memory.h>
#include <fs/vfs.h>
#include <video/presenter.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
    // ---------------------------------------------------------------- results
    constexpr uint32_t X_ERROR_SUCCESS = 0;
    constexpr uint32_t X_ERROR_FILE_NOT_FOUND = 2;
    constexpr uint32_t X_ERROR_PATH_NOT_FOUND = 3;
    constexpr uint32_t X_ERROR_ACCESS_DENIED = 5;
    constexpr uint32_t X_ERROR_INVALID_HANDLE = 6;
    constexpr uint32_t X_ERROR_NO_MORE_FILES = 0x12;
    constexpr uint32_t X_ERROR_INVALID_PARAMETER = 0x57;
    constexpr uint32_t X_ERROR_INSUFFICIENT_BUFFER = 0x7A;
    constexpr uint32_t X_ERROR_ALREADY_EXISTS = 0xB7;
    constexpr uint32_t X_ERROR_IO_INCOMPLETE = 0x3E4;
    constexpr uint32_t X_ERROR_IO_PENDING = 0x3E5;
    constexpr uint32_t X_ERROR_DEVICE_NOT_CONNECTED = 0x48F;
    constexpr uint32_t X_ERROR_NO_SUCH_USER = 0x525;
    constexpr uint32_t X_ERROR_FUNCTION_FAILED = 0x65B;
    constexpr uint32_t X_E_INVALIDARG = 0x80070057;
    constexpr uint32_t X_E_NO_SUCH_USER = 0x80070525;
    constexpr uint32_t X_E_DEVICE_NOT_CONNECTED = 0x8007048F;

    uint32_t HResultFromWin32(uint32_t e) { return e ? (0x80070000u | (e & 0xFFFF)) : 0; }

    // Arguments beyond r10 are on the caller's stack (Xenia: r1 + 0x54 + (n - 8) * 8).
    uint32_t StackArg(PPCContext& ctx, uint32_t index) { return Memory::Load32(ctx.r1.u32 + 0x54 + (index - 8) * 8); }

    // ---------------------------------------------------------------- profile
    constexpr uint64_t kXuid = 0xB13EBABEBABEBABEull; // Xenia's (passes titles' XUID masks)
    constexpr const char* kGamertag = "Player";
    constexpr uint32_t kUserIndex = 0;

    uint32_t TitleId()
    {
        const uint32_t info = Kernel::GetXexHeaderField(Kernel::ExecutableModule().headerAddr, 0x00040006); // EXECUTION_INFO
        return info ? Memory::Load32(info + 0xC) : 0;
    }

    std::string Hex8(uint32_t v)
    {
        char b[16];
        std::snprintf(b, sizeof(b), "%08X", v);
        return b;
    }

    // ---------------------------------------------------------------- overlapped
    // XAM_OVERLAPPED: result, length, context, event, completion routine, completion context, extended error.
    void CompleteOverlapped(uint32_t overlapped, uint32_t result, uint32_t extendedError, uint32_t length)
    {
        Memory::Store32(overlapped + 0x00, result);
        Memory::Store32(overlapped + 0x04, length);
        Memory::Store32(overlapped + 0x18, extendedError);
        if (const uint32_t event = Memory::Load32(overlapped + 0x0C))
            if (Kernel::ObjectPtr ev = Kernel::LookupHandle(event))
                Kernel::SetEvent(*ev);
    }

    // Xenia's CompleteOverlappedImmediate: some titles treat length != 0 as failure.
    uint32_t CompleteImmediate(uint32_t overlapped, uint32_t result)
    {
        CompleteOverlapped(overlapped, result, result, result ? 0xFFFFFFFFu : 0);
        return X_ERROR_IO_PENDING;
    }

    // ---------------------------------------------------------------- notifications
    struct Listener
    {
        uint64_t mask = 0;
        uint32_t maxVersion = 0;
        Kernel::ObjectPtr event; // the listener handle refers to this manual-reset event
        std::deque<std::pair<uint32_t, uint32_t>> queue;
    };
    std::mutex g_notifyMutex;
    std::map<uint32_t, std::shared_ptr<Listener>> g_listeners; // by handle
    bool g_notifiedStartup = false;

    void Enqueue(Listener& l, uint32_t id, uint32_t data)
    {
        const uint32_t maskIndex = (id >> 25) & 0x3F, version = (id >> 16) & 0x1FF;
        if (!(l.mask & (1ull << maskIndex)) || version > l.maxVersion)
            return;
        l.queue.emplace_back(id, data);
        Kernel::SetEvent(*l.event);
    }

    void Broadcast(uint32_t id, uint32_t data)
    {
        std::lock_guard lock(g_notifyMutex);
        for (auto& [handle, l] : g_listeners)
            Enqueue(*l, id, data);
    }

    constexpr uint32_t XN_SYS_UI = 0x9, XN_SYS_SIGNINCHANGED = 0xA, XN_SYS_INPUTDEVICESCHANGED = 0x12,
        XN_SYS_INPUTDEVICECONFIGCHANGED = 0x13;

    // Runs a "system UI" headlessly: UI-open notification, the action, overlapped completion
    // (on a helper thread when asynchronous), UI-closed notification.
    uint32_t DispatchHeadless(uint32_t overlapped, std::function<uint32_t()> action)
    {
        if (!overlapped)
        {
            Broadcast(XN_SYS_UI, 1);
            const uint32_t result = action();
            Broadcast(XN_SYS_UI, 0);
            return result;
        }
        Memory::Store32(overlapped, X_ERROR_IO_PENDING);
        std::thread([overlapped, action]
        {
            Broadcast(XN_SYS_UI, 1);
            const uint32_t result = action();
            CompleteOverlapped(overlapped, result, HResultFromWin32(result), 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            Broadcast(XN_SYS_UI, 0);
        }).detach();
        return X_ERROR_IO_PENDING;
    }

    // ---------------------------------------------------------------- profile settings
    // Setting id: id 0:13, unk 14:15, size 16:27, type 28:31.
    enum SettingType : uint8_t { kContent = 0, kInt32 = 1, kInt64 = 2, kDouble = 3, kWString = 4, kFloat = 5, kBinary = 6, kDateTime = 7 };

    struct Setting
    {
        SettingType type;
        int64_t value = 0;           // int32/int64/float bits
        std::u16string text;         // wstring
        std::vector<uint8_t> binary; // binary
        bool isSet = true;
    };

    bool IsTitleSpecific(uint32_t id) { return (id & 0x3F00) == 0x3F00; }

    std::map<uint32_t, Setting>& Settings()
    {
        static std::map<uint32_t, Setting> s = [] {
            std::map<uint32_t, Setting> m;
            auto i32 = [&](uint32_t id, int32_t v) { m[id] = Setting{ kInt32, v }; };
            float rep = 0.0f;
            int32_t repBits;
            std::memcpy(&repBits, &rep, 4);
            // Xenia's defaults (UserProfile::UserProfile).
            i32(0x10040002, 0); i32(0x10040003, 3); i32(0x10040004, 0); i32(0x10040005, 0); i32(0x10040006, 0xFA);
            m[0x5004000B] = Setting{ kFloat, repBits };
            i32(0x1004000C, 0); i32(0x1004000D, 0); i32(0x1004000E, 0x64);
            m[0x402C0011] = Setting{ kWString, 0, u"" };
            i32(0x10040012, 1); i32(0x10040013, 0); i32(0x10040015, 0); i32(0x10040018, 0);
            i32(0x1004001D, int32_t(0xFFFF0000u)); i32(0x1004001E, int32_t(0xFF00FF00u));
            i32(0x10040022, 1); i32(0x10040023, 0); i32(0x10040024, 0); i32(0x10040026, 0); i32(0x10040027, 0);
            i32(0x10040028, 0); i32(0x10040029, 0); i32(0x10040038, 0); i32(0x10040039, 0);
            m[0x4064000F] = Setting{ kWString, 0, u"gamercard_picture_key" };
            for (uint32_t id : { 0x63E83FFFu, 0x63E83FFEu, 0x63E83FFDu }) // title specific 1-3
            {
                Setting b{ kBinary };
                b.isSet = false;
                m[id] = b;
            }
            return m;
        }();
        return s;
    }

    std::filesystem::path ProfileDir() { return Fs::UserRoot() / "content" / Hex8(TitleId()) / "profile"; }

    // Title-specific binary settings persist in <user root>/content/<title>/profile/<id>.
    void LoadTitleSetting(uint32_t id, Setting& s)
    {
        if (!IsTitleSpecific(id)) return;
        FILE* f = std::fopen((ProfileDir() / Hex8(id)).string().c_str(), "rb");
        if (!f) return;
        std::fseek(f, 0, SEEK_END);
        s.binary.resize(size_t(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        std::fread(s.binary.data(), 1, s.binary.size(), f);
        std::fclose(f);
        s.isSet = true;
    }

    // ---------------------------------------------------------------- enumerators
    struct Enumerator
    {
        std::vector<uint8_t> items;
        uint32_t itemSize = 0, perEnumerate = 1, count = 0, current = 0;
    };
    std::mutex g_enumMutex;
    std::map<uint32_t, std::shared_ptr<Enumerator>> g_enumerators;

    uint32_t CreateEnumeratorHandle(std::shared_ptr<Enumerator> e)
    {
        auto object = std::make_shared<Kernel::KObject>();
        const uint32_t handle = Kernel::CreateHandle(object);
        std::lock_guard lock(g_enumMutex);
        g_enumerators[handle] = std::move(e);
        return handle;
    }

    // ---------------------------------------------------------------- content
    // XCONTENT_DATA (0x134): device id, content type, display name u16[128] (BE), file name char[42], pad.
    constexpr uint32_t kContentDataSize = 0x134;
    constexpr uint32_t kHddDeviceId = 1, kOddDeviceId = 2;

    struct ContentData
    {
        uint32_t deviceId = 0, contentType = 0;
        std::string fileName;
    };

    ContentData ReadContentData(uint32_t p)
    {
        ContentData c;
        c.deviceId = Memory::Load32(p);
        c.contentType = Memory::Load32(p + 4);
        for (uint32_t i = 0; i < 42; ++i)
        {
            const char ch = char(Memory::Load8(p + 0x108 + i));
            if (!ch) break;
            c.fileName += ch;
        }
        return c;
    }

    std::filesystem::path PackagePath(const ContentData& c)
    {
        return Fs::UserRoot() / "content" / Hex8(TitleId()) / Hex8(c.contentType) / c.fileName;
    }

    std::mutex g_contentMutex;
    std::map<std::string, std::filesystem::path> g_openContent; // root name -> host path

    std::string ReadGuestString(uint32_t p)
    {
        std::string s;
        for (uint32_t i = 0; p && i < 260; ++i)
        {
            const char c = char(Memory::Load8(p + i));
            if (!c) break;
            s += c;
        }
        return s;
    }

    uint32_t MountPackage(const std::string& root, const std::filesystem::path& path)
    {
        std::lock_guard lock(g_contentMutex);
        if (g_openContent.count(root)) return X_ERROR_ALREADY_EXISTS;
        g_openContent[root] = path;
        Fs::MountContent(root, path);
        return X_ERROR_SUCCESS;
    }
}

// ============================================================== notifications
// HANDLE XamNotifyCreateListener(u64 AreaMask, u32 MaxVersion)
GUEST_IMPORT(XamNotifyCreateListener)
{
    auto l = std::make_shared<Listener>();
    l->mask = ctx.r3.u64;
    l->maxVersion = std::min<uint32_t>(ctx.r4.u32, 10);
    l->event = Kernel::CreateEvent(true, false);
    const uint32_t handle = Kernel::CreateHandle(l->event);
    std::lock_guard lock(g_notifyMutex);
    g_listeners[handle] = l;
    // Titles expect a few notifications on startup, only for the first system listener.
    if (!g_notifiedStartup && (l->mask & 1))
    {
        g_notifiedStartup = true;
        Enqueue(*l, XN_SYS_UI, 1);
        Enqueue(*l, XN_SYS_UI, 0);
        Enqueue(*l, XN_SYS_SIGNINCHANGED, 1);
        Enqueue(*l, XN_SYS_SIGNINCHANGED, 1);
        Enqueue(*l, XN_SYS_INPUTDEVICESCHANGED, 0);
        Enqueue(*l, XN_SYS_INPUTDEVICESCHANGED, 0);
        Enqueue(*l, XN_SYS_INPUTDEVICECONFIGCHANGED, 0);
        Enqueue(*l, XN_SYS_INPUTDEVICECONFIGCHANGED, 0);
    }
    ctx.r3.u64 = handle;
}

// BOOL XNotifyGetNext(HANDLE, u32 MatchId, u32* Id, u32* Param)
GUEST_IMPORT(XNotifyGetNext)
{
    const uint32_t handle = ctx.r3.u32, matchId = ctx.r4.u32, idPtr = ctx.r5.u32, paramPtr = ctx.r6.u32;
    if (paramPtr) Memory::Store32(paramPtr, 0);
    ctx.r3.u64 = 0;
    if (!idPtr) return;
    Memory::Store32(idPtr, 0);
    std::lock_guard lock(g_notifyMutex);
    auto it = g_listeners.find(handle);
    if (it == g_listeners.end()) return;
    Listener& l = *it->second;
    for (auto q = l.queue.begin(); q != l.queue.end(); ++q)
    {
        if (matchId && q->first != matchId) continue;
        Memory::Store32(idPtr, q->first);
        if (paramPtr) Memory::Store32(paramPtr, q->second);
        l.queue.erase(q);
        if (l.queue.empty()) Kernel::ResetEvent(*l.event);
        ctx.r3.u64 = 1;
        return;
    }
}

// ============================================================== overlapped
// DWORD XamGetOverlappedResult(XAM_OVERLAPPED*, u32* Length, BOOL Wait)
GUEST_IMPORT(XamGetOverlappedResult)
{
    const uint32_t ov = ctx.r3.u32, lengthPtr = ctx.r4.u32;
    uint32_t result = Memory::Load32(ov);
    if (result == X_ERROR_IO_PENDING)
    {
        const uint32_t event = Memory::Load32(ov + 0x0C);
        Kernel::ObjectPtr ev = event ? Kernel::LookupHandle(event) : nullptr;
        if (!ev)
            result = X_ERROR_IO_INCOMPLETE;
        else
        {
            Kernel::WaitForObjects({ ev }, false, nullptr);
            result = Memory::Load32(ov);
        }
    }
    if (result == X_ERROR_SUCCESS && lengthPtr)
        Memory::Store32(lengthPtr, Memory::Load32(ov + 4));
    ctx.r3.u64 = result;
}

// ============================================================== users
// u32 XamUserGetSigninState(u32 UserIndex): 0 none, 1 signed in locally, 2 Live
GUEST_IMPORT(XamUserGetSigninState)
{
    const uint32_t user = ctx.r3.u32;
    ctx.r3.u64 = (user == kUserIndex || user == 0xFF) ? 1 : 0;
}

// HRESULT XamUserGetSigninInfo(u32 UserIndex, u32 Flags, X_USER_SIGNIN_INFO*)
// X_USER_SIGNIN_INFO (0x28): u64 xuid, u32 unk08, u32 signin_state, u32 unk10, u32 unk14, char name[16]
GUEST_IMPORT(XamUserGetSigninInfo)
{
    const uint32_t user = ctx.r3.u32, info = ctx.r5.u32;
    if (!info) { ctx.r3.u64 = X_E_INVALIDARG; return; }
    for (uint32_t i = 0; i < 0x28; i += 4) Memory::Store32(info + i, 0);
    if (user != kUserIndex) { ctx.r3.u64 = X_E_NO_SUCH_USER; return; }
    Memory::Store64(info, kXuid);
    Memory::Store32(info + 0xC, 1);
    for (uint32_t i = 0; kGamertag[i] && i < 15; ++i) Memory::Store8(info + 0x18 + i, uint8_t(kGamertag[i]));
    ctx.r3.u64 = 0;
}

// DWORD XamUserGetName(u32 UserIndex, char* Buffer, u32 Length)
GUEST_IMPORT(XamUserGetName)
{
    const uint32_t user = ctx.r3.u32, buffer = ctx.r4.u32, length = ctx.r5.u32;
    if (user >= 4) { ctx.r3.u64 = X_E_INVALIDARG; return; }
    if (user != kUserIndex) { ctx.r3.u64 = X_E_NO_SUCH_USER; return; }
    const uint32_t n = std::min<uint32_t>(length, 16);
    for (uint32_t i = 0; i < n; ++i)
    {
        const char c = i + 1 < n ? kGamertag[std::min<size_t>(i, std::strlen(kGamertag))] : 0;
        Memory::Store8(buffer + i, uint8_t(c));
        if (!c) break;
    }
    ctx.r3.u64 = 0;
}

// HRESULT XamUserGetXUID(u32 UserIndex, u32 TypeMask, u64* Xuid)
GUEST_IMPORT(XamUserGetXUID)
{
    const uint32_t user = ctx.r3.u32, xuidPtr = ctx.r5.u32;
    if (!xuidPtr) { ctx.r3.u64 = X_E_INVALIDARG; return; }
    if (user >= 4) { ctx.r3.u64 = X_E_INVALIDARG; return; }
    const bool ok = user == kUserIndex;
    Memory::Store64(xuidPtr, ok ? kXuid : 0);
    ctx.r3.u64 = ok ? 0 : X_E_NO_SUCH_USER;
}

// DWORD XamUserCheckPrivilege(u32 UserIndex, u32 Mask, u32* Result) - deny online privileges.
GUEST_IMPORT(XamUserCheckPrivilege)
{
    const uint32_t user = ctx.r3.u32, out = ctx.r5.u32;
    if (user != 0xFF && user >= 4) { ctx.r3.u64 = X_ERROR_INVALID_PARAMETER; return; }
    if (user != 0xFF && user != kUserIndex) { ctx.r3.u64 = X_ERROR_NO_SUCH_USER; return; }
    if (out) Memory::Store32(out, 0);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// DWORD XamUserAreUsersFriends(u32 UserIndex, u64* Xuids, u32 Count, BOOL* Result, XAM_OVERLAPPED*)
GUEST_IMPORT(XamUserAreUsersFriends)
{
    if (ctx.r6.u32) Memory::Store32(ctx.r6.u32, 0);
    ctx.r3.u64 = ctx.r7.u32 ? CompleteImmediate(ctx.r7.u32, X_ERROR_SUCCESS) : X_ERROR_SUCCESS;
}

// DWORD XamUserGetDeviceContext(u32 UserIndex, u32 Unknown, u32* Out)
GUEST_IMPORT(XamUserGetDeviceContext)
{
    const uint32_t user = ctx.r3.u32;
    if (ctx.r5.u32) Memory::Store32(ctx.r5.u32, 0);
    ctx.r3.u64 = (user == 0 || (user & 0xFF) == 0xFF) ? 0 : X_E_DEVICE_NOT_CONNECTED;
}

// DWORD XamUserReadProfileSettings(u32 TitleId, u32 UserIndex, u32 XuidCount, u64* Xuids,
//     u32 SettingCount, u32* SettingIds, u32* BufferSize, void* Buffer, XAM_OVERLAPPED*)
// Output: { u32 count, u32 settings_ptr } + count x X_USER_PROFILE_SETTING (0x28) + string/binary data.
GUEST_IMPORT(XamUserReadProfileSettings)
{
    const uint32_t userIndex = ctx.r4.u32, xuidCount = ctx.r5.u32, settingCount = ctx.r7.u32, ids = ctx.r8.u32;
    const uint32_t bufferSizePtr = ctx.r9.u32, buffer = ctx.r10.u32, overlapped = StackArg(ctx, 8);
    if (settingCount < 1 || settingCount > 32 || !bufferSizePtr) { ctx.r3.u64 = X_ERROR_INVALID_PARAMETER; return; }
    const uint32_t bufferSize = Memory::Load32(bufferSizePtr);

    uint32_t headerSize = 8 + settingCount * 0x28, dataSize = 0;
    for (uint32_t i = 0; i < settingCount; ++i)
    {
        const uint32_t id = Memory::Load32(ids + i * 4);
        const uint32_t type = id >> 28;
        if (type == kWString || type == kBinary) dataSize += (id >> 16) & 0xFFF;
    }
    const uint32_t needed = headerSize + dataSize;
    if (!buffer || bufferSize < needed)
    {
        if (!bufferSize) Memory::Store32(bufferSizePtr, needed);
        ctx.r3.u64 = X_ERROR_INSUFFICIENT_BUFFER;
        return;
    }
    if (!xuidCount && userIndex != kUserIndex)
    {
        ctx.r3.u64 = overlapped ? CompleteImmediate(overlapped, X_ERROR_NO_SUCH_USER) : X_ERROR_NO_SUCH_USER;
        return;
    }

    auto& settings = Settings();
    for (uint32_t i = 0; i < settingCount; ++i)
        if (!settings.count(Memory::Load32(ids + i * 4)))
        {
            static int logged = 0;
            if (logged++ < 10)
                std::fprintf(stderr, "[xam] XamUserReadProfileSettings: unknown setting %08X\n", Memory::Load32(ids + i * 4));
            ctx.r3.u64 = overlapped ? CompleteImmediate(overlapped, X_ERROR_INVALID_PARAMETER) : X_ERROR_INVALID_PARAMETER;
            return;
        }

    Memory::Store32(buffer, settingCount);
    Memory::Store32(buffer + 4, buffer + 8);
    uint32_t data = buffer + headerSize;
    for (uint32_t i = 0; i < settingCount; ++i)
    {
        const uint32_t id = Memory::Load32(ids + i * 4);
        Setting& s = settings[id];
        if (IsTitleSpecific(id) && !s.isSet) LoadTitleSetting(id, s);
        const uint32_t out = buffer + 8 + i * 0x28;
        for (uint32_t k = 0; k < 0x28; k += 4) Memory::Store32(out + k, 0);
        Memory::Store32(out + 0x00, !s.isSet ? 0 : IsTitleSpecific(id) ? 2 : 1); // from
        if (xuidCount) Memory::Store64(out + 0x08, kXuid);
        else Memory::Store32(out + 0x08, userIndex);
        Memory::Store32(out + 0x10, id);
        if (!s.isSet) continue;
        const uint32_t d = out + 0x18; // X_USER_PROFILE_SETTING_DATA: type, pad, unk, union @+8
        Memory::Store8(d, s.type);
        switch (s.type)
        {
        case kInt32:
        case kFloat: Memory::Store32(d + 8, uint32_t(s.value)); break;
        case kInt64:
        case kDouble:
        case kDateTime: Memory::Store64(d + 8, uint64_t(s.value)); break;
        case kWString:
            if (!s.text.empty())
            {
                const uint32_t bytes = uint32_t(s.text.size() + 1) * 2;
                Memory::Store32(d + 8, bytes);
                Memory::Store32(d + 12, data);
                for (size_t c = 0; c <= s.text.size(); ++c)
                    Memory::Store16(data + uint32_t(c) * 2, c < s.text.size() ? uint16_t(s.text[c]) : 0);
                data += bytes;
            }
            break;
        case kBinary:
            if (!s.binary.empty())
            {
                Memory::Store32(d + 8, uint32_t(s.binary.size()));
                Memory::Store32(d + 12, data);
                std::memcpy(Memory::Translate(data), s.binary.data(), s.binary.size());
                data += uint32_t(s.binary.size());
            }
            break;
        default: break;
        }
    }
    ctx.r3.u64 = overlapped ? CompleteImmediate(overlapped, X_ERROR_SUCCESS) : X_ERROR_SUCCESS;
}

// DWORD XamUserCreateAchievementEnumerator(u32 TitleId, u32 UserIndex, u64 Xuid, u32 Flags,
//     u32 Offset, u32 Count, u32* BufferSize, HANDLE*) - no achievements are reported.
GUEST_IMPORT(XamUserCreateAchievementEnumerator)
{
    const uint32_t count = ctx.r8.u32, bufferSizePtr = ctx.r9.u32, handlePtr = ctx.r10.u32;
    auto e = std::make_shared<Enumerator>();
    e->itemSize = 0x24; // X_ACHIEVEMENT_DETAILS
    e->perEnumerate = std::max<uint32_t>(count, 1);
    if (bufferSizePtr) Memory::Store32(bufferSizePtr, e->itemSize * e->perEnumerate);
    if (handlePtr) Memory::Store32(handlePtr, CreateEnumeratorHandle(e));
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// ============================================================== enumerators
// DWORD XamEnumerate(HANDLE, u32 Flags, void* Buffer, u32 BufferSize, u32* ItemsReturned, XAM_OVERLAPPED*)
GUEST_IMPORT(XamEnumerate)
{
    const uint32_t handle = ctx.r3.u32, buffer = ctx.r5.u32, itemsPtr = ctx.r7.u32, overlapped = ctx.r8.u32;
    std::shared_ptr<Enumerator> e;
    {
        std::lock_guard lock(g_enumMutex);
        auto it = g_enumerators.find(handle);
        if (it != g_enumerators.end()) e = it->second;
    }
    if (!e) { ctx.r3.u64 = X_ERROR_INVALID_HANDLE; return; }
    uint32_t result = X_ERROR_SUCCESS, written = 0;
    if (!buffer)
        result = X_ERROR_INVALID_PARAMETER;
    else
    {
        written = std::min(e->count - e->current, e->perEnumerate);
        if (!written)
            result = X_ERROR_NO_MORE_FILES;
        else
        {
            std::memcpy(Memory::Translate(buffer), e->items.data() + size_t(e->current) * e->itemSize, size_t(written) * e->itemSize);
            e->current += written;
        }
    }
    if (overlapped)
    {
        CompleteOverlapped(overlapped, result, HResultFromWin32(result), written);
        ctx.r3.u64 = X_ERROR_IO_PENDING;
        return;
    }
    if (itemsPtr) Memory::Store32(itemsPtr, result == X_ERROR_SUCCESS ? written : 0);
    ctx.r3.u64 = result;
}

GUEST_IMPORT(XamCreateEnumeratorHandle) { ctx.r3.u64 = X_ERROR_INVALID_PARAMETER; }
GUEST_IMPORT(XamGetPrivateEnumStructureFromHandle) { ctx.r3.u64 = Kernel::STATUS_INVALID_HANDLE; }

// ============================================================== content
// X_CONTENT_DEVICE_DATA (0x50): id, type, u64 total, u64 free, u16 name[28]
GUEST_IMPORT(XamContentGetDeviceData)
{
    const uint32_t id = ctx.r3.u32, out = ctx.r4.u32;
    if (id != kHddDeviceId && id != kOddDeviceId) { ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED; return; }
    for (uint32_t i = 0; i < 0x50; i += 4) Memory::Store32(out + i, 0);
    const bool hdd = id == kHddDeviceId;
    Memory::Store32(out, id);
    Memory::Store32(out + 4, hdd ? 1 : 4);
    Memory::Store64(out + 8, (hdd ? 20ull : 7ull) << 30);
    Memory::Store64(out + 16, (hdd ? 3ull : 0ull) << 30);
    const char* name = hdd ? "Hard Drive" : "Disc";
    for (uint32_t i = 0; name[i]; ++i) Memory::Store16(out + 24 + i * 2, uint16_t(name[i]));
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// DWORD XamContentGetDeviceState(u32 DeviceId, XAM_OVERLAPPED*)
GUEST_IMPORT(XamContentGetDeviceState)
{
    const uint32_t id = ctx.r3.u32, overlapped = ctx.r4.u32;
    const bool ok = id == kHddDeviceId || id == kOddDeviceId;
    if (overlapped)
    {
        if (ok) CompleteImmediate(overlapped, X_ERROR_SUCCESS);
        else CompleteOverlapped(overlapped, X_ERROR_FUNCTION_FAILED, X_ERROR_DEVICE_NOT_CONNECTED, 0);
        ctx.r3.u64 = X_ERROR_IO_PENDING;
        return;
    }
    ctx.r3.u64 = ok ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

// DWORD XamContentCreateEnumerator(u32 UserIndex, u32 DeviceId, u32 ContentType, u32 Flags,
//     u32 ItemsPerEnumerate, u32* BufferSize, HANDLE*)
GUEST_IMPORT(XamContentCreateEnumerator)
{
    const uint32_t device = ctx.r4.u32, type = ctx.r5.u32, perEnum = ctx.r7.u32, sizePtr = ctx.r8.u32, handlePtr = ctx.r9.u32;
    if ((device && device != kHddDeviceId && device != kOddDeviceId) || !handlePtr)
    {
        if (sizePtr) Memory::Store32(sizePtr, 0);
        ctx.r3.u64 = X_E_INVALIDARG;
        return;
    }
    if (sizePtr) Memory::Store32(sizePtr, kContentDataSize * perEnum);
    auto e = std::make_shared<Enumerator>();
    e->itemSize = kContentDataSize;
    e->perEnumerate = std::max<uint32_t>(perEnum, 1);
    if (!device || device == kHddDeviceId)
    {
        std::error_code ec;
        const auto root = Fs::UserRoot() / "content" / Hex8(TitleId()) / Hex8(type);
        for (const auto& entry : std::filesystem::directory_iterator(root, ec))
        {
            if (!entry.is_directory()) continue;
            const std::string name = entry.path().filename().string();
            std::vector<uint8_t> item(kContentDataSize, 0);
            auto put32 = [&](size_t o, uint32_t v) { for (int b = 0; b < 4; ++b) item[o + b] = uint8_t(v >> (24 - 8 * b)); };
            put32(0, kHddDeviceId);
            put32(4, type);
            for (size_t c = 0; c < name.size() && c < 127; ++c) { item[8 + c * 2] = 0; item[9 + c * 2] = uint8_t(name[c]); }
            for (size_t c = 0; c < name.size() && c < 41; ++c) item[0x108 + c] = uint8_t(name[c]);
            e->items.insert(e->items.end(), item.begin(), item.end());
            ++e->count;
        }
    }
    std::printf("[xam] content enumerator: type %08X, %u item(s)\n", type, e->count);
    Memory::Store32(handlePtr, CreateEnumeratorHandle(e));
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// DWORD XamContentCreateEx(u32 UserIndex, char* RootName, XCONTENT_DATA*, u32 Flags, u32* Disposition,
//     u32* LicenseMask, u32 CacheSize, u64 ContentSize, XAM_OVERLAPPED*)
GUEST_IMPORT(XamContentCreateEx)
{
    const std::string root = ReadGuestString(ctx.r4.u32);
    const ContentData content = ReadContentData(ctx.r5.u32);
    const uint32_t flags = ctx.r6.u32, dispositionPtr = ctx.r7.u32, licensePtr = ctx.r8.u32, overlapped = StackArg(ctx, 8);
    const auto path = PackagePath(content);
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);

    uint32_t result = X_ERROR_INVALID_PARAMETER, disposition = 0; // 1 created, 2 opened
    switch (flags & 0xF)
    {
    case 1: result = exists ? X_ERROR_ALREADY_EXISTS : 0; disposition = exists ? 0 : 1; break;              // CREATE_NEW
    case 2: if (exists) std::filesystem::remove_all(path, ec); disposition = 1; result = 0; break;           // CREATE_ALWAYS
    case 3: result = exists ? 0 : X_ERROR_PATH_NOT_FOUND; disposition = exists ? 2 : 0; break;               // OPEN_EXISTING
    case 4: disposition = exists ? 2 : 1; result = 0; break;                                                 // OPEN_ALWAYS
    case 5: if (exists) { std::filesystem::remove_all(path, ec); disposition = 1; result = 0; }              // TRUNCATE_EXISTING
            else result = X_ERROR_PATH_NOT_FOUND; break;
    default: break;
    }
    if (result == 0 && disposition == 1 && !std::filesystem::create_directories(path, ec) && !std::filesystem::exists(path, ec))
        result = X_ERROR_ACCESS_DENIED;
    if (result == 0)
        result = MountPackage(root, path);
    if (dispositionPtr) Memory::Store32(dispositionPtr, result == 0 ? disposition : 0);
    if (licensePtr && result == 0) Memory::Store32(licensePtr, 0);
    std::printf("[xam] XamContentCreateEx(%s:, type %08X, \"%s\", flags %X) -> %X (disposition %u)\n", root.c_str(),
        content.contentType, content.fileName.c_str(), flags, result, disposition);
    if (overlapped)
    {
        CompleteOverlapped(overlapped, result, HResultFromWin32(result), result == 0 ? disposition : 0);
        ctx.r3.u64 = X_ERROR_IO_PENDING;
        return;
    }
    ctx.r3.u64 = result;
}

// DWORD XamContentClose(char* RootName, XAM_OVERLAPPED*)
GUEST_IMPORT(XamContentClose)
{
    const std::string root = ReadGuestString(ctx.r3.u32);
    uint32_t result = X_ERROR_FILE_NOT_FOUND;
    {
        std::lock_guard lock(g_contentMutex);
        if (g_openContent.erase(root))
        {
            Fs::UnmountContent(root);
            result = X_ERROR_SUCCESS;
        }
    }
    ctx.r3.u64 = ctx.r4.u32 ? CompleteImmediate(ctx.r4.u32, result) : result;
}

// DWORD XamContentDelete(u32 UserIndex, XCONTENT_DATA*, XAM_OVERLAPPED*)
GUEST_IMPORT(XamContentDelete)
{
    const auto path = PackagePath(ReadContentData(ctx.r4.u32));
    std::error_code ec;
    const uint32_t result = std::filesystem::exists(path, ec) && std::filesystem::remove_all(path, ec) ? X_ERROR_SUCCESS
                                                                                                    : X_ERROR_FILE_NOT_FOUND;
    ctx.r3.u64 = ctx.r5.u32 ? CompleteImmediate(ctx.r5.u32, result) : result;
}

// ============================================================== system UI (headless)
// DWORD XamShowDeviceSelectorUI(u32 UserIndex, u32 ContentType, u32 ContentFlags, u64 TotalRequested,
//     u32* DeviceId, XAM_OVERLAPPED*)
GUEST_IMPORT(XamShowDeviceSelectorUI)
{
    const uint32_t deviceIdPtr = ctx.r7.u32, overlapped = ctx.r8.u32;
    std::printf("[xam] device selector -> hard drive\n");
    ctx.r3.u64 = DispatchHeadless(overlapped, [deviceIdPtr] {
        if (deviceIdPtr) Memory::Store32(deviceIdPtr, kHddDeviceId);
        return X_ERROR_SUCCESS;
    });
}

// DWORD XamShowSigninUI(u32 Panes, u32 Flags) - the profile is always signed in.
GUEST_IMPORT(XamShowSigninUI)
{
    Broadcast(XN_SYS_SIGNINCHANGED, 1);
    Broadcast(XN_SYS_UI, 0);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}

// DWORD XamShowMessageBoxUIEx(u32 UserIndex, wchar* Title, wchar* Text, u32 ButtonCount, wchar** Buttons,
//     u32 FocusButton, u32 Flags, u32* Result, XAM_OVERLAPPED*) - answers with the focused button.
GUEST_IMPORT(XamShowMessageBoxUIEx)
{
    auto readW = [](uint32_t p) {
        std::string s;
        for (uint32_t i = 0; p && i < 256; ++i)
        {
            const uint16_t c = Memory::Load16(p + i * 2);
            if (!c) break;
            s += c < 128 ? char(c) : '?';
        }
        return s;
    };
    const uint32_t focus = ctx.r8.u32, resultPtr = ctx.r10.u32, overlapped = StackArg(ctx, 8);
    std::printf("[xam] message box \"%s\": \"%s\" -> button %u\n", readW(ctx.r4.u32).c_str(), readW(ctx.r5.u32).c_str(), focus);
    ctx.r3.u64 = DispatchHeadless(overlapped, [resultPtr, focus] {
        if (resultPtr) Memory::Store32(resultPtr, focus);
        return X_ERROR_SUCCESS;
    });
}

// DWORD XamShowKeyboardUI(u32 UserIndex, u32 Flags, wchar* DefaultText, wchar* Title, wchar* Description,
//     wchar* Buffer, u32 BufferLength, XAM_OVERLAPPED*)
// The console's on-screen keyboard becomes a host text prompt (Video::PromptText: a dialog on
// Windows). Cancel -> ERROR_CANCELLED, as on the console. PES_KEYBOARD_TEXT=<text> answers
// without a prompt (scripted test runs).
namespace
{
    constexpr uint32_t X_ERROR_CANCELLED = 0x4C7;

    std::u16string ReadGuestU16(uint32_t address, uint32_t maxChars = 256)
    {
        std::u16string out;
        for (uint32_t i = 0; address && i < maxChars; ++i)
        {
            const uint16_t c = Memory::Load16(address + i * 2);
            if (!c) break;
            out += char16_t(c);
        }
        return out;
    }

    std::string Narrow(const std::u16string& s)
    {
        std::string out;
        for (char16_t c : s) out += c < 0x80 ? char(c) : '?';
        return out;
    }
}

GUEST_IMPORT(XamShowKeyboardUI)
{
    const uint32_t buffer = ctx.r8.u32, length = ctx.r9.u32, overlapped = ctx.r10.u32;
    const std::u16string defaultText = ReadGuestU16(ctx.r5.u32), title = ReadGuestU16(ctx.r6.u32),
        description = ReadGuestU16(ctx.r7.u32);
    std::fprintf(stderr, "[xam] XamShowKeyboardUI title \"%s\" description \"%s\" default \"%s\" length %u (lr 0x%08X)\n",
        Narrow(title).c_str(), Narrow(description).c_str(), Narrow(defaultText).c_str(), length, uint32_t(ctx.lr));
    std::fflush(stderr);
    ctx.r3.u64 = DispatchHeadless(overlapped, [=] {
        if (!buffer || !length) return X_ERROR_INVALID_PARAMETER;
        std::u16string text = defaultText;
        bool ok = true;
        if (const char* env = std::getenv("PES_KEYBOARD_TEXT"); env && *env)
        {
            text.clear();
            for (const char* p = env; *p; ++p) text += char16_t(uint8_t(*p));
        }
        else
            ok = Video::PromptText(title, description, text, length - 1);
        std::fprintf(stderr, "[xam] XamShowKeyboardUI -> %s \"%s\"\n", ok ? "ok" : "cancelled", Narrow(text).c_str());
        std::fflush(stderr);
        if (!ok)
        {
            Memory::Store16(buffer, 0);
            return X_ERROR_CANCELLED;
        }
        uint32_t i = 0;
        for (; i < text.size() && i + 1 < length; ++i) Memory::Store16(buffer + i * 2, text[i]);
        Memory::Store16(buffer + i * 2, 0);
        return X_ERROR_SUCCESS;
    });
}

// Online / social UIs: not available offline.
GUEST_IMPORT(XamShowFriendsUI)          { ctx.r3.u64 = X_ERROR_FUNCTION_FAILED; }
GUEST_IMPORT(XamShowFriendRequestUI)    { ctx.r3.u64 = X_ERROR_FUNCTION_FAILED; }
GUEST_IMPORT(XamShowGamerCardUIForXUID) { ctx.r3.u64 = X_ERROR_FUNCTION_FAILED; }
GUEST_IMPORT(XamShowMarketplaceUI)      { ctx.r3.u64 = X_ERROR_FUNCTION_FAILED; }
GUEST_IMPORT(XamShowPlayerReviewUI)     { ctx.r3.u64 = X_ERROR_FUNCTION_FAILED; }

// void XamShowDirtyDiscErrorUI(u32 UserIndex) - never returns on the console.
GUEST_IMPORT(XamShowDirtyDiscErrorUI)
{
    std::fprintf(stderr, "[xam] XamShowDirtyDiscErrorUI: the title failed to read game data (lr 0x%08X) - exiting\n", uint32_t(ctx.lr));
    std::fflush(stderr);
    std::_Exit(1);
}

// ============================================================== XMsg (built-in system apps)
// Synchronous handlers following Xenia's apps: XMP (music player) 0xFA, XGI 0xFB,
// XLiveBase 0xFC, XAM 0xFE.
namespace
{
    constexpr uint32_t X_E_SUCCESS = 0, X_E_FAIL = 0x80004005, X_E_NOTFOUND = 0x80070490;

    uint32_t DispatchAppMessage(uint32_t app, uint32_t message, uint32_t buffer, uint32_t length)
    {
        switch (app)
        {
        case 0xFA: // XMP: the title owns playback, nothing is playing
            if (message == 0x00070009 && buffer) // XMPGetStatus { client, state* }
            {
                if (const uint32_t state = Memory::Load32(buffer + 4)) Memory::Store32(state, 0);
            }
            else if (message == 0x0007001B && buffer) // XMPGetPlaybackController { client, controller*, locked* }
            {
                if (const uint32_t c = Memory::Load32(buffer + 4)) Memory::Store32(c, 0);
                if (const uint32_t l = Memory::Load32(buffer + 8)) Memory::Store32(l, 0);
            }
            return X_E_SUCCESS;
        case 0xFB: // XGI: contexts, properties, achievements, sessions - accepted
            if (message == 0x000B0041) // XGIUserGetContext
            {
                if (buffer)
                    if (const uint32_t context = Memory::Load32(buffer + 16)) Memory::Store32(context + 4, 0);
                return X_E_FAIL;
            }
            return X_E_SUCCESS;
        case 0xFC: // XLiveBase: offline
            switch (message)
            {
            case 0x00058004: if (buffer) Memory::Store32(buffer, 1); return X_E_SUCCESS;
            case 0x00058006: if (buffer) Memory::Store32(buffer, 1); return X_E_SUCCESS; // XONLINE_NAT_OPEN
            case 0x00058007: return 0x80151802;                                          // ERROR_CONNECTION_INVALID
            case 0x00058046: return X_E_SUCCESS;
            default: return X_E_FAIL;
            }
        case 0xFE: // XAM
            return X_E_SUCCESS;
        default:
            return X_E_NOTFOUND;
        }
    }

    void LogAppMessage(const char* api, uint32_t app, uint32_t message, uint32_t result)
    {
        static std::mutex m;
        static std::map<uint64_t, int> seen;
        std::lock_guard lock(m);
        if (seen[(uint64_t(app) << 32) | message]++ == 0)
            std::printf("[xam] %s(app %02X, message %08X) -> %08X\n", api, app, message, result);
    }
}

// DWORD XMsgInProcessCall(u32 App, u32 Message, u32 Arg1, u32 Arg2)
GUEST_IMPORT(XMsgInProcessCall)
{
    const uint32_t result = DispatchAppMessage(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
    LogAppMessage("XMsgInProcessCall", ctx.r3.u32, ctx.r4.u32, result);
    ctx.r3.u64 = result;
}

// DWORD XMsgStartIORequest(u32 App, u32 Message, XAM_OVERLAPPED*, void* Buffer, u32 BufferLength)
GUEST_IMPORT(XMsgStartIORequest)
{
    const uint32_t app = ctx.r3.u32, message = ctx.r4.u32, overlapped = ctx.r5.u32;
    uint32_t result = DispatchAppMessage(app, message, ctx.r6.u32, ctx.r7.u32);
    LogAppMessage("XMsgStartIORequest", app, message, result);
    if (result == X_E_NOTFOUND) result = X_E_INVALIDARG;
    if (overlapped)
    {
        CompleteImmediate(overlapped, result);
        result = X_ERROR_IO_PENDING;
    }
    ctx.r3.u64 = result;
}

// DWORD XMsgCancelIORequest(XAM_OVERLAPPED*, BOOL Wait) - requests complete immediately.
GUEST_IMPORT(XMsgCancelIORequest) { ctx.r3.u64 = 0; }

// DWORD XMsgCompleteIORequest(XAM_OVERLAPPED*, u32 Result, u32 ExtendedError, u32 Length)
GUEST_IMPORT(XMsgCompleteIORequest)
{
    CompleteOverlapped(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
    ctx.r3.u64 = 0;
}

// ============================================================== misc info
// DWORD XamGetExecutionId(XEX_EXECUTION_INFO** Info)
GUEST_IMPORT(XamGetExecutionId)
{
    const uint32_t info = Kernel::GetXexHeaderField(Kernel::ExecutableModule().headerAddr, 0x00040006);
    if (ctx.r3.u32) Memory::Store32(ctx.r3.u32, info);
    ctx.r3.u64 = info ? 0 : 0xC0000225; // STATUS_NOT_FOUND
}

GUEST_IMPORT(XamGetSystemVersion) { ctx.r3.u64 = 0x20000000; }
