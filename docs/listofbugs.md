# List of bugs

Known open bugs and gaps in the PES 2013 recomp (improvements: [improvements.md](improvements.md)), gathered from the development notes and
[IPAD.md](IPAD.md) on 2026-11-10. Add new bugs at the top of their section. When one is fixed, move it
to "Fixed" with the date and a one-line cause.

Format: **title** - symptom. Where / notes. (source / date)

## Open - crashes

- **77 `// ERROR` branches left, all in exception-handler code that never runs here** - after the
  2026-11-10 fix (see Fixed) the rest are in 34 pieces of Xbox CRT/library code around `__try/__except`:
  23 junk duplicates that XenonRecomp made from the 8 data bytes in front of each SEH function (nothing
  calls them; the real function covers the same code without errors), and 11 `__except` handler
  blocks that jump back into their parent function. The runtime never dispatches exceptions
  (`RtlRaiseException` only handles thread names), so none of them can run. Revisit only if
  exception dispatch is ever added. `node tools/error-branches.js` lists them. (2026-11-10)

## Open - rendering

- **Point sprites drawn at size 1** - the point-size export from XenosRecomp is not routed to the
  renderer. (RESUME "Known gaps")
- **Surface extent mismatches** - `FindSurfaceForTexture` often returns a surface that is smaller
  than the texture fetch (e.g. a 64x64 surface for a 128x128 fetch). This is common in normal scenes
  and has no visible effect yet. `PES_STRICT_SURFACE_EXTENT=<s>` rejects such surfaces. (2026-11-07)
- **GPU-only resolve surfaces never evicted** - about 97 old surfaces (~47 MB) stay in memory because
  they have no copy in guest memory. `WriteBackSurface` handles only small 32bpp atlases, so evicting
  them needs a general write-back. (2026-11-09)
- **Captures with many mid-frame dumps can crash** in the graphics driver at the final dump.
  Debug tool only. (RESUME, capture tool)
- **Validation messages from `--dump-surfaces`** - the textures lack TRANSFER_SRC usage. Harmless,
  debug only.

## Open - CPU / kernel / memory

- **`NtSuspendThread` on another running thread is ignored.** (RESUME "Known gaps")
- **E-view (0xE0000000) +0x1000 physical offset not modelled.** (RESUME "Known gaps")
- **"WPTR moved backwards?" warning** fires on a ring buffer that is legitimately full. It is a
  diagnostic only, but the message is misleading.
- **Main thread still spins a little** - in a match the process uses about 1.4 cores: the CP thread
  ~0.45, the main thread ~0.35, and thread 35 polling `KeDelayExecutionThread` ~0.25. This matters for
  battery life and for the iPad. (2026-11-09)
- **Intermittent slow boot followed by a crash** (2 of ~14 boots on 2026-11-08): 2-7 fps from the
  first frames, then a crash in the graphics driver or in `AllocPhysical` (heap corruption). Probably
  caused by the switch-table `.u64` bug that was fixed on 2026-11-09, but it was never reproduced, so
  this is not confirmed. Watch for it.

## Open - not implemented / unchecked

- **DLC `download:` device, XamContent\*, sign-in state** - only partly implemented.
  (RESUME "Known gaps")
- **Audio not yet heard by a person** - verified only from recordings (`PES_AUDIO_WAV`). In-match
  commentary and crowd have not been checked by ear.
- **Set pieces run at 30 fps** - this is the game's own choice (frames are held for a second vblank),
  not a bug. Listed so it is not reported again.

## Open - iPad / Apple

- **Free Training hangs after a match** - after finishing one match, entering Free Training stays on
  the loading screen (waited 2 min). After a restart of the app it loads at once. Not in the run #17
  log: that log starts after the restart. Does NOT reproduce on Windows (2026-11-14: full match, results
  "Return to Top Menu", Training, Free Training loads in ~10 s). Build #20+ keep one log per session and
  dump the guest threads when the app goes to the background - need a log of a hang. (tester, iPad run
  #17, 2026-11-12)
- **Heavy scenes slow on iPad** (GPU-bound) - tester on run #17: match play 60 fps (54 sometimes, ~50
  near the goals), replays 15-23 fps, goal celebration 13 fps, Free Training a steady 30 fps, Become a
  Legend cutscenes 20 fps. Log: those scenes take 33-65 ms of GPU time per frame with ~47-49 render
  passes (about 25 one-draw passes ended by resolves, about 6 empty passes, ~26 Mpx framebuffer
  load+store per frame). Device was "thermal serious" from launch. (tester + log, 2026-11-12)
  2026-11-14: resolve skip, pass areas, discard loads and clear fills (builds #20-#26; Windows: GPU
  -22..25%, tile copies -80%) - iPad effect not measured yet; the log now splits GPU time by category.
