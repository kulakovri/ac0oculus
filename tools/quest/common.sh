# common.sh -- sourced by the tools/quest/*.sh scripts (not run on its own): the Quest toolchain environment, the
# tree root, the paths every script shares and the wrappers for the .bat tools (apksigner.bat cannot run from Git Bash:
# it is called as `java -jar apksigner.jar`).
# The toolchain comes from QUEST_ENV (a filled-in copy of tools/quest/env.example.sh); an
# environment that already has ANDROID_NDK_ROOT and OPENXR_LOADER_DIR is kept as it is.
QUEST_ENV=${QUEST_ENV:-}
if [ -z "$ANDROID_NDK_ROOT" ] || [ -z "$OPENXR_LOADER_DIR" ] || [ -z "$ANDROID_BUILD_TOOLS" ]; then
    # shellcheck disable=SC1090
    [ -f "$QUEST_ENV" ] && . "$QUEST_ENV" || { echo "quest: no toolchain environment (QUEST_ENV='$QUEST_ENV': a filled-in copy of tools/quest/env.example.sh)" >&2; return 2 2>/dev/null || exit 2; }
fi
# the tree root: tools/quest/.. (scripts may be called from anywhere)
QROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
QTOOLS=$QROOT/tools/quest
NDK_BIN=$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/windows-x86_64/bin
NDK_SYSROOT=$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/windows-x86_64/sysroot
QUEST_API=${QUEST_API:-32}                      # minSdk: the NDK stubs the libraries are linked and checked against
NDK_STUBS=$NDK_SYSROOT/usr/lib/aarch64-linux-android/$QUEST_API
ANDROID_JAR=$ANDROID_SDK_ROOT/platforms/android-34/android.jar
OPENXR_LOADER_SO=$OPENXR_LOADER_DIR/prefab/modules/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so
OPENXR_INCLUDE=$OPENXR_LOADER_DIR/prefab/modules/headers/include
# the debug keystore: outside every repository, generated once by make_apk.sh (alias aczquest, password android)
QUEST_KEYSTORE=${QUEST_KEYSTORE:-$HOME/.android/ac0oculus_debug.jks}
QUEST_KS_ALIAS=${QUEST_KS_ALIAS:-aczquest}
QUEST_KS_PASS=${QUEST_KS_PASS:-android}
QUEST_PKG_GAME=${QUEST_PKG_GAME:-com.kulakovri.ac0oculus}   # (another id installs beside this one; the Java class keeps its name)
# NAME -> package: aczquest is the game, everything else $QUEST_PKG_GAME.NAME
quest_pkg() { if [ "$1" = aczquest ]; then echo "$QUEST_PKG_GAME"; else echo "$QUEST_PKG_GAME.$1"; fi; }
wpath() { cygpath -m "$1"; }                    # C:/... for the Windows-native tools (aapt2, zipalign, java, keytool)
apksigner() { java -jar "$(wpath "$ANDROID_BUILD_TOOLS/lib/apksigner.jar")" "$@"; }
now_ms() { echo $(( $(date +%s%N) / 1000000 )); }
# the Windows tools print CRLF: every parsed output goes through this
nocr() { tr -d '\r'; }

