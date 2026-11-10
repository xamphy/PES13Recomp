# Navigation scripts (task T3, owner Claude)

Input scripts for `PES_INPUT_SCRIPT` that reach a given screen. Timings are seconds after
launch; fractional seconds work (`down@101.5`). Each press lasts 200 ms. Measured on the build
with frames in flight (2026-11-07, RTX 4050, window **hidden** - see the warning below).

**Warning: launch with `-WindowStyle Hidden` (or normal), never `Minimized`.** The presenter
skips minimized windows, so nothing renders and `PES_SHOTS` writes nothing.

## Timeline (mapping run 2026-11-07, shots every 5 s: `userdata/shots/nav1/sheet.png`)

| Time | Screen | Input |
|---|---|---|
| ~50 s | title "PRESS START" | `start@55,start@57` |
| 60 s | "Select Storage Device" | `a@66` |
| 70 s | transition (movie frame behind) | - |
| 75-80 s | "Training now features Performance Training" Yes/No | `a@76` (Yes) |
| 85 s | controller layout + "Now Loading" | `a@86` |
| 90-95 s | training ground loading / black | (`a@96` harmless) |
| ~100 s | **Performance Training menu**, cursor on Dribbling | select below |
| 105 s+ | chosen drill | |

Performance Training menu order (down presses from Dribbling): Dribbling 0, Defence 1, Trap 2,
Long-pass 3, **Shot 4**, 1-2 Pass 5, Feint 6, Manual Controls 7, **Penalties 8**, Free Kicks 9,
Teammate Controls 10, Off the Ball Controls 11, then "Return to Top Menu".

**The Dribbling drill never shows a goal or a goalkeeper** (camera follows the dribbler in
midfield, 105-230 s). Use Shot or Penalties for goalkeeper work.

## Timing rules (learned the hard way)

- **Load times vary by ~10 s between runs.** In one run the training menu was ready at 100 s; in
  the next it was still animating in at 107 s and the down presses were lost. The training menu
  waits forever, so **make menu presses late (~125 s)**, not right when the menu usually appears.
- **`PES_SHOTS` times are not input times.** The screenshot clock starts at the first presented
  frame, the input clock at process launch, so a shot labelled `t` happens a few seconds after
  input time `t`.
- Penalties (8 down) reached the right menu entry, but A did not start it (tried at 106-130 s,
  cause unknown). Use the Shot drill.

## Scripts (`tools/nav/<name>.txt`, launcher `tools/nav/run-nav.ps1`)

| Name | Target | Status |
|---|---|---|
| `shot-drill` | **Shot drill with the goal + goalkeeper in view**: goal on screen at shot times ~141-150 s and ~165-174 s (after the `b` shots). | **verified 2026-11-07** (`userdata/shots/nav4/sheet.png`); reproduces the invisible goalkeeper: shadow in front of the goal, no body (`nav4/goal147.png`, `goal168.png`) |
| `dribbling` | Dribbling drill (no goal, no goalkeeper) | late A (125 s) not run yet; `a@106` verified in run nav1 |

Usage (holding the machine lock):
`tools\nav\run-nav.ps1 -Script shot-drill -Shots '141,144,147,150,165,168,171' -Log "$env:TEMP\pes_x.log"`
Add any `PES_*` debug variables in the shell first (e.g. `PES_CAPTURE=147`,
`PES_STRICT_SURFACE_EXTENT=160` for a same-run A/B: goal views before and after 160 s).
