#!/bin/bash
# check_apk.sh APK -- static checks of a Quest 3 APK of the aczquest fork (stage 1, 2026-11-07); exit 0 = all pass,
# 1 = a check failed, 2 = usage. Fast on big APKs: only the manifest, the signing block and the ELF tables are read
# (the .so files are copied out once for check_so.sh).
#   1. aapt2 dump badging (printed): package com.kulakovri.aczquest[.NAME] (= EXPECT_PKG when set), minSdk 32,
#      targetSdk 34, launchable activity android.app.NativeActivity, uses-feature android.hardware.vr.headtracking
#      version 1 + com.oculus.feature.BOUNDARYLESS_APP, native-code exactly 'arm64-v8a', OPENXR_SYSTEM,
#      MANAGE_EXTERNAL_STORAGE and RECORD_AUDIO (ACZ_QUEST_RECORD's microphone) permissions.
#   2. aapt2 dump xmltree of the manifest: hasCode=false, extractNativeLibs=false, debuggable (= EXPECT_DEBUGGABLE
#      true/false when set), supportedDevices quest3|quest3s, the activity (exported, singleTask, landscape,
#      excludeFromRecents, Theme.Black.NoTitleBar.Fullscreen = @0x0103000a, lib_name main, headtracking required),
#      the intent filter (MAIN, LAUNCHER, com.oculus.intent.category.VR, IMMERSIVE_HMD), the OpenXR broker <queries>.
#   3. apksigner verify --print-certs -v (the v3 block; minSdk 32 means the platform checks only v3) and again with
#      --min-sdk-version 24 (the v2 block is there too).
#   4. zipalign -c -P 16 4; every lib/arm64-v8a/*.so STORED with its data at a 16 KB boundary; no classes.dex, no
#      other ABI.
# EXPECT_ACTIVITY (default android.app.NativeActivity) and EXPECT_CODE=true (default false: hasCode=true and exactly one
# classes.dex) for an APK with Java code of its own (the game's PadActivity, make_apk.sh DEX=).
#   5. tools/quest/check_so.sh for every .so, the other .so files of the APK counting as bundled.
. "$(dirname "$0")/common.sh"
apk=$1
[ -f "$apk" ] || { echo "usage: check_apk.sh APK"; exit 2; }
tag="check_apk: $(basename "$apk")"
fail=0
bad() { echo "$tag: FAIL: $*"; fail=1; }
ok() { echo "$tag: ok: $*"; }
want() {   # $1 description, $2 text, $3 fixed string that must be in it
    if [[ "$2" == *"$3"* ]]; then ok "$1"; else bad "$1 (no '$3')"; fi
}
t0=$(now_ms)

# 1. badging
badging=$(aapt2 dump badging "$(wpath "$apk")" 2>&1) || { echo "$badging"; bad "aapt2 dump badging failed"; exit 1; }
badging=$(echo "$badging" | nocr)
echo "$badging" | grep -E "^(package|minSdkVersion|targetSdkVersion|launchable-activity|application-debuggable|native-code|uses-permission)|uses-feature|uses-gl-es" | sed 's/^/    /'
pkg=$(echo "$badging" | sed -n "s/^package: name='\([^']*\)'.*/\1/p")
if [ -n "$EXPECT_PKG" ]; then
    [ "$pkg" = "$EXPECT_PKG" ] && ok "package $pkg" || bad "package '$pkg', expected '$EXPECT_PKG'"
else
    [[ "$pkg" =~ ^com\.kulakovri\.aczquest(\.[a-z][a-z0-9_]*)?$ ]] && ok "package $pkg" || bad "package '$pkg' is not com.kulakovri.aczquest[.NAME]"
fi
want "minSdkVersion 32" "$badging" "minSdkVersion:'32'"
want "targetSdkVersion 34" "$badging" "targetSdkVersion:'34'"
activity=${EXPECT_ACTIVITY:-android.app.NativeActivity}; hascode=${EXPECT_CODE:-false}
want "launchable activity" "$badging" "launchable-activity: name='$activity'"
want "headtracking feature v1" "$badging" "uses-feature: name='android.hardware.vr.headtracking' version='1'"
want "boundaryless feature" "$badging" "uses-feature: name='com.oculus.feature.BOUNDARYLESS_APP'"
want "OPENXR_SYSTEM permission" "$badging" "uses-permission: name='org.khronos.openxr.permission.OPENXR_SYSTEM'"
want "MANAGE_EXTERNAL_STORAGE permission" "$badging" "uses-permission: name='android.permission.MANAGE_EXTERNAL_STORAGE'"
want "RECORD_AUDIO permission" "$badging" "uses-permission: name='android.permission.RECORD_AUDIO'"
nc=$(echo "$badging" | sed -n 's/^native-code: //p')
[ "$nc" = "'arm64-v8a'" ] && ok "native-code arm64-v8a only" || bad "native-code is '$nc', expected 'arm64-v8a' only"

