/* pad_test.c -- the Quest build's gamepad / joystick layer (runtime/src/android/ps2_pad_android.c) on the PC: the file is
   compiled into this test with fakes for the NDK input calls, JNI's few functions, SDL's event queue and hints, and fed
   the devices and events PadActivity would send (an Xbox pad, a DualShock 4 without a key layout file, a DualSense with
   its touchpad, a T.16000M stick, an Xbox-compatible HOTAS on the flight-stick list, hot-plug). It checks what the
   runtime's SDL calls then answer; then the display watcher's line for the GPU hang report (ps2_display_android.c, R-37).
   Build and run (Git Bash, MSYS2's gcc and SDL3 headers, the NDK's input headers):
     source $QUEST_ENV; export PATH=/c/msys64/ucrt64/bin:$PATH
     gcc -O1 -Wall -o build/pad_test.exe tools/quest/pad_test.c -Itools/quest/pad_test_inc -Iruntime/src/android \
         -I/c/msys64/ucrt64/include -idirafter "$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/include" \
         -idirafter "$ANDROID_NDK_ROOT/sources/android/native_app_glue" -lm -pthread && build/pad_test.exe
   Exit 0 = every check passed. */
#include <ctype.h>
#include <string.h>
static char *pt_strcasestr(const char *h, const char *n) {
    size_t l = strlen(n);
    for (; *h; h++) {
        size_t i = 0;
        while (i < l && h[i] && tolower((unsigned char)h[i]) == tolower((unsigned char)n[i])) i++;
        if (i == l) return (char *)h;
    }
    return l ? NULL : (char *)h;
}
#define strcasestr pt_strcasestr
#include "ps2_pad_android.c"
#include "ps2_display_android.c"

/* ---- fakes ---- */
int32_t AInputQueue_getEvent(AInputQueue *q, AInputEvent **e) { (void)q; (void)e; return -1; }
int32_t AInputQueue_preDispatchEvent(AInputQueue *q, AInputEvent *e) { (void)q; (void)e; return 0; }
void AInputQueue_finishEvent(AInputQueue *q, AInputEvent *e, int h) { (void)q; (void)e; (void)h; }
int32_t AInputEvent_getType(const AInputEvent *e) { (void)e; return 0; }
int32_t AInputEvent_getSource(const AInputEvent *e) { (void)e; return 0; }
int32_t AInputEvent_getDeviceId(const AInputEvent *e) { (void)e; return 0; }
int32_t AKeyEvent_getAction(const AInputEvent *e) { (void)e; return 0; }
int32_t AKeyEvent_getKeyCode(const AInputEvent *e) { (void)e; return 0; }
int32_t AKeyEvent_getScanCode(const AInputEvent *e) { (void)e; return 0; }
int32_t AKeyEvent_getFlags(const AInputEvent *e) { (void)e; return 0; }
int32_t AKeyEvent_getMetaState(const AInputEvent *e) { (void)e; return 0; }
int32_t AKeyEvent_getRepeatCount(const AInputEvent *e) { (void)e; return 0; }
int64_t AKeyEvent_getEventTime(const AInputEvent *e) { (void)e; return 0; }
int32_t AMotionEvent_getAction(const AInputEvent *e) { (void)e; return 0; }
size_t AMotionEvent_getHistorySize(const AInputEvent *e) { (void)e; return 0; }
size_t AMotionEvent_getPointerCount(const AInputEvent *e) { (void)e; return 0; }
int64_t AMotionEvent_getEventTime(const AInputEvent *e) { (void)e; return 0; }
int32_t AMotionEvent_getMetaState(const AInputEvent *e) { (void)e; return 0; }
int32_t AMotionEvent_getButtonState(const AInputEvent *e) { (void)e; return 0; }
float AMotionEvent_getAxisValue(const AInputEvent *e, int32_t a, size_t p) { (void)e; (void)a; (void)p; return 0; }
void ps2_sdl_android_error(const char *s) { (void)s; }
void ps2_thread_helper(const char *name, int kind) { (void)name; (void)kind; }
static SDL_Event pt_evs[4096];
static int pt_nev;
bool SDL_PushEvent(SDL_Event *e) { if (pt_nev < 4096) pt_evs[pt_nev++] = *e; return true; }
Uint64 SDL_GetTicksNS(void) { return 1; }
void SDL_free(void *m) { free(m); }
void SDL_GUIDToString(SDL_GUID g, char *out, int cap) {        /* (ps2_sdl_android.c's) */
    for (int i = 0; i < 16 && (i + 1) * 2 < cap; i++) snprintf(out + i * 2, 3, "%02x", g.data[i]);
}
static const char *hint_stick = "", *hint_thr = "";
const char *SDL_GetHint(const char *name) {
    return !strcmp(name, SDL_HINT_JOYSTICK_FLIGHTSTICK_DEVICES) ? hint_stick : !strcmp(name, SDL_HINT_JOYSTICK_THROTTLE_DEVICES) ? hint_thr : NULL;
}
/* JNI: strings and float arrays are plain C pointers here */
typedef struct { int n; const float *v; } farr;
static const char *f_utf(JNIEnv *e, jstring s, jboolean *c) { (void)e; if (c) *c = 0; return (const char *)s; }
static void f_rel(JNIEnv *e, jstring s, const char *c) { (void)e; (void)s; (void)c; }
static jsize f_len(JNIEnv *e, jarray a) { (void)e; return ((farr *)a)->n; }
static void f_freg(JNIEnv *e, jfloatArray a, jsize s, jsize n, jfloat *o) { (void)e; memcpy(o, ((farr *)a)->v + s, (size_t)n * sizeof(float)); }
static struct JNINativeInterface fni;
static JNIEnv fenv = &fni;

