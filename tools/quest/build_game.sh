#!/bin/bash
# build_game.sh [--clean] -- the Quest 3 standalone APK of the game (aczquest; NOTES_QUEST.md stage 2).
#   1. configures build/quest (CMake -DPS2_QUEST=ON: the NDK r29 toolchain, arm64-v8a, android-32) once;
#   2. builds libmain.so (the generated code + the runtime + runtime/src/android/) through a private script;
#   3. packs build/quest/shaders/*.spv as APK assets (assets/shaders/: unpacked to the files dir at start, ps2_android.c)
#      and the OpenXR loader as a second library, through tools/quest/make_apk.sh -> build/quest/aczquest.apk
#      (package com.kulakovri.ac0oculus, label "ac0oculus"; signed with QUEST_KEYSTORE, made once by make_apk.sh). make_apk.sh strips the
#      copies it packs (llvm-strip --strip-unneeded) and runs tools/quest/check_apk.sh (manifest, signature, 16 KB
#      alignment, NEEDED set, undefined symbols against the API-32 stubs); build/quest/libmain.so keeps its symbols for
#      symbolising tombstones ($ANDROID_NDK_ROOT/ndk-stack -sym build/quest).
# The game data never goes into the APK: the README's "Install" puts the disc image, gen_zero/ps2_image.bin and the
# texture pack on the headset (/sdcard/ac0oculus/).
#   4. compiles the activity runtime/src/android/java/ (PadActivity: NativeActivity + the gamepad's events over JNI,
#      ps2_pad_android.c) with the JDK's javac and the build-tools' d8 into classes.dex (PAD_JAVA=0: none, the plain
#      android.app.NativeActivity as before).
# Env: QUEST_ENV (the toolchain), JOBS (8), HEAVY (a command to run the build under, e.g. nice), NO_HEAVY=1 (none), CMAKE_ARGS (extra configure arguments), LABEL, PAD_JAVA (1),
#   SPV_GUARD (1: the R-45 shader guard, tools/quest/rq_probe/spv_guard.py with spv_guard_allow.txt, fails the build;
#   0 skips it, test builds only).
set -e
cd "$(dirname "$0")/../.."
. tools/quest/common.sh
# ninja and cmake from MSYS2's ucrt64, APPENDED: MSYS2's own coreutils (mktemp: another /tmp than Git Bash's) must not
# shadow Git Bash's -- the APK checks' temporary folders vanished that way; the compiler comes from the NDK's toolchain file
export PATH=$PATH:/c/msys64/ucrt64/bin
B=build/quest
[ "$1" = --clean ] && rm -rf "$B"
mkdir -p "$B"
t0=$(date +%s)
if [ ! -f "$B/build.ninja" ]; then
    cmake -S . -B "$B" -G Ninja -DPS2_QUEST=ON -DCMAKE_BUILD_TYPE=Release -DPS2_GENERATED_DIR=gen_zero $CMAKE_ARGS \
        > "$B/cfg.log" 2>&1 || { tail -20 "$B/cfg.log"; exit 1; }
fi
HEAVY=${HEAVY:-}
[ "${NO_HEAVY:-0}" = 1 ] && HEAVY=
$HEAVY ninja -C "$B" -j "${JOBS:-8}" > "$B/build.log" 2>&1 || { grep -E 'error|FAILED' "$B/build.log" | head -30; exit 1; }
t1=$(date +%s)
# the R-45 guard (outside note 6 item 5): no fragment stage of the foveated eye pass may read a subpass input, or hold
# specialization constants and read gl_FragCoord, unless tools/quest/rq_probe/spv_guard_allow.txt says why it never runs
# there; SPV_GUARD=0 skips it (a test build only)
if [ "${SPV_GUARD:-1}" != 0 ]; then
  python -I tools/quest/rq_probe/spv_guard.py "$B/shaders" tools/quest/rq_probe/spv_guard_allow.txt > "$B/spv_guard.log" 2>&1 \
    || { grep -E '^(FAIL|STALE)|^spv_guard' "$B/spv_guard.log"; echo "build_game: the R-45 shader guard failed (see $B/spv_guard.log)"; exit 1; }
  tail -1 "$B/spv_guard.log"
fi
mkdir -p "$B/apk_assets/shaders"
rm -f "$B/apk_assets/shaders/"*.spv
cp "$B/shaders/"*.spv "$B/apk_assets/shaders/"
# the pipeline states that ship with the build (PS2_RQ_PIPE_KEYS, rq4_water G.2: tools/quest/rq_keys.py makes the file); not a
# .spv, so the cache's shader-set stamp does not see it
rm -f "$B/apk_assets/shaders/rq_pipelines.keys"
[ -f tools/quest/rq_pipelines.keys ] && cp tools/quest/rq_pipelines.keys "$B/apk_assets/shaders/"
XRSO="$OPENXR_LOADER_DIR/prefab/modules/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so"
# NativeActivity finds the library's entry by name: with hidden visibility (PS2_QUEST_HIDDEN) it must still be exported
if ! llvm-nm -D --defined-only "$B/libmain.so" | grep -qw ANativeActivity_onCreate; then
    echo "build_game: FAIL: libmain.so does not export ANativeActivity_onCreate"; exit 1
fi
DEX=; ACT=
if [ "${PAD_JAVA:-1}" = 1 ]; then
    AJAR=$(cygpath -m "$ANDROID_SDK_ROOT/platforms/android-34/android.jar")
    rm -rf "$B/java"; mkdir -p "$B/java/classes" "$B/java/dex"
    mapfile -t jsrc < <(find runtime/src/android/java -name '*.java' | sort)
    javac --release 11 -nowarn -cp "$AJAR" -d "$(cygpath -m "$B/java/classes")" "${jsrc[@]}" > "$B/java/javac.log" 2>&1 \
        || { cat "$B/java/javac.log"; echo "build_game: FAIL: javac"; exit 1; }
    mapfile -t jcls < <(find "$B/java/classes" -name '*.class' | sort)
    java -cp "$(cygpath -m "$ANDROID_BUILD_TOOLS/lib/d8.jar")" com.android.tools.r8.D8 --release --min-api 32 --lib "$AJAR" \
        --output "$(cygpath -m "$B/java/dex")" "${jcls[@]}" > "$B/java/d8.log" 2>&1 \
        || { cat "$B/java/d8.log"; echo "build_game: FAIL: d8"; exit 1; }
    DEX=$B/java/dex/classes.dex; ACT=com.kulakovri.aczquest.PadActivity
fi
DEX="$DEX" ACTIVITY="$ACT" ASSETS="$B/apk_assets" GFX=vulkan OUT="$B/aczquest.apk" tools/quest/make_apk.sh aczquest "$B/libmain.so" "$XRSO"
t2=$(date +%s)
sz() { stat -c %s "$1" | awk '{printf "%.1f MB", $1 / 1048576}'; }
echo "build_game: libmain.so $(sz "$B/libmain.so") (with symbols), $(ls "$B/apk_assets/shaders" | wc -l) shaders," \
     "aczquest.apk $(sz "$B/aczquest.apk"); build $((t1 - t0)) s, package + checks $((t2 - t1)) s"