# 2. manifest tree
xml=$(aapt2 dump xmltree --file AndroidManifest.xml "$(wpath "$apk")" 2>&1 | nocr)
block() {  # the element block (its E: line and attribute lines) whose text contains $1
    echo "$xml" | awk -v key="$1" '/^ *E: / { if (index(blk, key)) printf "%s", blk; blk = "" } { blk = blk $0 "\n" }
        END { if (index(blk, key)) printf "%s", blk }'
}
app=$(block ':hasCode(')
want "hasCode=$hascode" "$app" ":hasCode(0x0101000c)=$hascode"
want "extractNativeLibs=false" "$app" ':extractNativeLibs(0x010104ea)=false'
if [[ "$app" == *':debuggable(0x0101000f)=true'* ]]; then dbg=true; else dbg=false; fi
if [ -n "$EXPECT_DEBUGGABLE" ]; then
    [ "$dbg" = "$EXPECT_DEBUGGABLE" ] && ok "debuggable=$dbg" || bad "debuggable=$dbg, expected $EXPECT_DEBUGGABLE"
else
    echo "$tag: info: debuggable=$dbg"
fi
want "supportedDevices quest3|quest3s" "$(block '"com.oculus.supportedDevices"')" '="quest3|quest3s"'
act=$(block "\"$activity\"")
want "activity exported" "$act" ':exported(0x01010010)=true'
want "activity singleTask" "$act" ':launchMode(0x0101001d)=2'
want "activity landscape" "$act" ':screenOrientation(0x0101001e)=0'
want "activity excludeFromRecents" "$act" ':excludeFromRecents(0x01010017)=true'
want "activity Theme.Black.NoTitleBar.Fullscreen" "$act" ':theme(0x01010000)=@0x0103000a'
want "lib_name main" "$(block '"android.app.lib_name"')" ':value(0x01010024)="main"'
want "headtracking required" "$(block '"android.hardware.vr.headtracking"')" ':required(0x0101028e)=true'
for c in android.intent.action.MAIN android.intent.category.LAUNCHER com.oculus.intent.category.VR \
         org.khronos.openxr.intent.category.IMMERSIVE_HMD org.khronos.openxr.OpenXRRuntimeService \
         org.khronos.openxr.OpenXRApiLayerService; do
    want "$c" "$xml" "\"$c\""
done
want "queries: runtime broker provider" "$(block ':authorities(')" '="org.khronos.openxr.runtime_broker;org.khronos.openxr.system_runtime_broker"'
qline=$(echo "$xml" | grep -n 'E: queries' | cut -d: -f1); aline=$(echo "$xml" | grep -n 'E: application' | cut -d: -f1)
[ -n "$qline" ] && [ -n "$aline" ] && [ "$qline" -lt "$aline" ] && ok "<queries> before <application>" || bad "no <queries> block at manifest level"

# 3. signature
v=$(apksigner verify --print-certs -v "$(wpath "$apk")" 2>&1); vrc=$?; v=$(echo "$v" | nocr)
echo "$v" | grep -E '^Verifies|scheme v[23] |Signer #?1?:? ?certificate DN|V3.0 Signer: certificate (DN|SHA-256)|^WARNING|^ERROR' | sed 's/^/    /'
[ $vrc = 0 ] && [[ "$v" == Verifies* ]] && ok "apksigner verify" || bad "apksigner verify (rc $vrc)"
want "v3 signature" "$v" "Verified using v3 scheme (APK Signature Scheme v3): true"
v24=$(apksigner verify --min-sdk-version 24 -v "$(wpath "$apk")" 2>&1 | nocr)
want "v2 signature (checked for API 24+)" "$v24" "Verified using v2 scheme (APK Signature Scheme v2): true"

# 4. zip layout
if zipalign -c -P 16 4 "$(wpath "$apk")" >/dev/null 2>&1; then ok "zipalign -c -P 16 4"; else bad "zipalign -c -P 16 4"; fi
libs=$(python -I "$QTOOLS/apkzip.py" libs "$apk") || bad "a native library is compressed"
echo "$libs" | sed 's/^/    /'
echo "$libs" | grep -q '^lib/arm64-v8a/libmain.so ' && ok "lib/arm64-v8a/libmain.so present" || bad "no lib/arm64-v8a/libmain.so"
echo "$libs" | awk '$1 ~ /\.so$/ && ($2 != "stored" || $5 != 0) {print $1; e = 1} END {exit e}' >/dev/null \
    && ok "every .so stored, data 16 KB aligned" || bad "a .so is compressed or not 16 KB aligned"
echo "$libs" | grep -v '^lib/arm64-v8a/' | grep -q . && bad "libraries outside lib/arm64-v8a/"
if [ "$hascode" = true ]; then
    [ "$(unzip -Z1 "$apk" 2>/dev/null | grep -c '^classes.*\.dex$')" = 1 ] && ok "one classes.dex (hasCode=true)" || bad "hasCode=true expects exactly one classes.dex"
else
    unzip -Z1 "$apk" 2>/dev/null | grep -q '^classes.*\.dex$' && bad "classes.dex present (hasCode=false expects none)"
fi

# 5. the libraries
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
mapfile -t sos < <(python -I "$QTOOLS/apkzip.py" extract "$apk" "$tmp" | nocr)
for s in "${sos[@]}"; do
    others=()
    for o in "${sos[@]}"; do [ "$o" = "$s" ] || others+=("$tmp/$o"); done
    "$QTOOLS/check_so.sh" "$tmp/$s" "${others[@]}" | sed 's/^/    /'
    [ "${PIPESTATUS[0]}" = 0 ] && ok "check_so $s" || bad "check_so $s"
done

echo "$tag: $(stat -c %s "$apk") bytes, $(( $(now_ms) - t0 )) ms"
if [ $fail = 0 ]; then echo "$tag: PASS"; else echo "$tag: FAILED"; fi
exit $fail
