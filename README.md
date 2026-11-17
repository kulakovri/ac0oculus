# ac0oculus

**Version 0.1.2, an early build.** Tested on one Meta Quest 3, by its author, so far. To play, follow the five steps below; everything
from Build the APK on is the source route, for working on the code.

A native recompilation of **Ace Combat Zero: The Belkan War** (PlayStation 2, US release, SLUS-21346) that runs **standalone on a
Meta Quest 3**: no PC while you play, no streaming, no emulator. The game's code is translated to C ahead of time on your own PC, from
your own disc, and compiled into an Android app (an APK) together with a runtime that stands in for the console: a Vulkan renderer with
a native eye pass for the headset's GPU, OpenXR, the vector units as native code, sound, memory cards, controllers. You sit in the
cockpit in stereo and look around with your head.

It is the sibling of [ac0vr](https://github.com/kulakovri/ac0vr), the PC VR version: the same runtime and recompiler, with the Quest's
platform layer and renderer on top. This tree still builds the PC version too, but for that ac0vr is the place to go.

**This repository holds no game.** No code, data, pictures, sound or movies of the game, no SDK files, no prebuilt program. You need your
own disc: the recompiler reads the executable on it, and the game loads everything else from it while you play. The APK you build
contains code translated from your disc: keep it for yourself, do not share it. This is a free fan project, not affiliated with or
endorsed by Bandai Namco Entertainment, Sony Interactive Entertainment or Meta. It unlocks nothing and sells nothing.

## Play it in five steps

You need your own US disc of the game, a Windows PC for one build and a USB-C cable. Nothing of the game is in these files: the setup
builds the Quest app on your PC from your disc.

1. **Download the installer.** On the [Releases page](../../releases/latest), take `ac0oculus-0.1.2-setup.exe` (about 18 MB) and put it
   in a new, empty folder.
2. **Put your disc image next to it.** An ISO image of your own *Ace Combat Zero: The Belkan War* disc, US version (SLUS-21346), in the
   same folder. A DVD drive and a free tool such as ImgBurn make the image from the disc. Other regions do not work.
3. **Optional: the asset pack.** [Download it here](https://mega.nz/file/OlRTjLJb#0u79tZS7vhGmdsZTuEKONcqgwlGS5ou7aZikHeTmfl0) (a 777 MB
   zip, 1.9 GB unpacked: 3D cockpits for all 36 aircraft and the gloves, HUD and menu textures), unpack it, and put its folder
   `ac0oculus-assets` next to the setup. This pack is not made by this project and is not part of it: it is a third-party collection
   that happens to fit, offered without any warranty or responsibility; use it at your own risk. Without it the game flies from its own
   views, and you can add the pack later: run the setup again with the folder beside it, then step 5 again.
4. **Run the setup.** Windows may say "Windows protected your PC" because the setup is not signed yet: click *More info*, then *Run
   anyway*. Click through the wizard; it finds the disc image by itself. If Google's Android NDK is not on your PC, the wizard shows
   Google's licence and, once you accept it, downloads the NDK (834 MB). Then it builds the Quest app from your disc in a console
   window: about a minute and a half.
5. **Copy it to the Quest.** Once: turn on developer mode (Meta Horizon app on your phone > your headset > Developer Mode; Meta asks
   for a free developer account). Connect the headset to the PC with a USB-C data cable, put it on and accept *Allow USB debugging*.
   Then Start menu > *ac0oculus - copy to the Quest*: it installs the app and copies your disc image and the pack (under a minute over
   USB 3).

Then, in the headset: Library > *Unknown sources* > *ac0oculus*. The first start takes a little longer (the shaders are prepared once).

**Controls.** A gamepad paired with the headset over Bluetooth flies best: its first press makes it the controller in use. The Touch
controllers work too, as an Xbox pad with the same letters, and the game's menus show them: A is OK, Y is cancel, and the Menu button
of the left controller is Start and pause. On the title screen (PRESS START BUTTON) A or a trigger starts as well. In flight, closing
a hand at the cockpit's stick or throttle takes it into your hand; if you would rather keep the controllers a plain pad, set pause
menu > ENHANCEMENTS > CONTROLS > TOUCH CONTROLLERS to AS A PAD (from the next start). The pause menu's TOUCH CONTROLS card shows what
every button does.

**What you need:** a Meta Quest 3 with about 4.5 GB free (plus 1.9 GB for the pack); Windows 10 or 11, 64-bit, with about 5 GB of disk
for the build; your own US disc of the game; an Xbox or PlayStation pad if you have one. Details for each step are under "The setup".

## What it does

- The whole campaign on the headset, both eyes in one pass, the display at 72 Hz with the game's 60 frames a second paced to it.
- Volumetric clouds that stay put when you turn your head, 3D cockpits with live instruments and the game's radar and map on the cockpit
  screens (with a cockpit set, see Cockpits), weather on the canopy, head-tracked stereo sound tuned for the headset's speakers.
- An Xbox or PlayStation pad paired with the headset, or the Touch controllers; controller glyphs for the device in your hands.
- An ENHANCEMENTS page in the pause menu: one tuned look for the Quest 3, every improvement can be switched off.
- No network: the app has no internet permission and sends nothing anywhere. There are no usage statistics in this build.

## What you need

- **The game:** your own US disc, *Ace Combat Zero: The Belkan War*, serial SLUS-213.46, as an ISO image. Other regions do not work: the
  configuration is tied to addresses in the US executable.
- **A Meta Quest 3** (Quest 3S is declared but untested) in developer mode, a USB-C data cable, and about 4.5 GB free on it for the disc
  image (plus about 1.9 GB for a cockpit set).
- **A Windows 10 or 11 PC**, 64-bit, for the build: about 4 GB of disk for the toolchain and 1 GB for the build.
- **A gamepad** (Xbox or PlayStation, paired with the headset over Bluetooth) is the best way to fly; the Touch controllers work too.

## The setup

`ac0oculus-0.1.x-setup.exe` (about 18 MB) holds no game: Python, the recompiler, our prebuilt runtime for the headset and two small
scripts. It needs no Java, Gradle, Android Studio or MSYS2.

1. **Put it in a folder with your disc image**, an ISO of your own US disc. Optional: a folder `ac0oculus-assets` beside it (step 3
   above), laid out as the headset's data folder (`cockpits_q4/`, `cockpits_ql_t9/`, `textures/`). Such a pack is not made by this project and is not part of
   it; without it the game flies from its own views.
2. **Run it.** Windows may say "Windows protected your PC" (the setup is not signed): *More info*, *Run anyway*. The wizard installs for
   your user only (`%LOCALAPPDATA%\Programs\ac0oculus`, no administrator), finds the disc beside it and checks it (only SLUS-21346
   passes), and looks for Google's Android NDK r29 on your PC (Android Studio's folder). If it is not there, the setup shows Google's
   Android SDK License Agreement and, once you accept it, downloads the NDK from Google during the install (834 MB, its SHA-1 checked;
   only the parts the build needs are unpacked).
