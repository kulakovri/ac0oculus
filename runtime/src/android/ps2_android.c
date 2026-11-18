/* ps2_android.c -- the Quest 3 standalone build's platform layer (ps2_android.h; NOTES_QUEST.md, stage 2).

   NativeActivity + the NDK's native_app_glue: android_main runs on the glue's thread and keeps serving the activity's
   events; the runtime's main() (ps2_main.c compiled as ps2_runtime_main) runs on a second thread, the EE thread, with
   a 64 MB stack. Before it starts:
   - stdout and stderr (ps2_log's sink) go through a pipe to logcat (tag aczquest) and to files/aczquest.log;
   - the working directory becomes the app's files dir, so every relative path of the runtime (saves/, out/,
     ac5_settings.ini) lands there; the APK's assets/shaders/ are unpacked to files/shaders/ when they differ;
   - commandline.txt in the data root, then scripts/<name>.txt for the intent extra "script" (am start --es script
     NAME), are applied as environment variables before any getenv of the runtime: KEY=VALUE per line, '#' comments,
     ARGS=... extra runtime arguments (e.g. --vblanks 2520), so every PS2_* / ACZ_* switch, pad and stick scripts and
     ACZ_BOOT_MISSION work unchanged;
   - the defaults of a headset run that the PC launchers would set: PS2_XR=1, stereo, multiview, no window surface
     (PS2_VK_HEADLESS), the native renderer (PS2_RN_VP=1), the tile-friendly frame (PS2_FRAME_GRAPH=2), native-mesh
     batching (PS2_RN_BATCH=2), ACZ_LAUNCHER=0, telemetry and statistics off; then the ACZQ_PRESET (faithful) defaults -- all only where commandline.txt / the script set nothing;
   - then the ENHANCEMENTS page's rows (ps2_enh_boot, ACZ_MENU_SETTINGS=1): each switch not set by then takes its saved value
     from files/ac5_settings.ini, else its row's Quest default (rq2_menu: the Quest's frame levers are rows now).
   When main returns (or the process quits through ps2_quit_exit) result.txt records how it ended. */
#include "ps2_android.h"

#include <android/asset_manager.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <jni.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define TAG "aczquest"

int ps2_runtime_main(int argc, char **argv);   /* ps2_main.c (-Dmain=ps2_runtime_main in the Quest build) */
void ps2_enh_boot(void);                       /* ps2_settings.c: the ENHANCEMENTS rows' switches (the PC's constructor) */
extern unsigned ps2_gs_frame_count;            /* fields the GS has shown (ps2_gs.c) */

static struct android_app *g_app;
static char g_files[512], g_data[512];
static volatile int g_quit, g_ee_tid, g_done;
static int g_result_written;
static pthread_mutex_t g_result_lock = PTHREAD_MUTEX_INITIALIZER;
static struct timespec g_t0;

void *ps2_android_vm(void) { return g_app ? (void *)g_app->activity->vm : NULL; }
void *ps2_android_activity(void) { return g_app ? (void *)g_app->activity->clazz : NULL; }
const char *ps2_android_files_dir(void) { return g_files; }
const char *ps2_android_data_dir(void) { return g_data; }
int ps2_android_ee_tid(void) { return g_ee_tid; }
int ps2_android_should_quit(void) { return __atomic_load_n(&g_quit, __ATOMIC_ACQUIRE); }

static void alog(int prio, const char *fmt, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    __android_log_write(prio, TAG, b);
}

void ps2_android_result(const char *status, int code) {
    pthread_mutex_lock(&g_result_lock);
    if (!g_result_written) {
        g_result_written = 1;
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        double secs = (double)(t.tv_sec - g_t0.tv_sec) + (double)(t.tv_nsec - g_t0.tv_nsec) * 1e-9;
        char path[600];
        snprintf(path, sizeof path, "%s/result.txt", g_files);
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "status=%s\nexit=%d\nfields=%u\nseconds=%.1f\n", status, code, ps2_gs_frame_count, secs);
            fclose(f);
        }
        alog(ANDROID_LOG_INFO, "result: status=%s exit=%d fields=%u seconds=%.1f", status, code, ps2_gs_frame_count,
             secs);
    }
    pthread_mutex_unlock(&g_result_lock);
}

/* ---- stdout / stderr -> logcat + files/aczquest.log ---- */
static int g_logpipe[2] = { -1, -1 };
static FILE *g_logfile;
int ps2_threads_quest(void);                  /* (ps2_gfxq.c, S4: PS2_THREADS=quest, the thread budget) */
void ps2_thread_helper(const char *name, int kind);
int ps2_helper_queue(void *(*fn)(void *), void *arg, const char *name);
static void *log_thread(void *arg) {
    (void)arg;
    ps2_thread_helper("log", 2);                 /* (S4: census name only) */
    char buf[4096], line[2048];
    size_t ln = 0;
    for (;;) {
        ssize_t n = read(g_logpipe[0], buf, sizeof buf);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
        if (g_logfile) { fwrite(buf, 1, (size_t)n, g_logfile); fflush(g_logfile); }
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || ln == sizeof line - 1) {
                line[ln] = 0;
                if (ln) __android_log_write(ANDROID_LOG_INFO, TAG, line);
                ln = 0;
                if (c == '\n') continue;
            }
            line[ln++] = c;
        }
    }
    return NULL;
}
static void log_start(void) {
    char path[600], prev[600];
    snprintf(path, sizeof path, "%s/aczquest.log", g_files);
    snprintf(prev, sizeof prev, "%s/aczquest_prev.log", g_files);
    rename(path, prev);                                   /* (the previous run's log is kept once) */
    g_logfile = fopen(path, "w");
    if (pipe(g_logpipe) != 0) { alog(ANDROID_LOG_ERROR, "log: no pipe (%d)", errno); return; }
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    dup2(g_logpipe[1], 1);
    dup2(g_logpipe[1], 2);
    pthread_t th;
    pthread_create(&th, NULL, log_thread, NULL);
    pthread_detach(th);
}

/* ---- paths ---- */
static int mkdirs(const char *p) {
    char b[600];
    snprintf(b, sizeof b, "%s", p);
    for (char *s = b + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(b, 0775); *s = '/'; }
    return mkdir(b, 0775) == 0 || errno == EEXIST ? 0 : -1;
}
static void paths_init(void) {
    const char *ext = g_app->activity->externalDataPath, *in = g_app->activity->internalDataPath;
    snprintf(g_files, sizeof g_files, "%s", ext && *ext ? ext : in ? in : "/data/local/tmp");
    mkdirs(g_files);
    const char *d = getenv("ACZQ_DATA");
    if (d && *d) snprintf(g_data, sizeof g_data, "%s", d);
    else if (access("/sdcard/ac0oculus", R_OK | X_OK) == 0) snprintf(g_data, sizeof g_data, "/sdcard/ac0oculus");
    else snprintf(g_data, sizeof g_data, "%s", g_files);
    size_t n = strlen(g_data);
    while (n > 1 && g_data[n - 1] == '/') g_data[--n] = 0;
    if (chdir(g_files) != 0) alog(ANDROID_LOG_ERROR, "paths: chdir %s failed (%d)", g_files, errno);
    /* out/: the eye dumps and captures open "out/..." relative to the files dir and do not create it (the PC's
       run_test.sh does); without it every dump failed silently on the first device runs (2026-11-08, Q-118) */
    if (mkdir("out", 0775) != 0 && errno != EEXIST) alog(ANDROID_LOG_ERROR, "paths: mkdir out failed (%d)", errno);
}

/* ---- APK assets: shaders/ -> files/shaders/ (rewritten only when the bytes differ) ---- */
static void unpack_assets(const char *dir) {
    AAssetManager *am = g_app->activity->assetManager;
    AAssetDir *ad = am ? AAssetManager_openDir(am, dir) : NULL;
    if (!ad) { alog(ANDROID_LOG_WARN, "assets: no %s/ in the APK", dir); return; }
    char out[600];
    snprintf(out, sizeof out, "%s/%s", g_files, dir);
    mkdirs(out);
    const char *name;
    unsigned files = 0, written = 0;
    size_t bytes = 0;
    while ((name = AAssetDir_getNextFileName(ad)) != NULL) {
        char an[512], fn[700];
        snprintf(an, sizeof an, "%s/%s", dir, name);
        snprintf(fn, sizeof fn, "%s/%s", out, name);
        AAsset *a = AAssetManager_open(am, an, AASSET_MODE_BUFFER);
        if (!a) continue;
        const void *p = AAsset_getBuffer(a);
        size_t len = (size_t)AAsset_getLength(a);
        files++;
        bytes += len;
        int same = 0;
        FILE *f = fopen(fn, "rb");
        if (f && p) {
            char *cur = (char *)malloc(len + 1);
            if (cur && fread(cur, 1, len + 1, f) == len && memcmp(cur, p, len) == 0) same = 1;
            free(cur);
        }
        if (f) fclose(f);
        if (!same && p && (f = fopen(fn, "wb")) != NULL) {
            if (fwrite(p, 1, len, f) == len) written++;
            fclose(f);
        }
        AAsset_close(a);
    }
    AAssetDir_close(ad);
    alog(ANDROID_LOG_INFO, "assets: %s/: %u files, %zu bytes, %u written", dir, files, bytes, written);
}

