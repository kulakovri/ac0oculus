/* ps2_sdl_android.c -- the SDL3 subset the runtime calls, over the NDK, for the Quest standalone build (no SDL library,
   no Java: NOTES_QUEST.md stage 2). The declarations are SDL 3.4's own headers (copied into the build folder at
   configure time), so every type and struct layout is the real one; only the functions the runtime's C files call are
   here (the link's --no-undefined is the inventory).
   Real: time, delays, strings, spinlocks, files and directories (POSIX), PNG reading (miniz inflate), the audio
   stream (AAudio, the same pull callback as SDL's: the callback asks for bytes and puts them with
   SDL_PutAudioStreamData), the event queue (SDL_PushEvent / SDL_PollEvent: nothing else produces events).
   "No device": windows (one placeholder handle with a size, no surface), displays, keyboard, haptics, virtual
   joysticks (PS2_PAD_SDL), file dialogs, PNG writing. The headset's controllers come through the OpenXR action path
   (ps2_touch.c), which needs none of this; the gamepad and joystick calls are in ps2_pad_android.c (ACZ_QUEST_PAD,
   ACZ_QUEST_JOY: Android input events behind SDL's gamepad and joystick API). Hints are kept (SDL_SetHint /
   SDL_GetHint: the profiles' flight-stick and throttle lists). */
#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <aaudio/AAudio.h>
#include <android/log.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "ps2_android.h"
#include "ps2_png.h"

/* ---- errors, hints, init ---- */
static __thread char err_buf[256];
static void set_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err_buf, sizeof err_buf, fmt, ap);
    va_end(ap);
}
const char *SDL_GetError(void) { return err_buf; }
void ps2_sdl_android_error(const char *s) { set_err("%s", s ? s : ""); }   /* (ps2_pad_android.c) */
/* hints: kept by name (ps2_hotas.c's flight-stick / throttle lists are read back by ps2_pad_android.c) */
static struct { char name[64]; char *value; } hints[16];
static pthread_mutex_t hint_m = PTHREAD_MUTEX_INITIALIZER;
bool SDL_SetHint(const char *name, const char *value) {
    if (!name) return false;
    pthread_mutex_lock(&hint_m);
    int i = 0;
    while (i < 16 && hints[i].name[0] && strcmp(hints[i].name, name)) i++;
    if (i < 16) {
        if (!hints[i].name[0]) snprintf(hints[i].name, sizeof hints[i].name, "%s", name);
        char *old = hints[i].value;
        hints[i].value = value ? strdup(value) : NULL;
        (void)old;                               /* (never freed: a reader may still hold it; a few short strings a run) */
    }
    pthread_mutex_unlock(&hint_m);
    return i < 16;
}
const char *SDL_GetHint(const char *name) {
    const char *v = NULL;
    if (!name) return NULL;
    pthread_mutex_lock(&hint_m);
    for (int i = 0; i < 16 && hints[i].name[0]; i++) if (!strcmp(hints[i].name, name)) { v = hints[i].value; break; }
    pthread_mutex_unlock(&hint_m);
    return v;
}
bool SDL_Init(SDL_InitFlags flags) { (void)flags; return true; }
bool SDL_InitSubSystem(SDL_InitFlags flags) { (void)flags; return true; }

/* ---- time ---- */
static Uint64 mono_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (Uint64)t.tv_sec * 1000000000ull + (Uint64)t.tv_nsec;
}
static Uint64 t_start;
static void t_init(void) { if (!t_start) t_start = mono_ns(); }
Uint64 SDL_GetTicksNS(void) { t_init(); return mono_ns() - t_start; }
Uint64 SDL_GetTicks(void) { return SDL_GetTicksNS() / 1000000ull; }
Uint64 SDL_GetPerformanceCounter(void) { return mono_ns(); }
Uint64 SDL_GetPerformanceFrequency(void) { return 1000000000ull; }
void SDL_Delay(Uint32 ms) {
    struct timespec t = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {}
}
SDL_ThreadID SDL_GetCurrentThreadID(void) { return (SDL_ThreadID)syscall(SYS_gettid); }

/* ---- strings, memory ---- */
void SDL_free(void *mem) { free(mem); }
char *SDL_strdup(const char *str) { return str ? strdup(str) : NULL; }
int SDL_strcasecmp(const char *a, const char *b) { return strcasecmp(a, b); }
int SDL_strncasecmp(const char *a, const char *b, size_t n) { return strncasecmp(a, b, n); }
char *SDL_strcasestr(const char *h, const char *n) { return strcasestr(h, n); }
char *SDL_strtok_r(char *s, const char *d, char **save) { return strtok_r(s, d, save); }
int SDL_toupper(int x) { return toupper(x); }

