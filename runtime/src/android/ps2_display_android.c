/* ps2_display_android.c -- the displays Android lists, for the GPU hang report (BUGS_RQ R-37; Android-only file: the PC
   build's objects do not change).

   R-37: any compositor capture mirror beside the game (adb shell screenrecord, Meta's own recorder) hangs our GPU context
   0.4-2 s after it starts; the hardware encoder alone does not. PadActivity's DisplayListener reports every display
   Android lists at the start and every one added, removed or changed later, with its flags, state (Display.getState())
   and mode (size, refresh rate) (ACZ_QUEST_CAPTURE_WATCH, default 1; 0 = no listener). One log line per event; a change
   that repeats the display's last report exactly is dropped:
     capture: display there at the start id 3 "MirrorRoot for <uuid>" flags 0x03 (protected-buffers secure) state OFF 1x1 60.0 Hz
     capture: display changed id 3 "MirrorRoot for <uuid>" flags 0x03 (...) state ON 1920x1080 72.0 Hz (was OFF 1x1 60.0 Hz)
     capture: display added id 12 "name" flags 0x0c (private presentation) state ON 1280x720 60.0 Hz
   Device run 2026-11-10 00:48 (Meta's recorder started mid-flight in M3): five "MirrorRoot for <uuid>" displays (flags
   0x03) are there from the start, and the recorder added none: it takes over one of them. So for each display the last
   few state / size changes are kept with their time, and ps2_video.c's gpu_hang_fire puts into crash.txt
   (ps2_android_capture_note) whichever came last: the MirrorRoot display (else any other display but the headset's own)
   whose state or size changed most recently,
     capture: "MirrorRoot for <uuid>" changed to ON 1.9 s before the hang (display 3, 1920x1080 72.0 Hz; was OFF 1x1)
   or the display added most recently,
     capture: "name" (1.4 s before the hang; display 12, flags 0x0c private presentation, still there)
   or "capture: none seen" with the displays there at the start and their states.
   Not seen: `adb shell screenrecord` makes its display inside SurfaceFlinger, not through DisplayManager, so it may never
   show up here; a display another app made private may come without a name or state. */
#include "ps2_android.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_watch = -1;                         /* ACZ_QUEST_CAPTURE_WATCH */
static int g_started;                            /* the listener was registered */
static unsigned disp_logged;                     /* (log lines written: tools/quest/pad_test.c counts the dropped repeats) */
#define ND 16                                    /* displays followed */
#define NH 4                                     /* state / size changes kept per display */
typedef struct { int64_t t; int state, w, h; } dchange;
static struct {
    int used, id, flags, state, w, h, at_start, added, gone;
    float hz;
    int64_t t_added, t_removed;
    char name[96];
    dchange ch[NH];                              /* the last state / size changes, ch[(nch - 1) % NH] the newest */
    unsigned nch;
    int prev_state, prev_w, prev_h;              /* before the newest change */
} DS[ND];
static pthread_mutex_t disp_m = PTHREAD_MUTEX_INITIALIZER;

static int64_t disp_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000ll + (int64_t)t.tv_nsec;
}

int ps2_display_android_watch(void) {
    if (g_watch < 0) {
        const char *e = getenv("ACZ_QUEST_CAPTURE_WATCH");
        g_watch = e && *e ? atoi(e) > 0 : 1;
    }
    return g_watch;
}
void ps2_display_android_started(void) { __atomic_store_n(&g_started, 1, __ATOMIC_RELEASE); }

