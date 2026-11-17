# Third-party notices

Our own code is under the Apache License 2.0 (`LICENSE`, `NOTICE`). Nothing below forbids that: every library the program links is
under a permissive licence, and the GPL material (the cockpit models, xBRZ) is not linked into the program and not in this source tree.

## In this repository

| Component | Where | Version | Licence | Text |
|---|---|---|---|---|
| ps2recomp: Ace Combat 5, by sal063 (recompiler, runtime, mod layer; our base) | `tools/ps2recomp`, `tools/vurecomp`, `runtime/` | fork of 2026-10-18 | Apache-2.0 | `LICENSE`; the changes are ours (`NOTICE`) |
| Dear ImGui, Omar Cornut (the PC build's settings window; not in the Quest build) | `third_party/imgui` | 1.92.9b | MIT | `third_party/imgui/LICENSE.txt` |
| stb_truetype, stb_rectpack, stb_textedit, Sean Barrett (through ImGui; stb_truetype also draws the quit panel's text, in both builds) | `third_party/imgui/imstb_*.h` | as in ImGui | MIT or public domain, at your choice | end of each file |
| ProggyClean and ProggyForever fonts, Tristan Grimmer (embedded in ImGui as its default fonts) | `third_party/imgui/imgui_draw.cpp` | as in ImGui | MIT | ImGui's `docs/FONTS.md` upstream |
| Lua, PUC-Rio (mod scripts) | `third_party/lua` | 5.4.9 | MIT | `third_party/lua/LICENSE.html` |
| miniz, Rich Geldreich and others (the Quest build's PNG reader; the PC updater's unzip) | `third_party/miniz` | 3.0.2 | MIT | `third_party/miniz/LICENSE` |
| sse2neon, SSE2NEON Contributors (the SSE code of the VU, GS and renderer on NEON; the Quest build only) | `third_party/sse2neon` | 1.9.1 (tag v1.9.1, commit 92f6de1) | MIT | `third_party/sse2neon/LICENSE` |

The upstream licences' copyright lines stay in those files. Removing them is not allowed by their terms.

## The Quest build: in the APK, not in this repository

| Component | Used as | Tested version | Licence |
|---|---|---|---|
| Khronos OpenXR loader for Android | `lib/arm64-v8a/libopenxr_loader.so` in the APK, its headers at build time | 1.1.65 (Maven Central `org.khronos.openxr:openxr_loader_for_android`) | Apache-2.0 (the .aar's `META-INF/LICENSE`; the headers Apache-2.0 OR MIT) |
| LLVM libc++ and compiler-rt builtins from the Android NDK | linked statically into `libmain.so` (`ANDROID_STL=c++_static`) | NDK r29 | Apache-2.0 with LLVM exception |
| The NDK's `android_native_app_glue.c` | compiled into `libmain.so` | NDK r29 | Apache-2.0 (AOSP) |
| The NDK's sysroot (Bionic headers, the platform's stub libraries) | headers and link stubs only; the libraries themselves are the headset's | NDK r29, API 32 | BSD-style and Apache-2.0 (the sysroot's `NOTICE`) |

## The Quest setup (`tools/setup/quest`): in the setup exe, not in this repository

The setup holds no Google package: the Android NDK (and, for the copy to the headset, Google's platform-tools) are downloaded from Google
on the player's PC after the player accepts the Android SDK License Agreement, or taken from an Android SDK already there. Its files
carry their licences in `licenses/`.

| Component | Used as | Version | Licence |
|---|---|---|---|
| Python embeddable package | runs the recompiler, the build and the copy to the headset | 3.12.10 | PSF-2.0 |
| Our runtime, compiled for the headset (objects, `liblua54.a`), the shaders, `classes.dex`, the APK's compiled manifest | linked with the player's own generated code | this repository | Apache-2.0 (ours), MIT (Lua, miniz, sse2neon) |
| Khronos OpenXR loader for Android | `libopenxr_loader.so` | 1.1.65 | Apache-2.0 |
| The text of the Android SDK License Agreement | shown on the setup's page before any download | Google's, from its repository manifest | Google's text, shown as is |

The APK is signed by the setup's own code (`apk_tool.py`: APK Signature Scheme v2) and aligned by it, without the Android build tools.

## The Quest build: tools, not in this repository and not in the APK

Fetched by whoever builds, each from its vendor (README, "Build the APK"); nothing of them is in this repository.

| Tool | Used for | Tested version | Licence |
|---|---|---|---|
| Android NDK (clang, lld, llvm-strip, glslc) | compiling `libmain.so` and the shaders | r29 (29.0.14206865) | the Android SDK License Agreement for the download; the toolchain's parts Apache-2.0 with LLVM exception and other open-source licences (its `NOTICE` files) |
| Android SDK build-tools (aapt2, zipalign, apksigner, d8), platforms;android-34 (android.jar), platform-tools (adb) | packing, signing and installing the APK; compiling the Java activity | 37.0.0; android-34; current | the Android SDK License Agreement |
| A JDK (javac, java, keytool) | the Java activity, d8 and apksigner, the debug key | Eclipse Temurin 21 | GPL-2.0 with the Classpath Exception |
| SDL3 headers, Vulkan headers | the declarations of the SDL subset the Quest build implements itself, and Vulkan's | 3.4.18; 1.4.363 | Zlib; Apache-2.0 OR MIT |
| CMake, Ninja, Python | the build and the recompiler | as in MSYS2 UCRT64 | BSD-3-Clause, Apache-2.0, PSF-2.0 |

The SPIR-V shaders, `rq_pipelines.keys` and `classes.dex` in the APK are compiled from this repository's own sources.

## The PC build: linked at build time, not in this repository

MSYS2 UCRT64 packages, versions as tested. A binary release has to carry these licence files next to the DLLs: MSYS2 installs them under
`ucrt64/share/licenses/<package>/`.

| Component | Used as | Tested version | Licence |
|---|---|---|---|
| SDL3 | `SDL3.dll`: window, input, audio | 3.4.18 | Zlib |
| OpenXR loader (Khronos OpenXR SDK) | `libopenxr_loader.dll`, headers | 1.1.62 | Apache-2.0 |
| Vulkan headers and loader (Khronos) | import library; `vulkan-1.dll` comes with the GPU driver | 1.4.363 | Apache-2.0 |
| GCC runtime: libgcc, libstdc++ | static in `ac5.exe`; `libgcc_s_seh-1.dll`, `libstdc++-6.dll` for the OpenXR loader | GCC 16.2 (15 or newer needed) | GPL-3.0-or-later with the GCC Runtime Library Exception 3.1, which lets our program carry them under its own licence |
| winpthreads (mingw-w64) | `libwinpthread-1.dll` | 14.0.0 | MIT and BSD-3-Clause |
| mingw-w64 CRT startup code | in `ac5.exe` | 14.0.0 | Zope Public License 2.1, public domain parts |

Build tools only, nothing of them ends up in the program: GCC (GPL-3.0-or-later), CMake (BSD-3-Clause), Ninja (Apache-2.0), glslc / shaderc (Apache-2.0; the
compiled SPIR-V is ours), Python (PSF).

## Optional tools the scripts can use

None of them is in this repository; each is fetched or installed by whoever wants it.

| Tool | Used by | Licence | Note |
|---|---|---|---|
| xBRZ, Zenju | `tools/textures/build_xbrz.sh` downloads and builds it for `upscale_hud.py` | GPL-3.0 | `tools/textures/xbrz_cli.cpp` is our small wrapper under Apache-2.0; a binary built from it with xBRZ is under the GPL-3.0 |
| Real-ESRGAN, Xintao Wang et al. (the models) and realesrgan-ncnn-vulkan | `tools/cockpit/upscale_tex.sh` | BSD-3-Clause; MIT | run as a separate program |
| FFmpeg | the PC build's optional upscaled movies (`ACZ_FMV_FFMPEG`) | LGPL or GPL, by build | run as a separate program, not linked |
| numpy, Pillow | a few cockpit paint scripts (`PYV`) | BSD-3-Clause, MIT-CMU | |

## Ideas and references, no code copied

The cloud noise follows the construction of sebh/TileableVolumeNoise (Sebastien Hillaire, MIT), written anew in GLSL
(`runtime/shaders/cloud_noise.comp`). The cloud rendering takes ideas from the published Nubis (Guerrilla) and Frostbite (Hillaire) talks.

## Texture packs

`textures/xbox/`, `textures/xbox_modern/`, `textures/touch/` and `textures/touch_modern/` are our own art, under the Apache-2.0 like the
code: button glyphs drawn by `tools/textures/make_glyphs.py`, with letter shapes rasterised from the Segoe UI Bold on the machine that
ran it. They are named after the runtime content hash of the game texture they stand in for; a hash is not a picture. The button names
and colours only say which controller button is meant; Xbox is a trademark of Microsoft, Touch and Quest of Meta.

The upscaled HUD and menu packs are not in this repository, because they are derivatives of the game's own pictures. The tools
that make them from your own disc are: `tools/textures/make_hud_pack.py` and `make_pack_from_dump.py`, from a run with `PS2_TEX_DUMP`.
Without them the game shows its original HUD and menu textures. What you make from your disc is for your own use.

## Cockpit models

No cockpit model is in this repository. A public cockpit set will be a separate download with its own `LICENSES.md`: per model the
source, the authors, the licence (GPL-2.0, GPL-3.0 and CC0 models) and the source offer. A set that contains the GPL-3.0 models is
GPL-3.0 as a whole. The program reads those files as data and is not a derivative of them; the converter in `tools/cockpit` and the
Quest's packer are ours. Texture upscales and compressed copies in a set are derivative works of the GPL textures, under the same
licence.