/* ---- spinlocks ---- */
void SDL_LockSpinlock(SDL_SpinLock *lock) {
    unsigned spins = 0;
    while (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(lock, __ATOMIC_RELAXED)) {
            if (++spins & 63u) __asm__ volatile("yield");
            else sched_yield();
        }
    }
}
void SDL_UnlockSpinlock(SDL_SpinLock *lock) { __atomic_store_n(lock, 0, __ATOMIC_RELEASE); }

/* ---- files ---- */
const char *SDL_GetBasePath(void) {
    static char base[600];
    if (!base[0]) {
        const char *f = ps2_android_files_dir();
        snprintf(base, sizeof base, "%s/", f && *f ? f : ".");
    }
    return base;
}
void *SDL_LoadFile(const char *file, size_t *datasize) {
    FILE *f = fopen(file, "rb");
    if (datasize) *datasize = 0;
    if (!f) { set_err("cannot open %s (%d)", file, errno); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *p = n >= 0 ? (char *)malloc((size_t)n + 1) : NULL;
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); p = NULL; }
    fclose(f);
    if (!p) { set_err("cannot read %s", file); return NULL; }
    p[n] = 0;
    if (datasize) *datasize = (size_t)n;
    return p;
}
static SDL_Time ts_ns(struct timespec t) { return (SDL_Time)t.tv_sec * 1000000000ll + (SDL_Time)t.tv_nsec; }
bool SDL_GetPathInfo(const char *path, SDL_PathInfo *info) {
    struct stat st;
    if (info) memset(info, 0, sizeof *info);
    if (stat(path, &st) != 0) { set_err("no %s", path); return false; }
    if (info) {
        info->type = S_ISREG(st.st_mode) ? SDL_PATHTYPE_FILE : S_ISDIR(st.st_mode) ? SDL_PATHTYPE_DIRECTORY : SDL_PATHTYPE_OTHER;
        info->size = (Uint64)st.st_size;
        info->create_time = ts_ns(st.st_ctim);
        info->modify_time = ts_ns(st.st_mtim);
        info->access_time = ts_ns(st.st_atim);
    }
    return true;
}
bool SDL_CreateDirectory(const char *path) {
    char b[1024];
    snprintf(b, sizeof b, "%s", path);
    for (char *s = b + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(b, 0775); *s = '/'; }
    if (mkdir(b, 0775) == 0 || errno == EEXIST) return true;
    set_err("mkdir %s (%d)", path, errno);
    return false;
}
bool SDL_EnumerateDirectory(const char *path, SDL_EnumerateDirectoryCallback callback, void *userdata) {
    DIR *d = opendir(path);
    if (!d) { set_err("cannot open folder %s", path); return false; }
    char dirname[1024];
    size_t l = strlen(path);
    snprintf(dirname, sizeof dirname, "%s%s", path, l && path[l - 1] == '/' ? "" : "/");   /* (SDL: with a slash) */
    struct dirent *e;
    bool ok = true;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        SDL_EnumerationResult r = callback(userdata, dirname, e->d_name);
        if (r == SDL_ENUM_FAILURE) { ok = false; break; }
        if (r == SDL_ENUM_SUCCESS) break;
    }
    closedir(d);
    return ok;
}
/* SDL_GlobDirectory: names relative to path, recursive like SDL's; the array and its strings are one allocation
   (callers free it with one SDL_free) */
