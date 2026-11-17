/* ps2_pad_android.c -- gamepads, joysticks (HOTAS) and their rumble on the Quest build (Android-only file: the PC build's
   objects do not change).

   Pads and sticks reach the runtime through the same SDL3 calls the PC uses (SDL_GetGamepads / SDL_OpenGamepad /
   SDL_GetGamepadAxis / Button / SDL_RumbleGamepad, SDL_GetJoysticks / SDL_OpenJoystick / SDL_GetJoystickAxis / Button /
   Hat / GUID, the SDL_EVENT_GAMEPAD_* and SDL_EVENT_JOYSTICK_* events): they are implemented here over Android input
   events instead of being stubs in ps2_sdl_android.c, so ps2_video.c's pad registry, the bindings, the AUTO switch
   between Touch and the pad, ps2_hotas.c's profiles ("joy:" lines, docs/HOTAS.md) and the settings pages are the PC's own.

   Where the events come from (NOTES_RQ 2026-11-09 pad):
   - the step-0 probe (ACZ_QUEST_PAD_LOG=1, an Xbox Wireless Controller over Bluetooth, immersive flight): Android lists
     the pad, but none of its events reached the NativeActivity's input queue. Step 1: PadActivity
     (runtime/src/android/java/), a NativeActivity subclass whose dispatchKeyEvent / dispatchGenericMotionEvent /
     dispatchTouchEvent hand the events of gamepads, joysticks and Sony sub-devices to j_key / j_motion below; that is the
     path that works on the device (all the pad's events, 0 through the queue). PadActivity also reports every such
     device at the start and on hot-plug (j_device / j_removed: name, VID:PID, sources, axis ranges, keys, vibrators);
   - the native queue (app->onInputEvent, the glue's process_input replaced by q_process, which skips pre-dispatch for
     pad keys) is kept as a second path, in case an OS version or a USB device delivers there.

   Devices. One slot per Android device (up to NDEV in a run), SDL id 0x5A00 + slot. The kind is decided when SDL asks:
   - a joystick (SDL_GetJoysticks, ps2_hotas.c): a device with the JOYSTICK source and no gamepad keys, or any device
     whose VID:PID is in the profiles' flight-stick / throttle lists (ps2_hotas_pre_init sets them as SDL hints);
     ACZ_QUEST_JOY=1. Axes in SDL's Linux order (by the evdev code Android's axis comes from: X Y Z RX RY RZ THROTTLE
     RUDDER WHEEL GAS BRAKE, then the generic ones), scaled from the axis's range to -32768..32767 as SDL does; HAT_X /
     HAT_Y = hat 0; button n = the HID button number from the key's scan code (BTN_TRIGGER.. / BTN_SOUTH.. /
     BTN_TRIGGER_HAPPY..), which is DirectInput's numbering on the PC; the GUID is SDL's USB layout from VID:PID (bus
     0003, crc 0, version 0), so the shipped profiles match by VID:PID unchanged;
   - a gamepad (SDL_GetGamepads, ps2_video.c): a device with gamepad keys; ACZ_QUEST_PAD=1. It is announced
     (SDL_EVENT_GAMEPAD_ADDED) on its first event, so with no pad in use nothing changes for the Touch controllers;
   - a Sony pad's touchpad (DualSense / DualShock 4 "... Touchpad"): its click is the TOUCHPAD button of the Sony pad
     with the same VID:PID.
   Pad mapping (Android's layout, the Sony pads' key layout files included): keys BUTTON_A/B/X/Y -> SOUTH/EAST/WEST/
   NORTH (Cross/Circle/Square/Triangle), L1/R1 -> shoulders, THUMBL/THUMBR -> stick clicks, START -> START (Menu /
   Options), SELECT (or BACK from a pad) -> BACK (View / Share / Create), MODE -> GUIDE (Xbox / PS), DPAD_* keys and
   HAT_X/HAT_Y -> d-pad, L2/R2 keys -> full trigger. Axes by the pad's profile (ACZ_QUEST_PAD_PROFILE, auto = from the
   axes Android lists): `standard` = left X/Y, right Z/RZ (RX/RY if it has no Z/RZ), triggers the larger of LTRIGGER and
   BRAKE, RTRIGGER and GAS; `rxry` (no trigger axes but RX/RY: a pad without a key layout file) = right RX/RY, triggers
   Z / RZ scaled from their range. Radial dead zone ACZ_QUEST_PAD_DEADZONE (0.15) on the sticks. Every pad key and
   motion is consumed while the pad is on (an unhandled B falls back to BACK and MODE to HOME). Pads are zeroed when the
   window loses focus or the activity pauses (joysticks: buttons and hat; an axis keeps its place, as a lever does).

   Logs (stdout: aczquest.log and logcat tag aczquest): one "input: device ..." line per device when Android reports it
   (name, VID:PID, sources, the kind and profile, axes, keys, vibrators), "pad: ... is the SDL gamepad" when a pad is
   announced, one "input: first use" line per device and binding the first time it fires (what Android sent, what it
   became), the rumble's answer ("pad: rumble: ..."). ACZ_QUEST_PAD_LOG=1|2: the probe (every event, the activity's
   commands, the Java InputDevice census, a 5 s heartbeat; 2 = the native queue skips pre-dispatch for pad keys).

   Rumble (ACZ_QUEST_PAD_RUMBLE, default 1): ps2_video.c's apply_rumble (the game's motors, weather, buffet, gun:
   unchanged) calls SDL_RumbleGamepad on the renderer thread, every 100 ms for 250 ms while it runs. That call only posts
   the levels for the pad's slot; the rumble thread (attached to the JVM once) hands them to PadActivity.rumble(): the
   pad's vibrators from InputDevice.getVibratorManager() (two motors: the large one gets the low-frequency level, the
   small one the high), else InputDevice.getVibrator(). An unchanged level is sent again only when less than 100 ms of
   the last effect is left. Android lists a pad's vibrators only when its kernel driver offers force feedback over that
   connection: if it lists none (a 2024 report: Xbox pads rumble on Quest by cable only), "no vibrator" is logged and
   SDL_RumbleGamepad fails with that text (ps2_video.c: "rumble: SDL_RumbleGamepad failed"); it asks again every 5 s. */
#include "ps2_android.h"

#include <SDL3/SDL.h>

#include <android/input.h>
#include <android/keycodes.h>
#include <android_native_app_glue.h>
#include <jni.h>

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define NDEV 32
#define NAX 48                                  /* Android axis ids 0..47 (PadActivity.NAX) */
#define JAX 16                                  /* SDL axes of a joystick */
#define ID_BASE 0x5A00u

static int g_on;                                /* ACZ_QUEST_PAD */
static int g_joy;                               /* ACZ_QUEST_JOY */
static int g_log;                               /* ACZ_QUEST_PAD_LOG */
static int g_rumble = 1;                        /* ACZ_QUEST_PAD_RUMBLE */
static int g_prof = -1;                         /* ACZ_QUEST_PAD_PROFILE: -1 auto */
static float g_dz = 0.15f;                      /* ACZ_QUEST_PAD_DEADZONE */
static int g_java;                              /* PadActivity's natives are registered */
static unsigned g_seq, g_keys, g_motions, g_pre, g_cb, g_other, g_jkeys, g_jmotions;
static int g_focus = -1;
/* every pad's View / Back press edges, counted where Android delivers them (R-76, the device round of 2026-11-11): an adb
   tap is down and up within ~1 ms, between two of ps2_video.c's polls, and never showed in SDL_GetGamepadButton's level;
   the View filter (PS2_XR_SELECT_HOLD_MS, ps2_selhold.h) takes the count as presses since its last poll */
static unsigned g_back_downs;
unsigned ps2_pad_android_back_downs(void) { return __atomic_load_n(&g_back_downs, __ATOMIC_ACQUIRE); }
static JavaVM *g_vm;
static jclass g_cls;                            /* PadActivity (a global reference) */
static jmethodID g_rumble_m;                    /* PadActivity.rumble(int, int, int, int) */
void ps2_sdl_android_error(const char *s);      /* (ps2_sdl_android.c: SDL_GetError's text) */
void ps2_thread_helper(const char *name, int kind);

#define LD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define ST(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)

static int64_t mono_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000ll + (int64_t)t.tv_nsec;
}

/* ---- names for the logs ---- */
static const char *akey_name(int c) {
    static const char *const b[] = { "BUTTON_A", "BUTTON_B", "BUTTON_C", "BUTTON_X", "BUTTON_Y", "BUTTON_Z", "BUTTON_L1",
        "BUTTON_R1", "BUTTON_L2", "BUTTON_R2", "BUTTON_THUMBL", "BUTTON_THUMBR", "BUTTON_START", "BUTTON_SELECT", "BUTTON_MODE" };
    static char n[24];
    if (c >= AKEYCODE_BUTTON_A && c <= AKEYCODE_BUTTON_MODE) return b[c - AKEYCODE_BUTTON_A];
    if (c >= AKEYCODE_BUTTON_1 && c <= AKEYCODE_BUTTON_16) { snprintf(n, sizeof n, "BUTTON_%d", c - AKEYCODE_BUTTON_1 + 1); return n; }
    switch (c) {
    case AKEYCODE_DPAD_UP: return "DPAD_UP";
    case AKEYCODE_DPAD_DOWN: return "DPAD_DOWN";
    case AKEYCODE_DPAD_LEFT: return "DPAD_LEFT";
    case AKEYCODE_DPAD_RIGHT: return "DPAD_RIGHT";
    case AKEYCODE_DPAD_CENTER: return "DPAD_CENTER";
    case AKEYCODE_BACK: return "BACK";
    case AKEYCODE_MENU: return "MENU";
    default: snprintf(n, sizeof n, "keycode %d", c); return n;
    }
}
static const char *aaxis_name(int a) {
    static char n[16];
    switch (a) {
    case AMOTION_EVENT_AXIS_X: return "X";
    case AMOTION_EVENT_AXIS_Y: return "Y";
    case AMOTION_EVENT_AXIS_Z: return "Z";
    case AMOTION_EVENT_AXIS_RX: return "RX";
    case AMOTION_EVENT_AXIS_RY: return "RY";
    case AMOTION_EVENT_AXIS_RZ: return "RZ";
    case AMOTION_EVENT_AXIS_HAT_X: return "HAT_X";
    case AMOTION_EVENT_AXIS_HAT_Y: return "HAT_Y";
    case AMOTION_EVENT_AXIS_LTRIGGER: return "LTRIGGER";
    case AMOTION_EVENT_AXIS_RTRIGGER: return "RTRIGGER";
    case AMOTION_EVENT_AXIS_THROTTLE: return "THROTTLE";
    case AMOTION_EVENT_AXIS_RUDDER: return "RUDDER";
    case AMOTION_EVENT_AXIS_WHEEL: return "WHEEL";
    case AMOTION_EVENT_AXIS_GAS: return "GAS";
    case AMOTION_EVENT_AXIS_BRAKE: return "BRAKE";
    default:
        if (a >= AMOTION_EVENT_AXIS_GENERIC_1 && a <= AMOTION_EVENT_AXIS_GENERIC_16) snprintf(n, sizeof n, "GENERIC_%d", a - AMOTION_EVENT_AXIS_GENERIC_1 + 1);
        else snprintf(n, sizeof n, "axis %d", a);
        return n;
    }
}
static const char *const sdl_button_name[] = { "south (A / Cross)", "east (B / Circle)", "west (X / Square)", "north (Y / Triangle)",
    "back (View / Share)", "guide (Xbox / PS)", "start (Menu / Options)", "left stick", "right stick", "left shoulder",
    "right shoulder", "dpad up", "dpad down", "dpad left", "dpad right", "misc1", "paddle1", "paddle2", "paddle3", "paddle4",
    "touchpad" };
