# PES 2013 -> iPad (Apple Silicon) via static recompilation

## Why the Xbox 360 route
iPadOS doesn't allow JIT, so x86 emulators (Box64/FEX/Wine) are out.
Static recompilation produces ahead-of-time native ARM64 code, so no JIT is needed.

## Pipeline
1. Dump:      your disc -> game/xex/default.xex + game/files/
2. Unpack:    decrypt/decompress the XEX to a raw PE image
3. Analyze:   XenonAnalyse -> function boundaries, jump tables (hand-fix TOML config)
4. Recompile: XenonRecomp -> generated/*.cpp (PPC -> C++)
5. Shaders:   XenosRecomp -> Xenos shaders -> HLSL -> Metal (via DXC + SPIRV-Cross/Metal shader converter)
6. Runtime:   reimplement what the game calls
   - runtime/kernel : xboxkrnl/xam imports (threads, sync, memory, timing)
   - runtime/gpu    : Xenos command buffers / D3D9-on-360 -> Metal (or Vulkan via MoltenVK)
   - runtime/audio  : XMA decoding -> AVAudioEngine
   - runtime/input  : XInput -> GameController.framework + touch overlay
   - runtime/fs     : STFS/disc paths -> app sandbox
7. App:       app/ios - Theos project, cross-built in WSL2, sideloaded

## Milestones
- M0: recompiled code compiles for arm64-apple-ios
- M1: boots to first kernel call on Windows (easier to debug than iPad)
- M2: first frame rendered
- M3: menus with input
- M4: match runs at full speed on iPad
- M5: audio, saves, polish

## No-Mac strategy
- Develop and debug on Windows first (x64 build, Visual Studio/WinDbg, Vulkan renderer).
- Renderer targets Vulkan; on iPad it runs through MoltenVK (Vulkan -> Metal).
- iPad builds: cross-compile in WSL2 (Ubuntu) with Theos + iOS toolchain/SDK -> unsigned .ipa.
- Fallback: GitHub Actions macOS runner (PRIVATE repo only - output is game-derived).
- Install with Sideloadly (it signs the .ipa with your Apple ID).
- iPad debugging: logs over USB (idevicesyslog) + in-app log file; no Xcode debugger.

## Constraints
- iPad memory per app is limited; the 360's 512 MB guest memory fits.
- Game data comes only from your own disc and is never distributed.