# ---- adb (headset_test.sh, push_data.sh, link_test.sh) ----
# adb is a Windows program: Git Bash would turn /sdcard/... into C:/Program Files/Git/sdcard/..., so adb runs without
# the path conversion and every local path is given as C:/... (wpath)
adbq() { MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' adb "$@"; }
ash() { MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' adb shell "$@" 2>&1 | nocr; }   # one remote command line, no CR
# exactly one authorised device (ANDROID_SERIAL picks one of several); exports ANDROID_SERIAL; $1 = the caller's name
quest_device() {
    local devs nok
    devs=$(adbq devices 2>/dev/null | nocr | awk 'NR > 1 && NF >= 2 {print $1, $2}')
    [ -n "$ANDROID_SERIAL" ] && devs=$(echo "$devs" | awk -v s="$ANDROID_SERIAL" '$1 == s')
    nok=$(echo "$devs" | awk '$2 == "device"' | grep -c .)
    if [ "$nok" != 1 ]; then
        echo "$1: need exactly one authorised device, adb devices says:"; echo "${devs:-  (none)}" | sed 's/^/  /'
        echo "$devs" | grep -q unauthorized && echo "  -> put the headset on and accept the USB debugging prompt (always allow)"
        [ "$nok" -gt 1 ] 2>/dev/null && echo "  -> set ANDROID_SERIAL to one of them"
        return 1
    fi
    export ANDROID_SERIAL=$(echo "$devs" | awk '$2 == "device" {print $1}')
    echo "$1: device $ANDROID_SERIAL ($(ash getprop ro.product.model), build $(ash getprop ro.build.version.incremental))"
}
# the package of the resumed (front) activity, or nothing when dumpsys says nothing usable (UNVERIFIED on Horizon OS:
# the line is topResumedActivity= / mResumedActivity: on Android 14)
quest_foreground() {
    ash dumpsys activity activities | grep -m1 -E 'topResumedActivity|mResumedActivity|ResumedActivity:' \
        | grep -oE '[a-z][a-zA-Z0-9_]*(\.[a-zA-Z0-9_]+)+/' | head -1 | tr -d /
}
# the headset must not be in someone's use: refuse when the front app is not the home environment, Link (only when $2
# is "link") or one of ours; FORCE=1 overrides. $1 = the caller's name. Exit status 0 = idle enough.
quest_idle() {
    local fg
    fg=$(quest_foreground)
    case $fg in
        ''|com.oculus.vrshell|com.oculus.systemux|com.oculus.shellenv|com.oculus.home|com.oculus.guardian|com.oculus.os.clearactivity|$QUEST_PKG_GAME|$QUEST_PKG_GAME.*)
            # (com.oculus.os.clearactivity: Horizon OS v209's placeholder after an app closes, 2026-11-08)
            echo "$1: front app ${fg:-unknown}"; return 0 ;;
        com.oculus.xrstreamingclient) [ "$2" = link ] && { echo "$1: front app Link"; return 0; } ;;
    esac
    if [ "${FORCE:-0}" = 1 ]; then echo "$1: front app $fg (FORCE=1: going on)"; return 0; fi
    echo "$1: the headset is running $fg -- someone may be using it; not starting (FORCE=1 overrides)"
    return 1
}
# the Guardian state that decides whether a launch with the proximity override goes through (first device session,
# 2026-11-08, Horizon OS v74): with tracking lost (the headset lying on a desk) and no boundary the shell holds every
# launch in its Guardian setup (GetShellGuardianSetupState: SETUP_ACTIVE) until someone sets a boundary. Passes:
# the developer setting "boundary off" (the dump's "Paused Via PrefStore value (guardian_pause)": true) or Meta's
# testing services (content://com.oculus.rc disable_guardian=true; setting it needs the account's Store PIN, which only
# the user types). debug.oculus.guardian_pause does nothing on v74, persist.oculus.* cannot be set from adb shell.
# Prints one line; exit status 0 = a launch should pass, 1 = it will probably be held. $1 = the caller's name.
quest_guardian() {
    local d lost pref rc
    d=$(ash dumpsys activity service com.oculus.guardian/com.oculus.vrguardianservice.VrGuardianService)
    # here-strings, not echo | grep -m1: the callers ignore SIGPIPE, so an early-exiting grep made echo print errors
    lost=$(grep -m1 -oE '"isTrackingLost": (true|false)' <<< "$d" | grep -oE 'true|false')
    pref=$(grep -m1 -oE '"Paused Via PrefStore value \(guardian_pause\)": (true|false)' <<< "$d" | grep -oE 'true|false')
    rc=$(ash content call --uri content://com.oculus.rc --method GET_PROPERTY | grep -oE 'disable_guardian=(true|false)' | cut -d= -f2)
    echo "$1: guardian: tracking lost ${lost:-?}, boundary paused (developer setting) ${pref:-?}, testing services disable_guardian ${rc:-?}"
    [ "$pref" = true ] || [ "$rc" = true ] || [ "$lost" = false ]
}
# why a launch did not start, from the device's whole logcat: the shell's launch checks (one line each, or nothing)
quest_launch_why() {
    local l
    l=$(adbq logcat -d -v threadtime | nocr)
    grep -q 'app_launch_blocked_controller_required' <<< "$l" \
        && echo "$1: the launch is blocked by the 'controller required' dialog (an APK built with HANDS=0? make_apk.sh)"
    grep -q -E 'GetShellGuardianSetupState: SETUP_ACTIVE|action\.GUARDIAN_SETUP' <<< "$l" \
        && echo "$1: the launch is held by the Guardian setup (tracking lost, no boundary: QUEST_FIRST_RUN.md 'Unattended runs')"
    return 0
}
