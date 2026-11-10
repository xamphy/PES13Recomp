// XamInput* imports. The Xbox 360 input structures are identical to XInput's,
// so on Windows the host controller state is passed straight through.
// (iPad: GameController.framework + touch overlay, later.)
//
// Player 1 also gets two extra sources, merged with a real controller if one is present:
//   - the keyboard while the game window has focus (see KeyboardState for the mapping),
//   - a scripted button sequence for automated tests: PES_INPUT_SCRIPT="start@55,a@62,down@70"
//     presses each button for 200 ms at the given second after launch (buttons: a b x y start
//     back up down left right lb rb).
#include <kernel/kernel.h>
#include <guest_memory.h>
#include <video/presenter.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   include <windows.h>
#   include <Xinput.h>
#   pragma comment(lib, "xinput.lib")
#endif

namespace
{
    constexpr uint32_t ERROR_SUCCESS_ = 0;
    constexpr uint32_t ERROR_DEVICE_NOT_CONNECTED_ = 0x48F;
    constexpr uint32_t ERROR_EMPTY_ = 0x10D2;

    // X_INPUT_GAMEPAD: u16 Buttons, u8 LeftTrigger, u8 RightTrigger, s16 ThumbLX/LY/RX/RY
    // X_INPUT_GAMEPAD button bits (identical to XInput's).
    constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008, kStart = 0x0010, kBack = 0x0020,
        kLB = 0x0100, kRB = 0x0200, kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;

    struct PadState
    {
        uint16_t buttons = 0;
        uint8_t lt = 0, rt = 0;
        int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    };

    // ---------------------------------------------------------------- scripted input
    struct ScriptPress { double at; uint16_t buttons; double duration = 0.2; };
    std::mutex g_scriptMutex; // live presses (Input::QueuePress) are added while the game polls
    std::vector<ScriptPress> g_script;
    std::once_flag g_scriptOnce;
    const auto g_start = std::chrono::steady_clock::now();

    uint16_t ButtonByName(const std::string& n)
    {
        static const struct { const char* name; uint16_t bit; } table[] = {
            { "a", kA }, { "b", kB }, { "x", kX }, { "y", kY }, { "start", kStart }, { "back", kBack },
            { "up", kUp }, { "down", kDown }, { "left", kLeft }, { "right", kRight }, { "lb", kLB }, { "rb", kRB } };
        for (const auto& e : table)
            if (n == e.name) return e.bit;
        return 0;
    }

    void LoadScript()
    {
        const char* env = std::getenv("PES_INPUT_SCRIPT");
        if (!env) return;
        std::string s(env);
        size_t pos = 0;
        while (pos < s.size())
        {
            const size_t end = std::min(s.find(',', pos), s.size());
            const std::string item = s.substr(pos, end - pos);
            const size_t at = item.find('@');
            if (at != std::string::npos)
                if (const uint16_t b = ButtonByName(item.substr(0, at)))
                    g_script.push_back({ std::atof(item.c_str() + at + 1), b, 0.2 });
            pos = end + 1;
        }
        std::printf("[input] scripted input: %zu presses\n", g_script.size());
    }

    uint16_t ScriptButtons()
    {
        std::call_once(g_scriptOnce, LoadScript);
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
        uint16_t b = 0;
        std::lock_guard lock(g_scriptMutex);
        for (const auto& p : g_script)
            if (t >= p.at && t < p.at + p.duration) b |= p.buttons;
        return b;
    }

    // ---------------------------------------------------------------- keyboard
#ifdef _WIN32
    bool Key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

