# Running PES 2013 on Android (early)

Status (2026-11-17): **early, playable on one device.** Tested on a Huawei MatePad 11.5
(Snapdragon 7 Gen 1, Adreno 644, HarmonyOS 3.1 = Android 12) with a PS4 controller and with the
on-screen touch controls: menus at 60 fps, light 3D around 40-60 fps, matches about 27 fps
(GPU-bound, faster than an iPhone 13 Pro). No other phones or tablets have been tried yet.

One APK covers every GPU (Adreno, Mali, Xclipse, ...): the game uses the phone's own Vulkan driver and
translates the Xbox shaders on the device.

**Requirements:** a 64-bit (arm64) device with **Vulkan 1.1** and **Android 9** or newer. About
1.3 GB of free RAM while playing; 6 GB devices or more are recommended.

## 1. Get the .apk

The `.apk` contains code recompiled from the game, so it is never published here: you build your own
from a checkout whose `generated/` folder was filled from your own disc (README, steps 1-3).

**Requirements:** Android NDK r27 or newer, CMake ≥ 3.20, Ninja, JDK 17 and the Android SDK
(build-tools, platform 35). The build needs about 16 GB of RAM. On Linux or macOS:

```sh
# 1. The native part: game + runtime + SDL3 -> libmain.so (arm64-v8a)
cmake -S . -B build-android -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DANDROID_STL=c++_static
ninja -C build-android pes13

# 2. The app: Java side (launch menu, touch controls, overlay) + libmain.so
mkdir -p android/app/libs/arm64-v8a
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-debug \
  -o android/app/libs/arm64-v8a/libmain.so build-android/libmain.so
cd android && ./gradlew assembleRelease
# -> android/app/build/outputs/apk/release/app-release.apk
```

The release APK is signed with your local debug key. **Keep using the same key for every build:**
Android only installs an update over an app signed with the same key, and uninstalling deletes the
app's folder (your copied game files and saves).

## 2. Install it

Copy the APK to the device and open it (allow installing from that source when asked).
To update later, install the new APK over the old one; **don't uninstall**.

## 3. Copy the game files

Start the app once: it creates its folder. Then, from a PC over USB (file transfer mode), copy your
dump into

```
Internal storage/Android/data/com.xamphy.pes13/files/game/xex/default.xex
Internal storage/Android/data/com.xamphy.pes13/files/game/files/   (dt00_e.img ... everything)
```

The launch menu says where the files go if it can't find them.

## 4. Play

The launch menu comes first: **Play Game**, **Settings** and **About**. It works with touch and with a
controller (D-pad / stick to move, A to select, B to go back).

Settings (shared with the Windows and iPad builds):
- **Upscaling** (Off / FSR 1 / SGSR 1) and **upscaling resolution**: SGSR 1 costs about 1 ms per frame on
  an Adreno 644.
- **Touch controls**: the iPad layout (floating stick, D-pad, A/B/X/Y, LB/RB/LT/RT, Back/Start, LS/RS),
  hidden while a controller is connected.
- **Performance overlay**: FPS, CP time, draws, RAM, temperature and battery under the picture.
- **Blur effects**: the game's glow, pause-menu blur and soft filter. Off makes the pause menu and Game
  Plan noticeably faster; cutscenes and replays are bound by their geometry and change little.

Bluetooth controllers (PlayStation, Xbox, ...) work as player 1. The Android back button / gesture is B.

## 5. Report back

Every session writes its own log pair to `Android/data/com.xamphy.pes13/files/logs/`
(`pes13 <date> <time>.log` and `.err.log`, the newest 10 sessions are kept). Send the session that
showed the problem; crashes are logged there too (`[crash]` lines).

## Known gaps

- Only one device tested so far (Adreno 644). Mali and Xclipse GPUs are untested.
- Switching to another app and back is new code (the display surface is recreated); tell us if the
  game closes or the picture freezes. `[android]` lines in the log say what Android did.
- The first time a scene is shown, its shaders are compiled (short stutters, `[gpu] slow batch` lines).