/* android.view.Display flags and states */
static void flag_text(int f, char *out, size_t n) {
    static const char *const names[] = { "protected-buffers", "secure", "private", "presentation", "round" };
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < 5; i++)
        if (f >> i & 1) o += (size_t)snprintf(out + o, n > o ? n - o : 0, "%s%s", o ? " " : "", names[i]);
    if (f >> 5) snprintf(out + o, n > o ? n - o : 0, "%sother 0x%x", o ? " " : "", (unsigned)(f >> 5) << 5);
}
static const char *state_name(int s) {
    switch (s) {
    case 0: return "UNKNOWN";
    case 1: return "OFF";
    case 2: return "ON";
    case 3: return "DOZE";
    case 4: return "DOZE_SUSPEND";
    case 5: return "VR";
    case 6: return "ON_SUSPEND";
    case -1: return "?";
    default: return "other";
    }
}
static int is_mirror(const char *name) { return !strncmp(name, "MirrorRoot", 10); }
static int ds_slot(int id, int make) {
    int free = -1;
    for (int i = 0; i < ND; i++) {
        if (DS[i].used && DS[i].id == id && !DS[i].gone) return i;
        if (!DS[i].used && free < 0) free = i;
    }
    if (!make) return -1;
    if (free < 0)                                    /* (full: reuse a removed one) */
        for (int i = 0; i < ND; i++) if (DS[i].gone) { free = i; break; }
    if (free < 0) return -1;
    memset(&DS[free], 0, sizeof DS[free]);
    DS[free].used = 1;
    DS[free].id = id;
    DS[free].state = -1;
    return free;
}

void ps2_display_android_event(int what, int id, const char *name, int flags, int state, int w, int h, float hz) {
    char fl[96], line[400];
    int64_t now = disp_ns();
    line[0] = 0;
    pthread_mutex_lock(&disp_m);
    int i = ds_slot(id, what != 2);
    if (what == 2) {
        if (i >= 0) { DS[i].gone = 1; DS[i].t_removed = now; }
        snprintf(line, sizeof line, "capture: display removed id %d%s%s%s", id, i >= 0 && DS[i].name[0] ? " \"" : "", i >= 0 ? DS[i].name : "",
                 i >= 0 && DS[i].name[0] ? "\"" : "");
    } else if (i >= 0) {
        int fresh = what != 3 || (DS[i].state == -1 && !DS[i].name[0] && !DS[i].w);
        int same = !fresh && DS[i].flags == flags && DS[i].state == state && DS[i].w == w && DS[i].h == h && DS[i].hz == hz
                   && (!name || !strcmp(DS[i].name, name));
        if (!same) {
            if (what == 3 && !fresh && (DS[i].state != state || DS[i].w != w || DS[i].h != h)) {
                DS[i].prev_state = DS[i].state; DS[i].prev_w = DS[i].w; DS[i].prev_h = DS[i].h;
                dchange *c = &DS[i].ch[DS[i].nch++ % NH];
                c->t = now; c->state = state; c->w = w; c->h = h;
            }
            char was[96] = "";
            if (what == 3 && !fresh)
                snprintf(was, sizeof was, " (was %s %dx%d %.1f Hz)", state_name(DS[i].state), DS[i].w, DS[i].h, (double)DS[i].hz);
            if (name) snprintf(DS[i].name, sizeof DS[i].name, "%s", name);
            DS[i].flags = flags; DS[i].state = state; DS[i].w = w; DS[i].h = h; DS[i].hz = hz;
            if (what == 0) DS[i].at_start = 1;
            if (what == 1) { DS[i].added = 1; DS[i].t_added = now; }
            flag_text(flags, fl, sizeof fl);
            snprintf(line, sizeof line, "capture: display %s id %d \"%s\" flags 0x%02x%s%s%s state %s %dx%d %.1f Hz%s",
                     what == 0 ? "there at the start" : what == 1 ? "added" : "changed", id,
                     DS[i].name[0] ? DS[i].name : "(no name: another app's private display)", (unsigned)flags, fl[0] ? " (" : "", fl,
                     fl[0] ? ")" : "", state_name(state), w, h, (double)hz, was);
        }
    }
    pthread_mutex_unlock(&disp_m);
    if (line[0]) { printf("%s\n", line); disp_logged++; }
}