/* ---- commandline.txt / scripts/<name>.txt ---- */
static char *g_args[64];
static int g_nargs;
static void add_args(const char *v) {
    char *s = strdup(v), *save = NULL;
    for (char *t = strtok_r(s, " \t", &save); t && g_nargs < 56; t = strtok_r(NULL, " \t", &save)) g_args[g_nargs++] = t;
}
static int apply_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[4096];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        size_t l = strlen(s);
        while (l && (s[l - 1] == '\n' || s[l - 1] == '\r' || s[l - 1] == ' ' || s[l - 1] == '\t')) s[--l] = 0;
        if (!*s || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq || eq == s) { alog(ANDROID_LOG_WARN, "commandline: %s: ignored line '%s'", path, s); continue; }
        *eq = 0;
        if (!strcmp(s, "ARGS")) add_args(eq + 1);
        else setenv(s, eq + 1, 1);
        n++;
    }
    fclose(f);
    alog(ANDROID_LOG_INFO, "commandline: %s: %d settings", path, n);
    return n;
}
/* runtime permissions through JNI on the NativeActivity (Context.checkSelfPermission, Activity.requestPermissions: both
   public, so no Java class of our own); attaches the calling thread only when it is not attached yet */
int ps2_android_permission(const char *perm, int request) {
    if (!g_app || !perm) return 0;
    JavaVM *vm = g_app->activity->vm;
    JNIEnv *env = NULL;
    int attached = 0, granted = 0;
    jint gs = (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6);
    if (gs == JNI_EDETACHED) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK || !env) return 0;
        attached = 1;
    } else if (gs != JNI_OK || !env) return 0;
    jobject act = g_app->activity->clazz;
    jclass acl = (*env)->GetObjectClass(env, act);
    jstring js = (*env)->NewStringUTF(env, perm);
    jmethodID chk = acl ? (*env)->GetMethodID(env, acl, "checkSelfPermission", "(Ljava/lang/String;)I") : NULL;
    if (chk && js) granted = (*env)->CallIntMethod(env, act, chk, js) == 0;     /* (PackageManager.PERMISSION_GRANTED) */
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); granted = 0; }
    if (!granted && request && acl && js) {
        jmethodID rq = (*env)->GetMethodID(env, acl, "requestPermissions", "([Ljava/lang/String;I)V");
        jclass scl = (*env)->FindClass(env, "java/lang/String");
        jobjectArray arr = rq && scl ? (*env)->NewObjectArray(env, 1, scl, js) : NULL;
        if (arr) (*env)->CallVoidMethod(env, act, rq, arr, (jint)4711);
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); alog(ANDROID_LOG_WARN, "permission: requestPermissions(%s) threw", perm); }
        else if (arr) alog(ANDROID_LOG_INFO, "permission: %s requested (the headset shows the dialog)", perm);
        if (arr) (*env)->DeleteLocalRef(env, arr);
        if (scl) (*env)->DeleteLocalRef(env, scl);
    }
    if (js) (*env)->DeleteLocalRef(env, js);
    if (acl) (*env)->DeleteLocalRef(env, acl);
    if (attached) (*vm)->DetachCurrentThread(vm);
    return granted;
}
static int intent_string(const char *key, char *out, size_t cap) {
    JavaVM *vm = g_app->activity->vm;
    JNIEnv *env = NULL;
    int ok = 0;
    if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK || !env) return 0;
    jobject act = g_app->activity->clazz;
    jclass acl = (*env)->GetObjectClass(env, act);
    jmethodID gi = acl ? (*env)->GetMethodID(env, acl, "getIntent", "()Landroid/content/Intent;") : NULL;
    jobject in = gi ? (*env)->CallObjectMethod(env, act, gi) : NULL;
    jclass icl = in ? (*env)->GetObjectClass(env, in) : NULL;
    jmethodID gs = icl ? (*env)->GetMethodID(env, icl, "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;") : NULL;
    if (gs) {
        jstring k = (*env)->NewStringUTF(env, key);
        jstring v = (jstring)(*env)->CallObjectMethod(env, in, gs, k);
        if (v) {
            const char *c = (*env)->GetStringUTFChars(env, v, NULL);
            if (c) { snprintf(out, cap, "%s", c); ok = 1; (*env)->ReleaseStringUTFChars(env, v, c); }
        }
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*vm)->DetachCurrentThread(vm);
    return ok;
}
/* ACZQ_PRESET (default faithful): the configuration a first headset run should start from (QUEST_NATIVE_RESEARCH section 5,
   NOTES_QUEST S1): the game's own renderer without our heavy extras -- no volumetric clouds, no in-between frames, no 3D
   cockpit model, 1x internal resolution, and none of our passes that break the frame on a tile GPU (the in-cloud measure,
   the early-submission split, the soft effect edges, the contrails' snapshot, the G veil). Each is only a default: a line of
   commandline.txt (or the script) sets it otherwise; ACZQ_PRESET=none leaves the runtime's own defaults. */
