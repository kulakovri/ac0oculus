#!/bin/bash
# make_apk.sh NAME LIB.so [EXTRA.so ...] -- a signed Quest 3 APK without Gradle (stage 1, 2026-11-07).
# NAME aczquest = the game (package $QUEST_PKG_GAME, default com.kulakovri.ac0oculus); any other NAME (hello, hellovk, ...) gives
# com.kulakovri.aczquest.NAME. LIB.so goes into lib/arm64-v8a/libmain.so (NativeActivity's android.app.lib_name =
# main); every EXTRA.so keeps its own name (the hello and the game always pass the OpenXR loader,
# $OPENXR_LOADER_DIR/prefab/modules/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so).
# Steps: fill tools/quest/AndroidManifest.xml.in -> aapt2 link against platforms/android-34/android.jar (+ -A ASSETS)
# -> the .so files appended STORED (apkzip.py; android:extractNativeLibs=false needs them uncompressed) -> zipalign -P 16
# -f 4 (16 KB page alignment of the .so data) -> apksigner sign v2+v3 (no v1: minSdk 32; no v4 .idsig) with the debug
# keystore QUEST_KEYSTORE (default ~/.android/ac0oculus_debug.jks) (generated here once: keytool, alias aczquest, RSA 2048, 10000 days, password android;
# never in a repository) -> tools/quest/check_apk.sh -> the size.
# Environment:
#   ASSETS=<dir>       packed as the APK's assets/ (the hello's cubemap256.ktx)
#   OUT=<apk>          default build/quest/NAME.apk (in the tree); the work folder is OUT without .apk + .work/
#   LABEL=<text>       launcher label (default "ac0oculus" for the game, "aczquest NAME" otherwise)
#   DEBUGGABLE=1|0     android:debuggable (default 1: run-as, dlopen of a pushed .so, ndk-gdb)
#   VERSION_CODE=1 VERSION_NAME=0.1.0
#   GFX=gles|vulkan|none   the graphics uses-feature line (default gles for hello, vulkan otherwise)
#   HANDS=1|0          hand tracking declared optional (default 1: no "controller required" launch block)
#   STRIP=1|0         llvm-strip --strip-unneeded the copies that go into the APK (default 1, as Gradle does; the
#                      input .so files are never touched: keep them for symbolising tombstones)
#   CHECK=1|0          run check_apk.sh at the end (default 1)
#   DEX=<classes.dex> ACTIVITY=<class>   Java code of our own (the game's PadActivity, build_game.sh): the dex goes into
#                      the APK as classes.dex, the manifest gets hasCode=true and that activity; without DEX the
#                      activity is android.app.NativeActivity and hasCode=false, as before; with DEX also the
#                      VIBRATE permission (the pad's rumble)
#   GAMEPAD_FEATURE=1  declare android.hardware.gamepad (not required); default 0 (see below)
set -e
. "$(dirname "$0")/common.sh"
name=$1; lib=$2
[ -n "$name" ] && [ -f "$lib" ] || { echo "usage: make_apk.sh NAME LIB.so [EXTRA.so ...]"; exit 2; }
shift 2
case $name in [a-z]*) ;; *) echo "make_apk: NAME must start with a lower-case letter"; exit 2 ;; esac
case $name in *[!a-z0-9_]*) echo "make_apk: NAME may hold only a-z 0-9 _"; exit 2 ;; esac
pkg=$(quest_pkg "$name")
out=${OUT:-$QROOT/build/quest/$name.apk}
mkdir -p "$(dirname "$out")"
out=$(cd "$(dirname "$out")" && pwd)/$(basename "$out")
work=${out%.apk}.work
rm -rf "$work"; mkdir -p "$work"
if [ "$name" = aczquest ]; then label=${LABEL:-ac0oculus}; else label=${LABEL:-aczquest $name}; fi
gfx=${GFX:-$([ "$name" = hello ] && echo gles || echo vulkan)}
case $gfx in
    gles)   feat='  <uses-feature android:glEsVersion="0x00030001" android:required="true" />' ;;
    vulkan) feat='  <uses-feature android:name="android.hardware.vulkan.version" android:version="0x401000" android:required="true" />' ;;
    none)   feat='' ;;
    *) echo "make_apk: GFX=gles|vulkan|none"; exit 2 ;;
esac
# HANDS=1 (default): hand tracking declared as supported (not required). Without it Horizon OS adds
# LaunchCheckFlag::RequiresControllers and blocks the launch with the "controller required" dialog while the
# controllers sleep (seen on the first device session, 2026-11-08: unattended runs, also a gamepad-only player).
if [ "${HANDS:-1}" = 1 ]; then
    feat="$feat
  <uses-feature android:name=\"oculus.software.handtracking\" android:required=\"false\" />
  <uses-permission android:name=\"com.oculus.permission.HAND_TRACKING\" />"
fi
# GAMEPAD_FEATURE=1 (default 0): <uses-feature android.hardware.gamepad required=false>. Off since 2026-11-09: the APKs
# that declared it (with PadActivity) stopped rendering on the device; kept as a switch for the bisection.
if [ "${GAMEPAD_FEATURE:-0}" = 1 ]; then
    feat="$feat
  <uses-feature android:name=\"android.hardware.gamepad\" android:required=\"false\" />"
