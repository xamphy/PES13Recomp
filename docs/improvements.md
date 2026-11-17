# Improvements

Planned improvements that go beyond the original console version (graphics, iPad comfort).
Bugs live in [listofbugs.md](listofbugs.md). When an item is done, move it to "Done" with the date.

Format: **title** - what it does. Difficulty / where / notes. (date added)

## iPhone 11 - goal: steady 30 fps everywhere (user, 2026-11-15)

Test device: the user's iPhone 11 (A13). Matches, replays, set pieces, the pause menu and Game Plan
should all hold 30 fps. Where the GPU time goes in normal match play (build #35 log, `[passgpu]`,
~32 ms per frame in total):

| Part | GPU ms/frame |
|---|---|
| Main 3D scene (two 640x720 halves) | ~11.5 |
| Full-screen post-processing (pixel shaders ..1046 2.4 ms for one draw, ..50c7, ..d4c6, ..b173) | ~6.6 |
| Copies between EDRAM render targets | ~4.5 |
| Shadow maps | ~1.7 |
| Resolves, small passes, other | rest |

Plan, in order (each step measured on the iPhone, second session after installing):
1. **Cheaper full-screen post effects (~6.6 ms)** - dump the four shaders, find what each effect is
   and remove translation waste (as #31/#32 did for the pause blur). Optional effects can get a
   cheaper path in a **Performance mode** launcher setting (half resolution or off). Fastest win.
2. **Fewer EDRAM copies (~4.5 ms)** - avoid the remaining tile copies like the lazy clears avoided
   passes.
3. **Main scene shader overhead (~11.5 ms)** - translator-wide savings (like #31, -10..15% everywhere).
4. **Pipeline warm-up** - removes the compile stutters (see 4b below); no average-fps gain.
Expected: 1 + 2 save ~5-8 ms per frame -> normal play from ~30 to mid/high 30s, enough headroom for a
steady 30 with the cap below. 60 fps is not realistic on the A13 (the 3D scene alone is ~11 ms).

Drastic options (user, 2026-11-15) - **each one an on/off toggle in the launch menu, off by
default, never forced**; for when the steps above are not enough on older devices:
- **Skip cutscenes** - tunnel walk-out, line-ups, ceremonies, goal celebrations, half-time/full-time
  scenes (the heaviest scenes: 550+ draws/frame, 60-70 ms GPU on the iPhone 11). Approach: check the
  game's own settings first; otherwise detect the cutscene state and send the skip button for the
  player, or patch the game code that starts them.
- **Skip replays** - goal replays and highlights (15-23 fps on devices). Same approach: game option if
  one exists, else auto-skip.
- **Crowd off / reduced** - the crowd is thousands of small draws (pixel shader ..550e: ~320 draws per
  frame on Windows). Renderer-side: skip or thin out draws with the crowd's shaders (identify them with
  `passlog` / captures first, so nothing else disappears).
- **Shadows off / lower resolution** - the shadow-map passes (~1.7 ms/frame on the iPhone 11).
- **Post effects off** - the full-screen effects (~6.6 ms/frame), as the strongest form of the
  Performance mode above.
- **Lower render resolution** - the biggest lever but the hardest (EDRAM model, resolves, every path to
  guest memory; see 5 below, in reverse). Last resort.
Each toggle must be measured on its own (`[passgpu]`, scene-weight GPU ms) and must not break game
logic (a skipped cutscene or replay must leave the game in the same state as watching it).

Next features (user, 2026-11-15):
- **30 fps cap setting** in the launch menu (Off / 30): present every second vblank (or pace frames
  to 33.3 ms) so the frame rate is steady instead of swinging between 25 and 40. Also lowers heat
  (the iPhone reaches "serious" thermal state after 10-15 minutes). The game itself already holds
  frames for a second vblank in set pieces; the cap must not stack with that.
- **Launch menu with a controller** - navigate Play Game / Settings / About and change settings with
  a connected controller (d-pad / stick, A / B), on iPhone/iPad (`video/launcher_ios.mm`) and
  Windows (`launcher_win32.cpp` already has controller input - share the logic in
  `launcher/launcher_core.cpp`).

## Planned (suggested order)

3. **Post-process anti-aliasing (FXAA, later SMAA)** - one full-screen pass on the final frame before
   presenting (compute or fragment shader in the presenter). Smooths the jagged edges we have now
   (see 5). Easy-medium, a few hours. Setting to switch it off. (2026-11-10)
4. **Measure device performance** - now on the user's iPhone 11 (iPad logs are not coming): read the
   second session of build #36 (`[passgpu]` shows the most expensive passes), then pick the next GPU
   target. Lazy clears are done (#34/#35). (2026-11-10, updated 2026-11-15)
4b. **Pipeline warm-up** - record the pipelines a session uses (desc + shader ucode) and compile them
   on a background thread during the intro and menus, so matches do not stutter while ~330 pipelines
   compile (17-50 s per session on the iPhone 11). Medium. (2026-11-15)
4c. **Split renderer.cpp into files** (passes, EDRAM tiles, resolves/surfaces, textures, pipelines,
   draw, control/stats) as a pure move with no behaviour change, and remove A/B switches that are
   settled. User asked; after the current perf work. (2026-11-15)
5. **Resolution scaling (1.5x / 2x: 1920x1080 / 2560x1440)** - the biggest visual upgrade; at 2x it
   is also very good anti-aliasing. Hard, several sessions: the EDRAM tile model, resolves,
   depth/tile transfers, viewport/scissor, and every path where resolved images reach guest memory
   (`WriteBackSurface`, kit atlases, CPU-read resolves) must handle scaled surfaces. Xenia's
   `draw_resolution_scale` is the reference design. Only after 4 shows headroom. (2026-11-10)
6. **The game's own MSAA** - the game asks for 2x/4x MSAA render targets, but the renderer draws with
   1 sample per pixel (`renderer.cpp`, `VK_SAMPLE_COUNT_1_BIT`), so edges are jagged. Doing it
   properly ties into the EDRAM model (sample layout, resolves). Medium-hard. Possibly unnecessary if
   5 + 3 look good enough. (2026-11-10)

## Later / maybe

- **Wider or taller view (no black bars)** - would need patching the game's camera/projection code,
  and the menus/HUD are laid out for 16:9 (they would sit in the wrong places). Hard and risky;
  letterbox (1) is the plan. Cheap alternative: zoom-to-fill (crops the sides, cuts off the
  scoreboard), as an option only. (2026-11-10)
- **Texture replacement packs** - the renderer uploads every texture itself, so it could load
  higher-quality replacements matched by a hash of the original. Needs someone to make the
  textures. (2026-11-10)

## Done

- **Blur effects launch-menu toggle** (2026-11-17, build #38) - glow (incl. its depth pre-pass), pause-menu Gaussian blur
  and the soft full-frame filter can be turned off (Settings > Performance). The "Post effects off" drastic option,
  partly: colour grading (..1046) stays. Device gain to be measured.

- 2026-11-15 - **Device GPU work** (iPhone 11 benchmark, see RESUME END OF DAY): depth kept in passes
  (d7c342a), implicit-LOD texture sampling (19ef837, -10..15% GPU), plain texture fetches (1a32529),
  `[passgpu]` per-pass GPU timing (0ec5f83, CPU fix 970e341), lazy clears and lazy resolve clears
  (1d372b0, 52952a7; passes per frame 51.5 -> 32.5 on Windows). Heavy scenes on the iPhone 47.9 ->
  40.2 ms GPU per frame (#31 -> #35).

- 2026-11-14 - **Anisotropic filtering + mipmaps**: samplers follow the game's mip filter / level range
  and enable anisotropy when the game asks for it; the guest mip chain is uploaded (d82f660, see
  listofbugs.md). No separate PES_ANISO setting needed so far.
- 2026-11-14 - **Upscaling** (FSR 1 / SGSR 1) in the presenter with an output-resolution setting
  (2731f63, f8a69ab). Windows: both correct and sharper than plain scaling; SGSR 1 0.1-0.3 ms GPU.
- 2026-11-14 - **Launch menu** on Windows and iPad/iPhone (Play Game / Settings / About, generated
  stadium background, userdata/launcher.ini; f8a69ab).
- 2026-11-11 - **Touch controls on iPad** (touch_controls_ios.mm).
- 2026-11-10 - **Letterbox / pillarbox** (branch `letterbox`): `RecordBlitToSwapImage` in
  `runtime/video/presenter.cpp` fits the game image into the window with its aspect ratio kept and
  clears the rest to black; window resizes recreate the swap chain as before. `PES_STRETCH=1` = old
  full-window stretch. Verified on Windows with `tools/nav/window-shot.ps1` (resizes the window and
  saves what it shows): 1600x700 -> side bars, 900x900 and 1180x820 (iPad shape) -> bars top/bottom,
  1280x720 -> full window. Not yet seen on the iPad.