/* ---- helpers ---- */
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
enum { KA = 1 << 0, KB = 1 << 1, KX = 1 << 3, KY = 1 << 4, KL1 = 1 << 6, KR1 = 1 << 7, KL2 = 1 << 8, KR2 = 1 << 9, KTL = 1 << 10,
       KTR = 1 << 11, KST = 1 << 12, KSE = 1 << 13, KMO = 1 << 14 };
#define PADKEYS (KA | KB | KX | KY | KL1 | KR1 | KTL | KTR | KST | KSE | KMO)
static void device(int dev, const char *name, int vid, int pid, int src, const float *r, int nr, long long keys, int vib, int added) {
    farr a = { nr * 5, r };
    j_device(&fenv, NULL, dev, (jstring)name, vid, pid, src, (jfloatArray)&a, keys, vib, (jboolean)added);
}
static int key(int dev, int src, int code, int scan, int down) { return j_key(&fenv, NULL, dev, src, down ? AKEY_EVENT_ACTION_DOWN : AKEY_EVENT_ACTION_UP, code, scan, 0, 0, 0, 0); }
static float AX[NAX];
static int motion(int dev, int src, int buttons) {
    farr a = { NAX, AX };
    int r = j_motion(&fenv, NULL, dev, src, AMOTION_EVENT_ACTION_MOVE, buttons, (jfloatArray)&a, 0);
    memset(AX, 0, sizeof AX);
    return r;
}
static SDL_Gamepad *pad_for(int dev) {
    pdev *d = slot_of(dev);
    return d ? SDL_OpenGamepad(id_of(d)) : NULL;
}
static int count_events(Uint32 type) { int n = 0; for (int i = 0; i < pt_nev; i++) n += pt_evs[i].type == type; return n; }

#define SRC_PAD 0x01000511
#define SRC_JOYKB 0x01000111
#define R(axis, lo, hi) (float)(axis), (float)(lo), (float)(hi), 0.0f, (float)AINPUT_SOURCE_JOYSTICK

