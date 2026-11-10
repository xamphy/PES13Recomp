// XAudio render driver / XMA kernel imports - sound output (2026-11-08).
//
// The title's audio engine (XAudio2, statically linked) registers render-driver clients. The
// console's audio driver calls each client's callback once per hardware frame (256 samples at
// 48 kHz, ~5.33 ms); the engine then mixes and hands the frame back through
// XAudioSubmitRenderDriverFrame: 6 channels x 256 big-endian floats, planar (FL FR FC LFE BL BR).
// PES decodes its sound in software (it never creates an XMA context), so these frames are the
// complete game audio.
//
// Here (like Xenia): the output device paces the callbacks - every 256 samples the device plays
// allow one more callback per client - so the title's audio clock (which movies sync to) follows
// the sound card. Submitted frames go into a short per-client queue, the device mixes the clients
// and downmixes 5.1 to stereo. Output: miniaudio (WASAPI / Core Audio).
// PES_NO_AUDIO=1: no device, callbacks driven by a timer (the old behaviour). PES_VOLUME=<gain>.
#include <kernel/kernel.h>
#include <kernel/thread.h>
#include <guest_memory.h>

#include "miniaudio.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
    constexpr uint32_t kSampleRate = 48000;
    constexpr uint32_t kFrameSamples = 256;  // per channel, per hardware frame
    constexpr uint32_t kGuestChannels = 6;
    constexpr uint32_t kPreroll = 4;         // callbacks issued ahead of playback (~21 ms)
    constexpr size_t kMaxQueued = 16;        // frames per client before the oldest are dropped

    using StereoFrame = std::array<float, kFrameSamples * 2>; // interleaved L R

    // Registered render clients (the console supports several; Xenia allows 8). Each callback
    // receives, in r3, the address of a guest dword holding its context value.
    constexpr uint32_t kMaxClients = 8;
    constexpr uint32_t kDriverHandleBase = 0x41550000;
    struct Client
    {
        std::atomic<uint32_t> callback{ 0 };
        uint32_t wrappedArg = 0;
        std::deque<StereoFrame> queue; // guarded by g_queueMutex
    };
    Client g_clients[kMaxClients];
    std::mutex g_queueMutex;

    std::atomic<bool> g_driverStarted{ false };
    std::atomic<uint64_t> g_framesRendered{ 0 }; // callbacks issued
    std::atomic<uint64_t> g_framesSubmitted{ 0 }, g_framesDropped{ 0 }, g_underruns{ 0 };

    // Ticks: hardware frames the device has consumed whose callbacks are still to be issued.
    std::mutex g_tickMutex;
    std::condition_variable g_tickCv;
    uint32_t g_pendingTicks = kPreroll;

    // PES_AUDIO_WAV=<path>[,seconds]: record the device output (debug; written once full).
    std::vector<float> g_wav;
    size_t g_wavCap = 0;
    std::atomic<bool> g_wavFull{ false };
    std::string g_wavPath;

    ma_device g_device;
    bool g_deviceOk = false;
    float g_volume = 1.0f;
    bool g_mute = false; // PES_AUDIO_MUTE=1: play silence (the WAV recording still gets the mix)

    // Device side: the mix of the current hardware frame and the read position in it.
    StereoFrame g_mix{};
    uint32_t g_mixPos = kFrameSamples; // exhausted -> mix the next frame

    void AddTicks(uint32_t n)
    {
        {
            std::lock_guard lock(g_tickMutex);
            g_pendingTicks += n;
        }
        g_tickCv.notify_one();
    }

    void MixNextFrame()
    {
        g_mix.fill(0.0f);
        bool any = false, starved = false;
        {
            std::lock_guard lock(g_queueMutex);
            for (Client& c : g_clients)
            {
                if (!c.callback.load()) continue;
                if (c.queue.empty()) { starved = true; continue; }
                const StereoFrame& f = c.queue.front();
                for (size_t i = 0; i < f.size(); ++i) g_mix[i] += f[i];
                c.queue.pop_front();
                any = true;
            }
        }
        if (starved && g_framesSubmitted.load()) ++g_underruns;
        if (any && g_volume != 1.0f)
            for (float& s : g_mix) s *= g_volume;
        g_mixPos = 0;
    }

    void DataCallback(ma_device*, void* output, const void*, ma_uint32 frameCount)
    {
        float* out = static_cast<float*>(output);
        uint32_t ticks = 0;
        while (frameCount)
        {
            if (g_mixPos == kFrameSamples)
            {
                MixNextFrame();
                ++ticks; // one hardware frame consumed -> one more callback per client
            }
            const uint32_t n = std::min<uint32_t>(frameCount, kFrameSamples - g_mixPos);
            std::memcpy(out, g_mix.data() + g_mixPos * 2, n * 2 * sizeof(float));
            out += n * 2;
            g_mixPos += n;
            frameCount -= n;
        }
        if (g_wavCap && !g_wavFull.load())
        {
            const size_t n = std::min<size_t>(size_t(out - static_cast<float*>(output)), g_wavCap - g_wav.size());
            g_wav.insert(g_wav.end(), static_cast<float*>(output), static_cast<float*>(output) + n);
            if (g_wav.size() >= g_wavCap) g_wavFull = true;
        }
        if (g_mute) std::memset(output, 0, size_t(out - static_cast<float*>(output)) * sizeof(float));
        if (ticks) AddTicks(ticks);
    }

    void WriteWav()
    {
        FILE* f = std::fopen(g_wavPath.c_str(), "wb");
        if (!f) return;
        const uint32_t dataBytes = uint32_t(g_wav.size() * sizeof(float));
        auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
        std::fwrite("RIFF", 1, 4, f); u32(36 + dataBytes); std::fwrite("WAVEfmt ", 1, 8, f);
        u32(16); u16(3); u16(2); u32(kSampleRate); u32(kSampleRate * 8); u16(8); u16(32); // IEEE float stereo
        std::fwrite("data", 1, 4, f); u32(dataBytes);
        std::fwrite(g_wav.data(), 1, dataBytes, f);
        std::fclose(f);
        std::printf("[audio] wrote %s (%.1f s)\n", g_wavPath.c_str(), double(g_wav.size()) / 2 / kSampleRate);
    }

    bool StartDevice()
    {
        if (const char* e = std::getenv("PES_NO_AUDIO"); e && std::strcmp(e, "0") != 0) return false;
        if (const char* v = std::getenv("PES_VOLUME")) g_volume = float(std::atof(v));
        if (const char* m = std::getenv("PES_AUDIO_MUTE")) g_mute = std::strcmp(m, "0") != 0;
        if (const char* w = std::getenv("PES_AUDIO_WAV"))
        {
            g_wavPath = w;
            double seconds = 120.0;
            if (const size_t comma = g_wavPath.find(','); comma != std::string::npos)
            {
                seconds = std::atof(g_wavPath.c_str() + comma + 1);
                g_wavPath.resize(comma);
            }
            g_wavCap = size_t(seconds * kSampleRate) * 2;
            g_wav.reserve(g_wavCap);
        }
        ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
        cfg.playback.format = ma_format_f32;
        cfg.playback.channels = 2;
        cfg.sampleRate = kSampleRate;
        cfg.dataCallback = DataCallback;
        cfg.periodSizeInMilliseconds = 10;
        cfg.performanceProfile = ma_performance_profile_low_latency;
        // iPadOS: playback-only audio session (miniaudio's default PlayAndRecord would ask for the microphone).
        ma_context* context = nullptr;
#ifdef __APPLE__
        static ma_context appleContext;
        ma_context_config ctxCfg = ma_context_config_init();
        ctxCfg.coreaudio.sessionCategory = ma_ios_session_category_playback;
        if (ma_context_init(nullptr, 0, &ctxCfg, &appleContext) == MA_SUCCESS) context = &appleContext;
#endif
        if (ma_device_init(context, &cfg, &g_device) != MA_SUCCESS)
        {
            std::printf("[audio] no output device - running silent (timer-paced)\n");
            return false;
        }
        if (ma_device_start(&g_device) != MA_SUCCESS)
        {
            ma_device_uninit(&g_device);
            std::printf("[audio] output device failed to start - running silent (timer-paced)\n");
            return false;
        }
        std::printf("[audio] output: %s, %u Hz stereo (backend %s)\n", g_device.playback.name, g_device.sampleRate,
            ma_get_backend_name(g_device.pContext->backend));
        return true;
    }

    // Silent fallback: a timer stands in for the device (frames are consumed and discarded).
    void TimerThread()
    {
        using clock = std::chrono::steady_clock;
        const auto period = std::chrono::nanoseconds(1'000'000'000LL * kFrameSamples / kSampleRate);
        auto next = clock::now() + period;
        for (;;)
        {
            std::this_thread::sleep_until(next);
            next += period;
            if (clock::now() > next + period * 8) next = clock::now() + period; // fell far behind: don't burst
            {
                std::lock_guard lock(g_queueMutex);
                for (Client& c : g_clients) if (!c.queue.empty()) c.queue.pop_front();
            }
            AddTicks(1);
        }
    }

    void AudioDriverThread()
    {
        Kernel::GuestThread* thread = Kernel::CreateGuestThread(0x10000);
        if (!thread)
            return;
        std::snprintf(thread->name, sizeof(thread->name), "[audio driver]");
        g_deviceOk = StartDevice();
        if (!g_deviceOk) std::thread(TimerThread).detach();
        auto lastReport = std::chrono::steady_clock::now();
        for (;;)
        {
            uint32_t ticks;
            {
                std::unique_lock lock(g_tickMutex);
                g_tickCv.wait(lock, [] { return g_pendingTicks > 0; });
                ticks = g_pendingTicks;
                g_pendingTicks = 0;
            }
            if (ticks > 32) ticks = 32; // after a long stall: don't flood the title with callbacks
            for (uint32_t t = 0; t < ticks; ++t)
            {
                for (Client& c : g_clients)
                    if (const uint32_t callback = c.callback.load())
                        Kernel::CallGuestFunction(*thread, callback, c.wrappedArg);
                ++g_framesRendered;
            }
            static bool wavWritten = false;
            if (g_wavFull.load() && !wavWritten) { wavWritten = true; std::thread(WriteWav).detach(); } // off the driver thread (a 180 MB write stalled the callbacks)
            const auto now = std::chrono::steady_clock::now();
            if (now - lastReport > std::chrono::seconds(30))
            {
                lastReport = now;
                std::printf("[audio] callbacks %llu, frames submitted %llu, dropped %llu, underruns %llu\n",
                    (unsigned long long)g_framesRendered.load(), (unsigned long long)g_framesSubmitted.load(),
                    (unsigned long long)g_framesDropped.load(), (unsigned long long)g_underruns.load());
            }
        }
    }

    uint32_t FloatBits(float f)
    {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        return u;
    }

    float LoadFloatBE(const uint8_t* p)
    {
        const uint32_t u = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    }
}