    // Arrows = D-pad, Enter = Start, Backspace = Back, Space = A, Esc = B, C = X, V = Y,
    // Q/E = LB/RB, WASD = left stick, I/J/K/L = right stick, Shift = LT, R = RT.
    PadState KeyboardState()
    {
        PadState p;
        if (!Video::WindowHasFocus()) return p;
        const struct { int vk; uint16_t bit; } map[] = {
            { VK_UP, kUp }, { VK_DOWN, kDown }, { VK_LEFT, kLeft }, { VK_RIGHT, kRight }, { VK_RETURN, kStart },
            { VK_BACK, kBack }, { VK_SPACE, kA }, { VK_ESCAPE, kB }, { 'C', kX }, { 'V', kY }, { 'Q', kLB }, { 'E', kRB } };
        for (const auto& m : map)
            if (Key(m.vk)) p.buttons |= m.bit;
        p.lx = int16_t((Key('D') ? 32767 : 0) - (Key('A') ? 32767 : 0));
        p.ly = int16_t((Key('W') ? 32767 : 0) - (Key('S') ? 32767 : 0));
        p.rx = int16_t((Key('L') ? 32767 : 0) - (Key('J') ? 32767 : 0));
        p.ry = int16_t((Key('I') ? 32767 : 0) - (Key('K') ? 32767 : 0));
        p.lt = Key(VK_SHIFT) ? 255 : 0;
        p.rt = Key('R') ? 255 : 0;
        return p;
    }
#elif defined(__APPLE__)
    PadState KeyboardState()
    {
        const Video::HostPad k = Video::GetHostKeyboard();
        return { k.buttons, k.lt, k.rt, k.lx, k.ly, k.rx, k.ry };
    }
#else
    PadState KeyboardState() { return {}; }
#endif

    // Player 1's state: real controller (if any) + keyboard + script. Returns false if
    // nothing is connected for this user (other players: real controllers only).
    bool GetPad(uint32_t user, PadState& out, uint32_t& packet)
    {
        static std::atomic<uint32_t> virtualPacket{ 0 };
        bool connected = false;
        out = {};
#ifdef _WIN32
        XINPUT_STATE s{};
        if (user < XUSER_MAX_COUNT && XInputGetState(user, &s) == ERROR_SUCCESS)
        {
            connected = true;
            out = { s.Gamepad.wButtons, s.Gamepad.bLeftTrigger, s.Gamepad.bRightTrigger,
                s.Gamepad.sThumbLX, s.Gamepad.sThumbLY, s.Gamepad.sThumbRX, s.Gamepad.sThumbRY };
            packet = s.dwPacketNumber;
        }
#elif defined(__APPLE__)
        // SDL gamepads (Bluetooth Xbox / PlayStation / MFi controllers on iPad and Mac).
        Video::HostPad h;
        if (Video::GetHostGamepad(user, h))
        {
            connected = true;
            out = { h.buttons, h.lt, h.rt, h.lx, h.ly, h.rx, h.ry };
            static std::atomic<uint32_t> hostPackets[4];
            static PadState lastHost[4];
            if (user < 4 && std::memcmp(&out, &lastHost[user], sizeof(out)) != 0) { lastHost[user] = out; ++hostPackets[user]; }
            packet = user < 4 ? hostPackets[user].load() : 0;
        }
#endif
        if (user != 0) return connected;
        const PadState k = KeyboardState();
        out.buttons |= k.buttons | ScriptButtons();
        out.lt = std::max(out.lt, k.lt);
        out.rt = std::max(out.rt, k.rt);
        if (k.lx) out.lx = k.lx;
        if (k.ly) out.ly = k.ly;
        if (k.rx) out.rx = k.rx;
        if (k.ry) out.ry = k.ry;
        // The packet number must change whenever the state changes.
        static PadState last;
        if (std::memcmp(&out, &last, sizeof(out)) != 0) { last = out; ++virtualPacket; }
        if (!connected) packet = virtualPacket.load();
        else packet += virtualPacket.load();
        return true; // player 1 always has a (virtual) controller
    }

    void StoreGamepad(uint32_t addr, uint16_t buttons, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly, int16_t rx, int16_t ry)
    {
        Memory::Store16(addr + 0x0, buttons);
        Memory::Store8(addr + 0x2, lt);
        Memory::Store8(addr + 0x3, rt);
        Memory::Store16(addr + 0x4, uint16_t(lx));
        Memory::Store16(addr + 0x6, uint16_t(ly));
        Memory::Store16(addr + 0x8, uint16_t(rx));
        Memory::Store16(addr + 0xA, uint16_t(ry));
    }
}