static const char *const sdl_axis_name[] = { "leftx", "lefty", "rightx", "righty", "left trigger", "right trigger" };

/* ---- the devices ---- */
enum { K_NONE, K_PAD, K_JOY, K_PADTOUCH };
enum { PROF_STD, PROF_RXRY };
struct SDL_Gamepad { int slot; };
struct SDL_Joystick { int slot; };
typedef struct {
    int used, gone;                             /* (atomic) published; removed */
    int dev, info;                              /* the Android device id; 1 = PadActivity reported it (name, ranges, keys) */
    char name[96];
    int vid, pid, sources, vib, touchpad;
    uint64_t keymask;
    unsigned char rhas[NAX];
    float rmin[NAX], rmax[NAX];
    /* as a pad */
    int prof, rx, ry, lt, rt;                   /* the Android axes of the right stick and (rxry) the triggers */
    int announced;                              /* (atomic) SDL_EVENT_GAMEPAD_ADDED pushed */
    uint32_t keys, hat;                         /* SDL button bits: from keys (with the touchpad's), from the hat */
    int lt_key, rt_key;
    int16_t ax[SDL_GAMEPAD_AXIS_COUNT];
    uint32_t seen_b, seen_a;                    /* first-use bits */
    /* as a joystick */
    int jn, jaxis[JAX], nbtn, nhat, jlisted;
    int16_t jv[JAX], jbase[JAX];
    unsigned char jknown[JAX];
    uint32_t jb[2];
    unsigned jhat;
    uint64_t jseen_b;
    uint32_t jseen_a, jseen_h;
    struct SDL_Gamepad gp;
    struct SDL_Joystick js;
} pdev;
static pdev D[NDEV];
static int nD;                                  /* (atomic) slots used */
static pthread_mutex_t dm = PTHREAD_MUTEX_INITIALIZER;

static int src_is(int source, int what) { return (source & what) == what; }
static int is_pad_key(int source, int code) {
    return src_is(source, AINPUT_SOURCE_GAMEPAD) || (code >= AKEYCODE_BUTTON_A && code <= AKEYCODE_BUTTON_MODE);
}
static pdev *slot_of(int dev) {
    int n = LD(nD);
    for (int i = 0; i < n; i++) if (LD(D[i].used) && !LD(D[i].gone) && D[i].dev == dev) return &D[i];
    return NULL;
}
static pdev *slot_by_id(SDL_JoystickID id) {
    if (id < ID_BASE || id >= ID_BASE + (unsigned)LD(nD)) return NULL;
    pdev *d = &D[id - ID_BASE];
    return LD(d->used) ? d : NULL;
}
static SDL_JoystickID id_of(const pdev *d) { return (SDL_JoystickID)(ID_BASE + (unsigned)(d - D)); }

/* the profiles' flight-stick / throttle lists (ps2_hotas_pre_init: SDL hints "0xVVVV/0xPPPP,...") */
static int in_hint(const char *hint, int vid, int pid) {
    const char *h = SDL_GetHint(hint);
    char want[16];
    if (!h || !*h) return 0;
    snprintf(want, sizeof want, "0x%04x/0x%04x", vid & 0xffff, pid & 0xffff);
    return strcasestr(h, want) != NULL;
}
static SDL_JoystickType flight_type(const pdev *d) {
    if (in_hint(SDL_HINT_JOYSTICK_FLIGHTSTICK_DEVICES, d->vid, d->pid)) return SDL_JOYSTICK_TYPE_FLIGHT_STICK;
    if (in_hint(SDL_HINT_JOYSTICK_THROTTLE_DEVICES, d->vid, d->pid)) return SDL_JOYSTICK_TYPE_THROTTLE;
    return SDL_JOYSTICK_TYPE_UNKNOWN;
}
static int kind_raw(const pdev *d) {             /* (what it is, switches aside) */
    if (d->touchpad) return K_PADTOUCH;
    int s = d->sources;
    if (!(s & 0x400) && !src_is(s, AINPUT_SOURCE_JOYSTICK)) return K_NONE;
    if (flight_type(d) != SDL_JOYSTICK_TYPE_UNKNOWN) return K_JOY;
    return (s & 0x400) ? K_PAD : K_JOY;
}
static int kind_of(const pdev *d) {
    if (!d || !LD(d->used) || LD(d->gone)) return K_NONE;
    int k = kind_raw(d);
    if ((k == K_PAD || k == K_PADTOUCH) && !g_on) return K_NONE;
    if (k == K_JOY && !g_joy) return K_NONE;
    return k;
}

/* the pad's profile from the axes Android lists */
static void pad_profile(pdev *d) {
    int has_trig = d->rhas[AMOTION_EVENT_AXIS_LTRIGGER] || d->rhas[AMOTION_EVENT_AXIS_RTRIGGER] || d->rhas[AMOTION_EVENT_AXIS_BRAKE]
                   || d->rhas[AMOTION_EVENT_AXIS_GAS];
    int has_zrz = d->rhas[AMOTION_EVENT_AXIS_Z] && d->rhas[AMOTION_EVENT_AXIS_RZ];
    int has_rxy = d->rhas[AMOTION_EVENT_AXIS_RX] && d->rhas[AMOTION_EVENT_AXIS_RY];
    d->prof = g_prof >= 0 ? g_prof : (!d->info || has_trig || !has_rxy) ? PROF_STD : PROF_RXRY;
    if (d->prof == PROF_RXRY) {
        d->rx = AMOTION_EVENT_AXIS_RX; d->ry = AMOTION_EVENT_AXIS_RY; d->lt = AMOTION_EVENT_AXIS_Z; d->rt = AMOTION_EVENT_AXIS_RZ;
    } else {
        int rxy = d->info && !has_zrz && has_rxy;
        d->rx = rxy ? AMOTION_EVENT_AXIS_RX : AMOTION_EVENT_AXIS_Z;
        d->ry = rxy ? AMOTION_EVENT_AXIS_RY : AMOTION_EVENT_AXIS_RZ;
        d->lt = AMOTION_EVENT_AXIS_LTRIGGER; d->rt = AMOTION_EVENT_AXIS_RTRIGGER;
    }
}
/* the joystick's SDL axes in SDL's Linux order (the evdev code each Android axis comes from) */
static int evdev_rank(int a) {
    switch (a) {
    case AMOTION_EVENT_AXIS_X: return 0;
    case AMOTION_EVENT_AXIS_Y: return 1;
    case AMOTION_EVENT_AXIS_Z: return 2;
    case AMOTION_EVENT_AXIS_RX: return 3;
    case AMOTION_EVENT_AXIS_RY: return 4;
    case AMOTION_EVENT_AXIS_RZ: return 5;
    case AMOTION_EVENT_AXIS_THROTTLE: return 6;
    case AMOTION_EVENT_AXIS_RUDDER: return 7;
    case AMOTION_EVENT_AXIS_WHEEL: return 8;
    case AMOTION_EVENT_AXIS_GAS: return 9;
    case AMOTION_EVENT_AXIS_BRAKE: return 10;
    case AMOTION_EVENT_AXIS_HAT_X: case AMOTION_EVENT_AXIS_HAT_Y: return -1;     /* (hat 0) */
    case AMOTION_EVENT_AXIS_LTRIGGER: return 11;     /* (only from a key layout file) */
    case AMOTION_EVENT_AXIS_RTRIGGER: return 12;
    default: return a >= AMOTION_EVENT_AXIS_GENERIC_1 && a <= AMOTION_EVENT_AXIS_GENERIC_16 ? 0x28 + (a - AMOTION_EVENT_AXIS_GENERIC_1) : 0x40 + a;
    }
}
static void joy_layout(pdev *d) {
    int n = 0;
    for (int a = 0; a < NAX; a++) {
        if (!d->rhas[a] || evdev_rank(a) < 0) continue;
        if (n == JAX) break;
        int i = n++;
        while (i > 0 && evdev_rank(d->jaxis[i - 1]) > evdev_rank(a)) { d->jaxis[i] = d->jaxis[i - 1]; i--; }
        d->jaxis[i] = a;
    }
    d->jn = n;
    d->nhat = d->rhas[AMOTION_EVENT_AXIS_HAT_X] || d->rhas[AMOTION_EVENT_AXIS_HAT_Y] ? 1 : 0;
    int nb = 0;                                  /* (the keys Android reports: BUTTON_1..16 = buttons 0..15; A..MODE = 0..14) */
    for (int i = 0; i < 15; i++) if (d->keymask >> i & 1u) nb = nb > i + 1 ? nb : i + 1;
    for (int i = 0; i < 16; i++) if (d->keymask >> (16 + i) & 1u) nb = nb > i + 1 ? nb : i + 1;
    d->nbtn = nb ? nb : 1;
}
static float range01(const pdev *d, int a, float v) {
    if (a < 0 || a >= NAX) return 0.0f;
    float lo = d->rhas[a] ? d->rmin[a] : 0.0f, hi = d->rhas[a] ? d->rmax[a] : 1.0f;
    if (hi - lo < 1e-6f) { lo = 0.0f; hi = 1.0f; }
    float t = (v - lo) / (hi - lo);
    return t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
}

/* a slot for this Android device: an existing one, else a new one (the device was not reported: the native queue's path,
   an `input` injection) with what the event says */