3. **It builds the Quest app** from your disc, in a console window, in about a minute and a half on 8 threads (plus the download):
   `ac0oculus.apk` in the program's folder, signed with a key of your own that stays in `%APPDATA%\ac0oculus` (an uninstall keeps it,
   so a later build installs over this one).
4. **Start menu, "ac0oculus - copy to the Quest"**, with the headset in developer mode on the USB-C cable: it installs the app, gives it
   access to its data folder, and copies your disc image and the data to `/sdcard/ac0oculus/` (about 20 seconds over USB 3). It uses the
   adb of your Android SDK if there is one, else Google's platform-tools, downloaded under the same licence. A second run copies only
   what changed. "ac0oculus - build the APK again" rebuilds (another disc, a new release).

The setup can also be built from this repository with your own disc's build: `tools/setup/quest/make_release_quest.sh`.

## Build the APK

All commands run in Git Bash from the repository's folder. Nothing is installed system-wide: every tool below is an archive you
unpack somewhere of your own.

**1. The toolchain.** From their vendors (the exact versions the published build was made with; newer ones are untested):

| Piece | What for | Where from |
|---|---|---|
| Android NDK r29 (29.0.14206865) | clang for arm64, the Android sysroot, glslc, the app glue | developer.android.com/ndk/downloads |
| Android SDK build-tools 37.0.0, platforms;android-34, platform-tools | aapt2, zipalign, apksigner, d8; android.jar; adb | the Android command-line tools' `sdkmanager` |
| A JDK 17 or newer (Temurin 21 tested) | javac, java, keytool | adoptium.net |
| Khronos OpenXR loader for Android 1.1.65 | `libopenxr_loader.so` and the OpenXR headers | Maven Central, `org.khronos.openxr:openxr_loader_for_android:1.1.65` (an .aar: unzip it) |
| [MSYS2](https://www.msys2.org) UCRT64: cmake, ninja, python, the SDL3 and Vulkan headers | the build system, the recompiler, headers | `pacman -S mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-vulkan-headers` |

Google's packages come under the Android SDK License Agreement, which you accept when you download them. Copy
`tools/quest/env.example.sh` outside the repository, fill in the folders, and point the build at it:

```
export QUEST_ENV=/c/path/to/your/env.sh
export PATH=/c/msys64/ucrt64/bin:$PATH        # MSYS2's folder, if it is not in C:\msys64 change it
```

If MSYS2's headers are not in `C:\msys64\ucrt64\include`, add `CMAKE_ARGS=-DPS2_QUEST_HOST_INCLUDE=<that folder>` to step 4.

**2. Take the executable from your disc.** Copy the file `SLUS_213.46` from the root of the disc image to `game/SLUS_213.46` in this
folder (7-Zip opens an ISO), then check it:

```
sha256sum game/SLUS_213.46
```

It has to print `34ee58ca92a6e8913aa84b6dc5af3534b878a44ca4e33173e962c6090ea243c1` (3,293,064 bytes). Anything else is a different
version: stop here.

**3. Recompile it to C** (well under a minute; the generated code goes to `gen_zero/` and stays on your machine):

```
PYTHONPATH=tools python -m ps2recomp game/SLUS_213.46 -o gen_zero \
    --ida-db config_zero/ida_like_zero.json --ida-seeds config_zero/seeds_zero.json \
    --symbols config_zero/sdk/sdk_symbols.json --symbols config_zero/sdk/manual_symbols.json \
    --overrides config_zero/sdk/overrides_draft.json
python tools/setup/vu_images_from_elf.py build game/SLUS_213.46 config_zero/vu_recipe.json out/vu_programs
PYTHONPATH=tools python -m vurecomp --programs out/vu_programs -o gen_zero/ps2_vu1_progs.inc
```

The first command ends with `emitted 14644 functions into 59 files`; the last two turn the game's vector-unit programs into C.

**4. Compile and package:**

```
bash tools/quest/build_game.sh
```

The first build compiles the generated code and the runtime with the NDK's clang at -O2 (about 2 minutes on 8 threads; the vector units' file alone takes over a minute). Then it
checks the shaders, compiles the small Java activity that passes the gamepad to the game, packs the APK without Gradle (aapt2, the
libraries stored and 16 KB aligned, zipalign, apksigner) and checks it. The result is `build/quest/aczquest.apk` (about 40 MB). The APK
is signed with a debug key that `make_apk.sh` makes once in `~/.android/ac0oculus_debug.jks`: keep that file. An APK signed with
another key cannot be installed over this one without uninstalling it first, and uninstalling deletes your saves and settings.