int main(void) {
    fni.GetStringUTFChars = f_utf; fni.ReleaseStringUTFChars = f_rel; fni.GetArrayLength = f_len; fni.GetFloatArrayRegion = f_freg;
    g_on = 1; g_joy = 1;

    /* 1. an Xbox pad over Bluetooth (the device run's census) */
    const float xr[] = { R(0, -1, 1), R(1, -1, 1), R(11, -1, 1), R(14, -1, 1), R(15, -1, 1), R(16, -1, 1), R(17, 0, 1), R(18, 0, 1),
                         R(22, 0, 1), R(23, 0, 1) };
    device(44, "Xbox Wireless Controller", 0x045e, 0x0b13, SRC_PAD, xr, 10, PADKEYS, 2, 0);
    int n0 = 0;
    SDL_free(SDL_GetGamepads(&n0));
    CHECK(n0 == 0, "a pad is listed before its first use (%d)", n0);
    CHECK(key(44, 0x401, AKEYCODE_BUTTON_A, 0x130, 1), "BUTTON_A not consumed");
    int n1 = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n1);
    CHECK(n1 == 1 && count_events(SDL_EVENT_GAMEPAD_ADDED) == 1, "the pad is not announced (%d listed)", n1);
    SDL_free(ids);
    SDL_Gamepad *g = pad_for(44);
    CHECK(g && SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_SOUTH), "A is not south");
    CHECK(SDL_GetGamepadType(g) == SDL_GAMEPAD_TYPE_XBOXONE && SDL_GetGamepadButtonLabel(g, SDL_GAMEPAD_BUTTON_SOUTH) == SDL_GAMEPAD_BUTTON_LABEL_A, "Xbox type / label");
    key(44, 0x401, AKEYCODE_BUTTON_A, 0x130, 0);
    CHECK(!SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_SOUTH), "A stays down");
    unsigned bd0 = ps2_pad_android_back_downs();
    key(44, 0x401, AKEYCODE_BUTTON_SELECT, 0x13a, 1);
    CHECK(SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_BACK), "View is not back (the recentre's Select)");
    key(44, 0x401, AKEYCODE_BUTTON_SELECT, 0x13a, 1);   /* (a second down without an up: not a new press) */
    key(44, 0x401, AKEYCODE_BUTTON_SELECT, 0x13a, 0);
    key(44, 0x401, AKEYCODE_BUTTON_SELECT, 0x13a, 1);   /* a tap as adb sends it: down and up at once (R-76) */
    key(44, 0x401, AKEYCODE_BUTTON_SELECT, 0x13a, 0);
    CHECK(ps2_pad_android_back_downs() - bd0 == 2u, "View press edges counted %u, want 2 (the held press and the tap)",
          ps2_pad_android_back_downs() - bd0);
    CHECK(!SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_BACK), "View stays down");
    AX[0] = 0.5f; AX[14] = -1.0f; AX[17] = 0.7f; AX[22] = 0.2f; AX[15] = -1.0f;
    CHECK(motion(44, AINPUT_SOURCE_JOYSTICK, 0), "motion not consumed");
    int lx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), ry = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY);
    int lt = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER), rt = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    CHECK(abs(lx - 13492) < 40, "left x %d (0.5 after the 0.15 radial dead zone: 13492)", lx);
    CHECK(ry == -32767, "right y %d (RZ -1)", ry);
    CHECK(abs(lt - 22937) < 40 && abs(rt - 6553) < 40, "triggers %d %d (LTRIGGER 0.7, GAS 0.2)", lt, rt);
    CHECK(SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_LEFT), "HAT_X -1 is not d-pad left");
    AX[0] = 0.1f; motion(44, AINPUT_SOURCE_JOYSTICK, 0);
    CHECK(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX) == 0 && !SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_LEFT), "dead zone / hat release");
    CHECK(key(44, 0x401, AKEYCODE_BUTTON_B, 0x131, 1) && key(44, 0x401, AKEYCODE_BUTTON_MODE, 0x13c, 1), "B / MODE not consumed");
    zero_all("test");
    CHECK(!SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_EAST), "focus loss does not zero the pad");
    CHECK(SDL_GetJoystickTypeForID(id_of(slot_of(44))) == SDL_JOYSTICK_TYPE_GAMEPAD && SDL_IsGamepad(id_of(slot_of(44))), "the pad's SDL type");

    /* 2. a DualShock 4 without a key layout file: right stick RX/RY, triggers Z/RZ from -1 */
    const float dr[] = { R(0, -1, 1), R(1, -1, 1), R(11, -1, 1), R(12, -1, 1), R(13, -1, 1), R(14, -1, 1), R(15, -1, 1), R(16, -1, 1) };
    device(50, "Wireless Controller", 0x054c, 0x09cc, SRC_PAD, dr, 8, PADKEYS, 2, 1);
    pdev *ds4 = slot_of(50);
    CHECK(ds4 && ds4->prof == PROF_RXRY, "DS4 profile %d (want rxry)", ds4 ? ds4->prof : -1);
    AX[11] = -1.0f; AX[14] = 1.0f; AX[12] = 0.9f;
    motion(50, AINPUT_SOURCE_JOYSTICK, 0);
    SDL_Gamepad *g4 = pad_for(50);
    CHECK(g4 && SDL_GetGamepadAxis(g4, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) == 0 && SDL_GetGamepadAxis(g4, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) == 32767,
          "DS4 triggers %d %d (Z -1 = rest, RZ 1 = full)", g4 ? SDL_GetGamepadAxis(g4, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) : -1,
          g4 ? SDL_GetGamepadAxis(g4, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) : -1);
    CHECK(g4 && SDL_GetGamepadAxis(g4, SDL_GAMEPAD_AXIS_RIGHTX) > 28000, "DS4 right x from RX");
    CHECK(g4 && SDL_GetGamepadType(g4) == SDL_GAMEPAD_TYPE_PS4 && SDL_GetGamepadButtonLabel(g4, SDL_GAMEPAD_BUTTON_SOUTH) == SDL_GAMEPAD_BUTTON_LABEL_CROSS, "DS4 type / Cross");

    /* 3. a DualSense with its key layout file (standard) and its touchpad device */
    const float sr[] = { R(0, -1, 1), R(1, -1, 1), R(11, -1, 1), R(14, -1, 1), R(15, -1, 1), R(16, -1, 1), R(17, 0, 1), R(18, 0, 1) };
    device(60, "DualSense Wireless Controller", 0x054c, 0x0ce6, SRC_PAD, sr, 8, PADKEYS | KL2 | KR2, 2, 1);
    device(61, "DualSense Wireless Controller Touchpad", 0x054c, 0x0ce6, 0x00102002, NULL, 0, 0, 0, 1);
    pdev *dsn = slot_of(60);
    CHECK(dsn && dsn->prof == PROF_STD && dsn->rx == AMOTION_EVENT_AXIS_Z, "DualSense profile");
    AX[18] = 1.0f;
    motion(60, AINPUT_SOURCE_JOYSTICK, 0);
    SDL_Gamepad *g5 = pad_for(60);
    CHECK(g5 && SDL_GetGamepadAxis(g5, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) == 32767, "DualSense R2 axis");
    CHECK(motion(61, 0x00102002, AMOTION_EVENT_BUTTON_PRIMARY), "touchpad click not consumed");
    CHECK(g5 && SDL_GetGamepadButton(g5, SDL_GAMEPAD_BUTTON_TOUCHPAD), "touchpad click is not the touchpad button");
    motion(61, 0x00102002, 0);
    CHECK(g5 && !SDL_GetGamepadButton(g5, SDL_GAMEPAD_BUTTON_TOUCHPAD), "touchpad button stays down");
    key(60, 0x401, AKEYCODE_BUTTON_MODE, 0x13c, 1);
    CHECK(g5 && SDL_GetGamepadButton(g5, SDL_GAMEPAD_BUTTON_GUIDE), "PS button is not guide");
    CHECK(g5 && SDL_GetGamepadType(g5) == SDL_GAMEPAD_TYPE_PS5, "DualSense type");
    int np = 0;
    SDL_free(SDL_GetGamepads(&np));
    CHECK(np == 3, "%d pads listed (want 3: the touchpad is no pad)", np);

    /* 4. a T.16000M over USB: a joystick (no gamepad keys) */
    hint_stick = "0x044f/0xb10a,0x044f/0xb67b"; hint_thr = "0x044f/0xb687";
    const float tr[] = { R(0, -1, 1), R(1, -1, 1), R(14, -1, 1), R(19, 0, 1), R(15, -1, 1), R(16, -1, 1) };
    long long bk = 0xffffll << 16;
    pt_nev = 0;
    device(70, "Thrustmaster T.16000M", 0x044f, 0xb10a, SRC_JOYKB, tr, 6, bk, 0, 1);
    CHECK(count_events(SDL_EVENT_JOYSTICK_ADDED) == 1, "no JOYSTICK_ADDED for a plugged stick");
    int nj = 0;
    ids = SDL_GetJoysticks(&nj);
    CHECK(nj == 1, "%d joysticks listed (want 1)", nj);
    SDL_Joystick *j = nj ? SDL_OpenJoystick(ids[0]) : NULL;
    SDL_free(ids);
    CHECK(j && SDL_GetNumJoystickAxes(j) == 4 && SDL_GetNumJoystickButtons(j) == 16 && SDL_GetNumJoystickHats(j) == 1,
          "T.16000M axes %d buttons %d hats %d (want 4 16 1)", j ? SDL_GetNumJoystickAxes(j) : -1, j ? SDL_GetNumJoystickButtons(j) : -1,
          j ? SDL_GetNumJoystickHats(j) : -1);
    pdev *tm = slot_of(70);
    CHECK(tm && tm->jaxis[2] == AMOTION_EVENT_AXIS_RZ && tm->jaxis[3] == AMOTION_EVENT_AXIS_THROTTLE, "axis order (want X Y RZ THROTTLE)");
    char gs[40];
    SDL_GUIDToString(SDL_GetJoystickGUID(j), gs, sizeof gs);
    CHECK(!strcmp(gs, "030000004f0400000ab1000000000000"), "GUID %s (docs/HOTAS.md: 030000004f0400000ab1000000000000)", gs);
    Uint16 v = 0, p = 0;
    SDL_GetJoystickGUIDInfo(SDL_GetJoystickGUID(j), &v, &p, NULL, NULL);
    CHECK(v == 0x044f && p == 0xb10a, "GUID info %04x:%04x", v, p);
    CHECK(SDL_GetJoystickTypeForID(id_of(tm)) == SDL_JOYSTICK_TYPE_FLIGHT_STICK && !SDL_IsGamepad(id_of(tm)), "T.16000M type");
    Sint16 st;
    CHECK(!SDL_GetJoystickAxisInitialState(j, 3, &st), "a lever's place is known before it moved");
    pt_nev = 0;
    AX[19] = 0.5f; AX[0] = -1.0f; AX[16] = -1.0f;
    CHECK(motion(70, AINPUT_SOURCE_JOYSTICK, 0), "stick motion not consumed");
    CHECK(abs(SDL_GetJoystickAxis(j, 3)) <= 1 && SDL_GetJoystickAxis(j, 0) == -32768, "throttle %d (0.5 = 0), x %d", SDL_GetJoystickAxis(j, 3),
          SDL_GetJoystickAxis(j, 0));
    CHECK(SDL_GetJoystickAxisInitialState(j, 3, &st), "the lever's place after it moved");
    CHECK(SDL_GetJoystickHat(j, 0) == SDL_HAT_UP, "hat %d (want up)", SDL_GetJoystickHat(j, 0));
    CHECK(count_events(SDL_EVENT_JOYSTICK_AXIS_MOTION) == 4 && count_events(SDL_EVENT_JOYSTICK_HAT_MOTION) == 1, "joystick events %d axis %d hat",
          count_events(SDL_EVENT_JOYSTICK_AXIS_MOTION), count_events(SDL_EVENT_JOYSTICK_HAT_MOTION));
    CHECK(key(70, 0x101, AKEYCODE_BUTTON_1, 0x120, 1) && SDL_GetJoystickButton(j, 0), "BTN_TRIGGER is not button 0");
    CHECK(key(70, 0x101, AKEYCODE_BUTTON_16, 0x12f, 1) && SDL_GetJoystickButton(j, 15), "BTN_DEAD is not button 15");
    key(70, 0x101, AKEYCODE_BUTTON_1, 0x120, 0);
    CHECK(!SDL_GetJoystickButton(j, 0) && count_events(SDL_EVENT_JOYSTICK_BUTTON_DOWN) == 2 && count_events(SDL_EVENT_JOYSTICK_BUTTON_UP) == 1, "button events");
    zero_all("test");
    CHECK(!SDL_GetJoystickButton(j, 15) && SDL_GetJoystickAxis(j, 0) == -32768, "focus loss: buttons zero, axes kept");

    /* 5. an Xbox-compatible HOTAS (gamepad keys) on the flight-stick list: a joystick, never a pad */
    hint_stick = "0x044f/0xb10a,0x044f/0xb68d";
    device(80, "Thrustmaster T.Flight Hotas One", 0x044f, 0xb68d, SRC_PAD, xr, 10, PADKEYS, 0, 0);
    pdev *tf = slot_of(80);
    CHECK(tf && kind_of(tf) == K_JOY && !SDL_IsGamepad(id_of(tf)), "a listed HOTAS with gamepad keys is a pad");
    CHECK(key(80, 0x401, AKEYCODE_BUTTON_A, 0x130, 1) && SDL_GetGamepads(&np) && np == 3, "the HOTAS was announced as a pad");

    /* 6. removal */
    pt_nev = 0;
    j_removed(&fenv, NULL, 44);
    j_removed(&fenv, NULL, 70);
    CHECK(count_events(SDL_EVENT_GAMEPAD_REMOVED) == 1 && count_events(SDL_EVENT_JOYSTICK_REMOVED) == 1, "removal events");
    CHECK(!SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_SOUTH) && SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX) == 0, "a removed pad reads 0");

    /* 7. switches off: nothing is a pad or a joystick, keys are not consumed */
    g_on = 0; g_joy = 0;
    device(90, "Xbox Wireless Controller", 0x045e, 0x0b13, SRC_PAD, xr, 10, PADKEYS, 2, 1);
    CHECK(!key(90, 0x401, AKEYCODE_BUTTON_B, 0x131, 1), "ACZ_QUEST_PAD=0 consumes B");
    SDL_free(SDL_GetGamepads(&np));
    SDL_free(SDL_GetJoysticks(&nj));
    CHECK(np == 0 && nj == 0, "switches off: %d pads, %d joysticks", np, nj);

    /* 8. the displays for the GPU hang report (R-37) */
    char note[400];
    ps2_android_capture_note(note, sizeof note);
    CHECK(strstr(note, "listener did not start") != NULL, "%s", note);
    ps2_display_android_started();
    /* the device run's start (2026-11-10 00:48): the headset's screen and five MirrorRoot displays */
    ps2_display_android_event(0, 0, "Built-in Screen", 0x83, 2, 4128, 2208, 72.0f);
    static const int mid[] = { 2, 3, 7, 9, 10 };
    for (int i = 0; i < 5; i++) {
        char nm[64];
        snprintf(nm, sizeof nm, "MirrorRoot for 0000000%d-aaaa-bbbb-cccc-dddddddddddd", mid[i]);
        ps2_display_android_event(0, mid[i], nm, 0x03, 1, 1, 1, 60.0f);
    }
    ps2_android_capture_note(note, sizeof note);
    printf("%s\n", note);
    CHECK(!strncmp(note, "capture: none seen", 18) && strstr(note, " 2 MirrorRoot OFF") && strstr(note, "10 MirrorRoot OFF") && !strstr(note, " 0 "), "%s", note);
    /* the built-in display's rate changes (not a capture), then the recorder takes over MirrorRoot 7 */
    ps2_display_android_event(3, 0, "Built-in Screen", 0x83, 2, 4128, 2208, 90.0f);
    unsigned lg0 = disp_logged;
    ps2_display_android_event(3, 7, "MirrorRoot for 00000007-aaaa-bbbb-cccc-dddddddddddd", 0x03, 2, 1920, 1080, 72.0f);
    ps2_display_android_event(3, 7, "MirrorRoot for 00000007-aaaa-bbbb-cccc-dddddddddddd", 0x03, 2, 1920, 1080, 72.0f);
    ps2_display_android_event(3, 7, "MirrorRoot for 00000007-aaaa-bbbb-cccc-dddddddddddd", 0x03, 2, 1920, 1080, 72.0f);
    CHECK(disp_logged == lg0 + 1, "exact repeats logged: %u lines for 3 identical changes", disp_logged - lg0);
    ps2_android_capture_note(note, sizeof note);
    printf("%s\n", note);
    CHECK(strstr(note, "capture: \"MirrorRoot for 00000007") && strstr(note, "changed to ON") && strstr(note, "s before the hang (display 7, 1920x1080")
          && strstr(note, "was OFF 1x1"), "%s", note);
    /* a display added later, older than the change: the MirrorRoot change still wins; an added one alone is named */
    ps2_display_android_event(1, 12, "Overlay #1", 0x4, 2, 1280, 720, 60.0f);
    ps2_android_capture_note(note, sizeof note);
    printf("%s\n", note);
    CHECK(strstr(note, "changed to ON") && strstr(note, "added: \"Overlay #1\""), "%s", note);
    ps2_display_android_event(2, 12, NULL, 0, -1, 0, 0, 0.0f);
    CHECK(ds_slot(12, 0) < 0, "a removed display is still followed");
    char small[24];
    int l = ps2_android_capture_note(small, sizeof small);
    CHECK(l == (int)strlen(small) && l < (int)sizeof small, "a short buffer: %d", l);

    printf("pad_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