static void push_dev_event(Uint32 type, SDL_JoystickID id) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = type;
    e.jdevice.timestamp = SDL_GetTicksNS();
    e.jdevice.which = id;
    SDL_PushEvent(&e);
}
static int kind_of(const pdev *d);
static pdev *slot_get(int dev, int source) {
    pdev *d = slot_of(dev);
    if (d) {
        if (!d->info) __atomic_or_fetch(&d->sources, source, __ATOMIC_ACQ_REL);
        return d;
    }
    int made = 0;
    pthread_mutex_lock(&dm);
    d = slot_of(dev);
    if (!d && nD < NDEV) {
        d = &D[nD];
        memset(d, 0, sizeof *d);
        d->dev = dev;
        d->sources = source;
        snprintf(d->name, sizeof d->name, "Controller (Android device %d)", dev);
        d->gp.slot = d->js.slot = nD;
        static const int def[] = { AMOTION_EVENT_AXIS_X, AMOTION_EVENT_AXIS_Y, AMOTION_EVENT_AXIS_Z, AMOTION_EVENT_AXIS_RX,
            AMOTION_EVENT_AXIS_RY, AMOTION_EVENT_AXIS_RZ, AMOTION_EVENT_AXIS_HAT_X, AMOTION_EVENT_AXIS_HAT_Y };
        for (size_t i = 0; i < sizeof def / sizeof def[0]; i++) { d->rhas[def[i]] = 1; d->rmin[def[i]] = -1.0f; d->rmax[def[i]] = 1.0f; }
        d->rhas[AMOTION_EVENT_AXIS_THROTTLE] = 1; d->rmin[AMOTION_EVENT_AXIS_THROTTLE] = 0.0f; d->rmax[AMOTION_EVENT_AXIS_THROTTLE] = 1.0f;
        pad_profile(d);                          /* (not reported by PadActivity: Android's standard layout assumed) */
        joy_layout(d);
        ST(d->used, 1);
        ST(nD, nD + 1);
        made = 1;
    }
    pthread_mutex_unlock(&dm);
    if (made) {
        printf("input: device %d (not reported at the start: the native queue, an injection) sources 0x%08x: %s, Android's standard layout assumed\n",
               dev, (unsigned)source, (source & 0x400) ? "gamepad" : src_is(source, AINPUT_SOURCE_JOYSTICK) ? "joystick" : "not used");
        if (kind_of(d) == K_JOY) push_dev_event(SDL_EVENT_JOYSTICK_ADDED, id_of(d));
    }
    return d;
}

static void pad_announce(pdev *d) {
    if (LD(d->announced)) return;
    ST(d->announced, 1);
    push_dev_event(SDL_EVENT_GAMEPAD_ADDED, id_of(d));
    printf("pad: \"%s\" (Android device %d, %04x:%04x) is the SDL gamepad %u, profile %s (right stick %s/%s, triggers %s%s / %s%s)\n",
           d->name, d->dev, (unsigned)d->vid, (unsigned)d->pid, (unsigned)id_of(d), d->prof == PROF_RXRY ? "rxry" : "standard",
           aaxis_name(d->rx), aaxis_name(d->ry), aaxis_name(d->lt), d->prof == PROF_STD ? "+BRAKE" : "", aaxis_name(d->rt),
           d->prof == PROF_STD ? "+GAS" : "");
}

static void pad_zero(pdev *d) {
    ST(d->keys, 0u); ST(d->hat, 0u); ST(d->lt_key, 0); ST(d->rt_key, 0);
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; a++) ST(d->ax[a], (int16_t)0);
    ST(d->jb[0], 0u); ST(d->jb[1], 0u); ST(d->jhat, 0u);
}
static void zero_all(const char *why) {
    int n = LD(nD), any = 0;
    for (int i = 0; i < n; i++) {
        pdev *d = &D[i];
        if (LD(d->keys) || LD(d->hat) || LD(d->lt_key) || LD(d->rt_key) || LD(d->jb[0]) || LD(d->jb[1]) || LD(d->jhat)) any = 1;
        for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; a++) any |= LD(d->ax[a]) != 0;
        pad_zero(d);
    }
    if (any) printf("pad: state zeroed (%s)\n", why);
}

static int key_button(int code) {
    switch (code) {
    case AKEYCODE_BUTTON_A: return SDL_GAMEPAD_BUTTON_SOUTH;
    case AKEYCODE_BUTTON_B: return SDL_GAMEPAD_BUTTON_EAST;
    case AKEYCODE_BUTTON_X: return SDL_GAMEPAD_BUTTON_WEST;
    case AKEYCODE_BUTTON_Y: return SDL_GAMEPAD_BUTTON_NORTH;
    case AKEYCODE_BUTTON_SELECT: case AKEYCODE_BACK: return SDL_GAMEPAD_BUTTON_BACK;
    case AKEYCODE_BUTTON_MODE: return SDL_GAMEPAD_BUTTON_GUIDE;
    case AKEYCODE_BUTTON_START: return SDL_GAMEPAD_BUTTON_START;
    case AKEYCODE_BUTTON_THUMBL: return SDL_GAMEPAD_BUTTON_LEFT_STICK;
    case AKEYCODE_BUTTON_THUMBR: return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
    case AKEYCODE_BUTTON_L1: return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    case AKEYCODE_BUTTON_R1: return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    case AKEYCODE_DPAD_UP: return SDL_GAMEPAD_BUTTON_DPAD_UP;
    case AKEYCODE_DPAD_DOWN: return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    case AKEYCODE_DPAD_LEFT: return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    case AKEYCODE_DPAD_RIGHT: return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    default: return -1;
    }
}
/* a joystick button's index: the HID button number from the evdev scan code (as DirectInput numbers them on the PC),
   else from the Android key code */
static int joy_button(int code, int scan) {
    if (scan >= 0x120 && scan <= 0x12f) return scan - 0x120;          /* BTN_TRIGGER .. BTN_DEAD: HID buttons 1-16 */
    if (scan >= 0x130 && scan <= 0x13f) return scan - 0x130;          /* BTN_SOUTH ..: a gamepad-application device's 1-16 */
    if (scan >= 0x2c0 && scan <= 0x2e7) return 16 + scan - 0x2c0;     /* BTN_TRIGGER_HAPPY1..: HID buttons 17-56 */
    if (code >= AKEYCODE_BUTTON_1 && code <= AKEYCODE_BUTTON_16) return code - AKEYCODE_BUTTON_1;
    if (code >= AKEYCODE_BUTTON_A && code <= AKEYCODE_BUTTON_MODE) return code - AKEYCODE_BUTTON_A;
    return -1;
}

/* the Sony pad a touchpad belongs to (the same VID:PID) */
static pdev *touch_owner(const pdev *t) {
    int n = LD(nD);
    pdev *best = NULL;
    for (int i = 0; i < n; i++) {
        pdev *d = &D[i];
        if (d == t || kind_of(d) != K_PAD || d->vid != t->vid || d->pid != t->pid) continue;
        if (!best || (LD(d->announced) && !LD(best->announced))) best = d;
    }
    return best;
}
static void touchpad_button(pdev *t, int down, const char *what) {
    pdev *p = touch_owner(t);
    if (!p) return;
    pad_announce(p);
    uint32_t bit = 1u << SDL_GAMEPAD_BUTTON_TOUCHPAD;
    if (down) __atomic_or_fetch(&p->keys, bit, __ATOMIC_ACQ_REL);
    else __atomic_and_fetch(&p->keys, ~bit, __ATOMIC_ACQ_REL);
    if (down && !(p->seen_b & bit)) {
        p->seen_b |= bit;
        printf("input: \"%s\": first use: %s on its touchpad (Android device %d) -> touchpad\n", p->name, what, t->dev);
    }
}

/* a key event of either path; returns 1 to consume it */
static int pad_key(int dev, int source, int action, int code, int scan, int repeat) {
    pdev *d = slot_get(dev, source);
    int k = kind_of(d);
    if (k == K_PADTOUCH) {
        if (action != AKEY_EVENT_ACTION_MULTIPLE && !repeat) {
            char w[48];
            snprintf(w, sizeof w, "%s (scan 0x%x)", akey_name(code), (unsigned)scan);
            touchpad_button(d, action == AKEY_EVENT_ACTION_DOWN, w);
        }
        return 1;
    }
    if (k == K_JOY) {
        if (action == AKEY_EVENT_ACTION_MULTIPLE || repeat > 0) return 1;
        int b = joy_button(code, scan), down = action == AKEY_EVENT_ACTION_DOWN;
        if (b < 0 || b >= 64) return 1;
        if (b >= d->nbtn) d->nbtn = b + 1;
        uint32_t bit = 1u << (b & 31);
        uint32_t was = LD(d->jb[b >> 5]) & bit;
        if (down) __atomic_or_fetch(&d->jb[b >> 5], bit, __ATOMIC_ACQ_REL);
        else __atomic_and_fetch(&d->jb[b >> 5], ~bit, __ATOMIC_ACQ_REL);
        if (!was != !down) {
            SDL_Event e;
            memset(&e, 0, sizeof e);
            e.type = down ? SDL_EVENT_JOYSTICK_BUTTON_DOWN : SDL_EVENT_JOYSTICK_BUTTON_UP;
            e.jbutton.timestamp = SDL_GetTicksNS();
            e.jbutton.which = id_of(d);
            e.jbutton.button = (Uint8)b;
            e.jbutton.down = down != 0;
            SDL_PushEvent(&e);
        }
        if (down && !(d->jseen_b >> b & 1u)) {
            d->jseen_b |= 1ull << b;
            printf("input: \"%s\": first use: %s (scan 0x%x) -> joystick button %d\n", d->name, akey_name(code), (unsigned)scan, b);
        }
        return 1;
    }
    if (k != K_PAD) return g_log && is_pad_key(source, code);   /* (the probe alone: consumed, so B cannot end the test through BACK) */
    if (action == AKEY_EVENT_ACTION_MULTIPLE || repeat > 0) return 1;
    int down = action == AKEY_EVENT_ACTION_DOWN;
    pad_announce(d);
    int b;
    if (code == AKEYCODE_BUTTON_L2) { ST(d->lt_key, down); b = 32 + SDL_GAMEPAD_AXIS_LEFT_TRIGGER; }
    else if (code == AKEYCODE_BUTTON_R2) { ST(d->rt_key, down); b = 32 + SDL_GAMEPAD_AXIS_RIGHT_TRIGGER; }
    else {
        b = key_button(code);
        if (b >= 0) {
            uint32_t was = down ? __atomic_fetch_or(&d->keys, 1u << b, __ATOMIC_ACQ_REL)
                                : __atomic_fetch_and(&d->keys, ~(1u << b), __ATOMIC_ACQ_REL);
            if (down && b == SDL_GAMEPAD_BUTTON_BACK && !(was >> b & 1u))   /* (a press edge: ps2_pad_android_back_downs) */
                __atomic_add_fetch(&g_back_downs, 1u, __ATOMIC_ACQ_REL);
        }
    }
    if (down) {
        if (b < 0) {
            if (!(d->seen_a & 0x80000000u)) {
                d->seen_a |= 0x80000000u;
                printf("input: \"%s\": first use: %s (scan 0x%x) -> nothing (no mapping)\n", d->name, akey_name(code), (unsigned)scan);
            }
        } else if (b < 32 ? !(d->seen_b >> b & 1u) : !(d->seen_a >> (b - 32) & 1u)) {
            if (b < 32) d->seen_b |= 1u << b; else d->seen_a |= 1u << (b - 32);
            printf("input: \"%s\": first use: %s (scan 0x%x) -> %s%s\n", d->name, akey_name(code), (unsigned)scan,
                   b < 32 ? sdl_button_name[b < 21 ? b : 0] : sdl_axis_name[b - 32], b < 32 ? "" : " (full, a key)");
        }
    }
    return 1;
}