// DWORD XamInputGetCapabilities(u32 UserIndex, u32 Flags, X_INPUT_CAPABILITIES*)
// X_INPUT_CAPABILITIES: u8 Type, u8 SubType, u16 Flags, X_INPUT_GAMEPAD Gamepad, X_INPUT_VIBRATION Vibration
GUEST_IMPORT(XamInputGetCapabilities)
{
    const uint32_t user = ctx.r3.u32 & 0xFF, caps = ctx.r5.u32;
    if (user == 0 && caps)
    {
        // Player 1 is always a standard gamepad (keyboard/script fallback).
        Memory::Store8(caps + 0x0, 1);   // XINPUT_DEVTYPE_GAMEPAD
        Memory::Store8(caps + 0x1, 1);   // XINPUT_DEVSUBTYPE_GAMEPAD
        Memory::Store16(caps + 0x2, 0);
        StoreGamepad(caps + 0x4, 0xF3FF, 0xFF, 0xFF, -1, -1, -1, -1); // all inputs supported
        Memory::Store16(caps + 0x10, 0xFFFF);
        Memory::Store16(caps + 0x12, 0xFFFF);
        ctx.r3.u64 = ERROR_SUCCESS_;
        return;
    }
#ifdef _WIN32
    XINPUT_CAPABILITIES c{};
    if (user < XUSER_MAX_COUNT && XInputGetCapabilities(user, 0, &c) == ERROR_SUCCESS && caps)
    {
        Memory::Store8(caps + 0x0, c.Type);
        Memory::Store8(caps + 0x1, c.SubType);
        Memory::Store16(caps + 0x2, c.Flags);
        StoreGamepad(caps + 0x4, c.Gamepad.wButtons, c.Gamepad.bLeftTrigger, c.Gamepad.bRightTrigger,
            c.Gamepad.sThumbLX, c.Gamepad.sThumbLY, c.Gamepad.sThumbRX, c.Gamepad.sThumbRY);
        Memory::Store16(caps + 0x10, c.Vibration.wLeftMotorSpeed);
        Memory::Store16(caps + 0x12, c.Vibration.wRightMotorSpeed);
        ctx.r3.u64 = ERROR_SUCCESS_;
        return;
    }
#endif
    ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED_;
}

// DWORD XamInputGetState(u32 UserIndex, u32 Flags, X_INPUT_STATE*)
// X_INPUT_STATE: u32 PacketNumber, X_INPUT_GAMEPAD Gamepad
GUEST_IMPORT(XamInputGetState)
{
    const uint32_t user = ctx.r3.u32 & 0xFF, state = ctx.r5.u32;
    PadState p;
    uint32_t packet = 0;
    if (GetPad(user, p, packet))
    {
        if (state)
        {
            Memory::Store32(state, packet);
            StoreGamepad(state + 4, p.buttons, p.lt, p.rt, p.lx, p.ly, p.rx, p.ry);
        }
        ctx.r3.u64 = ERROR_SUCCESS_;
        return;
    }
    ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED_;
}

// DWORD XamInputSetState(u32 UserIndex, u32 Flags, X_INPUT_VIBRATION* { u16 Left, u16 Right })
GUEST_IMPORT(XamInputSetState)
{
    const uint32_t user = ctx.r3.u32 & 0xFF, vib = ctx.r5.u32;
#ifdef _WIN32
    if (user < XUSER_MAX_COUNT && vib)
    {
        XINPUT_VIBRATION v{ Memory::Load16(vib), Memory::Load16(vib + 2) };
        ctx.r3.u64 = XInputSetState(user, &v) == ERROR_SUCCESS ? ERROR_SUCCESS_ : ERROR_DEVICE_NOT_CONNECTED_;
        return;
    }
#endif
    ctx.r3.u64 = ERROR_DEVICE_NOT_CONNECTED_;
}

// DWORD XamInputGetKeystrokeEx(u32* UserIndex, u32 Flags, X_INPUT_KEYSTROKE*)
// Keystrokes (chatpad/virtual keys) are not provided; the title falls back to GetState.
GUEST_IMPORT(XamInputGetKeystrokeEx)
{
    ctx.r3.u64 = ERROR_EMPTY_;
}

// Live input for test control (renderer's PES_CONTROL file): press buttons now for `seconds`.
// `names` may combine buttons with '+', e.g. "a" or "down+a". Returns false for unknown names.
namespace Input
{
    bool QueuePress(const std::string& names, double seconds)
    {
        std::call_once(g_scriptOnce, LoadScript);
        uint16_t buttons = 0;
        size_t pos = 0;
        while (pos <= names.size())
        {
            const size_t end = std::min(names.find('+', pos), names.size());
            const uint16_t b = ButtonByName(names.substr(pos, end - pos));
            if (!b) return false;
            buttons |= b;
            pos = end + 1;
        }
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
        std::lock_guard lock(g_scriptMutex);
        g_script.push_back({ t, buttons, seconds });
        return true;
    }
}