static const char *const preset_faithful[][2] = {
    /* (ACZ_VR_CLOUDS: the volumetric clouds, the user's must-have since 2026-11-08, are the default of the CLOUDS row since
       rq2_menu -- VOLUMETRIC on the PC and the Quest alike --, so the page can save GAME; commandline.txt ACZ_VR_CLOUDS= holds GAME) */
    { "PS2_XR_RERENDER", "0" }, { "ACZ_VR_COCKPIT_MODEL", "none" }, { "PS2_UPSCALE", "1" },
    { "ACZ_IMM_CLOUD", "0" }, { "PS2_PERF_EARLY_SUBMIT", "0" }, { "ACZ_VR_FX_SOFT", "0" }, { "ACZ_VR_CONTRAILS", "0" },
    { "ACZ_IMM_GLOC", "0" },
};
static void defaults(void) {
    /* what play_zero_vr.sh / run_test.sh export for a headset run; applied after commandline.txt and the script, only
       where they set nothing */
    setenv("PS2_XR", "1", 0);
    setenv("PS2_XR_MODE", "stereo", 0);
    setenv("PS2_XR_MULTIVIEW", "1", 0);
    setenv("PS2_VK_HEADLESS", "1", 0);
    setenv("PS2_RN_VP", "1", 0);                 /* (the native renderer: the PC player profile's word `native`, what
                                                    run_test.sh / headset_test.sh and every S1-S4 measurement ran) */
    setenv("PS2_FRAME_GRAPH", "2", 0);           /* (quest_s2: no local read here -- DATE and the self-reads in core
                                                    passes, the off-screen builds hoisted in front of the scene) */
    setenv("PS2_RN_BATCH", "2", 0);              /* (quest S3b: native-mesh draws batched, the terrain's tiles pass-major;
                                                    off by itself, logged, without the device features) */
    setenv("PS2_VK_STRICT", "1", 0);             /* (quest bring-up: the white image initialised, timelineSemaphore and
                                                    shaderClipDistance enabled, the eye dump's layer barrier; Q-71 / Q-87) */
    { char ql[600], q9[600]; snprintf(ql, sizeof ql, "%s/cockpits_ql", g_data); snprintf(q9, sizeof q9, "%s/cockpits_ql_t9", g_data);
      setenv("ACZ_VR_COCKPIT_LOD", access(ql, R_OK | X_OK) == 0 || access(q9, R_OK | X_OK) == 0 ? "2" : "1", 0); }        /* (below the Quest LOD set, if pushed; first device session 2026-11-08: the 3D cockpit's mesh LOD 1 from its
                                                    .lod side file, 0.5 px error; M1 F-5E 17.79 -> 16.85 ms a stereo frame;
                                                    the Adreno's cockpit is geometry bound, Q-122) */
    setenv("ACZ_VR_COCKPIT_TRI_BUDGET", "250000", 0);  /* (rq3_cockpit 2026-11-10, R-61: the first baked level from LOD 2 on whose
                                                    triangles fit; the Quest set's levels 3 / 4 (tools/cockpit/build_lod_quest.sh);
                                                    the user's flights: the Rafale M at 719,756 triangles (L2), App GPU 13.8 ms at
                                                    the scale's floor; the start value of the prompt, the default from the sweep) */
    setenv("ACZ_VR_COCKPIT_TEX_BUDGET", "192", 0);   /* (R-61: the cockpit's textures within 192 MB, the largest halved first:
                                                    11 of the 36 models are over it, the F-117 766 MB, the Rafale 416 MB; device
                                                    "MB on the GPU" Rafale 484, MiG-29A 271) */
    setenv("ACZ_VR_COCKPIT_SHADOW_SIZE", "1024", 0);  /* (the user's option 2, 2026-11-08: the cockpit's sun shadow map
                                                    at 1024: 17.05 -> 16.61 ms with LOD 1, no visible change at 1x) */
    setenv("ACZ_VR_GOV_PERIOD", "render", 0);    /* (the mission sweep 2026-11-08: the governor's budget 0.9 x the game
                                                    frame's 16.68 ms = 15.0 instead of 0.8 x the 72 Hz display's = 11.11,
                                                    which held level 3 in 17 of 18 missions; M2 / M3 now level 1 at 60.0
                                                    drawings/s with shader water, ground materials and MSAA 4x back) */
    setenv("PS2_DATE_BATCH_ALL", "3", 0);        /* (the mission sweep 2026-11-08: tile runs without DATE re-issued
                                                    pass-major from the tiles PS2_RN_BATCH=2 refuses for their texture's
                                                    alpha -- M4's sea: 1,316 -> ~350 pipeline changes a frame, GPU 19.6 ->
                                                    13.4 ms, 50 -> 59.5 drawings/s, images bit-equal in the same-frame A/B
                                                    (PS2_PERF_AB_FLIP=datebatch); M14 one dump +-4 in 0.5 % of the pixels:
                                                    Q-127) */
    /* ---- the Quest frame recipe (plan 2, a private script, docs/rq/BUDGET.md): one eye-sized 4x pass; the old path is PS2_RQ=0 ---- */
    setenv("PS2_RQ", "eye", 0);
    /* (PS2_XR_EYE_SCALE 1.0 and PS2_XR_FOVEATION medium: the Quest defaults of their ENHANCEMENTS rows since rq2_menu,
       ps2_settings.c enh_quest_tab / the QUALITY PRESET's Quest ladder) */
    setenv("PS2_XR_FIELD_LOCK", "1", 0);         /* (the field clock follows the display: 120 Hz M3 Stale2 961 -> 105, jerks 0-0.7 %; 72 Hz the exact 5-field pattern) */
    setenv("PS2_XR_FIELD_PHASE_MS", "31", 0);
    setenv("PS2_XR_REFRESH_HZ", "72", 0);        /* (the rate is the app's, plan 4.7: a library start has no headset_test.sh to set it) */
    setenv("PS2_RQ_FDM", "1", 0);
    setenv("PS2_RQ_TERRAIN", "2", 0);
    setenv("PS2_RQ_LEAN", "1", 0);
    setenv("PS2_RQ_ORDER", "1", 0);
    setenv("PS2_RQ_HAZE", "1", 0);
    setenv("PS2_RQ_WATER", "1", 0);
    setenv("PS2_RQ_OCLS", "1", 0);
    setenv("PS2_RQ_L3V", "6000", 0);
    setenv("PS2_RQ_WFAR", "1500", 0);
    setenv("PS2_RQ_MAT", "1", 0);
    setenv("PS2_RQ_X3", "1", 0);
    setenv("PS2_RQ_HUD", "batch", 0);
    setenv("PS2_RQ_ZPRE", "1", 0);
    setenv("PS2_RQ_DYNRES", "1", 0);
    setenv("PS2_RQ_DYNRES_CEIL", "1", 0);       /* (prompt 6, 2026-11-11: the heat state caps the GPU at 545 MHz; the scale reads its
                                                    budget at that ceiling (kgsl max_gpuclk) and gives up resolution to its floor before
                                                    it drops frames; at PLS 0 nothing changes) */
    setenv("PS2_RQ_SNAP_AT", "2", 0);
    setenv("PS2_RQ_EMPTY_HOLD", "1", 0);        /* (rq22_blink 2026-11-17, BUGS_RQ R-290: the user's "blinking" of the 11-17 flights -- a field in
                                                    which the game finished no frame left a stretched copy of the eye's picture in the feed
                                                    for the next frame; the code's default, set here so the Quest's values read in one place) */
    /* (rq4_water 2026-11-11, D.3, BUGS_RQ R-90: the user's decision 6 -- no shader water in the night mission M11, chosen as it
       loads; in the day missions a water patch never switches to the shader water in one frame; numbers: NOTES_RQ rq4_water) */
    setenv("PS2_RQ_WATER_NIGHT", "0", 0);
    setenv("PS2_RQ_WATER_FADEIN", "1", 0);
    /* (rq4_water 2026-11-11, G.3, BUGS_RQ R-92: the static frames -- intros, the quote card, menus, the results -- at 2x the internal
       resolution, the flight at its own; the switch before the first head-tracked list is drawn; numbers: NOTES_RQ rq4_water) */
    setenv("PS2_RQ_FLAT_SCALE", "2", 0);
    setenv("PS2_RQ_FLAT_SCALE_GPU_MS", "12", 0);  /* (t10a on the device: M2's 3D take-off intro at 2x ran 9 s below 60 fps; the static
                                                    frames go back to 1x while their GPU time at 2x is over 12 ms at the top clock) */
    setenv("PS2_RQ_DET_ALPHA", "1", 0);          /* (rq4_water, G.4, BUGS_RQ R-93: the merged terrain's detail subtraction on blended texels as
                                                    the game's passes do it -- M18's dark specks beside the runway; NOTES_RQ rq4_water) */
    setenv("PS2_RQ_BURST", "1", 0);              /* (rq5_partb 2026-11-12, BUGS_RQ R-120 / R-121: M18's MPBM burst as a volume inside the eye pass, the PC's
                                                    ACZ_VR_FX_BURST at the Quest's price; the game's flat glow columns and their halo collapsed --
                                                    the user's M18 flight "it turns, it is not volumetric"; NOTES_RQ rq5_partb) */            /* (R-59, ground3 2026-11-10: the hoist's snapshot at the crossing run, one per crossing --
                                                    the trees read last frame's cloud impostor with 0, the white cards of M2 / M4 / M6 / M9;
                                                    simulator class check 0 mismatched reads; NOTES_RQ 2026-11-10 ground3) */
    setenv("PS2_RQ_GLOW_CLEAN", "1", 0);         /* (rq5_partb 2026-11-12, BUGS_RQ R-124: the game's glow copies the feed without last list's sun
                                                    overlays -- the user's M4 "the sun blinds; on the PC the sun works well"; device 2aa2770, M4 tape
                                                    f7865 / 7985 / 8225: the disc r 79 / 165 / 143 -> 43 / 64 / 62 px at >= 250; NOTES_RQ rq5_partb) */
    /* (ACZ_VR_GOVERNOR 0 -- no look may switch in flight on the Quest, the user 2026-11-09 --, ACZ_VR_CLOUD_DETAIL medium and
       ACZ_VR_CLOUD_FAR_RES 1024: their rows' Quest defaults since rq2_menu, so the saved page and the QUALITY PRESET can change them) */
    setenv("PS2_RQ_CLOUDS", "1", 0);
    setenv("ACZ_VR_COCKPIT_MATERIAL", "quest", 0);
    setenv("ACZ_VR_RQ_COCKPIT_FIRST", "0", 0);     /* (R-73, 2026-11-11: with the cockpit drawn first the canopy bow was missing looking up
                                                    in M1 -- the game's sky dome there is drawn at depth ALWAYS over it (T-2 on the
                                                    simulator; device: bow_q_f1 against bow_q_f0, field 2700, head 55 deg up); at its
                                                    game draw, after the world, as on the PC: the bow is drawn. Cost on the device:
                                                    M2 Rafale M 12.5 -> 12.7, M4 F-15C 12.4 -> 12.5 ms raw App) */
    /* (R-57 / R-58, the glass round of 2026-11-10, agent glass3: the readable canopy and the cracks in the eye pass, rq/rq_glass.inc;
       the user's M9 flight: "there is no feeling of glass, and the feeling of glass is the main feature". On the simulator the PC's
       glass is above all the interior's reflection (M4 F-15C 64 / 41 / 24 % of the image over 2 levels toward the sun / across /
       away); every ACZ_VR_RQ_GLASS_* 0 = the dry glass of t8. Device cost and the user's A/B: NOTES_RQ glass3) */
    setenv("ACZ_VR_RQ_GLASS_FRESNEL", "0", 0);   /* (the PC's rim: the sky's tone fed back through the scene light, NOTES_RQ) */
    setenv("ACZ_VR_RQ_GLASS_REFLECT", "1.0", 0);   /* (0.6 = the PC's strength at the same frames, NOTES_RQ glass3; 1.0 since
                                                    2026-11-11, the user's decision 1 after the second flight round: at 0.6 the
                                                    Rafale M showed no glass at all; scratches, dust and glare stay as they are) */
    setenv("ACZ_VR_RQ_GLASS_REFLECT_FS", "1", 0);
    setenv("ACZ_VR_RQ_GLASS_REFLECT_TH", "0.006", 0);   /* (the device's cost lever: 0.012 / 0.024 draw less of the F-15C's canopy) */
    setenv("ACZ_VR_RQ_GLASS_WEAR", "3", 0);
    setenv("ACZ_VR_RQ_GLASS_GLARE", "1", 0);
    setenv("ACZ_VR_RQ_COCKPIT_CRACKS", "1", 0);
    /* (the second flight round's night and cloud items, agent rq4_night 2026-11-11; NOTES_RQ, BUGS_RQ R-85 to R-88; the same values are
       the code's defaults, set here so the Quest's values read in one place) */
    setenv("ACZ_VR_RQ_CRACK_NIGHT", "0.5", 0);  /* (R-86: at night the cracks catch the cockpit's own light; M11 141 s had rings, no rays) */
    setenv("ACZ_VR_RQ_COCKPIT_SELFLIT", "0.5", 0);   /* (R-85: the F-22A's and three more models' display pages lit from within at night) */
    setenv("ACZ_VR_RQ_COCKPIT_NIGHT_FS", "1", 0);     /* (R-85: the night panel's area per fragment: big floor / sill triangles get their flood) */
    setenv("ACZ_VR_RQ_COCKPIT_WET_UNTINT", "1", 0);   /* (R-87: the wet canopy's world tap without the glass's own tint: no red-brown drops) */
    setenv("ACZ_VR_RQ_COCKPIT_WET_LIGHT", "1", 0);    /* (R-88: in cloud the water on the glass never darker than the cloud behind it) */
    setenv("ACZ_VR_CANOPY_FROZEN_HOLD", "1", 0);      /* (R-300, rq23_drops 2026-11-17, the owner's M1: the frozen drops slid forward as he sped up;
                                                    ice keeps the stretch and the clearing it froze with; the code's default) */
    setenv("ACZ_VR_MFD_FRONT", "1", 0);               /* (R-280, rq21_screens 2026-11-16, the user's F-1: the Mirage 2000D's dark head-level display shows
                                                    the big map, its lower centre display the radar, both live, its left display the wingman commands;
                                                    the code's default, set here so the Quest's values read in one place; 0 = the cockpits as built) */
    {   /* the 3D cockpit on by default (a must-have): the F-5E's for any aircraft without a file of its own, the rest by the game's aircraft id */
        static const struct { int id; const char *f; } M[] = {
            { 5, "f15c" }, { 10, "f16c" }, { 21, "a10a" }, { 28, "mig21bis" }, { 0, "j35j" }, { 24, "su27" }, { 18, "f14a" }, { 22, "m2000d" }, { 29, "mig29a" },
            { 16, "f20a" }, { 3, "tornado" }, { 2, "typhoon" }, { 8, "fa18c" }, { 4, "f4e" }, { 20, "ea6b" }, { 1, "gripen" }, { 13, "f22a" }, { 19, "yf23" },
            { 27, "su47" }, { 6, "f15e" }, { 7, "f15smtd" }, { 26, "su37" }, { 31, "f1" }, { 15, "f5e" }, { 17, "f5e" }, { 9, "fa18e" }, { 12, "f117" }, { 14, "f35a" },
            { 35, "adfx01" }, { 11, "f16xl" }, { 34, "adf01" }, { 33, "x02s" }, { 30, "mig31" }, { 23, "rafale_m" }, { 32, "f2a" }, { 25, "su34" } };
        char f[700], list[9000]; size_t n = 0;
        /* ACZQ_COCKPIT_SET (rq3_tex 2026-11-11, PROMPT_2026-11-11_night_cockpits N1.2, BUGS_RQ R-65): the folder under the data
           folder the cockpit models are read from. Unset or "cockpits" = the RGBA8 set as before (format 3: every texture as
           built, 32 bits a texel, the mips made on the device). The Quest default, only when it was pushed: cockpits_q4 =
           tools/quest/make_cockpits_q4.sh's set (format 4: BC7 / BC1 with their stored mips, uploaded as they are; each
           texture stored from the finest mip level the eye box can sample: cpit_pack --need; NOTES_RQ rq3_tex). A model
           the set lacks is read from cockpits/, so a partial set still flies every aircraft; the side files of
           ACZ_VR_COCKPIT_LOD_DIR fit both sets (the same meshes). Back to RGBA8: ACZQ_COCKPIT_SET=cockpits in
           commandline.txt; ACZ_VR_COCKPIT_BC=0 decodes the BC set to RGBA8 at load (the same texels, 4x the memory). */
        {   char q4[600]; snprintf(q4, sizeof q4, "%s/cockpits_q4", g_data);
            if (access(q4, R_OK | X_OK) == 0) setenv("ACZQ_COCKPIT_SET", "cockpits_q4", 0); }
        const char *set = getenv("ACZQ_COCKPIT_SET");
        if (!set || !*set || !strcmp(set, "0") || strchr(set, '/') || strstr(set, "..")) set = "cockpits";
        int from_set = 0, from_rgba = 0;
        list[0] = 0;
        for (size_t k = 0; k < sizeof M / sizeof M[0]; k++) {
            snprintf(f, sizeof f, "%s/%s/%s.cpit", g_data, set, M[k].f);
            if (strcmp(set, "cockpits") && access(f, R_OK) == 0) from_set++;
            else { snprintf(f, sizeof f, "%s/cockpits/%s.cpit", g_data, M[k].f); if (access(f, R_OK) == 0) from_rgba++; }
            if (access(f, R_OK) != 0 || n + strlen(f) + 16 >= sizeof list) continue;
            n += (size_t)snprintf(list + n, sizeof list - n, "%s%d=%s", n ? "," : "", M[k].id, f);
        }
        if (n) setenv("ACZ_VR_COCKPIT_MODELS", list, 0);
        snprintf(f, sizeof f, "%s/%s/f5e_t38.cpit", g_data, set);
        if (access(f, R_OK) != 0) snprintf(f, sizeof f, "%s/cockpits/f5e_t38.cpit", g_data);
        if (access(f, R_OK) == 0) setenv("ACZ_VR_COCKPIT_MODEL", f, 0);   /* (before the preset's `none`: the first setenv wins) */
        alog(ANDROID_LOG_INFO, "cockpit set %s (ACZQ_COCKPIT_SET): %d models from it, %d from cockpits/ (RGBA8); the default "
             "model %s", set, from_set, from_rgba, f);
        snprintf(f, sizeof f, "%s/cockpits_ql_t9", g_data);   /* (the night of 2026-11-11: cockpit3's set v4, levels 3 / 4 for the triangle
                                                    budget, hidden triangles out; its own folder so t8 / t8a keep reading t8's set in
                                                    cockpits_ql -- the rollback stays exact) */
        if (access(f, R_OK | X_OK) != 0) snprintf(f, sizeof f, "%s/cockpits_ql", g_data);
        if (access(f, R_OK | X_OK) == 0) setenv("ACZ_VR_COCKPIT_LOD_DIR", f, 0);   /* (the Quest LOD set: level 2 above) */
    }
    setenv("ACZ_VR_CLOUD_NEAR_RANGE", "1500", 0);   /* (v1, 2026-11-12: back to 1500 in every view; the far map holds 1.4-4 km, the 4000 m field cost 0.9-1.0 ms) */  /* (our clouds on the Quest 3, 2026-11-08: the far map's tiles re-marched
                                                    for the parallax of clouds nearer than its start -- 1,250-1,540 a frame
                                                    from 600 m, ~100-350 from 2500 m; M3 26.6 -> 16.2 ms with FAR_SLICE 8.
                                                    1500 -> 4000 m by clouds6 2026-11-11 (A.4 P2, R-81, the mid field, form N):
                                                    the near march, re-marched every frame, carries the clouds the far tiles
                                                    tore on (M2's deck); device gate M2 f1405-2445 holes 7.1 -> 5.1 %, tile
                                                    edges 3778 -> 949 an image against 1500 (3000 m: 5.2 %, 2435); its far tiers
                                                    paid by the density grid below; the near-only art held by
                                                    ACZ_VR_CLOUD_NEAR_ART_M) */
    setenv("ACZ_VR_CLOUD_NEAR_ART_M", "0", 0);     /* (v1: with the 1500 m field the near-only art fades as on t9r) */  /* (clouds6: the near-only art -- the detail octave's fade -- ends by 0.9 x
                                                    1500 m whatever the range: the far map's look kept past it) */
    setenv("ACZ_VR_CLOUD_FAR_SLICE", "8", 0);    /* (the visible tiles' staggered refresh every 8 frames instead of 3; the
                                                    parallax limit still re-marches what moved: no patchwork in the dumps) */
    setenv("ACZ_VR_CLOUD_NEAR_MONO", "1", 0);    /* (the near march beyond 60 m once for both eyes: M4 near 4.6 -> 4.3 ms,
                                                    M3 3.7 -> 3.2 ms) */
    setenv("ACZ_VR_CLOUD_NEAR_MONO_VIS", "1", 0);   /* (R-49 / R-45a, outside note 5, 2026-11-10: the second eye takes the
                                                    first eye's far part only where the first eye marched; without it the
                                                    right eye's near clouds stopped at a fixed angle (an edge at 0.76 of its
                                                    width); device M3 f6600 at scale 0.92: App GPU p50 9.9 -> 10.1 ms) */
    setenv("PS2_RQ_DATE_FREE", "1", 0);          /* (R-45c / R-51 / R-51a-d: every DATE draw read-free. t8's 1 sent the lists
                                                    it could not express OFF the eye pass as hybrid frames (the user's flights);
                                                    t8a's 2 was the stopgap. Now PS2_RQ_KINDS keeps every tracked list in the
                                                    eye pass (a list it cannot express drawn as mode 2 inside it) and dfree4's
                                                    rules (the shadow, the mesh pairs, the sky-only haze) leave none: device
                                                    2026-11-11 t9k, M2 Rafale / M18 Su-37 / M4 sea start with intros: 0 lists
                                                    in mode 2, 0 off the pass, App raw 12.3 / 11.5 / 11.2 against 12.6 /
                                                    11.4 / 11.7 ms with 2; pictures equal) */
    /* (the clouds round of 2026-11-10, agent clouds3, PROMPT_2026-11-10_flight_bugs.md Part B; simulator proofs in NOTES_RQ 23:50,
       the device's in NOTES_RQ 2026-11-11 01:20) */
    setenv("PS2_RQ_CLOUDS_FARSWAP", "1", 0);     /* (B-1, R-52: the far tiles re-marched in sweeps from one origin: the square blocks
                                                    at cloud edges gone; M3 f2700 border / inside step 1.20-1.28 -> 1.03-1.17) */
    setenv("PS2_RQ_CLOUDS_FARWARP", "1", 0);     /* (B-1, R-52 on the device: FARSWAP alone left the tiles' staircase -- isolated
                                                    tile shards and a tile-sized zigzag at M6's cloud edge, a saw-tooth under M3's
                                                    cloud over the ridge; the corners moving with their texels make it one smooth
                                                    edge; App within the noise: M3 12.2 / 12.1, M6 11.7 / 12.1 ms raw) */
    setenv("PS2_RQ_ZPRE_AMASK", "2", 0);         /* (B-5, R-54: no false cockpit marks from the sun's overlays: the dark rings in
                                                    cloud facing the sun gone; M4 f2420 8 % of the pixels over 16 levels. 2, not 1:
                                                    on the headset no eye frame has the HUD batch's skipped fade quad (3000 of
                                                    3000 in M4), so =1 never drew and the rings stayed; =2 every eye frame: M4
                                                    f2900 the rings gone, the cockpit's own mark kept, App 12.5 / 12.5 ms raw) */
    setenv("PS2_RQ_PREWARM", "1", 0);            /* (R-64, 2026-11-11: the eye pass's objects at the session's start and its ~60 pipelines on a
                                                    worker thread before the first eye frame, instead of in the hand-over second from the intro
                                                    (VrApi FPS 37-45 / 72, 22-42 stale; 12 / 72 with glass3); numbers: NOTES_RQ R-64) */
    /* (rq4_water 2026-11-11, PROMPT_2026-11-11_flights2_fixes G.2, BUGS_RQ R-91: the user's M4 flight met an eye-pass state the
       device had never built, 243.7 ms on demand at field 3470 -- the list that ships with the APK and the states built on past
       the first eye frame; numbers: NOTES_RQ rq4_water) */
    setenv("PS2_RQ_PIPE_KEYS", "shaders/rq_pipelines.keys", 0);
    setenv("PS2_RQ_PREWARM_BG", "1", 0);
    /* (rq4_water 2026-11-11, the coordinator's fresh-install runs of t10a: the warm-up of the shipped list ended after the mission's
       first tracked list (7.4 s against 4.6 s), step A never ran and the first eye frame took 2.8 s -- the eye-pass states leave the
       warm-up at load, step A at the session's first frames, three workers until the first eye frame, and the first tracked list
       waits at most 2 s for them with the last layer shown; numbers: NOTES_RQ rq4_water round 2) */
    setenv("PS2_RQ_PREWARM_EARLY", "1", 0);
    setenv("PS2_RQ_PREWARM_THREADS", "3", 0);
    setenv("PS2_RQ_PREWARM_HOLD_MS", "2000", 0);
    setenv("PS2_CLOUDS_NEAR_PAR", "4", 0);       /* (the near clouds' 6 march pipelines on 4 threads: 9.0 s of a fresh boot on one) */
    setenv("PS2_RQ_CLOUDS_COLD", "256", 0);      /* (a new far-map generation: at most 128 tiles in view a frame instead of the
                                                    whole map in one frame: 508 tiles at once in M3's simulator run; 256 with
                                                    the budget of 256 below, clouds6) */
    /* (agent clouds4 2026-11-11, simulator-proven; the device proof NOTES_RQ 2026-11-11 05:45, BUGS_RQ R-55 / R-67) */
    setenv("PS2_RQ_CLOUDS_FXAT", "1", 0);        /* (C4: the effects before the game's own cockpit meshes no longer go under our
                                                    composite: device M4 F-15C f5000-5100 a burning enemy's smoke trail and fire
                                                    drawn, gone without; App 12.6 / 12.5 ms raw with FXFADE) */
    setenv("PS2_RQ_CLOUDS_FXFADE", "1", 0);      /* (B-4: the effects fade per vertex by our cloud in front of them, the PC's
                                                    mapping; device cost within the noise) */
    /* (PS2_RQ_CLOUDS_OPTICS, B-6, stays off: the glory is proven against the PC on the simulator, the device showed no
       scene with it in view (the F-5E's sill hides the antisolar point) and +0.2 ms; the user's A/B in the morning report) */
    /* (agent clouds5 2026-11-11, R-70, simulator-proven; the device proof NOTES_RQ 2026-11-11 08:40): the far tiles' mosaic --
       FARSWAP's sweeps cut at 4096 tiles missed the same tiles every sweep (the dark diamonds at M1 / M2's zenith), tiles came
       into view kilometres stale (M4's walls and blocks), and the corner warp folded under a sweep's parallax (M2's deck, M3's
       ridge). The same 64 marches a frame; far update 0.0903 / 0.0904 ms with / without on the simulator; +1.7 MB of staging. */
    setenv("PS2_RQ_CLOUDS_FARFAIR", "2", 0);
    setenv("PS2_RQ_CLOUDS_FARSWAP_SLOTS", "5760", 0);
    setenv("PS2_RQ_CLOUDS_FARLEAD", "1.5", 0);   /* (the separable part: M2's deck and M3's ridge better with it, M4's head turn mixed) */
    /* (agent clouds6 2026-11-11, PROMPT_2026-11-11_flights2_fixes.md A.4 / A.5, BUGS_RQ R-80 to R-84, NOTES_RQ clouds6: the far
       tiles' mosaic (R-80) was tearing between march origins under FARWARP's corner warp plus tiles shown before their first
       march; the list's bodies vanished by their centre's distance (R-83). Numbers: NOTES_RQ clouds6 2026-11-11 build) */
    /* (ACZ_VR_CLOUD_LGRID_DENS, P3, stays OFF on the Quest -- the device's prices of 2026-11-11 (t10d / b5d7cb1, M4 at a fixed 0.85):
       it does not pay on the Adreno, App 0.2 ms cheaper without it; with it off the light grid is off too (ACZ_VR_CLOUD_LGRID unset):
       the near march takes its own probe and cast shadow, as in t9r) */
    setenv("ACZ_VR_CLOUD_NEAR_RANGE_COCKPIT", "0", 0);     /* (v1: one range for every view, ACZ_VR_CLOUD_NEAR_RANGE) */   /* (the near range in the cockpit view; ACZ_VR_CLOUD_NEAR_RANGE (4000 above)
                                                    outside it: the device's price of the near march to 4000 m was +0.9-1.0 ms
                                                    (its bracket M4 1.18 -> 2.04); with the per-pixel resolve the far map holds
                                                    the cockpit's 1.5-4 km clouds, crisper; third person keeps 4000. Device M4
                                                    fixed 0.85, same batch: T9 10.3 / 16.6, candidate 11.8 / 15.0, this + DENS
                                                    off 11.4 / 15.1 ms p50 / p95) */
    setenv("PS2_RQ_CLOUDS_FARONE", "1", 0);      /* (P1a: the never-marched and outside tiles marched from the shown sweep's origin) */
    setenv("PS2_RQ_CLOUDS_FARRESOLVE", "0", 0);  /* (v1: the corner warp from one offset (FARWARP_ONE) instead of the per-pixel resolve: the resolve and the draw-all cost 0.4-0.8 ms) */  /* (P1b: the far tiles unmoved, their parallax resolved per pixel against the map's
                                                    distance; FARWARP above retired with it; 2 = every marched tile in view drawn:
                                                    with one ring (1) third person's offsets cut the moved content in teeth along
                                                    the horizon, M10 f12125-12365) */
    setenv("PS2_RQ_CLOUDS_FARCOARSE", "1", 0);   /* (P1c: a new generation's coarse level, one ray per tile corner in its first
                                                    frame, fills the never-marched tiles in view until their own march) */
    setenv("ACZ_VR_CLOUD_RQ_TILES", "256", 0);   /* (64 -> 256 tiles a frame, the sweeps 4x shorter; the device's far bracket ~1.0 ms
                                                    with 64, 128 or 256) */
    setenv("ACZ_VR_CLOUD_LIST_HOLD", "1", 0);    /* (P4, R-83: a body the game drops fades by its own surface's distance, 0 GPU) */
    /* (v1, the coordinator 2026-11-12 03:30, prompt 6 step 7 -- the device at PLS 0, M4 at a fixed 0.85: t9 10.2-10.9 ms, v1
       11.2-11.8 (the cloud bracket 1.05 -> 1.92), heavy 12.1-12.5; the zenith mosaic of t9 gone on the device (M2 f1660-1800),
       in-run pops at or under t9 (M11 13 against 23), the deck slabs and the third-person shards of L4 gone on the simulator;
       NOTES_RQ coordinator 2026-11-12) */
    setenv("PS2_RQ_CLOUDS_FARWARP_ONE", "1", 0); /* (the corner warp takes one offset for every tile: the quads stay watertight) */
    setenv("PS2_RQ_CLOUDS_FARSWAP_MAX", "30", 0); /* (sweeps at most 30 frames: the shown origin never falls 0.8-1.5 km behind; the M2 deck) */
    /* (clouds7 2026-11-12, prompt 8 steps 1-2 -> t11a; numbers in NOTES_RQ clouds7, rows R-104..R-108) */
    setenv("ACZ_VR_CLOUD_STATIC", "1", 0);       /* (step 1, R-104: the list by each body's place, the coverage opacity alone; the game's far
                                                    fade per sample in both marches: no body enters, leaves or ramps inside the far range) */
    setenv("PS2_RQ_CLOUDS_NEAR_CKPIT", "1", 0);  /* (R-105: the cockpit band no longer clips the near march; the eye pass's depth cuts the
                                                    cloud at the cockpit: no stair of near texels along the canopy bow, W-1) */
    /* (PS2_RQ_CLOUDS_NEAR_PROBE stays 0 since t11b, R-106: the interpolated probe cost the near pass 0.4-0.6 ms in M10's heavy part on the
       device -- t11a2's absence rows at 0.85, 11-12 tape -- with no visible gain found in step 0) */
    setenv("PS2_RQ_CLOUDS_NEAR_SOFT", "1", 0);   /* (R-108: the near envelope prefiltered by the plane spacing: softer near clouds, the comb
                                                    streaks of thin features between the planes smoothed; "soft and still beats crisp and crawling") */
    /* (clouds7 2026-11-12, t11a2: the static field's price; numbers in NOTES_RQ clouds7 t11a2, rows R-114..R-117) */
    setenv("ACZ_VR_CLOUD_STATIC_FADE", "1", 0);  /* (R-116: the game's fade by the body's centre distance kept, continuous, no threshold;
                                                    LIST_HOLD off for that list: the near pass no longer marches parts the game never showed) */
    setenv("PS2_RQ_CLOUDS_SPLAT_EVERY", "30", 0); /* (R-114: a static list's small changes re-splat at most every 30 frames, pending, the last
                                                    state always lands; they no longer restart the far map's full refresh) */
    setenv("PS2_RQ_CLOUDS_SPLAT_CULL", "1", 0);  /* (R-115: the splat per 8 x 8 texels over the clouds that reach them; the same map) */
    setenv("PS2_RQ_CLOUDS_SPLAT_FLOOR", "0.1", 0); /* (R-117 / R-119: a body's coverage from opacity 0.1, the game's own drop: the bodies it
                                                    does not show leave the occupancy and the near gate -- t11a2 had 0.05: M4's big faint
                                                    bodies kept the near pass marching, 2.4 ms against t10i's 0.78) */
    /* (clouds7 t11b, prompt 8 step 3, R-113 / R-118: the far map's parallax per texel into a shown map, the sweeps kept, the near fade-out
       moved to each far tile's band; FARFAIR's level 2 acts as 1 with it. Simulator: gate 1 p99 13 -> 1 (R-111 gone), gate 2 within
       FARALL's floor, the 36 s stairs gone, +0.03 ms of frame GPU; absence row: PS2_RQ_CLOUDS_FARSHOWN=0 = t11a2's far) */
    setenv("PS2_RQ_CLOUDS_FARSHOWN", "1", 0);
    setenv("PS2_RQ_CLOUDS_FARSHOWN_SWEEPS", "1", 0);
    setenv("PS2_RQ_CLOUDS_FARSHOWN_HANDOVER", "0", 0);   /* (2026-11-12 21:00, R-118: the coordinator's device rows on t11b, the user's M10
                                                    tape: 33 s under 60 fps with the handover, 6 s without -- it cost the third-person frames;
                                                    clouds7 is making it cheaper) */
    setenv("PS2_RQ_CLOUDS_BAKE", "1", 0);        /* (prompt 9 steps 2-3, rq7_bake, R-131; the device 2026-11-14, the user's M10 tape at a
                                                    fixed 0.85: the far bracket 1.28 ms at 512^2 against t11b2's 2.02 -- every far texel
                                                    in view marched every frame from the eye, from volumes baked once; no sweeps, no
                                                    history: the block staircase at the cloud edges gone. FAR CLOUD MAP's Quest default
                                                    is 512 with it (ps2_settings.c quest_far_clouds); 768^2 costs 1.95 ms) */
    setenv("PS2_RQ_CLOUDS_ONE", "2", 0);         /* (prompt 10 steps 2-3, rq8_onelayer, R-136..R-151; t13a 2026-11-15: ONE cloud layer -- the baked
                                                    march from the eye, no procedural near march, no 1500 m hand-over -- over a static
                                                    field (rule 3: whole bodies at their peak up to the game's mean amount; no body fades
                                                    in flight), the thin step in cloud (_THIN). Device, the M10 tape of the user's 11-14
                                                    flight at a fixed 0.85, one path: clouds 2.34 ms against t12b's 4.39, 2 s under 60
                                                    against 6; M4 through a body 2.89 ms) */
    /* (prompt 11 Part A, rq10_artefacts, R-156..R-160; t13b 2026-11-15: the artefacts of the user's t13a flight; device look rows on
       the user's tape after its RETRY, `a private script`; the shipping row with all four 6 s under 60 against 8) */
    setenv("PS2_RQ_CLOUDS_FOLD", "2", 0);        /* (t13c: =2, the corner's own octahedron face, exact at any map size; device look = 1, price 0) */
                                                 /* (t13b: =1, A1, R-156: a tile corner's parallax shift capped at 4 texels: the flat wedges / sheets
                                                    at aircraft were the octahedral map's mirrored border stretched by a tile) */
    setenv("PS2_RQ_CLOUDS_SUN_T", "1", 0);       /* (A4, R-159: the sun's glare scaled by the cloud map's transmittance toward the sun) */
    setenv("PS2_RQ_TFADE", "1", 0);              /* (t13c; prompt 10 step 5, rq9_terrain, R-152..R-155: the far terrain fades into the dome's haze
                                                    colour from 0.75 x fog far: M10's ridges arrive out of the haze instead of the game's
                                                    8-step ring; device price 0 on the M10 tape, the M2 horizon unchanged) */
    setenv("PS2_RQ_TREE_AA", "26", 0);           /* (t13l; R-242, rq14_small treemip: one trilinear tap from a mip chain of the
                                                    impostor + the coverage mask on the near cards; device 2026-11-16 15:54-16:21 on the
                                                    M2 tape's map B, two rows each: p50 11.72 / 11.42 vs TREE_AA=0 11.68 / 11.65 ms,
                                                    s under 60 of 80: 14 / 12 vs 8 / 6 (11: 24 / 21), the build 0.12 ms mean; the look
                                                    as 11: soft card edges, no speckled crosses; simulator popping 3.87 -> 0.47 %) */
    /* t13i measured 2026-11-16 12:04-12:42 and NOT a default: the box read costs on the device (BUGS_RQ R-241, NOTES_RQ the same
       day 12:45): the line was
    setenv("PS2_RQ_TREE_AA", "11", 0); */        /* (t13i; R-241: 3 + bit 8, the Adreno-safe tree modules -- the near cards keep the box read and the
                                                    coverage mask without the second blend source, the far cards the box read and the
                                                    blend without the mask; s1_tree8_m11 2026-11-16 07:03: both card paths drawn on the
                                                    device, near edges as the simulator's 3) */
    /* t13g: PS2_RQ_TREE_AA=3 is NOT a Quest default any more -- on the device the tree cards drew invisible with it (BUGS_RQ R-241,
       s1_t13f_look 2026-11-16: no card in either eye at field 12420, every card with TREE_AA=0); t13e / t13f's line was:
    setenv("PS2_RQ_TREE_AA", "3", 0);  */      /* (t13e; prompt 12 Part B, rq14_small, BUGS_RQ R-185 / R-233: the game's tree cards read their
                                                    128 x 128 impostor box-filtered over the pixel's footprint and take the alpha test as
                                                    coverage of the 4 samples; on a frozen scene panned by 0.2 eye px the trees' popping
                                                    pixels (over 16 levels) 4.30 % -> 0.72 %, residual 3.18 -> 1.74 levels against the
                                                    terrain's 0.66; under PS2_RQ_CLOUDS_CUT its twin cut_rq_gs_tree.frag keeps the cards'
                                                    flat cloud) */
    setenv("PS2_RQ_CLOUDS_CUT", "1", 0);         /* (t13d; prompt 12 Part A, rq13_cut cut2, R-190..R-197: the object applies the cloud in front of it
                                                    from its own distance -- terrain and natives per fragment, the game's cards flat --
                                                    and the cloud layer draws only where nothing stands, with no scene-depth read: the
                                                    ridges' and aircraft's 1x stairs gone; the band test skips the cloud code where no
                                                    cloud can lie between eye and object) */
    setenv("PS2_RQ_CLOUDS_THIN_MAX", "150", 0);  /* (rq20_edges 2026-11-16, the user's F-2 on t13h, BUGS_RQ R-270: the far march's thin-medium step
                                                    at most 150 m, never under the fine step -- beyond ~1.1 km it grew to 3 fine steps,
                                                    150-800 m, and each map texel's one or two long samples hit or missed the faint fringe:
                                                    the "pattern", dark notches along the outline and a scaly grain over far bodies. The
                                                    term cut on the user's tape: the pattern goes with the thin step alone; within 1.1 km
                                                    nothing changes. Simulator, in-run A/B: far update +6 / +20 / +17 % on the gate's
                                                    M10 / M4 / M11 (the faint veils of M4 and M11); PS2_RQ_CLOUDS_THIN_MAX_T the fallback) */
    /* (PS2_RQ_CLOUDS_SOFT_CUT (A2, R-157) and PS2_RQ_GSHADOW_FILTER=160 (A5, R-160) stay off: they ship only where a crop shows
       the gain (the review, 2026-11-15); their scenes are on the 11-14 M10 tape, being checked) */
    setenv("PS2_XR_LAYER_FILTER", "quality", 0);   /* (Meta Quest Super Resolution on our layers: the compositor's edge-aware
                                                    upscale of the 512 x 448 eye images instead of bilinear; TW 0.53 -> 0.77 ms) */
    setenv("PS2_XR_QUAD_FILTER", "off", 0);      /* (rq19_sparkle, BUGS_RQ R-260, the user's F-3 "sparkles in the menu when I turn my head":
                                                    the cinema quad -- menus, briefing map, hangar, loading screens -- without
                                                    PS2_XR_LAYER_FILTER's Super Resolution: the compositor's plain bilinear on a 512 x 448
                                                    image magnified ~3x; the eye layer keeps quality. docs/rq/SPARKLE.md: the model) */
    setenv("PS2_RQ_MAP_HOST", "1", 0);           /* (P-5, R-66: the render thread never reads the mapped buffers back -- the Quest's
                                                    host-visible type 4 is uncached; the same bytes reach the GPU; NOTES_RQ P-5) */
    setenv("PS2_RQ_CPU_LOG", "1", 0);            /* (P-5: the `rq cpu:` line, the render thread's busy ms p50 / p95 / max per 600 fields) */
    /* (ACZ_QUEST_RECORD 1 -- the in-app recorder, ps2_qrec_android.c: off unless RECORD MY SESSIONS is on, the voice only with ACZ_QUEST_RECORD_MIC=1; adb screenrecord
       and Meta's own recorder hang the game's GPU, R-37 / R-38 --: the Quest default of its ENHANCEMENTS row, SYSTEM > RECORD MY
       SESSIONS, since rq2_menu; ps2_enh_boot below sets it, before the recorder reads it; 0 = off) */
    /* (the input and lifecycle round of 2026-11-11, agent rq4_input, PROMPT_2026-11-11_flights2_fixes.md Part B and G.1 / G.6 / G.7;
       BUGS_RQ R-76 to R-79, NOTES_RQ 2026-11-11 input) */
    setenv("PS2_XR_SELECT_HOLD_MS", "1000", 0);  /* (R-76: View held 1.0 s by the clock recentres; every shorter press is one
                                                    Select tap of 4 fields -- the M18 weapon change lost in the old filter's
                                                    0.25-0.5 s dead band; host test tools/quest/selhold_test.c) */
    setenv("PS2_XR_BG_PAUSE", "1", 0);           /* (R-77: the session STOPPING / IDLE or the activity stopped: the game held, no
                                                    fields, and muted; M18 ran on for two minutes behind HOME) */
    setenv("ACZ_PERF_HINT", "0", 0);             /* (R-78: the PC's "lower QUALITY PRESET / Meta Quest Link app" line; the Quest
                                                    has one tuned frame and its page never shows the hint, ENH_QUEST) */
    setenv("PS2_XR_MONO_SETTLE", "90", 0);       /* (R-79: the mono fallback's trigger off with multiview, where it cannot act; its
                                                    first-frame arming at 49.7-56.9 ms in all six flights gone) */
    setenv("ACZ_LAUNCHER", "0", 0);
    setenv("ACZ_TELEMETRY", "off", 0);
    setenv("ACZ_STATS", "0", 0);
    char t[600];                                 /* the texture pack in the data root (texrep: ACZ_TEX_DIR) */
    snprintf(t, sizeof t, "%s/textures", g_data);
    if (access(t, R_OK | X_OK) == 0) setenv("ACZ_TEX_DIR", t, 0);
    const char *pre = getenv("ACZQ_PRESET");
    if (!pre || !*pre || !strcmp(pre, "faithful")) {
        int n = 0;
        for (size_t i = 0; i < sizeof preset_faithful / sizeof preset_faithful[0]; i++)
            if (!getenv(preset_faithful[i][0])) { setenv(preset_faithful[i][0], preset_faithful[i][1], 0); n++; }
        alog(ANDROID_LOG_INFO, "preset faithful (ACZQ_PRESET): %d defaults set, %d given by commandline.txt", n,
             (int)(sizeof preset_faithful / sizeof preset_faithful[0]) - n);
    } else alog(ANDROID_LOG_INFO, "preset %s (ACZQ_PRESET): the runtime's own defaults", pre);
    /* The ENHANCEMENTS page (rq2_menu, docs/SETTINGS.md "The Quest"): its saved rows set their switches now, as the PC's
       settings constructor does before main() -- after commandline.txt and the lines above, so a line of commandline.txt still
       wins (its row shows locked), and each row the lines above no longer set takes its Quest default or the saved value from
       files/ac5_settings.ini. ACZ_MENU_SETTINGS=0 in commandline.txt: the file is not applied, every row at its Quest default. */
    setenv("ACZ_MENU_SETTINGS", "1", 0);
    /* Gamepads (2026-11-10 night): PadActivity's key and motion events reach the game (Xbox, DualSense / DualShock 4);
       without this a paired pad did nothing. A HOTAS stays opt-in (ACZ_QUEST_JOY=1): the Touch controllers' own Android
       input device lists as a joystick and must not become one. */
    setenv("ACZ_QUEST_PAD", "1", 0);
    /* Touch flies as a gamepad unless the player chooses HANDS ON STICK (BUGS_RQ R-312, 2026-11-18): with the PC's default a
       hand closing at the stick or the lever took it and the session turned HANDS ON STICK by itself -- the two players of the
       public thread who flew with Touch: "wonky with grabbing the stick", "impossible to fly with the quest controllers"; the
       owner: "clearly a work-in-progress mode, weakly playable for me too". AUTO here = a pad while no gamepad flies, silent
       beside one, and it never takes the controls; ENHANCEMENTS > CONTROLS > TOUCH CONTROLLERS: HANDS ON STICK is the way in. */
    setenv("ACZ_TOUCH_AUTO_GRAB", "0", 0);
    ps2_enh_boot();
}