static int16_t to_s16(float v) {
    v = v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
    return (int16_t)lrintf(v * 32767.0f);
}
static void pad_axis_set(pdev *d, int sdl_axis, int16_t v, int from) {
    ST(d->ax[sdl_axis], v);
    if ((v > 16383 || v < -16383) && !(d->seen_a >> sdl_axis & 1u)) {
        d->seen_a |= 1u << sdl_axis;
        printf("input: \"%s\": first use: AXIS_%s -> %s\n", d->name, aaxis_name(from), sdl_axis_name[sdl_axis]);
    }
}
static void stick(pdev *d, const float *ax, int a_x, int a_y, int sx, int sy) {
    float x = ax[a_x], y = ax[a_y], m = sqrtf(x * x + y * y);
    if (m <= g_dz) x = y = 0.0f;
    else {
        float s = (m - g_dz) / (1.0f - g_dz) / m;
        if (m * s > 1.0f) s = 1.0f / m;
        x *= s; y *= s;
    }
    pad_axis_set(d, sx, to_s16(x), a_x);
    pad_axis_set(d, sy, to_s16(y), a_y);
}
static void trigger(pdev *d, int sdl_axis, float v, int from) {
    v = v < 0.02f ? 0.0f : v;                    /* (a resting trigger's noise) */
    pad_axis_set(d, sdl_axis, to_s16(v), from);
}
static void hat_bits(pdev *d, float hx, float hy) {
    uint32_t h = 0;
    if (hx < -0.5f) h |= 1u << SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    if (hx > 0.5f) h |= 1u << SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    if (hy < -0.5f) h |= 1u << SDL_GAMEPAD_BUTTON_DPAD_UP;
    if (hy > 0.5f) h |= 1u << SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    ST(d->hat, h);
    uint32_t nw = h & ~d->seen_b;
    if (nw) {
        d->seen_b |= nw;
        for (int b = 0; b < 32; b++)
            if (nw >> b & 1u) printf("input: \"%s\": first use: AXIS_HAT_%s -> %s\n", d->name, b == SDL_GAMEPAD_BUTTON_DPAD_LEFT
                                     || b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT ? "X" : "Y", sdl_button_name[b]);
    }
}
static int16_t joy_value(const pdev *d, int a, float v) {
    float lo = d->rhas[a] ? d->rmin[a] : -1.0f, hi = d->rhas[a] ? d->rmax[a] : 1.0f;
    if (hi - lo < 1e-6f) { lo = -1.0f; hi = 1.0f; }
    float t = (v - lo) / (hi - lo);
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    int s = (int)lrintf(t * 65535.0f) - 32768;
    return (int16_t)(s < -32768 ? -32768 : s > 32767 ? 32767 : s);
}
/* a motion event of either path (ax: Android axis ids 0..NAX-1); returns 1 to consume it */
static int pad_motion(int dev, int source, int action, int buttons, const float *ax) {
    (void)action;
    pdev *d = slot_get(dev, source);
    int k = kind_of(d);
    if (k == K_PADTOUCH) {
        static int was[NDEV];
        int i = (int)(d - D), down = (buttons & (AMOTION_EVENT_BUTTON_PRIMARY | AMOTION_EVENT_BUTTON_SECONDARY | AMOTION_EVENT_BUTTON_TERTIARY)) != 0;
        if (down != was[i]) { was[i] = down; touchpad_button(d, down, "a click"); }
        return 1;
    }
    if (k == K_JOY) {
        for (int i = 0; i < d->jn; i++) {
            int a = d->jaxis[i];
            int16_t v = joy_value(d, a, ax[a]), o = LD(d->jv[i]);
            if (!d->jknown[i] || abs((int)v - (int)o) >= 128) {
                ST(d->jv[i], v);
                if (!d->jknown[i]) d->jbase[i] = v;
                d->jknown[i] = 1;
                SDL_Event e;
                memset(&e, 0, sizeof e);
                e.type = SDL_EVENT_JOYSTICK_AXIS_MOTION;
                e.jaxis.timestamp = SDL_GetTicksNS();
                e.jaxis.which = id_of(d);
                e.jaxis.axis = (Uint8)i;
                e.jaxis.value = v;
                SDL_PushEvent(&e);
                if (!(d->jseen_a >> i & 1u) && abs((int)v - (int)d->jbase[i]) > 16384) {
                    d->jseen_a |= 1u << i;
                    printf("input: \"%s\": first use: AXIS_%s -> joystick axis %d\n", d->name, aaxis_name(a), i);
                }
            }
        }
        if (d->nhat) {
            float hx = ax[AMOTION_EVENT_AXIS_HAT_X], hy = ax[AMOTION_EVENT_AXIS_HAT_Y];
            unsigned h = (hy < -0.5f ? SDL_HAT_UP : 0) | (hy > 0.5f ? SDL_HAT_DOWN : 0) | (hx < -0.5f ? SDL_HAT_LEFT : 0) | (hx > 0.5f ? SDL_HAT_RIGHT : 0);
            if (h != LD(d->jhat)) {
                ST(d->jhat, h);
                SDL_Event e;
                memset(&e, 0, sizeof e);
                e.type = SDL_EVENT_JOYSTICK_HAT_MOTION;
                e.jhat.timestamp = SDL_GetTicksNS();
                e.jhat.which = id_of(d);
                e.jhat.hat = 0;
                e.jhat.value = (Uint8)h;
                SDL_PushEvent(&e);
                if (h && !(d->jseen_h & h)) {
                    d->jseen_h |= h;
                    printf("input: \"%s\": first use: AXIS_HAT_X/Y (%s%s%s%s) -> joystick hat 0\n", d->name, h & SDL_HAT_UP ? "up" : "",
                           h & SDL_HAT_DOWN ? "down" : "", h & SDL_HAT_LEFT ? "left" : "", h & SDL_HAT_RIGHT ? "right" : "");
                }
            }
        }
        return 1;
    }
    if (k != K_PAD) return 0;
    pad_announce(d);
    stick(d, ax, AMOTION_EVENT_AXIS_X, AMOTION_EVENT_AXIS_Y, SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY);
    stick(d, ax, d->rx, d->ry, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY);
    if (d->prof == PROF_RXRY) {
        trigger(d, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, range01(d, d->lt, ax[d->lt]), d->lt);
        trigger(d, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, range01(d, d->rt, ax[d->rt]), d->rt);
    } else {                                     /* the larger of each pair: whichever one this pad moves */
        float l1 = range01(d, AMOTION_EVENT_AXIS_LTRIGGER, ax[AMOTION_EVENT_AXIS_LTRIGGER]), l2 = range01(d, AMOTION_EVENT_AXIS_BRAKE, ax[AMOTION_EVENT_AXIS_BRAKE]);
        float r1 = range01(d, AMOTION_EVENT_AXIS_RTRIGGER, ax[AMOTION_EVENT_AXIS_RTRIGGER]), r2 = range01(d, AMOTION_EVENT_AXIS_GAS, ax[AMOTION_EVENT_AXIS_GAS]);
        trigger(d, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, l1 >= l2 ? l1 : l2, l1 >= l2 ? AMOTION_EVENT_AXIS_LTRIGGER : AMOTION_EVENT_AXIS_BRAKE);
        trigger(d, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, r1 >= r2 ? r1 : r2, r1 >= r2 ? AMOTION_EVENT_AXIS_RTRIGGER : AMOTION_EVENT_AXIS_GAS);
    }
    hat_bits(d, ax[AMOTION_EVENT_AXIS_HAT_X], ax[AMOTION_EVENT_AXIS_HAT_Y]);
    return 1;
}