## Install it on the headset

1. **Developer mode:** in the Meta Horizon app on your phone, the headset's settings, Developer Mode on (Meta asks you to create a
   developer organisation, free). Connect the headset to the PC with the USB-C cable, put it on and accept *Allow USB debugging*
   (tick *Always allow from this computer*). `adb devices` lists it as `device`.
2. **The app, and its permission to read its data folder** (Git Bash rewrites `/sdcard/...` arguments unless told not to, hence the
   first line; `cygpath -m` gives adb the Windows form of a path):

   ```
   export MSYS_NO_PATHCONV=1
   adb install -r -g build/quest/aczquest.apk
   adb shell appops set --uid com.kulakovri.ac0oculus MANAGE_EXTERNAL_STORAGE allow
   ```

3. **The data:** the disc image and the recompiler's memory image go to `/sdcard/ac0oculus/` on the headset; the game reads the disc
   from there while you play.

   ```
   adb shell mkdir -p /sdcard/ac0oculus/gen_zero
   adb push "$(cygpath -m /c/path/to/your_disc.iso)" /sdcard/ac0oculus/disc.iso
   adb push gen_zero/ps2_image.bin /sdcard/ac0oculus/gen_zero/ps2_image.bin
   adb push textures/. /sdcard/ac0oculus/textures/              # optional: the controller glyphs (and packs you made)
   ```

   The disc image is 4.24 GB: about 20 seconds over a USB 3 cable (285 MB/s measured with the Link cable), a few minutes over USB 2. It
   is pushed once; later updates only reinstall the APK.