typedef struct { char **names; size_t n, cap, bytes; } glob_list;
static void glob_walk(const char *root, const char *rel, const char *pattern, int fold, glob_list *g, int depth) {
    char dir[1200];
    snprintf(dir, sizeof dir, "%s%s%s", root, *rel ? "/" : "", rel);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[1200];
        snprintf(sub, sizeof sub, "%s%s%s", rel, *rel ? "/" : "", e->d_name);
        if (!pattern || fnmatch(pattern, sub, fold ? FNM_CASEFOLD : 0) == 0) {
            if (g->n == g->cap) {
                g->cap = g->cap ? g->cap * 2 : 64;
                g->names = (char **)realloc(g->names, g->cap * sizeof *g->names);
            }
            g->names[g->n++] = strdup(sub);
            g->bytes += strlen(sub) + 1;
        }
        if (depth < 16) {
            char full[1300];
            struct stat st;
            snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
            if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) glob_walk(root, sub, pattern, fold, g, depth + 1);
        }
    }
    closedir(d);
}
char **SDL_GlobDirectory(const char *path, const char *pattern, SDL_GlobFlags flags, int *count) {
    glob_list g = { 0 };
    char root[1024];
    snprintf(root, sizeof root, "%s", path);
    size_t l = strlen(root);
    while (l > 1 && root[l - 1] == '/') root[--l] = 0;
    glob_walk(root, "", pattern, (flags & SDL_GLOB_CASEINSENSITIVE) != 0, &g, 0);
    char **out = (char **)malloc((g.n + 1) * sizeof(char *) + g.bytes);
    if (out) {
        char *s = (char *)(out + g.n + 1);
        for (size_t i = 0; i < g.n; i++) {
            size_t k = strlen(g.names[i]) + 1;
            memcpy(s, g.names[i], k);
            out[i] = s;
            s += k;
        }
        out[g.n] = NULL;
    }
    for (size_t i = 0; i < g.n; i++) free(g.names[i]);
    free(g.names);
    if (count) *count = out ? (int)g.n : 0;
    return out;
}