/* ---- the SDL3 gamepad calls (ps2_video.c's pad registry) ---- */
static pdev *pad_of(SDL_Gamepad *g) {
    if (!g) return NULL;
    pdev *d = &D[g->slot];
    return g == &d->gp ? d : NULL;
}
SDL_JoystickID *SDL_GetGamepads(int *count) {
    SDL_JoystickID *ids = (SDL_JoystickID *)calloc(NDEV + 1, sizeof(SDL_JoystickID));
    int n = 0, m = LD(nD);
    for (int i = 0; ids && i < m; i++) if (kind_of(&D[i]) == K_PAD && LD(D[i].announced)) ids[n++] = id_of(&D[i]);
    if (count) *count = ids ? n : 0;
    return ids;
}
bool SDL_IsGamepad(SDL_JoystickID id) { return kind_of(slot_by_id(id)) == K_PAD; }
SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID id) {
    pdev *d = slot_by_id(id);
    if (kind_of(d) == K_PAD) return &d->gp;
    ps2_sdl_android_error("not a gamepad of the Quest build");
    return NULL;
}
void SDL_CloseGamepad(SDL_Gamepad *g) { (void)g; }
Sint16 SDL_GetGamepadAxis(SDL_Gamepad *g, SDL_GamepadAxis a) {
    pdev *d = pad_of(g);
    if (!d || LD(d->gone) || a < 0 || a >= SDL_GAMEPAD_AXIS_COUNT) return 0;
    int v = LD(d->ax[a]);
    if (a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER && LD(d->lt_key)) v = 32767;
    if (a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER && LD(d->rt_key)) v = 32767;
    return (Sint16)v;
}
bool SDL_GetGamepadButton(SDL_Gamepad *g, SDL_GamepadButton b) {
    pdev *d = pad_of(g);
    if (!d || LD(d->gone) || b < 0 || b >= 32) return false;
    return ((LD(d->keys) | LD(d->hat)) >> b) & 1u;
}
static int sony(const pdev *d) { return d->vid == 0x054c; }
SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad *g) {
    pdev *d = pad_of(g);
    if (!d) return SDL_GAMEPAD_TYPE_UNKNOWN;
    if (d->vid == 0x045e) return SDL_GAMEPAD_TYPE_XBOXONE;
    if (sony(d)) return d->pid == 0x0ce6 || d->pid == 0x0df2 ? SDL_GAMEPAD_TYPE_PS5 : SDL_GAMEPAD_TYPE_PS4;
    return SDL_GAMEPAD_TYPE_STANDARD;
}
SDL_GamepadType SDL_GetRealGamepadType(SDL_Gamepad *g) { return SDL_GetGamepadType(g); }
SDL_GamepadButtonLabel SDL_GetGamepadButtonLabel(SDL_Gamepad *g, SDL_GamepadButton b) {
    pdev *d = pad_of(g);
    if (!d) return SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return sony(d) ? SDL_GAMEPAD_BUTTON_LABEL_CROSS : SDL_GAMEPAD_BUTTON_LABEL_A;
    case SDL_GAMEPAD_BUTTON_EAST: return sony(d) ? SDL_GAMEPAD_BUTTON_LABEL_CIRCLE : SDL_GAMEPAD_BUTTON_LABEL_B;
    case SDL_GAMEPAD_BUTTON_WEST: return sony(d) ? SDL_GAMEPAD_BUTTON_LABEL_SQUARE : SDL_GAMEPAD_BUTTON_LABEL_X;
    case SDL_GAMEPAD_BUTTON_NORTH: return sony(d) ? SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE : SDL_GAMEPAD_BUTTON_LABEL_Y;
    default: return SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;
    }
}
SDL_JoystickConnectionState SDL_GetGamepadConnectionState(SDL_Gamepad *g) {
    return pad_of(g) ? SDL_JOYSTICK_CONNECTION_UNKNOWN : SDL_JOYSTICK_CONNECTION_INVALID;
}
const char *SDL_GetGamepadName(SDL_Gamepad *g) { pdev *d = pad_of(g); return d ? d->name : NULL; }
const char *SDL_GetGamepadPath(SDL_Gamepad *g) { (void)g; return NULL; }
Uint16 SDL_GetGamepadProduct(SDL_Gamepad *g) { pdev *d = pad_of(g); return d ? (Uint16)d->pid : 0; }
Uint16 SDL_GetGamepadVendor(SDL_Gamepad *g) { pdev *d = pad_of(g); return d ? (Uint16)d->vid : 0; }
SDL_PropertiesID SDL_GetGamepadProperties(SDL_Gamepad *g) { (void)g; return 0; }
bool SDL_SendGamepadEffect(SDL_Gamepad *g, const void *p, int n) { (void)g; (void)p; (void)n; return false; }

/* ---- the SDL3 joystick calls (ps2_hotas.c) ---- */
static pdev *joy_of(SDL_Joystick *j) {
    if (!j) return NULL;
    pdev *d = &D[j->slot];
    return j == &d->js ? d : NULL;
}
SDL_JoystickID *SDL_GetJoysticks(int *count) {
    SDL_JoystickID *ids = (SDL_JoystickID *)calloc(NDEV + 1, sizeof(SDL_JoystickID));
    int n = 0, m = LD(nD);
    for (int i = 0; ids && i < m; i++)
        if (kind_of(&D[i]) == K_JOY) { ids[n++] = id_of(&D[i]); D[i].jlisted = 1; }
    if (count) *count = ids ? n : 0;
    return ids;
}
SDL_Joystick *SDL_OpenJoystick(SDL_JoystickID id) {
    pdev *d = slot_by_id(id);
    if (kind_of(d) == K_JOY) return &d->js;
    ps2_sdl_android_error("not a joystick of the Quest build");
    return NULL;
}
void SDL_CloseJoystick(SDL_Joystick *j) { (void)j; }
Sint16 SDL_GetJoystickAxis(SDL_Joystick *j, int a) {
    pdev *d = joy_of(j);
    return d && a >= 0 && a < d->jn ? (Sint16)LD(d->jv[a]) : 0;
}
bool SDL_GetJoystickAxisInitialState(SDL_Joystick *j, int a, Sint16 *s) {
    pdev *d = joy_of(j);
    if (s) *s = 0;
    if (!d || a < 0 || a >= d->jn || !d->jknown[a]) return false;   /* (Android gives an axis's place only with its first event) */
    if (s) *s = (Sint16)LD(d->jv[a]);
    return true;
}
bool SDL_GetJoystickButton(SDL_Joystick *j, int b) {
    pdev *d = joy_of(j);
    return d && b >= 0 && b < 64 && (LD(d->jb[b >> 5]) >> (b & 31) & 1u);
}
Uint8 SDL_GetJoystickHat(SDL_Joystick *j, int h) {
    pdev *d = joy_of(j);
    return d && h == 0 && d->nhat ? (Uint8)LD(d->jhat) : SDL_HAT_CENTERED;
}
int SDL_GetNumJoystickAxes(SDL_Joystick *j) { pdev *d = joy_of(j); return d ? d->jn : 0; }
int SDL_GetNumJoystickButtons(SDL_Joystick *j) { pdev *d = joy_of(j); return d ? d->nbtn : 0; }
int SDL_GetNumJoystickHats(SDL_Joystick *j) { pdev *d = joy_of(j); return d ? d->nhat : 0; }
/* SDL's GUID of a USB device: bus 0003, crc16 0, vendor, 0, product, 0, version 0, driver 0 (little-endian words) */
SDL_GUID SDL_GetJoystickGUID(SDL_Joystick *j) {
    SDL_GUID g;
    memset(&g, 0, sizeof g);
    pdev *d = joy_of(j);
    if (!d) return g;
    g.data[0] = 0x03;
    g.data[4] = (Uint8)(d->vid & 0xff); g.data[5] = (Uint8)(d->vid >> 8 & 0xff);
    g.data[8] = (Uint8)(d->pid & 0xff); g.data[9] = (Uint8)(d->pid >> 8 & 0xff);
    return g;
}
void SDL_GetJoystickGUIDInfo(SDL_GUID g, Uint16 *v, Uint16 *p, Uint16 *ver, Uint16 *crc) {
    if (v) *v = (Uint16)(g.data[4] | g.data[5] << 8);
    if (p) *p = (Uint16)(g.data[8] | g.data[9] << 8);
    if (ver) *ver = (Uint16)(g.data[12] | g.data[13] << 8);
    if (crc) *crc = (Uint16)(g.data[2] | g.data[3] << 8);
}
const char *SDL_GetJoystickName(SDL_Joystick *j) { pdev *d = joy_of(j); return d ? d->name : NULL; }
const char *SDL_GetJoystickNameForID(SDL_JoystickID id) { pdev *d = slot_by_id(id); return d ? d->name : NULL; }
SDL_JoystickType SDL_GetJoystickTypeForID(SDL_JoystickID id) {
    pdev *d = slot_by_id(id);
    if (!d) return SDL_JOYSTICK_TYPE_UNKNOWN;
    SDL_JoystickType t = flight_type(d);
    if (t != SDL_JOYSTICK_TYPE_UNKNOWN) return t;
    return kind_of(d) == K_PAD ? SDL_JOYSTICK_TYPE_GAMEPAD : SDL_JOYSTICK_TYPE_UNKNOWN;
}

