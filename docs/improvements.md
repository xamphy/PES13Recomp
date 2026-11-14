# Improvements

Planned improvements that go beyond the original console version (graphics, iPad comfort).
Bugs live in [listofbugs.md](listofbugs.md). When an item is done, move it to "Done" with the date.

Format: **title** - what it does. Difficulty / where / notes. (date added)

## Planned (suggested order)

3. **Post-process anti-aliasing (FXAA, later SMAA)** - one full-screen pass on the final frame before
   presenting (compute or fragment shader in the presenter). Smooths the jagged edges we have now
   (see 5). Easy-medium, a few hours. Setting to switch it off. (2026-11-10)
4. **Measure iPad performance** - fps / GPU time on the M2 with the 2026-11-14 changes (builds #20-#28;
   the log splits GPU time by category on the iPad now), before turning on anything heavier. Waiting
   for the testers' logs. Then: lazy clears (skip known clear draws, ~15 passes/frame). (2026-11-10)
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