/* ---- the EE thread ---- */
static void *game_thread(void *arg) {
    (void)arg;
    g_ee_tid = (int)syscall(SYS_gettid);
    static char a0[] = "libmain.so";
    char *argv[72];
    int argc = 0;
    argv[argc++] = a0;
    for (int i = 0; i < g_nargs && argc < 70; i++) argv[argc++] = g_args[i];
    argv[argc] = NULL;
    {   char line[1024];
        int o = snprintf(line, sizeof line, "runtime main:");
        for (int i = 1; i < argc && o < (int)sizeof line - 2; i++) o += snprintf(line + o, sizeof line - (size_t)o, " %s", argv[i]);
        alog(ANDROID_LOG_INFO, "%s", line);
    }
    int rc = ps2_runtime_main(argc, argv);
    ps2_qrec_stop("the runtime returned");                /* (ACZ_QUEST_RECORD: the file finished before result.txt) */
    fflush(NULL);
    ps2_android_result("ok", rc);
    __atomic_store_n(&g_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void on_cmd(struct android_app *app, int32_t cmd) {
    ps2_pad_android_cmd(app, cmd);
    switch (cmd) {
    case APP_CMD_DESTROY:
        __atomic_store_n(&g_quit, 1, __ATOMIC_RELEASE);
        alog(ANDROID_LOG_INFO, "activity: destroy");
        break;
    case APP_CMD_PAUSE: alog(ANDROID_LOG_INFO, "activity: pause"); ps2_qrec_pause(1); break;   /* (the recording's file finished) */
    case APP_CMD_RESUME: alog(ANDROID_LOG_INFO, "activity: resume"); ps2_qrec_pause(0); break; /* (a new file) */
    case APP_CMD_STOP:                          /* (PS2_XR_BG_PAUSE, R-77: the activity stopped = the background, ps2_xr.c) */
    case APP_CMD_START: {
        extern void ps2_xr_bg_activity(int stopped);
        alog(ANDROID_LOG_INFO, "activity: %s", cmd == APP_CMD_STOP ? "stop" : "start");
        ps2_xr_bg_activity(cmd == APP_CMD_STOP);
        break;
    }
    default: break;
    }
}

static void at_exit(void) { ps2_qrec_stop("exit"); ps2_android_result("exit", 0); }

void android_main(struct android_app *app) {
    g_app = app;
    app->onAppCmd = on_cmd;
    clock_gettime(CLOCK_MONOTONIC, &g_t0);
    paths_init();
    log_start();
    unpack_assets("shaders");
    {   char p[700];
        snprintf(p, sizeof p, "%s/commandline.txt", g_data);
        if (apply_file(p) < 0) alog(ANDROID_LOG_INFO, "commandline: no %s", p);
        char name[128];
        if (intent_string("script", name, sizeof name) && name[0] && !strchr(name, '/')) {
            snprintf(p, sizeof p, "%s/scripts/%s.txt", g_data, name);
            if (apply_file(p) < 0) alog(ANDROID_LOG_WARN, "commandline: script '%s': no %s", name, p);
        }
    }
    defaults();
    /* ACZ_QUEST_RECORD: the recorder's thread now (it opens the first file at once); the microphone's permission asked for
       here when the app does not hold it (adb install -g, or adb shell pm grant, gives it without the dialog) */
    if (ps2_qrec_wanted()) {
        if (ps2_qrec_mic_wanted() && !ps2_android_permission("android.permission.RECORD_AUDIO", 1))
            alog(ANDROID_LOG_INFO, "qrec: no RECORD_AUDIO yet: asked; the microphone track starts when it is granted");
        ps2_qrec_start();
    }
    ps2_pad_android_install(app);   /* (ACZ_QUEST_PAD_LOG, ps2_pad_android.c: off by default) */
    /* --data / --disc / --watchdog unless commandline.txt's ARGS gave them */
    {   int has_data = 0, has_disc = 0, has_wd = 0;
        for (int i = 0; i < g_nargs; i++) {
            if (!strcmp(g_args[i], "--data")) has_data = 1;
            if (!strcmp(g_args[i], "--disc")) has_disc = 1;
            if (!strcmp(g_args[i], "--watchdog")) has_wd = 1;
        }
        static char data_arg[600], disc_arg[600];
        if (!has_data) {
            snprintf(data_arg, sizeof data_arg, "%s/gen_zero", g_data);
            g_args[g_nargs++] = (char *)"--data"; g_args[g_nargs++] = data_arg;
        }
        if (!has_disc) {
            snprintf(disc_arg, sizeof disc_arg, "%s/disc.iso", g_data);
            if (access(disc_arg, R_OK) != 0) snprintf(disc_arg, sizeof disc_arg, "%s/disc", g_data);
            g_args[g_nargs++] = (char *)"--disc"; g_args[g_nargs++] = disc_arg;
        }
        if (!has_wd) { g_args[g_nargs++] = (char *)"--watchdog"; g_args[g_nargs++] = (char *)"0"; }
    }
    printf("aczquest: files %s, data %s\n", g_files, g_data);
    atexit(at_exit);

    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64u << 20);
    pthread_t th;
    if (pthread_create(&th, &at, game_thread, NULL) != 0) {
        alog(ANDROID_LOG_ERROR, "no EE thread");
        ps2_android_result("crash", -1);
        ANativeActivity_finish(app->activity);
    }
    pthread_attr_destroy(&at);

    int finishing = 0;
    while (!app->destroyRequested) {
        int events;
        struct android_poll_source *src = NULL;
        int r = ALooper_pollOnce(100, NULL, &events, (void **)&src);
        if (r >= 0 && src) src->process(app, src);
        ps2_pad_android_tick(app);
        if (!finishing && __atomic_load_n(&g_done, __ATOMIC_ACQUIRE)) {
            finishing = 1;
            alog(ANDROID_LOG_INFO, "the runtime returned: finishing the activity");
            ANativeActivity_finish(app->activity);
        }
        if (__atomic_load_n(&g_quit, __ATOMIC_ACQUIRE) && !finishing) {
            extern void ps2_quit_request(const char *why);
            ps2_quit_request("the activity is being destroyed");
            finishing = 1;
        }
    }
    /* The activity is gone. A quit the game was asked for (above) waits for the memory card and ends the process itself
       (ps2_quit_exit -> result.txt "quit"); give it up to ACZQ_QUIT_WAIT_S (10) seconds, then end it here. The glue tears
       its thread down after android_main returns; the runtime's threads end with the process. */
    if (!__atomic_load_n(&g_done, __ATOMIC_ACQUIRE)) {
        extern void ps2_quit_request(const char *why);
        ps2_quit_request("the activity was destroyed");
        const char *w = getenv("ACZQ_QUIT_WAIT_S");
        int waits = (w && *w ? atoi(w) : 10) * 10;
        for (int i = 0; i < waits && !__atomic_load_n(&g_done, __ATOMIC_ACQUIRE); i++) usleep(100000);
        ps2_android_result("destroyed", 0);
    }
    ps2_qrec_stop("the activity was destroyed");          /* (ACZ_QUEST_RECORD: no-op when the quit path finished it) */
    fflush(NULL);
    _exit(0);
}