/* ---- rumble: posted here (renderer thread), sent by the rumble thread through PadActivity.rumble ---- */
static pthread_mutex_t rm_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rm_c = PTHREAD_COND_INITIALIZER;
static unsigned rm_gen, rm_sgen[NDEV];
static int rm_lo[NDEV], rm_hi[NDEV], rm_ms[NDEV], rm_thread_on;
static int rm_state[NDEV];                      /* the last answer: 0 none yet, > 0 motors, -1 no device, -2 no vibrator, -3 exception */
static void *rm_thread(void *arg) {
    (void)arg;
    ps2_thread_helper("padrumble", 2);
    JNIEnv *env = NULL;
    JavaVMAttachArgs aa = { JNI_VERSION_1_6, "pad-rumble", NULL };
    if ((*g_vm)->AttachCurrentThread(g_vm, &env, &aa) != JNI_OK || !env) {
        printf("pad: rumble: the rumble thread could not attach to the JVM\n");
        for (int i = 0; i < NDEV; i++) __atomic_store_n(&rm_state[i], -3, __ATOMIC_RELEASE);
        return NULL;
    }
    unsigned seen = 0, sseen[NDEV] = { 0 }, n = 0;
    int last_lo[NDEV] = { 0 }, last_hi[NDEV] = { 0 }, said[NDEV] = { 0 };
    int64_t until[NDEV] = { 0 };
    for (;;) {
        int lo[NDEV], hi[NDEV], ms[NDEV], due[NDEV];
        pthread_mutex_lock(&rm_m);
        while (rm_gen == seen) pthread_cond_wait(&rm_c, &rm_m);
        seen = rm_gen;
        for (int i = 0; i < NDEV; i++) {
            due[i] = rm_sgen[i] != sseen[i];
            sseen[i] = rm_sgen[i];
            lo[i] = rm_lo[i]; hi[i] = rm_hi[i]; ms[i] = rm_ms[i];
        }
        pthread_mutex_unlock(&rm_m);
        for (int i = 0; i < NDEV; i++) {
            if (!due[i]) continue;
            int64_t now = mono_ns();
            if (!lo[i] && !hi[i] && !last_lo[i] && !last_hi[i]) continue;                     /* (already still) */
            if (lo[i] == last_lo[i] && hi[i] == last_hi[i] && (lo[i] || hi[i]) && until[i] - now > 100000000ll) continue;
            pdev *d = &D[i];
            int r = (*env)->CallStaticIntMethod(env, g_cls, g_rumble_m, (jint)d->dev, (jint)lo[i], (jint)hi[i], (jint)ms[i]);
            if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); r = -3; }
            double call_ms = (double)(mono_ns() - now) * 1e-6;
            n++;
            last_lo[i] = lo[i]; last_hi[i] = hi[i];
            until[i] = now + (int64_t)ms[i] * 1000000ll;
            __atomic_store_n(&rm_state[i], r, __ATOMIC_RELEASE);
            if (r != said[i]) {                   /* (always: the first answer and every change of it) */
                said[i] = r;
                printf("pad: rumble: \"%s\" (device %d): %s\n", d->name, d->dev,
                       r > 0 && r < 100 ? "driven through VibratorManager" : r == 100 ? "driven through its Vibrator (one level)"
                       : r == -1 ? "Android no longer lists the device"
                       : r == -2 ? "Android offers no vibrator for it: no rumble over this connection (Bluetooth? a cable may give it)"
                       : "an exception in PadActivity.rumble (logcat tag aczquest)");
                if (r > 0 && r < 100) printf("pad: rumble: %d motor%s\n", r, r == 1 ? " (both levels on it)" : "s (large = low frequency, small = high)");
            }
            if (g_log) printf("padlog: rumble #%u dev=%d lo=%d hi=%d ms=%d -> %d (%.2f ms)\n", n, d->dev, lo[i], hi[i], ms[i], r, call_ms);
        }
    }
    return NULL;
}
static int rm_level(Uint16 v) { int l = ((int)v + 128) >> 8; return v && !l ? 1 : l > 255 ? 255 : l; }
bool SDL_RumbleGamepad(SDL_Gamepad *g, Uint16 lo, Uint16 hi, Uint32 ms) {
    pdev *d = pad_of(g);
    if (!d) { ps2_sdl_android_error("not a gamepad of the Quest build"); return false; }
    if (!g_rumble) { ps2_sdl_android_error("rumble off (ACZ_QUEST_PAD_RUMBLE=0)"); return false; }
    if (!g_cls || !g_rumble_m) { ps2_sdl_android_error("no rumble: the activity is not PadActivity"); return false; }
    int i = (int)(d - D);
    int st = __atomic_load_n(&rm_state[i], __ATOMIC_ACQUIRE);
    if (d->info && d->vib == 0 && st == 0) st = -2;     /* (PadActivity reported no vibrator at connect) */
    if (st == -1 || st == -2) {                   /* (asked again every 5 s: a cable plugged in later is another device) */
        static int64_t retry[NDEV];
        int64_t now = mono_ns();
        ps2_sdl_android_error(st == -2 ? "Android offers no vibrator for this pad (no rumble over this connection)"
                                       : "Android no longer lists the pad's device");
        if (now < retry[i] || (!lo && !hi)) return false;
        retry[i] = now + 5000000000ll;
    }
    if (st == -3) { ps2_sdl_android_error("PadActivity.rumble failed (logcat tag aczquest)"); return false; }
    pthread_mutex_lock(&rm_m);
    if (!rm_thread_on) {
        pthread_t th;
        rm_thread_on = pthread_create(&th, NULL, rm_thread, NULL) == 0 ? 1 : -1;
        if (rm_thread_on > 0) pthread_detach(th);
    }
    int ok = rm_thread_on > 0;
    if (ok) {
        rm_lo[i] = rm_level(lo); rm_hi[i] = rm_level(hi); rm_ms[i] = ms ? (int)(ms > 10000u ? 10000u : ms) : 1;
        rm_sgen[i]++;
        rm_gen++;
        pthread_cond_signal(&rm_c);
    }
    pthread_mutex_unlock(&rm_m);
    if (!ok) ps2_sdl_android_error("no rumble thread");
    return ok && st != -1 && st != -2;
}

/* ---- the probe's lines ---- */
typedef struct {
    int type, source, device, action, code, scan, flags, meta, repeat, hist, ptrs, buttons;
    int64_t when;
    float ax[NAX];
} ev_t;

static void ev_read(const AInputEvent *e, ev_t *v) {
    memset(v, 0, sizeof *v);
    v->type = AInputEvent_getType(e);
    v->source = AInputEvent_getSource(e);
    v->device = AInputEvent_getDeviceId(e);
    if (v->type == AINPUT_EVENT_TYPE_KEY) {
        v->action = AKeyEvent_getAction(e);
        v->code = AKeyEvent_getKeyCode(e);
        v->scan = AKeyEvent_getScanCode(e);
        v->flags = AKeyEvent_getFlags(e);
        v->meta = AKeyEvent_getMetaState(e);
        v->repeat = AKeyEvent_getRepeatCount(e);
        v->when = AKeyEvent_getEventTime(e);
    } else if (v->type == AINPUT_EVENT_TYPE_MOTION) {
        v->action = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK;
        v->hist = (int)AMotionEvent_getHistorySize(e);
        v->ptrs = (int)AMotionEvent_getPointerCount(e);
        v->when = AMotionEvent_getEventTime(e);
        v->meta = AMotionEvent_getMetaState(e);
        v->buttons = AMotionEvent_getButtonState(e);
        if (v->ptrs > 0)
            for (int i = 0; i < NAX; i++) v->ax[i] = AMotionEvent_getAxisValue(e, i, 0);
    }
}

static const char *motion_action(int a) {
    switch (a) {
    case AMOTION_EVENT_ACTION_DOWN: return "down";
    case AMOTION_EVENT_ACTION_UP: return "up";
    case AMOTION_EVENT_ACTION_MOVE: return "move";
    case AMOTION_EVENT_ACTION_CANCEL: return "cancel";
    case AMOTION_EVENT_ACTION_HOVER_MOVE: return "hover";
    case AMOTION_EVENT_ACTION_SCROLL: return "scroll";
    case AMOTION_EVENT_ACTION_BUTTON_PRESS: return "btn-press";
    case AMOTION_EVENT_ACTION_BUTTON_RELEASE: return "btn-release";
    default: return "other";
    }
}

static void ev_log(unsigned n, const char *via, const ev_t *v, const char *tail) {
    double age = v->when ? (double)(mono_ns() - v->when) * 1e-6 : -1.0;
    if (v->type == AINPUT_EVENT_TYPE_KEY) {
        printf("padlog: #%u %s key src=0x%08x dev=%d %s code=%d scan=%d flags=0x%08x meta=0x%x rep=%d age=%.1f | %s\n", n, via,
               (unsigned)v->source, v->device, v->action == AKEY_EVENT_ACTION_DOWN ? "down" : v->action == AKEY_EVENT_ACTION_UP ? "up" : "multi",
               v->code, v->scan, (unsigned)v->flags, (unsigned)v->meta, v->repeat, age, tail);
    } else if (v->type == AINPUT_EVENT_TYPE_MOTION) {
        char axes[400];
        int o = 0;
        for (int i = 0; i < NAX && o < (int)sizeof axes - 28; i++)
            if (i < 2 || v->ax[i] != 0.0f) o += snprintf(axes + o, sizeof axes - (size_t)o, " %s=%.3f", aaxis_name(i), (double)v->ax[i]);
        printf("padlog: #%u %s mot src=0x%08x dev=%d %s btn=0x%x hist=%d ptr=%d age=%.1f%s | %s\n", n, via, (unsigned)v->source, v->device,
               motion_action(v->action), (unsigned)v->buttons, v->hist, v->ptrs, age, axes, tail);
    } else {
        printf("padlog: #%u %s type=%d src=0x%08x dev=%d | %s\n", n, via, v->type, (unsigned)v->source, v->device, tail);
    }
}

/* ---- path 1: the native input queue (the glue's process_input with pre-dispatch skipped for pad keys) ---- */
static void q_process(struct android_app *app, struct android_poll_source *src) {
    (void)src;
    AInputEvent *event = NULL;
    while (AInputQueue_getEvent(app->inputQueue, &event) >= 0) {
        ev_t v;
        unsigned n = ++g_seq;
        ev_read(event, &v);
        if (v.type == AINPUT_EVENT_TYPE_KEY) g_keys++;
        else if (v.type == AINPUT_EVENT_TYPE_MOTION) g_motions++;
        else g_other++;
        int pre = 0;
        int skip = (g_on || g_log >= 2) && v.type == AINPUT_EVENT_TYPE_KEY && is_pad_key(v.source, v.code);
        if (!skip) pre = AInputQueue_preDispatchEvent(app->inputQueue, event) != 0;
        if (pre) {                                /* the queue has finished the event: do not touch it again */
            g_pre++;
            if (g_log) ev_log(n, "queue", &v, "pre=1");
            continue;
        }
        int handled = 0;
        if (app->onInputEvent) { g_cb++; handled = app->onInputEvent(app, event); }
        AInputQueue_finishEvent(app->inputQueue, event, handled);
        if (g_log) {
            char t[48];
            snprintf(t, sizeof t, "pre=0 cb=1 hnd=%d", handled);
            ev_log(n, "queue", &v, t);
        }
    }
}
static int32_t q_on_input(struct android_app *app, AInputEvent *e) {
    (void)app;
    int type = AInputEvent_getType(e), source = AInputEvent_getSource(e), dev = AInputEvent_getDeviceId(e);
    if (type == AINPUT_EVENT_TYPE_KEY)
        return pad_key(dev, source, AKeyEvent_getAction(e), AKeyEvent_getKeyCode(e), AKeyEvent_getScanCode(e), AKeyEvent_getRepeatCount(e));
    if (type == AINPUT_EVENT_TYPE_MOTION && AMotionEvent_getPointerCount(e) > 0) {
        float ax[NAX];
        for (int i = 0; i < NAX; i++) ax[i] = AMotionEvent_getAxisValue(e, i, 0);
        return pad_motion(dev, source, AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK, AMotionEvent_getButtonState(e), ax);
    }
    return 0;
}

