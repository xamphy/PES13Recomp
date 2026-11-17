// Optional post effects: PES 2013's blur passes, switchable from the launch menu ("Blur effects")
// for slow devices. Off = the draws below are not issued. Identified from per-draw captures
// (2026-11-17, control "capture"; docs/RESUME.md "Claude, 2026-11-17"):
//
//  glow   Stadium-light glow / bloom in 3D scenes. The scene is drawn a second time into a
//         1280x720 colour target holding encoded view depth (ps ..0cbd, alpha-tested ..acba), that
//         depth goes back into a 640x360 depth buffer (..2c0c), the bright light sprites are drawn
//         there, then downsampled and blurred to 320x180 / 160x90 / 80x45 (..d5f2, ..adb6, ..2e6b);
//         a depth-faded 1280x720 flare layer (..9836) joins them. ..4af2 adds everything onto each
//         scene half (blend one + one), so skipping it leaves the scene as it was.
//  pause  Background blur behind the pause menu, Game Plan and other in-match menus: two
//         full-screen 15-tap Gaussian passes (..bfb7, horizontal + vertical) at 1280x720. Each
//         draws into the target it was just resolved from, so skipping keeps the sharp image.
//  soft   3x3 filter over the whole frame after colour grading (..50c7): a slight softening in
//         every scene. Same pattern: draws into the target it was resolved from.
//
// The glow producers only feed ..4af2; if any other draw samples a surface whose last resolve
// came from a skipped draw, the renderer logs it once ("[post] ... samples a skipped surface").
#include "renderer_internal.h"

namespace RendererImpl
{
    namespace
    {
        struct PostDraw { uint64_t ps; PostEffect effect; bool producer; bool singleSampleOnly; };
        constexpr PostDraw kPostDraws[] = {
            { 0x46366866ce4a4af2ull, PostEffect::Glow, false, false }, // composite: + glow levels + flare layer
            { 0xff6ed32dd6b10cbdull, PostEffect::Glow, true, true },   // depth pre-pass (encoded view depth)
            { 0x8fc4969d2144acbaull, PostEffect::Glow, true, true },   // depth pre-pass, alpha-tested
            { 0xd65e711803752c0cull, PostEffect::Glow, true, false },  // encoded depth -> 640x360 depth
            { 0x181522484d22d5f2ull, PostEffect::Glow, true, false },  // 640x360 copy
            { 0xa17371e75018adb6ull, PostEffect::Glow, true, false },  // downsample 5-tap
            { 0xcd726b0efbba2e6bull, PostEffect::Glow, true, false },  // blur 9-tap
            { 0x31878b0e7f5f9836ull, PostEffect::Glow, true, false },  // flare layer depth fade
            { 0x6cc5e2efa918bfb7ull, PostEffect::PauseBlur, false, false },
            { 0x1ae09346bc2c50c7ull, PostEffect::Soft, false, false },
        };
    }

    const char* PostEffectName(PostEffect e)
    {
        switch (e)
        {
        case PostEffect::Glow: return "glow";
        case PostEffect::PauseBlur: return "pause";
        case PostEffect::Soft: return "soft";
        default: return "?";
        }
    }

    bool SkipPostEffectDraw(uint64_t psHash, RenderTarget* rt)
    {
        g_postDrawPs = psHash;
        if (!g_postOff || !rt) { if (rt) rt->postSkipped = false; return false; }
        for (const PostDraw& d : kPostDraws)
            if (d.ps == psHash && (g_postOff & (1u << uint32_t(d.effect))))
            {
                if (d.singleSampleOnly && rt->msaa != xenos::MsaaSamples::k1X) break; // not the pre-pass target
                ++g_postSkippedDraws[uint32_t(d.effect)];
                if (d.producer) rt->postSkipped = true; // a skipped composite leaves its target as it was
                return true;
            }
        rt->postSkipped = false;
        return false;
    }

    void NotePostSkippedSample(const Surface* s)
    {
        static std::set<uint64_t> logged;
        if (s->postSkipped && g_postOff && logged.insert(g_postDrawPs).second)
            std::fprintf(stderr, "[post] pixel shader %016llx samples a skipped surface 0x%08X (its producer was turned off)\n",
                (unsigned long long)g_postDrawPs, s->base);
    }

    uint32_t ParsePostEffects(const char* list)
    {
        uint32_t mask = 0;
        const std::string s = list ? list : "";
        if (s.find("all") != std::string::npos) return (1u << uint32_t(PostEffect::Count)) - 1;
        for (uint32_t e = 0; e < uint32_t(PostEffect::Count); ++e)
            if (s.find(PostEffectName(PostEffect(e))) != std::string::npos) mask |= 1u << e;
        return mask;
    }
}
