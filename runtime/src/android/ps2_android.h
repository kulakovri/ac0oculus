/* ps2_android.h -- the Quest 3 standalone build's platform layer (ps2_android.c; NOTES_QUEST.md).

   android_main (NativeActivity through the NDK's native_app_glue) prepares the process the way the Windows
   launchers do and then runs the runtime's main() (ps2_main.c, compiled as ps2_runtime_main) on its own big-stack
   thread, the EE thread. Paths:
     data root   /sdcard/ac0oculus/ (ACZQ_DATA overrides; the per-app files dir if it is not readable): disc.iso or
                 disc/, gen_zero/ps2_image.bin, textures/, commandline.txt, scripts/<name>.txt
     files dir   /sdcard/Android/data/<package>/files/ (the process's working directory: saves, ac5_settings.ini,
                 out/ dumps, aczquest.log, result.txt, shaders/ unpacked from the APK) */
#ifndef PS2_ANDROID_H
#define PS2_ANDROID_H

#ifdef __cplusplus
extern "C" {
#endif

void *ps2_android_vm(void);              /* JavaVM * (xrInitializeLoaderKHR, XrInstanceCreateInfoAndroidKHR) */
void *ps2_android_activity(void);        /* the NativeActivity's jobject (a global reference) */
const char *ps2_android_files_dir(void); /* no trailing slash */
const char *ps2_android_data_dir(void);  /* no trailing slash */
int  ps2_android_ee_tid(void);           /* the EE (runtime main) thread's kernel id, 0 before it started */
/* result.txt in the files dir (first call wins): status ok / quit / crash / exit, the code, fields and seconds run */
void ps2_android_result(const char *status, int code);
/* the activity is being destroyed or the system asked the app to finish: 1 once seen (any thread) */
int  ps2_android_should_quit(void);
/* 1 = the app holds the runtime permission `perm` (e.g. "android.permission.RECORD_AUDIO"); request = 1: ask for it when
   it does not (Activity.requestPermissions through JNI: the headset shows its dialog). Any thread. */
int  ps2_android_permission(const char *perm, int request);

/* ACZ_QUEST_RECORD, the in-app recorder (ps2_qrec_android.c; the GPU half is runtime/src/rq/rq_rec.inc) */
int  ps2_qrec_wanted(void);              /* ACZ_QUEST_RECORD=1 (after the defaults are set) */
int  ps2_qrec_mic_wanted(void);          /* ... and ACZ_QUEST_RECORD_MIC is not 0 */
void ps2_qrec_start(void);               /* the recorder's thread (android_main, before the runtime starts) */
void ps2_qrec_pause(int paused);         /* APP_CMD_PAUSE (1: the file is finished) / APP_CMD_RESUME (0: a new file) */
void ps2_qrec_stop(const char *why);     /* the end of the run: the file finished (waits up to 6 s); idempotent */
void ps2_qrec_game_pcm(const void *pcm, int frames, int channels, int rate);   /* the game's mix as it is played */

/* the gamepad (ps2_pad_android.c): ACZ_QUEST_PAD_LOG, called from android_main on the glue's thread */
struct android_app;
void ps2_pad_android_install(struct android_app *app);
void ps2_pad_android_cmd(struct android_app *app, int cmd);
void ps2_pad_android_tick(struct android_app *app);

/* the displays (ps2_display_android.c, BUGS_RQ R-37: a capture's mirror is a virtual display): ACZ_QUEST_CAPTURE_WATCH;
   the events from PadActivity's DisplayListener (what: 0 there at the start, 1 added, 2 removed, 3 changed; state =
   Display.getState(), -1 no access; the mode's size and rate); the GPU hang report's line "capture: <name> changed to ON
   <s> s before the hang" / "capture: <name> (<s> s before the hang ...)" or "capture: none seen" (any thread; returns its
   length) */
int  ps2_display_android_watch(void);
void ps2_display_android_started(void);
void ps2_display_android_event(int what, int id, const char *name, int flags, int state, int w, int h, float hz);
int  ps2_android_capture_note(char *out, int cap);

#ifdef __cplusplus
}
#endif
#endif