/* ---- path 2: PadActivity (Java UI thread) ---- */
static jboolean JNICALL j_key(JNIEnv *env, jclass cls, jint dev, jint source, jint action, jint code, jint scan, jint flags,
                              jint meta, jint repeat, jlong t) {
    (void)env; (void)cls;
    g_jkeys++;
    int r = pad_key(dev, source, action, code, scan, repeat);
    if (g_log) {
        ev_t v;
        memset(&v, 0, sizeof v);
        v.type = AINPUT_EVENT_TYPE_KEY; v.source = source; v.device = dev; v.action = action; v.code = code; v.scan = scan;
        v.flags = flags; v.meta = meta; v.repeat = repeat; v.when = (int64_t)t;
        ev_log(++g_seq, "java", &v, r ? "hnd=1" : "hnd=0");
    }
    return r ? JNI_TRUE : JNI_FALSE;
}
static jboolean JNICALL j_motion(JNIEnv *env, jclass cls, jint dev, jint source, jint action, jint buttons, jfloatArray axes, jlong t) {
    (void)cls;
    float ax[NAX];
    memset(ax, 0, sizeof ax);
    jsize n = axes ? (*env)->GetArrayLength(env, axes) : 0;
    if (n > NAX) n = NAX;
    if (n > 0) (*env)->GetFloatArrayRegion(env, axes, 0, n, ax);
    g_jmotions++;
    int r = pad_motion(dev, source, action, buttons, ax);
    if (g_log) {
        ev_t v;
        memset(&v, 0, sizeof v);
        v.type = AINPUT_EVENT_TYPE_MOTION; v.source = source; v.device = dev; v.action = action; v.buttons = buttons; v.ptrs = 1;
        v.when = (int64_t)t;
        memcpy(v.ax, ax, sizeof ax);
        ev_log(++g_seq, "java", &v, r ? "hnd=1" : "hnd=0");
    }
    return r ? JNI_TRUE : JNI_FALSE;
}
static const char *kind_name(int k) { return k == K_PAD ? "gamepad" : k == K_JOY ? "joystick" : k == K_PADTOUCH ? "a Sony pad's touchpad" : "not used"; }
static void j_device(JNIEnv *env, jclass cls, jint dev, jstring name, jint vendor, jint product, jint sources, jfloatArray ranges,
                     jlong keys, jint vib, jboolean added) {
    (void)cls;
    char nm[96];
    const char *c = name ? (*env)->GetStringUTFChars(env, name, NULL) : NULL;
    snprintf(nm, sizeof nm, "%s", c && *c ? c : "Controller");
    if (c) (*env)->ReleaseStringUTFChars(env, name, c);
    float r[64 * 5];
    jsize nr = ranges ? (*env)->GetArrayLength(env, ranges) : 0;
    if (nr > (jsize)(sizeof r / sizeof r[0])) nr = (jsize)(sizeof r / sizeof r[0]);
    if (nr > 0) (*env)->GetFloatArrayRegion(env, ranges, 0, nr, r);
    pthread_mutex_lock(&dm);
    pdev *d = slot_of(dev);
    int fresh = !d;
    if (!d && nD < NDEV) { d = &D[nD]; memset(d, 0, sizeof *d); d->gp.slot = d->js.slot = nD; }
    if (!d) { pthread_mutex_unlock(&dm); printf("input: device %d \"%s\": more than %d devices this run: left out\n", (int)dev, nm, NDEV); return; }
    d->dev = dev;
    snprintf(d->name, sizeof d->name, "%s", nm);
    d->vid = vendor & 0xffff; d->pid = product & 0xffff; d->sources = sources; d->vib = vib; d->keymask = (uint64_t)keys;
    memset(d->rhas, 0, sizeof d->rhas);
    for (jsize i = 0; i + 4 < nr; i += 5) {
        int a = (int)r[i];
        if (a < 0 || a >= NAX) continue;
        if (d->rhas[a] && ((int)r[i + 4] & AINPUT_SOURCE_JOYSTICK) != AINPUT_SOURCE_JOYSTICK) continue;   /* (the joystick source's range first) */
        d->rhas[a] = 1; d->rmin[a] = r[i + 1]; d->rmax[a] = r[i + 2];
    }
    d->touchpad = d->vid == 0x054c && strcasestr(d->name, "touchpad") != NULL;
    d->info = 1;
    pad_profile(d);
    joy_layout(d);
    if (fresh) { ST(d->used, 1); ST(nD, nD + 1); }
    pthread_mutex_unlock(&dm);
    int k = kind_raw(d);
    char extra[300] = "";
    if (k == K_PAD)
        snprintf(extra, sizeof extra, ", profile %s (right stick %s/%s, triggers %s / %s), %d vibrator%s%s", d->prof == PROF_RXRY ? "rxry" : "standard",
                 aaxis_name(d->rx), aaxis_name(d->ry), d->prof == PROF_RXRY ? aaxis_name(d->lt) : "LTRIGGER+BRAKE",
                 d->prof == PROF_RXRY ? aaxis_name(d->rt) : "RTRIGGER+GAS", vib == 100 ? 1 : vib < 0 ? 0 : vib, vib == 1 ? "" : "s",
                 g_on ? "" : " (ACZ_QUEST_PAD=0: not used)");
    else if (k == K_JOY) {
        int o = snprintf(extra, sizeof extra, ", SDL axes [");
        for (int i = 0; i < d->jn && o < (int)sizeof extra - 40; i++) o += snprintf(extra + o, sizeof extra - (size_t)o, "%s%s", i ? " " : "", aaxis_name(d->jaxis[i]));
        snprintf(extra + o, sizeof extra - (size_t)o, "] buttons %d hats %d%s", d->nbtn, d->nhat, g_joy ? "" : " (ACZ_QUEST_JOY=0: not used)");
    }
    printf("input: device %d \"%s\" %04x:%04x sources 0x%08x%s: %s%s\n", (int)dev, d->name, (unsigned)d->vid, (unsigned)d->pid,
           (unsigned)sources, added ? " (plugged in)" : "", kind_name(k), extra);
    if (added && kind_of(d) == K_JOY) push_dev_event(SDL_EVENT_JOYSTICK_ADDED, id_of(d));
}
static void j_removed(JNIEnv *env, jclass cls, jint dev) {
    (void)env; (void)cls;
    pdev *d = slot_of(dev);
    if (!d) return;
    int k = kind_of(d);
    printf("input: device %d \"%s\" removed\n", (int)dev, d->name);
    if (k == K_PAD && LD(d->announced)) push_dev_event(SDL_EVENT_GAMEPAD_REMOVED, id_of(d));
    if (k == K_JOY) push_dev_event(SDL_EVENT_JOYSTICK_REMOVED, id_of(d));
    pad_zero(d);
    ST(d->gone, 1);
}
static void JNICALL j_focus(JNIEnv *env, jclass cls, jboolean focus) {
    (void)env; (void)cls;
    if (g_log) printf("padlog: java window focus %d\n", (int)focus);
    if (!focus) zero_all("the window lost focus");
}
static void JNICALL j_display(JNIEnv *env, jclass cls, jint what, jint id, jstring name, jint flags, jint state, jint w, jint h, jfloat hz) {
    (void)cls;
    const char *c = name ? (*env)->GetStringUTFChars(env, name, NULL) : NULL;
    ps2_display_android_event(what, id, c, flags, state, w, h, hz);
    if (c) (*env)->ReleaseStringUTFChars(env, name, c);
}
/* mode: PadActivity.sMode (0: the pad path stays off); displays: start the display listener (ACZ_QUEST_CAPTURE_WATCH) */
static void java_hook(struct android_app *app, int mode, int displays) {
    JavaVM *vm = app->activity->vm;
    JNIEnv *env = NULL;
    if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK || !env) { printf("pad: java: no JNI env\n"); return; }
    jclass c = (*env)->GetObjectClass(env, app->activity->clazz);
    jfieldID f = c ? (*env)->GetStaticFieldID(env, c, "sMode", "I") : NULL;
    if (!f) {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        printf("pad: java: the activity is not PadActivity (no sMode): only the native queue path\n");
    } else {
        static const JNINativeMethod m[] = {
            { "nKey", "(IIIIIIIIJ)Z", (void *)j_key },
            { "nMotion", "(IIII[FJ)Z", (void *)j_motion },
            { "nDevice", "(ILjava/lang/String;III[FJIZ)V", (void *)j_device },
            { "nRemoved", "(I)V", (void *)j_removed },
            { "nFocus", "(Z)V", (void *)j_focus },
            { "nDisplay", "(IILjava/lang/String;IIIIF)V", (void *)j_display },
        };
        if ((*env)->RegisterNatives(env, c, m, (jint)(sizeof m / sizeof m[0])) != JNI_OK) {
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
            printf("pad: java: RegisterNatives failed: only the native queue path\n");
        } else {
            if (mode) {
                jmethodID r = (*env)->GetStaticMethodID(env, c, "rumble", "(IIII)I");
                if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); r = NULL; }
                if (r) { g_vm = vm; g_cls = (jclass)(*env)->NewGlobalRef(env, c); g_rumble_m = r; }
                else printf("pad: java: no PadActivity.rumble: no rumble\n");
                (*env)->SetStaticIntField(env, c, f, mode);
                g_java = 1;
                printf("pad: java: PadActivity forwards %s\n", mode & 2 ? "every key and motion event (log)" : "gamepad, joystick and Sony pad events");
                jmethodID sd = (*env)->GetStaticMethodID(env, c, "startDevices", "()V");
                if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); sd = NULL; }
                if (sd) (*env)->CallStaticVoidMethod(env, c, sd);
                if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); printf("pad: java: startDevices failed\n"); }
            }
            if (displays) {
                jmethodID sd = (*env)->GetStaticMethodID(env, c, "startDisplays", "()V");
                if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); sd = NULL; }
                if (sd) (*env)->CallStaticVoidMethod(env, c, sd);
                if (!sd || (*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); printf("capture: the display listener did not start\n"); }
                else ps2_display_android_started();
            }
        }
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*vm)->DetachCurrentThread(vm);
}

/* ---- the devices Android lists (java.view.InputDevice), printed when the list changes (the probe) ---- */
static uint32_t g_census_sig;
static int g_census_done;