int ps2_android_capture_note(char *out, int cap) {
    if (!out || cap <= 0) return 0;
    out[0] = 0;
    if (!ps2_display_android_watch()) return snprintf(out, (size_t)cap, "capture: not watched (ACZ_QUEST_CAPTURE_WATCH=0)");
    if (!__atomic_load_n(&g_started, __ATOMIC_ACQUIRE))
        return snprintf(out, (size_t)cap, "capture: not watched (the display listener did not start)");
    int64_t now = disp_ns();
    int o = 0;
    pthread_mutex_lock(&disp_m);
    int ci = -1, cm = 0, ai = -1;                    /* the newest change (a MirrorRoot display first), the newest added */
    int64_t ct = 0, at = 0;
    for (int i = 0; i < ND; i++) {
        if (!DS[i].used || DS[i].id == 0) continue;    /* (the headset's own display) */
        if (DS[i].nch) {
            int m = is_mirror(DS[i].name);
            int64_t t = DS[i].ch[(DS[i].nch - 1u) % NH].t;
            if (ci < 0 || (m && !cm) || (m == cm && t > ct)) { ci = i; ct = t; cm = m; }
        }
        if (DS[i].added && (ai < 0 || DS[i].t_added > at)) { ai = i; at = DS[i].t_added; }
    }
    if (ci >= 0 && (ai < 0 || ct >= at || cm)) {
        const dchange *c = &DS[ci].ch[(DS[ci].nch - 1u) % NH];
        o = snprintf(out, (size_t)cap, "capture: \"%s\" changed to %s %.1f s before the hang (display %d, %dx%d %.1f Hz; was %s %dx%d; %u change%s",
                     DS[ci].name[0] ? DS[ci].name : "?", state_name(c->state), (double)(now - c->t) * 1e-9, DS[ci].id, c->w, c->h,
                     (double)DS[ci].hz, state_name(DS[ci].prev_state), DS[ci].prev_w, DS[ci].prev_h, DS[ci].nch, DS[ci].nch == 1 ? "" : "s");
        if (o < cap) o += snprintf(out + o, (size_t)(cap - o), "%s)", DS[ci].gone ? ", removed since" : "");
        if (ai >= 0 && o < cap)
            o += snprintf(out + o, (size_t)(cap - o), "; added: \"%s\" %.1f s before the hang", DS[ai].name[0] ? DS[ai].name : "?", (double)(now - at) * 1e-9);
    } else if (ai >= 0) {
        char fl[96];
        flag_text(DS[ai].flags, fl, sizeof fl);
        o = snprintf(out, (size_t)cap, "capture: \"%s\" (%.1f s before the hang; display %d, flags 0x%02x%s%s, state %s, ", DS[ai].name[0] ? DS[ai].name : "?",
                     (double)(now - at) * 1e-9, DS[ai].id, (unsigned)DS[ai].flags, fl[0] ? " " : "", fl, state_name(DS[ai].state));
        if (o < cap && DS[ai].gone) o += snprintf(out + o, (size_t)(cap - o), "removed %.1f s before the hang)", (double)(now - DS[ai].t_removed) * 1e-9);
        else if (o < cap) o += snprintf(out + o, (size_t)(cap - o), "still there)");
    } else {
        o = snprintf(out, (size_t)cap, "capture: none seen");
        int extra = 0;
        for (int i = 0; i < ND && o < cap; i++) {
            if (!DS[i].used || !DS[i].at_start || DS[i].id == 0) continue;
            o += snprintf(out + o, (size_t)(cap - o), "%s %d%s %s", extra++ ? "," : " (displays there at the start, no change seen:",
                          DS[i].id, is_mirror(DS[i].name) ? " MirrorRoot" : "", state_name(DS[i].state));
        }
        if (extra && o < cap) o += snprintf(out + o, (size_t)(cap - o), "; names in the log's capture: lines)");
    }
    pthread_mutex_unlock(&disp_m);
    return o < cap ? o : cap - 1;
}