/* ---- surfaces and PNG ---- */
static SDL_Surface *surf_new(int w, int h, SDL_PixelFormat fmt, void *pixels, int pitch, int own) {
    SDL_Surface *s = (SDL_Surface *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->format = fmt;
    s->w = w;
    s->h = h;
    s->pitch = pitch;
    s->refcount = 1;
    if (!own) { s->pixels = pixels; s->flags = SDL_SURFACE_PREALLOCATED; }
    else s->pixels = pixels;
    return s;
}
SDL_Surface *SDL_CreateSurfaceFrom(int width, int height, SDL_PixelFormat format, void *pixels, int pitch) {
    return surf_new(width, height, format, pixels, pitch, 0);
}
void SDL_DestroySurface(SDL_Surface *s) {
    if (!s || --s->refcount > 0) return;
    if (!(s->flags & SDL_SURFACE_PREALLOCATED)) free(s->pixels);
    free(s);
}
bool SDL_LockSurface(SDL_Surface *s) { (void)s; return true; }
void SDL_UnlockSurface(SDL_Surface *s) { (void)s; }
SDL_Surface *SDL_ConvertSurface(SDL_Surface *s, SDL_PixelFormat format) {
    if (!s) return NULL;
    if (s->format != format || (format != SDL_PIXELFORMAT_RGBA32)) {   /* (the loader below makes RGBA32 only) */
        set_err("SDL_ConvertSurface: only RGBA32 in the Quest build");
        return NULL;
    }
    size_t row = (size_t)s->w * 4u;
    unsigned char *p = (unsigned char *)malloc(row * (size_t)s->h);
    if (!p) return NULL;
    for (int y = 0; y < s->h; y++) memcpy(p + row * (size_t)y, (unsigned char *)s->pixels + (size_t)s->pitch * (size_t)y, row);
    return surf_new(s->w, s->h, format, p, (int)row, 1);
}
/* the PNG reader: ps2_png.h (shared with the PC test tools/quest/png_test.c, which checks it against SDL3's own loader) */
SDL_Surface *SDL_LoadPNG(const char *file) {
    size_t n = 0;
    unsigned char *d = (unsigned char *)SDL_LoadFile(file, &n);
    if (!d) return NULL;
    uint32_t w = 0, h = 0;
    char why[160];
    unsigned char *px = ps2_png_decode(d, n, &w, &h, why, sizeof why);
    free(d);
    if (!px) { set_err("%s: %s", file, why); return NULL; }
    return surf_new((int)w, (int)h, SDL_PIXELFORMAT_RGBA32, px, (int)(w * 4u), 1);
}
bool SDL_SavePNG(SDL_Surface *surface, const char *file) {
    (void)surface;
    set_err("%s: PNG writing is not in the Quest build", file);
    return false;
}

/* ---- audio: one AAudio output stream per SDL_OpenAudioDeviceStream ---- */
struct SDL_AudioStream {
    AAudioStream *as;
    SDL_AudioStreamCallback cb;
    void *user;
    SDL_AudioSpec spec;
    int frame_bytes;
    unsigned char *dst;            /* the AAudio buffer being filled (inside the data callback) */
    int dst_left;                  /* bytes still free in it */
    unsigned long long frames, underruns_said;
};
static aaudio_data_callback_result_t aa_data(AAudioStream *as, void *user, void *audio, int32_t frames) {
    SDL_AudioStream *st = (SDL_AudioStream *)user;
    (void)as;
    int bytes = frames * st->frame_bytes;
    st->dst = (unsigned char *)audio;
    st->dst_left = bytes;
    st->cb(st->user, st, bytes, bytes);
    if (st->dst_left > 0) memset(st->dst, 0, (size_t)st->dst_left);   /* (the callback gave less: silence) */
    st->dst = NULL;
    if (st->spec.format == SDL_AUDIO_S16LE)              /* ACZ_QUEST_RECORD: the game track = the bytes the device plays */
        ps2_qrec_game_pcm(audio, frames, st->spec.channels, st->spec.freq);
    st->frames += (unsigned long long)frames;
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static void aa_error(AAudioStream *as, void *user, aaudio_result_t err) {
    (void)as; (void)user;
    __android_log_print(ANDROID_LOG_WARN, "aczquest", "audio: AAudio error %d (%s)", err, AAudio_convertResultToText(err));
}
SDL_AudioDeviceID *SDL_GetAudioPlaybackDevices(int *count) {
    if (count) *count = 0;
    SDL_AudioDeviceID *ids = (SDL_AudioDeviceID *)calloc(1, sizeof *ids);
    return ids;
}
const char *SDL_GetAudioDeviceName(SDL_AudioDeviceID devid) { (void)devid; return "AAudio default output"; }
SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec *spec,
                                           SDL_AudioStreamCallback callback, void *userdata) {
    (void)devid;
    if (!spec || !callback || (spec->format != SDL_AUDIO_S16LE && spec->format != SDL_AUDIO_F32LE)) {
        set_err("SDL_OpenAudioDeviceStream: only s16 / f32 with a callback in the Quest build");
        return NULL;
    }
    AAudioStreamBuilder *b = NULL;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) { set_err("AAudio_createStreamBuilder failed"); return NULL; }
    SDL_AudioStream *st = (SDL_AudioStream *)calloc(1, sizeof *st);
    st->cb = callback;
    st->user = userdata;
    st->spec = *spec;
    st->frame_bytes = spec->channels * (spec->format == SDL_AUDIO_S16LE ? 2 : 4);
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setFormat(b, spec->format == SDL_AUDIO_S16LE ? AAUDIO_FORMAT_PCM_I16 : AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(b, spec->channels);
    AAudioStreamBuilder_setSampleRate(b, spec->freq);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
    AAudioStreamBuilder_setDataCallback(b, aa_data, st);
    AAudioStreamBuilder_setErrorCallback(b, aa_error, st);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &st->as);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        set_err("AAudio openStream: %s", AAudio_convertResultToText(r));
        free(st);
        return NULL;
    }
    __android_log_print(ANDROID_LOG_INFO, "aczquest", "audio: AAudio %d Hz, %d channels, %s, burst %d frames, buffer %d",
                        AAudioStream_getSampleRate(st->as), AAudioStream_getChannelCount(st->as),
                        spec->format == SDL_AUDIO_S16LE ? "s16" : "f32", AAudioStream_getFramesPerBurst(st->as),
                        AAudioStream_getBufferSizeInFrames(st->as));
    return st;
}
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *st) {
    if (!st || !st->as) return false;
    aaudio_result_t r = AAudioStream_requestStart(st->as);
    if (r != AAUDIO_OK) { set_err("AAudio start: %s", AAudio_convertResultToText(r)); return false; }
    return true;
}
bool SDL_PutAudioStreamData(SDL_AudioStream *st, const void *buf, int len) {
    if (!st || !st->dst || len <= 0) return st != NULL;
    int k = len < st->dst_left ? len : st->dst_left;
    memcpy(st->dst, buf, (size_t)k);
    st->dst += k;
    st->dst_left -= k;
    return true;
}
void SDL_DestroyAudioStream(SDL_AudioStream *st) {
    if (!st) return;
    if (st->as) { AAudioStream_requestStop(st->as); AAudioStream_close(st->as); }
    free(st);
}