4. **Play:** in the headset, Library, the *Unknown sources* filter, *ac0oculus*. The first start takes longer (the shaders' pipelines
   are built and cached). Your saves and settings live in the app's own folder (`/sdcard/Android/data/com.kulakovri.ac0oculus/files/`).

The app can record each session as a video (the game's picture and sound) into `rec/` in that folder, for bug reports: pause >
ENHANCEMENTS > SYSTEM > RECORD MY SESSIONS, off by default. Your voice goes into it only with `ACZ_QUEST_RECORD_MIC=1` in
`commandline.txt` (the app then asks for the microphone). Nothing leaves the headset.

## Cockpits

The cockpit models are not in this repository: each file holds a model's meshes and textures. The Quest reads its own compressed set
from `/sdcard/ac0oculus/cockpits_q4/` (BC7 / BC1 textures with their mips, about 1.8 GB for 36 aircraft and the gloves), made from a set
of PC cockpit files by `tools/quest/make_cockpits_q4.sh` (its packer, `tools/cockpit/cpit_pack.cpp`, builds with
`tools/cockpit/build_cpit_pack.sh`). The PC set's models of the FlightGear community (GPL) are built by the scripts in
`tools/cockpit/`; a public set is not published yet. The optional pack of step 3 above brings a full set laid out this way. Mesh LOD side files, if a set brings them, go to `/sdcard/ac0oculus/cockpits_ql_t9/`.

Without a cockpit set you fly from the game's own views, which is the original game.

## Settings

The pause menu's ENHANCEMENTS page holds the Quest's options (docs/SETTINGS.md, "The Quest"). For testing, `/sdcard/ac0oculus/commandline.txt`
sets any switch of `docs/ENV_REFERENCE.md` before the game starts: one `KEY=VALUE` per line, `#` for comments.

## Known issues

- **Tested on:** one Quest 3 (Horizon OS v209), on the cable and on battery. The Quest 3S, other Horizon OS versions and every other
  headset are untested.
- On battery the headset holds its GPU at a lower clock than on the charger; the frame is tuned to hold either way, with less margin on
  battery.
- Campaign saves work, but the full save and reload cycle has not had a systematic test.
- No upscaled movies on the Quest: the game's own play at their original size.
- The source route needs a Windows PC; the build scripts are Git Bash scripts.

## Credits and licences

- **sal063**, [ps2recomp: Ace Combat 5](https://github.com/sal063/Ace-Combat-5-Static-recompilation): the recompiler, the runtime and the
  native renderer this project grew from. Its README is kept in `docs/README_ac5recomp_upstream.md`.
- Dear ImGui, Lua, stb, miniz, sse2neon, SDL3's headers, the Khronos OpenXR and Vulkan projects.

Our code is under the Apache License 2.0 (`LICENSE`, `NOTICE`). Everything else, with versions and licences: `THIRD_PARTY_NOTICES.md`.
The game belongs to Bandai Namco Entertainment; please buy and keep your own copy.

## Rights holders

If you hold rights in something here and want it changed or removed, open an issue in this repository (or use GitHub's own notice
process). We will answer and act quickly.