fi
dbg=true; [ "${DEBUGGABLE:-1}" = 0 ] && dbg=false
activity=android.app.NativeActivity; hascode=false
if [ -n "$DEX" ]; then
    [ -f "$DEX" ] && [ -n "$ACTIVITY" ] || { echo "make_apk: DEX=$DEX needs the file and ACTIVITY"; exit 2; }
    activity=$ACTIVITY; hascode=true
    # the pad's rumble (PadActivity.rumble: InputDevice vibrators; a normal permission, granted at install)
    feat="$feat
  <uses-permission android:name=\"android.permission.VIBRATE\" />"
fi
# QUEST_TRADE_CPU_GPU=1 (2026-11-16, the review's research R3 q7; Meta "CPU and GPU levels"): the manifest asks the system to trade one
# CPU level for one GPU level (com.oculus.trade_cpu_for_gpu_amount): on the battery the GPU ceiling 545 -> 599 MHz, the CPU's 1.92 ->
# 1.65 GHz; the start-up log says "CreateClient: Value of tradeCpuForGpu is 1". Off by default (a test build)
appmeta=
[ "${QUEST_TRADE_CPU_GPU:-0}" = 1 ] && appmeta='    <meta-data android:name="com.oculus.trade_cpu_for_gpu_amount" android:value="1" />
'
t0=$(now_ms)

# 1. manifest
python -I "$QTOOLS/apkzip.py" fill "$QTOOLS/AndroidManifest.xml.in" "$work/AndroidManifest.xml" \
    "PACKAGE=$pkg" "LABEL=$label" "VERSION_CODE=${VERSION_CODE:-1}" "VERSION_NAME=${VERSION_NAME:-0.1.0}" \
    "LIB_NAME=main" "DEBUGGABLE=$dbg" "EXTRA_FEATURES_RAW=$feat" "ACTIVITY=$activity" "HAS_CODE=$hascode" "APP_META_RAW=$appmeta"

# 2. aapt2 link: manifest (+ assets) -> base.apk (no resources, no DEX: hasCode=false)
aargs=()
if [ -n "$ASSETS" ]; then
    [ -d "$ASSETS" ] || { echo "make_apk: ASSETS=$ASSETS is not a folder"; exit 2; }
    aargs=(-A "$(wpath "$ASSETS")")
fi
aapt2 link -o "$(wpath "$work/base.apk")" -I "$(wpath "$ANDROID_JAR")" --manifest "$(wpath "$work/AndroidManifest.xml")" \
    "${aargs[@]}"
t1=$(now_ms)

# 3. the native libraries, STORED
mkdir -p "$work/lib"
add=()
cp_lib() {      # $1 source, $2 name in the APK
    if [ "${STRIP:-1}" = 1 ]; then llvm-strip --strip-unneeded -o "$work/lib/$2" "$1"; else cp "$1" "$work/lib/$2"; fi
    add+=("lib/arm64-v8a/$2=$work/lib/$2")
}
cp_lib "$lib" libmain.so
for x in "$@"; do
    [ -f "$x" ] || { echo "make_apk: no $x"; exit 2; }
    b=$(basename "$x")
    [ "$b" = libmain.so ] && { echo "make_apk: an extra library may not be called libmain.so"; exit 2; }
    cp_lib "$x" "$b"
done
[ -n "$DEX" ] && add+=("classes.dex=$DEX")     # (stored like the libraries: ART reads an uncompressed dex in place)
cp "$work/base.apk" "$work/unaligned.apk"
python -I "$QTOOLS/apkzip.py" addlibs "$work/unaligned.apk" "${add[@]}"
t2=$(now_ms)

# 4. 16 KB page alignment of the uncompressed .so data, 4-byte alignment of the rest
zipalign -P 16 -f 4 "$(wpath "$work/unaligned.apk")" "$(wpath "$work/aligned.apk")"
t3=$(now_ms)

# 5. signing (the keystore is made once, outside every repository)
if [ ! -f "$QUEST_KEYSTORE" ]; then
    echo "make_apk: generating the debug keystore $QUEST_KEYSTORE (alias $QUEST_KS_ALIAS)"
    mkdir -p "$(dirname "$QUEST_KEYSTORE")"
    keytool -genkeypair -keystore "$(wpath "$QUEST_KEYSTORE")" -storetype PKCS12 -alias "$QUEST_KS_ALIAS" \
        -keyalg RSA -keysize 2048 -validity 10000 -storepass "$QUEST_KS_PASS" -keypass "$QUEST_KS_PASS" \
        -dname "CN=aczquest debug" >/dev/null
fi
apksigner sign --ks "$(wpath "$QUEST_KEYSTORE")" --ks-key-alias "$QUEST_KS_ALIAS" --ks-pass "pass:$QUEST_KS_PASS" \
    --key-pass "pass:$QUEST_KS_PASS" --v1-signing-enabled false --v2-signing-enabled true --v3-signing-enabled true \
    --v4-signing-enabled false --out "$(wpath "$out")" "$(wpath "$work/aligned.apk")"
t4=$(now_ms)
echo "make_apk: $out ($pkg): link $((t1 - t0)) ms, libs $((t2 - t1)) ms, zipalign $((t3 - t2)) ms, sign $((t4 - t3)) ms"

# 6. checks and size
rc=0
if [ "${CHECK:-1}" = 1 ]; then
    EXPECT_PKG=$pkg EXPECT_DEBUGGABLE=$dbg EXPECT_ACTIVITY=$activity EXPECT_CODE=$hascode "$QTOOLS/check_apk.sh" "$out" || rc=$?
fi
sz=$(stat -c %s "$out")
echo "make_apk: $(basename "$out") $sz bytes ($(( (sz + 524288) / 1048576 )) MB), total $(( $(now_ms) - t0 )) ms"
exit $rc