/* ---- events: a small queue for SDL_PushEvent; nothing else makes events ---- */
#define EVQ 1024u                                /* (joystick axis events: a HOTAS sends many between two frames) */
static SDL_Event evq[EVQ];
static unsigned evq_r, evq_w;
static pthread_mutex_t evq_m = PTHREAD_MUTEX_INITIALIZER;
bool SDL_PushEvent(SDL_Event *e) {
    pthread_mutex_lock(&evq_m);
    bool ok = evq_w - evq_r < EVQ;
    if (ok) evq[evq_w++ & (EVQ - 1u)] = *e;
    pthread_mutex_unlock(&evq_m);
    return ok;
}
bool SDL_PollEvent(SDL_Event *e) {
    pthread_mutex_lock(&evq_m);
    bool got = evq_r != evq_w;
    if (got) { SDL_Event x = evq[evq_r++ & (EVQ - 1u)]; if (e) *e = x; }
    pthread_mutex_unlock(&evq_m);
    return got;
}
bool SDL_AddEventWatch(SDL_EventFilter filter, void *userdata) { (void)filter; (void)userdata; return true; }
void SDL_RemoveEventWatch(SDL_EventFilter filter, void *userdata) { (void)filter; (void)userdata; }
const bool *SDL_GetKeyboardState(int *numkeys) {
    static bool keys[SDL_SCANCODE_COUNT];
    if (numkeys) *numkeys = SDL_SCANCODE_COUNT;
    return keys;
}
const char *SDL_GetScancodeName(SDL_Scancode scancode) { (void)scancode; return ""; }

/* ---- the window: a handle with a size, no surface (the renderer runs headless: PS2_VK_HEADLESS) ---- */
struct SDL_Window { int w, h; char title[128]; };
static struct SDL_Window the_window;
SDL_Window *SDL_CreateWindow(const char *title, int w, int h, SDL_WindowFlags flags) {
    (void)flags;
    the_window.w = w > 0 ? w : 1280;
    the_window.h = h > 0 ? h : 896;
    snprintf(the_window.title, sizeof the_window.title, "%s", title ? title : "");
    return &the_window;
}
bool SDL_GetWindowSizeInPixels(SDL_Window *win, int *w, int *h) {
    if (w) *w = win ? win->w : 0;
    if (h) *h = win ? win->h : 0;
    return win != NULL;
}
bool SDL_SetWindowSize(SDL_Window *win, int w, int h) { if (win && w > 0 && h > 0) { win->w = w; win->h = h; } return true; }
bool SDL_SetWindowTitle(SDL_Window *win, const char *title) {
    if (win) snprintf(win->title, sizeof win->title, "%s", title ? title : "");
    return true;
}
bool SDL_SetWindowFullscreen(SDL_Window *win, bool fullscreen) { (void)win; (void)fullscreen; return true; }
bool SDL_SetWindowFullscreenMode(SDL_Window *win, const SDL_DisplayMode *mode) { (void)win; (void)mode; return true; }
bool SDL_SetWindowPosition(SDL_Window *win, int x, int y) { (void)win; (void)x; (void)y; return true; }
bool SDL_SyncWindow(SDL_Window *win) { (void)win; return true; }
SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window *win) { (void)win; return 1; }
const SDL_DisplayMode *SDL_GetDesktopDisplayMode(SDL_DisplayID id) {
    static SDL_DisplayMode m;
    m.displayID = id; m.format = SDL_PIXELFORMAT_ABGR8888; m.w = 1920; m.h = 1080; m.pixel_density = 1.0f;
    m.refresh_rate = 72.0f;
    return &m;
}
bool SDL_GetClosestFullscreenDisplayMode(SDL_DisplayID id, int w, int h, float rr, bool hd, SDL_DisplayMode *closest) {
    (void)id; (void)w; (void)h; (void)rr; (void)hd; (void)closest;
    set_err("no displays in the Quest build");
    return false;
}
float SDL_GetDisplayContentScale(SDL_DisplayID id) { (void)id; return 1.0f; }
char const *const *SDL_Vulkan_GetInstanceExtensions(Uint32 *count) {
    static const char *const none[1] = { NULL };
    if (count) *count = 0;
    return none;
}
bool SDL_Vulkan_CreateSurface(SDL_Window *win, VkInstance inst, const struct VkAllocationCallbacks *a, VkSurfaceKHR *s) {
    (void)win; (void)inst; (void)a;
    if (s) *s = VK_NULL_HANDLE;
    set_err("no window surface in the Quest build (PS2_VK_HEADLESS)");
    return false;
}