// u32 XAudioGetSpeakerConfig(u32* Config)
GUEST_IMPORT(XAudioGetSpeakerConfig)
{
    if (ctx.r3.u32) Memory::Store32(ctx.r3.u32, 0x00010001); // stereo
    ctx.r3.u64 = 0;
}

// u32 XAudioRegisterRenderDriverClient(u32* CallbackInfo { Callback, Context }, u32* Driver)
GUEST_IMPORT(XAudioRegisterRenderDriverClient)
{
    const uint32_t info = ctx.r3.u32;
    const uint32_t callback = info ? Memory::Load32(info) : 0;
    const uint32_t arg = info ? Memory::Load32(info + 4) : 0;
    uint32_t index = 0;
    while (index < kMaxClients && g_clients[index].callback.load()) ++index;
    if (index == kMaxClients || !callback)
    {
        ctx.r3.u64 = 0xC0000001; // STATUS_UNSUCCESSFUL
        return;
    }
    Client& c = g_clients[index];
    if (!c.wrappedArg) c.wrappedArg = Memory::AllocKernel(4, 4);
    Memory::Store32(c.wrappedArg, arg);
    {
        std::lock_guard lock(g_queueMutex);
        c.queue.clear();
    }
    c.callback = callback;
    if (ctx.r4.u32) Memory::Store32(ctx.r4.u32, kDriverHandleBase | index);
    std::printf("[audio] render client %u registered (callback 0x%08X, context 0x%08X)\n", index, callback, arg);
    if (!g_driverStarted.exchange(true))
        std::thread(AudioDriverThread).detach();
    ctx.r3.u64 = 0;
}

