# Running PES 2013 on the iPad (from a Windows PC, no Mac)

Status (2026-11-11): **playable** on an M2 iPad Air (installed with Sideloadly and a free Apple ID).
Menus, exhibition matches and sound work; on-screen touch controls appear when no controller is
connected. Matches run at about 40-45 fps; set pieces, cutscenes and the Game Plan screen drop to
10-25 fps for now. If something goes wrong, the logs below are how we find it.

## 1. Get the .ipa

The `.ipa` contains code recompiled from the game, so it is never published here: you build your own.
It is built on macOS (a Mac, or a macOS runner in your own **private** repository) from a checkout
whose `generated/` folder was filled from your own disc (see the README), with the iOS SDK and
MoltenVK. The result is `pes13.ipa` (~26 MB).

## 2. Install it with AltStore (Windows)

1. Install **iTunes** and **iCloud** from Apple's website (not the Microsoft Store versions - AltServer
   needs the web installers), then **AltServer for Windows** from altstore.io.
2. Connect the iPad by USB, trust the PC, run AltServer, and from its tray icon install **AltStore**
   onto the iPad with your Apple ID.
3. On the iPad: Settings -> General -> VPN & Device Management -> trust your Apple ID.
   iPadOS 16+: also turn on **Developer Mode** (Settings -> Privacy & Security) and restart.
4. Copy `pes13.ipa` to the iPad (or open it from Files) and install it in AltStore (My Apps -> +).
5. A free Apple ID signs apps for **7 days**; AltStore refreshes them when the PC with AltServer is on
   the same Wi-Fi.

**Sideloadly** works too. Make sure the **iPad** is selected in its device list: Apple Silicon Macs
run iPad apps, so with a Mac on the same network it can install to the Mac instead.

**Entitlements:** the app asks for `extended-virtual-addressing` (the runtime reserves 4 GB of address
space for the Xbox 360 memory) and `increased-memory-limit` (a match needs ~1 GB). A free Apple ID may
not be allowed to keep them - AltStore then drops them. If the app quits immediately, check
`pes13.err.log` for `failed to reserve 4 GB guest address space`: that means the entitlement was
dropped, and we either need a paid developer account or to shrink the reservation (work on our side).
On the first device test (free Apple ID, Sideloadly) the reservation succeeded.

## 3. Copy the game files

The app looks for the game in **its own Documents folder** (Files app -> On My iPad -> PES 2013):

```
PES 2013/
  game/
    xex/default.xex      (46 MB, from the PC: pes13\game\xex)
    files/...            (5.7 GB, from the PC: pes13\game\files)
```

From Windows: the **Apple Devices** app (or iTunes) -> the iPad -> **Files** -> PES 2013 -> drag the
`game` folder in. The app folder appears after the first launch (launch once, it will stop because the
game files are missing).

## 4. Play / report back

- Controller: any Bluetooth Xbox / PlayStation / MFi controller (pair it in iPad Settings first).
- Logs: `pes13.log` and `pes13.err.log` in the same Documents folder (copy them back to the PC the
  same way) - send those to Claude after a failed or odd run.
- `userdata/` (saves, shader cache) is created next to them.

## Known gaps

- Set pieces, cutscenes and the Game Plan screen drop to 10-25 fps (matches: 40-45 fps).
- The iPad throttles when it gets hot: the overlay under the picture shows the thermal state (and
  LOW POWER when Low Power Mode is on); changes are logged as `[perf]` lines in `pes13.log`.
  Playing while charging makes it heat up sooner.
- The first launch is slow: shaders are translated on first use and cached in `userdata`.
- Fixed: the main menu background movie did not play (file names with capitals were not found
  on the case-sensitive iPad file system).