/* ---- haptics, virtual joysticks: none (the gamepad and joystick calls: ps2_pad_android.c) ---- */
/* names the bindings use: SDL's own strings */
static const char *const axis_names[SDL_GAMEPAD_AXIS_COUNT] = { "leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger" };
static const char *const button_names[] = {
    "a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder", "rightshoulder",
    "dpup", "dpdown", "dpleft", "dpright", "misc1", "paddle1", "paddle2", "paddle3", "paddle4", "touchpad",
    "misc2", "misc3", "misc4", "misc5", "misc6" };
SDL_GamepadAxis SDL_GetGamepadAxisFromString(const char *s) {
    for (int i = 0; s && i < SDL_GAMEPAD_AXIS_COUNT; i++) if (!strcasecmp(s, axis_names[i])) return (SDL_GamepadAxis)i;
    return SDL_GAMEPAD_AXIS_INVALID;
}
SDL_GamepadButton SDL_GetGamepadButtonFromString(const char *s) {
    for (int i = 0; s && i < (int)(sizeof button_names / sizeof button_names[0]) && i < SDL_GAMEPAD_BUTTON_COUNT; i++)
        if (!strcasecmp(s, button_names[i])) return (SDL_GamepadButton)i;
    return SDL_GAMEPAD_BUTTON_INVALID;
}
const char *SDL_GetGamepadStringForType(SDL_GamepadType type) {
    switch (type) {
    case SDL_GAMEPAD_TYPE_STANDARD: return "standard";
    case SDL_GAMEPAD_TYPE_XBOX360: return "xbox360";
    case SDL_GAMEPAD_TYPE_XBOXONE: return "xboxone";
    case SDL_GAMEPAD_TYPE_PS3: return "ps3";
    case SDL_GAMEPAD_TYPE_PS4: return "ps4";
    case SDL_GAMEPAD_TYPE_PS5: return "ps5";
    default: return NULL;
    }
}
void SDL_GUIDToString(SDL_GUID g, char *out, int cap) {
    for (int i = 0; i < 16 && (i + 1) * 2 < cap; i++) snprintf(out + i * 2, 3, "%02x", g.data[i]);
}
SDL_PropertiesID SDL_GetJoystickProperties(SDL_Joystick *j) { (void)j; return 0; }
bool SDL_RumbleJoystick(SDL_Joystick *j, Uint16 lo, Uint16 hi, Uint32 ms) { (void)j; (void)lo; (void)hi; (void)ms; return false; }
SDL_JoystickID SDL_AttachVirtualJoystick(const SDL_VirtualJoystickDesc *desc) {
    (void)desc;
    set_err("no virtual joysticks in the Quest build (PS2_PAD_SDL)");
    return 0;
}
bool SDL_DetachVirtualJoystick(SDL_JoystickID id) { (void)id; return false; }
bool SDL_SetJoystickVirtualAxis(SDL_Joystick *j, int a, Sint16 v) { (void)j; (void)a; (void)v; return false; }
bool SDL_SetJoystickVirtualButton(SDL_Joystick *j, int b, bool d) { (void)j; (void)b; (void)d; return false; }
bool SDL_SetJoystickVirtualHat(SDL_Joystick *j, int h, Uint8 v) { (void)j; (void)h; (void)v; return false; }
bool SDL_IsJoystickHaptic(SDL_Joystick *j) { (void)j; return false; }
SDL_Haptic *SDL_OpenHapticFromJoystick(SDL_Joystick *j) { (void)j; return NULL; }
void SDL_CloseHaptic(SDL_Haptic *h) { (void)h; }
Uint32 SDL_GetHapticFeatures(SDL_Haptic *h) { (void)h; return 0; }
SDL_HapticEffectID SDL_CreateHapticEffect(SDL_Haptic *h, const SDL_HapticEffect *e) { (void)h; (void)e; return -1; }
bool SDL_UpdateHapticEffect(SDL_Haptic *h, SDL_HapticEffectID id, const SDL_HapticEffect *e) { (void)h; (void)id; (void)e; return false; }
bool SDL_RunHapticEffect(SDL_Haptic *h, SDL_HapticEffectID id, Uint32 it) { (void)h; (void)id; (void)it; return false; }
void SDL_DestroyHapticEffect(SDL_Haptic *h, SDL_HapticEffectID id) { (void)h; (void)id; }
bool SDL_GetBooleanProperty(SDL_PropertiesID p, const char *name, bool def) { (void)p; (void)name; return def; }