- **4 GB guest reservation needs `extended-virtual-addressing`** - a free Apple ID may drop this
  entitlement. If it does, the session's `logs/pes13 <date> <time>.err.log` shows `failed to reserve 4 GB guest address space`. Fix:
  a paid developer account, or shrink the reservation. (IPAD.md)
- **Slow first launch** - shaders are translated on first use and cached in `userdata`. (IPAD.md)

## Fixed (recent, for reference)

- 2026-11-14 - **Pitch shimmered like TV static in motion** (user, d82f660): textures had one mip level
  and samplers maxLod 0.25, so distant surfaces were sampled at full resolution. The guest mip chain
  (packed tail included) is now uploaded and sampled with the game's mip filter, level range and
  anisotropy. `PES_NO_MIPS=1` = old behaviour. Checked on Windows stills; in motion still to be
  confirmed by the user.
- 2026-11-10 - 188 `// ERROR` branches in 15 functions (branch `fix-error-branches`, commit 45d71a0):
  14 jump-table functions had hand-set sizes in `config/pes13.toml` that stopped before their last
  few instructions (the Game Plan bug again), so every `switch` case that jumped to the shared end
  returned early with the wrong value or skipped its last step. Sizes now run to the next real
  function (checked against .pdata, `bl` targets and the disassembly); `sub_830AA0BC` grew too.
  Only these 15 functions changed in generated/. Verified: 2 stability match sessions (~20k frames
  each), exhibition, Football Life menus, training match. `PES_CALL_WATCH=<hex,...>` showed
  `sub_82279888` runs ~1000x/s in matches; the other 14 were not reached in those runs.

- 2026-11-10 - Game Plan crashed the game (pre-match and in-match pause): `sub_82278CB8` (a 107-case
  getter) was cut off at 0x82279108 by its hand-set size in `config/pes13.toml` (0x450, should be
  0x480). Its final `b 0x8227910C` became a return, so it returned the input pointer instead of the
  value, and `sub_8297BD70` used that pointer as an index into a string table at 0x82141FC4 ->
  read of uncommitted memory. Size fixed, generated/ regenerated (the only real change is that
  function), and the stale line removed from `runtime/debug/call_counter.gen.cpp`. Verified: Game
  Plan opens from the pre-match menu and from the pause menu. Note: `XenonRecomp.exe` was older than
  its source (it emitted `.u64` switches again) - it was rebuilt before regenerating.

- 2026-11-10 - Become a Legend (Original Player) looped on "no name entered": there is no
  on-screen keyboard, and `XamShowKeyboardUI` returned the empty default text. Now a text dialog
  opens over the game window (`Video::PromptText`, `window_win32.cpp`): Enter = OK, Esc = Cancel
  (Cancel returns ERROR_CANCELLED, as on the console). Calls and answers are logged as
  `[xam] XamShowKeyboardUI ...`. `PES_KEYBOARD_TEXT=<text>` answers without a dialog (scripted
  runs). Verified: the name typed in the dialog was accepted, and the game moved on to step 2
  (nationality). **Still open:** the dialog on iPad/Mac (window_sdl.cpp keeps the default or
  "Player"); the Cancel path is untested.

- 2026-11-09 - Crashes at jump tables: XenonRecomp emitted `switch (rN.u64)`, but the guest checks
  only 32 bits. Changed to `.u32`.
- 2026-11-08 - Corrupted file reads with write tracking: `NtReadFile` now reads into a host buffer
  first, then copies into guest memory.
- 2026-11-08 - Jersey numbers drawn as cyan blocks: wrong byte order in the depth->color tile
  transfer, and tile claims covered the whole scissor instead of the clear rectangle.
- 2026-11-08 - Black bars (padded-surface sampling); flag wedges (face culling was never implemented).
- 2026-11-07 - Flat dark or invisible players and goalkeeper: kit resolves were not mirrored to guest
  memory.
- 2026-11-07 - Missing letters in menus/HUD (glyph texture fix).
- 2026-11-09 - Image leak (no image was ever freed): added eviction.