// u32 XAudioUnregisterRenderDriverClient(u32 Driver)
GUEST_IMPORT(XAudioUnregisterRenderDriverClient)
{
    const uint32_t index = ctx.r3.u32 & 0xFFFF;
    if ((ctx.r3.u32 & 0xFFFF0000) == kDriverHandleBase && index < kMaxClients)
    {
        g_clients[index].callback = 0;
        std::lock_guard lock(g_queueMutex);
        g_clients[index].queue.clear();
    }
    ctx.r3.u64 = 0;
}

// u32 XAudioSubmitRenderDriverFrame(u32 Driver, float* Samples) - 6 x 256 big-endian floats, planar.
GUEST_IMPORT(XAudioSubmitRenderDriverFrame)
{
    const uint32_t index = ctx.r3.u32 & 0xFFFF;
    const uint32_t samples = ctx.r4.u32;
    if ((ctx.r3.u32 & 0xFFFF0000) != kDriverHandleBase || index >= kMaxClients || !samples)
    {
        ctx.r3.u64 = 0;
        return;
    }
    // 5.1 -> stereo as Xenia: centre halved onto both sides, LFE dropped, scaled by 1/2.5.
    const uint8_t* in = static_cast<const uint8_t*>(Memory::Translate(samples));
    auto ch = [&](uint32_t c, uint32_t s) { return LoadFloatBE(in + (c * kFrameSamples + s) * 4); };
    StereoFrame f;
    for (uint32_t s = 0; s < kFrameSamples; ++s)
    {
        const float centre = ch(2, s) * 0.5f;
        f[s * 2] = (ch(0, s) + ch(4, s) + centre) * (1.0f / 2.5f);
        f[s * 2 + 1] = (ch(1, s) + ch(5, s) + centre) * (1.0f / 2.5f);
    }
    {
        std::lock_guard lock(g_queueMutex);
        auto& q = g_clients[index].queue;
        q.push_back(f);
        while (q.size() > kMaxQueued)
        {
            q.pop_front();
            ++g_framesDropped;
        }
    }
    ++g_framesSubmitted;
    ctx.r3.u64 = 0;
}

// u32 XAudioGetVoiceCategoryVolume(u32 Category, float* Volume)
GUEST_IMPORT(XAudioGetVoiceCategoryVolume)
{
    if (ctx.r4.u32) Memory::Store32(ctx.r4.u32, FloatBits(1.0f));
    ctx.r3.u64 = 0;
}

// u32 XMACreateContext(u32* Context) - XMA decoder context (0x40 bytes in guest memory). PES does not
// use XMA (none created in a full match); a title that does would need Xenia's XMA decoder here.
GUEST_IMPORT(XMACreateContext)
{
    const uint32_t context = Memory::AllocKernel(0x40, 0x40);
    if (ctx.r3.u32) Memory::Store32(ctx.r3.u32, context);
    static std::atomic<uint32_t> created{ 0 };
    const uint32_t n = ++created;
    if (n <= 8 || (n & (n - 1)) == 0)
        std::printf("[audio] XMACreateContext #%u -> 0x%08X (not decoded!, lr 0x%08X)\n", n, context, uint32_t(ctx.lr));
    ctx.r3.u64 = context ? 0 : 0xC0000017;
}

GUEST_IMPORT(XMAReleaseContext) { ctx.r3.u64 = 0; }