static void census(struct android_app *app) {
    JavaVM *vm = app->activity->vm;
    JNIEnv *env = NULL;
    if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK || !env) { printf("padlog: census: no JNI env\n"); return; }
    jclass cls = (*env)->FindClass(env, "android/view/InputDevice");
    jmethodID get_ids = cls ? (*env)->GetStaticMethodID(env, cls, "getDeviceIds", "()[I") : NULL;
    jmethodID get_dev = cls ? (*env)->GetStaticMethodID(env, cls, "getDevice", "(I)Landroid/view/InputDevice;") : NULL;
    jmethodID get_name = cls ? (*env)->GetMethodID(env, cls, "getName", "()Ljava/lang/String;") : NULL;
    jmethodID get_src = cls ? (*env)->GetMethodID(env, cls, "getSources", "()I") : NULL;
    jmethodID get_vid = cls ? (*env)->GetMethodID(env, cls, "getVendorId", "()I") : NULL;
    jmethodID get_pid = cls ? (*env)->GetMethodID(env, cls, "getProductId", "()I") : NULL;
    jmethodID get_ctl = cls ? (*env)->GetMethodID(env, cls, "getControllerNumber", "()I") : NULL;
    jmethodID is_virt = cls ? (*env)->GetMethodID(env, cls, "isVirtual", "()Z") : NULL;
    jmethodID is_ext = cls ? (*env)->GetMethodID(env, cls, "isExternal", "()Z") : NULL;
    jmethodID get_rng = cls ? (*env)->GetMethodID(env, cls, "getMotionRanges", "()Ljava/util/List;") : NULL;
    jclass lc = (*env)->FindClass(env, "java/util/List");
    jmethodID l_size = lc ? (*env)->GetMethodID(env, lc, "size", "()I") : NULL;
    jmethodID l_get = lc ? (*env)->GetMethodID(env, lc, "get", "(I)Ljava/lang/Object;") : NULL;
    jclass rc = (*env)->FindClass(env, "android/view/InputDevice$MotionRange");
    jmethodID r_axis = rc ? (*env)->GetMethodID(env, rc, "getAxis", "()I") : NULL;
    jmethodID r_src = rc ? (*env)->GetMethodID(env, rc, "getSource", "()I") : NULL;
    jmethodID r_min = rc ? (*env)->GetMethodID(env, rc, "getMin", "()F") : NULL;
    jmethodID r_max = rc ? (*env)->GetMethodID(env, rc, "getMax", "()F") : NULL;
    jmethodID r_flat = rc ? (*env)->GetMethodID(env, rc, "getFlat", "()F") : NULL;
    jmethodID r_fuzz = rc ? (*env)->GetMethodID(env, rc, "getFuzz", "()F") : NULL;
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (!get_ids || !get_dev || !get_name || !get_src || !get_vid || !get_pid || !get_ctl || !is_virt || !is_ext || !get_rng
        || !l_size || !l_get || !r_axis || !r_src || !r_min || !r_max || !r_flat || !r_fuzz) {
        printf("padlog: census: the InputDevice methods are not there\n");
        (*vm)->DetachCurrentThread(vm);
        return;
    }
    jintArray ids = (jintArray)(*env)->CallStaticObjectMethod(env, cls, get_ids);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    jsize nid = ids ? (*env)->GetArrayLength(env, ids) : 0;
    jint id[64];
    if (nid > 64) nid = 64;
    if (ids) (*env)->GetIntArrayRegion(env, ids, 0, nid, id);
    uint32_t sig = 2166136261u;
    for (jsize i = 0; i < nid; i++) {
        jobject d = (*env)->CallStaticObjectMethod(env, cls, get_dev, id[i]);
        jint s = d ? (*env)->CallIntMethod(env, d, get_src) : 0;
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        sig = (sig ^ (uint32_t)id[i]) * 16777619u;
        sig = (sig ^ (uint32_t)s) * 16777619u;
        if (d) (*env)->DeleteLocalRef(env, d);
    }
    if (g_census_done && sig == g_census_sig) { (*vm)->DetachCurrentThread(vm); return; }
    g_census_done = 1;
    g_census_sig = sig;
    printf("padlog: census: %d input devices\n", (int)nid);
    for (jsize i = 0; i < nid; i++) {
        if ((*env)->PushLocalFrame(env, 256) != JNI_OK) { (*env)->ExceptionClear(env); break; }
        jobject d = (*env)->CallStaticObjectMethod(env, cls, get_dev, id[i]);
        if (d) {
            jstring ns = (jstring)(*env)->CallObjectMethod(env, d, get_name);
            const char *cs = ns ? (*env)->GetStringUTFChars(env, ns, NULL) : NULL;
            int src = (*env)->CallIntMethod(env, d, get_src);
            printf("padlog: census: id=%d \"%s\" sources=0x%08x vendor=%04x product=%04x controller=%d virtual=%d external=%d\n", (int)id[i],
                   cs ? cs : "?", (unsigned)src, (unsigned)(*env)->CallIntMethod(env, d, get_vid), (unsigned)(*env)->CallIntMethod(env, d, get_pid),
                   (int)(*env)->CallIntMethod(env, d, get_ctl), (int)(*env)->CallBooleanMethod(env, d, is_virt),
                   (int)(*env)->CallBooleanMethod(env, d, is_ext));
            if (cs) (*env)->ReleaseStringUTFChars(env, ns, cs);
            if (src_is(src, AINPUT_SOURCE_JOYSTICK) || src_is(src, AINPUT_SOURCE_GAMEPAD)) {
                jobject list = (*env)->CallObjectMethod(env, d, get_rng);
                int cnt = list ? (*env)->CallIntMethod(env, list, l_size) : 0;
                for (int k = 0; k < cnt && k < 40; k++) {
                    jobject r = (*env)->CallObjectMethod(env, list, l_get, k);
                    if (!r) continue;
                    printf("padlog: census:   axis %d src=0x%08x min=%.3f max=%.3f flat=%.3f fuzz=%.3f\n", (int)(*env)->CallIntMethod(env, r, r_axis),
                           (unsigned)(*env)->CallIntMethod(env, r, r_src), (double)(*env)->CallFloatMethod(env, r, r_min),
                           (double)(*env)->CallFloatMethod(env, r, r_max), (double)(*env)->CallFloatMethod(env, r, r_flat),
                           (double)(*env)->CallFloatMethod(env, r, r_fuzz));
                    (*env)->DeleteLocalRef(env, r);
                }
            }
        }
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        (*env)->PopLocalFrame(env, NULL);
    }
    (*vm)->DetachCurrentThread(vm);
}

/* ---- the hooks in ps2_android.c (the glue's thread) ---- */
void ps2_pad_android_install(struct android_app *app) {
    const char *e = getenv("ACZ_QUEST_PAD");
    g_on = e && *e ? atoi(e) > 0 : 0;
    e = getenv("ACZ_QUEST_JOY");
    g_joy = e && *e ? atoi(e) > 0 : 0;
    e = getenv("ACZ_HOTAS");
    if (e && *e == '0') g_joy = 0;               /* (the PC's switch: no joystick is opened) */
    e = getenv("ACZ_QUEST_PAD_LOG");
    g_log = e && *e ? atoi(e) : 0;
    if (g_log < 0) g_log = 0;
    e = getenv("ACZ_QUEST_PAD_RUMBLE");
    if (e && *e) g_rumble = atoi(e) > 0;
    e = getenv("ACZ_QUEST_PAD_PROFILE");
    if (e && *e) g_prof = !strcasecmp(e, "rxry") ? PROF_RXRY : !strcasecmp(e, "standard") ? PROF_STD : -1;
    e = getenv("ACZ_QUEST_PAD_DEADZONE");
    if (e && *e) { float d = strtof(e, NULL); g_dz = d < 0.0f ? 0.0f : d > 0.9f ? 0.9f : d; }
    int watch = ps2_display_android_watch();     /* (ACZ_QUEST_CAPTURE_WATCH, ps2_display_android.c: R-37) */
    if (!g_on && !g_joy && !g_log) {
        if (watch) java_hook(app, 0, 1);
        return;
    }
    app->onInputEvent = q_on_input;
    app->inputPollSource.process = q_process;   /* (the glue reads the pointer on every wake-up) */
    printf("pad: ACZ_QUEST_PAD=%d (dead zone %.2f, profile %s, rumble %d), ACZ_QUEST_JOY=%d, ACZ_QUEST_PAD_LOG=%d; native queue %p%s\n",
           g_on, (double)g_dz, g_prof < 0 ? "auto" : g_prof == PROF_RXRY ? "rxry" : "standard", g_rumble, g_joy, g_log,
           (void *)app->inputQueue, g_on || g_log >= 2 ? ", pad keys skip pre-dispatch" : "");
    java_hook(app, 1 | (g_log ? 2 : 0), watch);
    if (g_log) census(app);
}

void ps2_pad_android_cmd(struct android_app *app, int cmd) {
    if (!g_on && !g_joy && !g_log) return;
    const char *n = NULL;
    switch (cmd) {
    case APP_CMD_INPUT_CHANGED: n = "input-changed"; break;
    case APP_CMD_INIT_WINDOW: n = "init-window"; break;
    case APP_CMD_TERM_WINDOW: n = "term-window"; break;
    case APP_CMD_GAINED_FOCUS: n = "gained-focus"; g_focus = 1; break;
    case APP_CMD_LOST_FOCUS: n = "lost-focus"; g_focus = 0; zero_all("the activity lost focus"); break;
    case APP_CMD_PAUSE: n = "pause"; zero_all("the activity paused"); break;
    case APP_CMD_RESUME: n = "resume"; break;
    case APP_CMD_START: n = "start"; break;
    case APP_CMD_STOP: n = "stop"; break;
    default: return;
    }
    if (g_log) printf("padlog: cmd %s (queue %p, window %p)\n", n, (void *)app->inputQueue, (void *)app->window);
}

void ps2_pad_android_tick(struct android_app *app) {
    if (!g_log) return;
    static int64_t next_census, next_beat;
    int64_t now = mono_ns();
    if (now >= next_census) {
        next_census = now + 2000000000ll;
        census(app);
    }
    if (now >= next_beat) {
        next_beat = now + 5000000000ll;
        printf("padlog: alive: focus=%d queue=%p events=%u queue keys=%u motions=%u other=%u pre-dispatched=%u to-onInputEvent=%u;"
               " java=%d keys=%u motions=%u; devices %d\n", g_focus, (void *)app->inputQueue, g_seq, g_keys, g_motions, g_other, g_pre,
               g_cb, g_java, g_jkeys, g_jmotions, LD(nD));
        int n = LD(nD);
        for (int i = 0; i < n; i++) {
            pdev *d = &D[i];
            int k = kind_of(d);
            if (k == K_PAD)
                printf("padlog:   pad %d \"%s\" announced=%d buttons=0x%x LX=%d LY=%d RX=%d RY=%d LT=%d RT=%d\n", d->dev, d->name,
                       LD(d->announced), (unsigned)(LD(d->keys) | LD(d->hat)), LD(d->ax[0]), LD(d->ax[1]), LD(d->ax[2]), LD(d->ax[3]),
                       LD(d->ax[4]), LD(d->ax[5]));
            else if (k == K_JOY)
                printf("padlog:   joystick %d \"%s\" listed=%d buttons=0x%x%08x hat=%u axes %d: %d %d %d %d %d %d\n", d->dev, d->name,
                       d->jlisted, (unsigned)LD(d->jb[1]), (unsigned)LD(d->jb[0]), (unsigned)LD(d->jhat), d->jn, LD(d->jv[0]),
                       LD(d->jv[1]), LD(d->jv[2]), LD(d->jv[3]), LD(d->jv[4]), LD(d->jv[5]));
        }
    }
}
