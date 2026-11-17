# env.example.sh -- the Quest 3 toolchain for Git Bash, as tools/quest/common.sh and build_game.sh read it. Copy this file outside
# the repository, fill in the folders, and point QUEST_ENV at the copy (export QUEST_ENV=/c/path/to/env.sh) before
# tools/quest/build_game.sh. Everything is user-local: unpacked archives, no installers, no system PATH changes.
# The versions the published build was made with (README, "Build the APK"): Android NDK r29 (29.0.14206865), Android SDK
# build-tools 37.0.0 (aapt2, zipalign, apksigner, d8), platforms;android-34 (android.jar), platform-tools (adb), a JDK 17 or newer
# (Temurin 21 tested: javac, java, keytool), the Khronos OpenXR loader for Android 1.1.65 (the .aar from Maven Central, unpacked).
export ANDROID_SDK_ROOT="/c/path/to/Android/Sdk"                  # holds build-tools/, platforms/android-34/, platform-tools/
export ANDROID_HOME="$ANDROID_SDK_ROOT"
export ANDROID_NDK_ROOT="$ANDROID_SDK_ROOT/ndk/29.0.14206865"     # or wherever android-ndk-r29 is unpacked
export ANDROID_NDK_HOME="$ANDROID_NDK_ROOT"
export JAVA_HOME="/c/path/to/jdk-21"
export OPENXR_LOADER_DIR="/c/path/to/openxr_loader_1.1.65"         # the unpacked openxr_loader_for_android-1.1.65.aar (prefab/ inside)
# the newest build-tools folder (aapt2, zipalign, apksigner, d8)
ANDROID_BUILD_TOOLS="$(printf '%s\n' "$ANDROID_SDK_ROOT"/build-tools/*/ | sort -V | tail -1)"
export ANDROID_BUILD_TOOLS="${ANDROID_BUILD_TOOLS%/}"
export PATH="$JAVA_HOME/bin:$ANDROID_SDK_ROOT/platform-tools:$ANDROID_BUILD_TOOLS:$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/windows-x86_64/bin:$PATH"
# Optional: where the debug signing key is kept (made once by make_apk.sh; keep it: an APK signed with another key cannot be
# installed over this one without uninstalling it, and uninstalling deletes the game's saves and settings on the headset)
# export QUEST_KEYSTORE="$HOME/.android/ac0oculus_debug.jks"
