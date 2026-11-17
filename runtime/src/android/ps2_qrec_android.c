/* ps2_qrec_android.c -- ACZ_QUEST_RECORD: the Quest build's in-app recorder (NOTES_RQ.md "in-app recorder"; BUGS_RQ R-37,
   R-38: `adb shell screenrecord` and Meta's own recorder both hang the game's GPU, so the recording is the app's own).
   ACZ_QUEST_RECORD=1 (ENHANCEMENTS > SYSTEM > RECORD MY SESSIONS) turns it on, off by default; 0 = nothing of this runs: no thread,
   no encoder, no GPU work, the device created as before.

   Files: one MP4 per session -- from the start (or a resume) to a pause (the headset taken off, the app sent to the
   background), the quit, or the system menu's close -- in <files>/rec/ (ACZ_QUEST_RECORD_DIR; a relative path is relative
   to the files dir), named aczq_<local date>_<time>.mp4. It is written as NAME.mp4.part and renamed once the muxer has
   stopped (the moov atom written): a .part file is a session that did not end cleanly (a crash, a kill) and is lost.
   Tracks, always in this order: video (H.264; ACZ_QUEST_RECORD_CODEC=hevc: HEVC), the game's mix (AAC-LC 48 kHz stereo,
   192 kbit/s), the microphone (AAC-LC mono, 96 kbit/s; ACZ_QUEST_RECORD_MIC=0: no such track).

   Video: the renderer (runtime/src/rq/rq_rec.inc) converts a W x H picture to NV12 on the GPU straight into one of
   ACZ_QUEST_RECORD_BUFFERS host buffers, tagged with the game frame's publication time; after the frame's fence it is
   queued here, and this file's thread copies it into a MediaCodec input buffer (the encoder's stride and slice height)
   with the presentation time = tag - the session's first tag. The renderer never waits: a picture without a free buffer
   is dropped and counted. ACZ_QUEST_RECORD_VIDEO=0: no video track and no GPU work at all (the A/B for the GPU hang):
   the file holds the two audio tracks.
   Audio: the game's mix is tapped in the AAudio output callback (ps2_sdl_android.c: the bytes the device plays), the
   microphone comes from an AAudio input stream (RECORD_AUDIO: requested at start by ps2_android.c; `adb install -g`
   grants it); each chunk carries the monotonic time of its callback and is placed on the video's timeline as
   ACZ_REC_EXACT places the game track on the PC (ps2_recexact.c): silence where nothing came, a sample repeated or left
   out in 200 while the device's clock is more than 10 ms off, a hard resync beyond 100 ms.
   One thread does all of it (encoders, muxer, the microphone's stream); every 10 s one "qrec:" line. */
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaMuxer.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "ps2_android.h"

void ps2_log(const char *fmt, ...);

typedef unsigned long long u64;
typedef unsigned int u32;

#define QR_MAXB  16               /* host buffers at most (rq_rec.inc RQR_MAXB) */
#define QA_NCH   1024             /* audio chunks in a ring (~4 s of 192-frame callbacks) */
#define QA_F     512              /* frames per chunk at most */
#define QA_RATE  48000
#define QS_OFF 0
#define QS_IDLE 1                 /* no session: one opens unless paused */
#define QS_REC 2
#define QS_CLOSING 3
#define QS_HALT 4                 /* no space or no encoder: waits for the next resume */
#define QS_DONE 5                 /* stopped for good (the quit) */

static u64 mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

typedef struct { u64 t; int n; short d[QA_F * 2]; } qa_chunk;   /* t: the monotonic time of the chunk's last frame */
typedef struct qpend { struct qpend *next; AMediaCodecBufferInfo bi; unsigned char d[]; } qpend;
typedef struct {
    AMediaCodec *c; const char *what; char name[96];
    int track, eos_in, eos_out, on;
    AMediaFormat *ofmt;                  /* the output format (csd), from INFO_OUTPUT_FORMAT_CHANGED */
    qpend *ph, *pt; size_t pend_bytes;   /* samples before the muxer started */
    u64 out_bytes, out_n, errors;
} qcodec;
typedef struct {
    const char *name; int ch, rate;
    qa_chunk *ring; u32 w, r;            /* single producer (the callback), single consumer (the thread) */
    u64 ring_drop;
    long long pos;                       /* frames placed on the timeline (stage + fed) */
    int started, refill, corr; double ema; u64 dup, skip, sil, hard; u64 last_t;
    short *stage; int stage_n, stage_cap; u64 stage_over;
    long long fed;                       /* frames given to the encoder: its presentation time */
    double sq; u64 nsq; int peak;        /* the level of the last 10 s (real samples only) */
    qcodec cd;
} qtrack;

static struct {
    int cfg, on, video, vcpu, mic, fps, kbps, hevc, nbuf, min_free_mb, mic_preset, brmode, qpmax; u32 cw, ch; float zoom; char dir[600];
    pthread_mutex_t mu; pthread_cond_t cv, done_cv;
    pthread_t th; int th_ok, th_done;
    int state;                           /* QS_* (atomic reads by the renderer and the audio callbacks) */
    int paused, final; char why[120];
    /* the video frame size the encoder accepted (kept for the whole run: the GPU's buffers have it) */
    int vsize_ok; u32 vw, vh; int vfail, vfail_req; char vfail_why[96];
    /* host buffers (the renderer's) */
    int nb; unsigned char *ptr[QR_MAXB]; int slot[QR_MAXB]; u64 slot_t[QR_MAXB];   /* slot: 0 free, 1 GPU, 2 queued */
    int q[QR_MAXB]; int qn, qh;
    /* the session */
    int fd; char path[700], part[720]; AMediaMuxer *mux; int mux_started, mux_tracks;
    qcodec vc; int v_stride, v_slice, v_cf; size_t v_size;
    qtrack game, micx;
    AAudioStream *mic_as; int mic_lost, mic_said; u64 mic_next_try; int mic_rate_bad;
    u64 t0, open_ns, last_pts_us, frames_sess;
    /* statistics */
    u64 v_in, v_drop, v_busy, v_in_tot, v_drop_tot, cap_eye, cap_rt, syn_n, syn_next;
    double copy_ms, copy_max; u64 copy_n;
    long long lat_pts[64]; u64 lat_t[64]; u32 lat_i; double lat_sum, lat_max; u64 lat_n;
    u64 last_log;
    u64 vis_since, vis_last, resume_ns, settle_ns;   /* (R-47) the XR session shown since / last seen shown (the renderer's frames), the last resume */
    int open_wait_said;
    int v_on;                            /* the renderer may hand pictures in (atomic) */
} q = { .fd = -1 };

static int qa_ring_alloc(qtrack *a, const char *name, int ch, int rate) {
    a->name = name; a->ch = ch; a->rate = rate;
    if (!a->ring) a->ring = (qa_chunk *)calloc(QA_NCH, sizeof(qa_chunk));
    a->stage_cap = rate * 4;
    if (!a->stage) a->stage = (short *)malloc((size_t)a->stage_cap * 2u * sizeof(short));
    return a->ring && a->stage ? 0 : -1;
}

static void cfg(void) {
    if (q.cfg) return;
    q.cfg = 1;
    const char *e = getenv("ACZ_QUEST_RECORD");
    q.on = e && *e == '1';
    if (!q.on) return;
    e = getenv("ACZ_QUEST_RECORD_VIDEO"); q.video = !(e && *e == '0'); q.vcpu = e && !strcmp(e, "cpu");
    e = getenv("ACZ_QUEST_RECORD_MIC"); q.mic = e && *e == '1';
    q.cw = 1440; q.ch = 1080;
    e = getenv("ACZ_QUEST_RECORD_SIZE");
    if (e && *e) { unsigned a = 0, b = 0; if (sscanf(e, "%ux%u", &a, &b) == 2 && a >= 64 && b >= 64 && a <= 3840 && b <= 2160) { q.cw = a & ~15u; q.ch = b & ~1u; } }
    e = getenv("ACZ_QUEST_RECORD_FPS"); q.fps = e && *e ? atoi(e) : 30; if (q.fps < 5) q.fps = 5; if (q.fps > 60) q.fps = 60;
    e = getenv("ACZ_QUEST_RECORD_KBPS"); q.kbps = e && *e ? atoi(e) : 20000; if (q.kbps < 500) q.kbps = 500; if (q.kbps > 100000) q.kbps = 100000;
    e = getenv("ACZ_QUEST_RECORD_CODEC"); q.hevc = e && !strcmp(e, "hevc");
    /* (2026-11-15, the review: the flight videos ran at 5.6-6.4 Mbit/s of the 20,000 asked -- the encoder's VBR spends less on
       low-contrast content and flattened thin cloud into patches a review took for render artefacts) ACZ_QUEST_RECORD_MODE: vbr
       (default, as before), cbr (the asked bitrate held), cq (constant quality); ACZ_QUEST_RECORD_QPMAX: a ceiling on the quantiser
       ("video-qp-max", 0 = the encoder's own) */
    e = getenv("ACZ_QUEST_RECORD_MODE"); q.brmode = e && !strcmp(e, "cbr") ? 2 : e && !strcmp(e, "cq") ? 0 : 1;
    e = getenv("ACZ_QUEST_RECORD_QPMAX"); q.qpmax = e && *e ? atoi(e) : 0; if (q.qpmax < 0 || q.qpmax > 51) q.qpmax = 0;
    e = getenv("ACZ_QUEST_RECORD_BUFFERS"); q.nbuf = e && *e ? atoi(e) : 6; if (q.nbuf < 2) q.nbuf = 2; if (q.nbuf > QR_MAXB) q.nbuf = QR_MAXB;
    e = getenv("ACZ_QUEST_RECORD_ZOOM"); q.zoom = e && *e ? (float)atof(e) : 1.0f; if (!(q.zoom >= 0.3f && q.zoom <= 4.0f)) q.zoom = 1.0f;
    e = getenv("ACZ_QUEST_RECORD_SETTLE_MS"); { int ms = e && *e ? atoi(e) : 500; if (ms < 0) ms = 0; if (ms > 10000) ms = 10000; q.settle_ns = (u64)ms * 1000000ull; }
    e = getenv("ACZ_QUEST_RECORD_MIN_FREE_MB"); q.min_free_mb = e && *e ? atoi(e) : 2048; if (q.min_free_mb < 64) q.min_free_mb = 64;
    e = getenv("ACZ_QUEST_RECORD_MIC_PRESET"); q.mic_preset = 0;
    if (e && *e) {
        static const struct { const char *n; int v; } P[] = { { "generic", AAUDIO_INPUT_PRESET_GENERIC }, { "camcorder", AAUDIO_INPUT_PRESET_CAMCORDER },
            { "voice_recognition", AAUDIO_INPUT_PRESET_VOICE_RECOGNITION }, { "voice_communication", AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION },
            { "unprocessed", AAUDIO_INPUT_PRESET_UNPROCESSED }, { "voice_performance", 10 } };
        for (size_t i = 0; i < sizeof P / sizeof P[0]; i++) if (!strcmp(e, P[i].n)) q.mic_preset = P[i].v;
    }
    e = getenv("ACZ_QUEST_RECORD_DIR");
    const char *files = ps2_android_files_dir();
    if (e && *e == '/') snprintf(q.dir, sizeof q.dir, "%s", e);
    else snprintf(q.dir, sizeof q.dir, "%s/%s", files && *files ? files : ".", e && *e ? e : "rec");
    if (qa_ring_alloc(&q.game, "game", 2, QA_RATE) != 0 || qa_ring_alloc(&q.micx, "mic", 1, QA_RATE) != 0) {
        ps2_log("qrec: no memory for the audio rings: the recorder is off"); q.on = 0; return;
    }
    q.vc.what = "video"; q.game.cd.what = "game"; q.micx.cd.what = "mic";
    pthread_mutex_init(&q.mu, NULL);
    pthread_cond_init(&q.cv, NULL);
    pthread_cond_init(&q.done_cv, NULL);
}

/* ---- the renderer's side (rq_rec.inc) ---- */
static int st(void) { return __atomic_load_n(&q.state, __ATOMIC_ACQUIRE); }
static void set_st(int s) { __atomic_store_n(&q.state, s, __ATOMIC_RELEASE); }
int ps2_qrec_accepting(void) { return q.on && st() == QS_REC && __atomic_load_n(&q.v_on, __ATOMIC_ACQUIRE); }
int ps2_qrec_size(u32 *w, u32 *h) { if (!q.on || !q.video || q.vcpu || !q.vsize_ok) return 0; *w = q.vw; *h = q.vh; return 1; }
int ps2_qrec_fps(void) { return q.fps; }
float ps2_qrec_zoom(void) { return q.zoom; }
int ps2_qrec_nbuf(void) { return q.nbuf; }
void ps2_qrec_buffers(int n, unsigned char *const *ptrs, u32 w, u32 h) {
    if (!q.on) return;
    pthread_mutex_lock(&q.mu);
    q.nb = n > QR_MAXB ? QR_MAXB : n;
    for (int i = 0; i < q.nb; i++) { q.ptr[i] = ptrs[i]; q.slot[i] = 0; }
    q.qn = 0; q.qh = 0;
    if (w != q.vw || h != q.vh) { ps2_log("qrec: the GPU's buffers are %ux%u, the encoder's %ux%u: no video", w, h, q.vw, q.vh); q.vfail_req = 1; }
    pthread_mutex_unlock(&q.mu);
}
int ps2_qrec_acquire(void) {
    if (!ps2_qrec_accepting()) return -1;
    int got = -1;
    pthread_mutex_lock(&q.mu);
    if (q.nb <= 0) { pthread_mutex_unlock(&q.mu); return -1; }
    for (int i = 0; i < q.nb && got < 0; i++) if (q.slot[i] == 0) got = i;
    if (got >= 0) q.slot[got] = 1;
    else { q.v_drop++; q.v_drop_tot++; }
    pthread_mutex_unlock(&q.mu);
    return got;
}
void ps2_qrec_cancel(int s) {
    if (!q.on || s < 0 || s >= q.nb) return;
    pthread_mutex_lock(&q.mu); q.slot[s] = 0; pthread_mutex_unlock(&q.mu);
}
void ps2_qrec_ready(int s, u64 t_ns) {
    if (!q.on || s < 0 || s >= q.nb) return;
    pthread_mutex_lock(&q.mu);
    if (st() == QS_REC && q.qn < QR_MAXB) {
        q.slot[s] = 2; q.slot_t[s] = t_ns;
        q.q[(q.qh + q.qn) % QR_MAXB] = s; q.qn++;
        pthread_cond_signal(&q.cv);
    } else q.slot[s] = 0;                /* (closing: the picture is left out) */
    pthread_mutex_unlock(&q.mu);
}
/* every renderer frame (rq_rec.inc, at the frame's fence): is the XR session running and VISIBLE / FOCUSED */
void ps2_qrec_xr_visible(int vis) {
    if (!q.on) return;
    u64 now = mono_ns();
    if (vis) {
        if (!__atomic_load_n(&q.vis_since, __ATOMIC_ACQUIRE)) __atomic_store_n(&q.vis_since, now, __ATOMIC_RELEASE);
        __atomic_store_n(&q.vis_last, now, __ATOMIC_RELEASE);
    } else __atomic_store_n(&q.vis_since, 0, __ATOMIC_RELEASE);
}
/* R-47 (the headset taken off and put back hung the GPU with the recorder on: a new encoder was made while the XR session stopped and
   began again, and the capture ran in its first frames): a new file opens only while the renderer shows frames in a VISIBLE / FOCUSED
   session (one seen in the last 200 ms), and only once that has lasted ACZ_QUEST_RECORD_SETTLE_MS (500) both since the session became
   shown and since the last resume */
static int open_ok(u64 now) {
    u64 vs = __atomic_load_n(&q.vis_since, __ATOMIC_ACQUIRE), vl = __atomic_load_n(&q.vis_last, __ATOMIC_ACQUIRE);
    u64 rs = __atomic_load_n(&q.resume_ns, __ATOMIC_ACQUIRE);
    if (!vs || !vl || now - vl > 200000000ull) return 0;
    u64 start = vs > rs ? vs : rs;
    return now >= start && now - start >= q.settle_ns;
}
void ps2_qrec_note(int kind) { if (kind) __atomic_add_fetch(&q.cap_rt, 1, __ATOMIC_RELAXED); else __atomic_add_fetch(&q.cap_eye, 1, __ATOMIC_RELAXED); }
void ps2_qrec_video_failed(const char *why) {
    if (!q.on) return;
    pthread_mutex_lock(&q.mu);
    q.vfail_req = 1; snprintf(q.vfail_why, sizeof q.vfail_why, "%s", why ? why : "?");
    pthread_cond_signal(&q.cv);
    pthread_mutex_unlock(&q.mu);
}

/* ---- audio input: the game's mix (ps2_sdl_android.c's AAudio callback) and the microphone ---- */
static void ring_push(qtrack *a, const short *src, int frames, u64 t_end) {
    while (frames > 0) {
        int n = frames > QA_F ? QA_F : frames;
        u32 w = __atomic_load_n(&a->w, __ATOMIC_RELAXED), r = __atomic_load_n(&a->r, __ATOMIC_ACQUIRE);
        if (w - r >= QA_NCH) { a->ring_drop++; return; }
        qa_chunk *c = &a->ring[w % QA_NCH];
        c->t = t_end - (u64)((double)(frames - n) * 1e9 / (double)a->rate);
        c->n = n;
        memcpy(c->d, src, (size_t)n * (size_t)a->ch * sizeof(short));
        __atomic_store_n(&a->w, w + 1u, __ATOMIC_RELEASE);
        src += n * a->ch; frames -= n;
    }
}
void ps2_qrec_game_pcm(const void *pcm, int frames, int channels, int rate) {   /* (the AAudio output callback; never blocks) */
    if (!q.on || frames <= 0 || st() != QS_REC) return;
    if (channels != 2 || rate != QA_RATE) {
        static int said;
        if (!said++) ps2_log("qrec: the game's stream is %d Hz x %d: not recorded (48000 x 2 expected)", rate, channels);
        return;
    }
    ring_push(&q.game, (const short *)pcm, frames, mono_ns());
}
static aaudio_data_callback_result_t mic_cb(AAudioStream *as, void *user, void *audio, int32_t frames) {
    (void)as; (void)user;
    if (st() == QS_REC && frames > 0) ring_push(&q.micx, (const short *)audio, frames, mono_ns());
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static void mic_err(AAudioStream *as, void *user, aaudio_result_t err) {
    (void)as; (void)user;
    __android_log_print(ANDROID_LOG_WARN, "aczquest", "qrec: microphone stream error %d (%s): reopened", err, AAudio_convertResultToText(err));
    __atomic_store_n(&q.mic_lost, 1, __ATOMIC_RELEASE);
}
static void mic_close(void) {
    if (!q.mic_as) return;
    AAudioStream_requestStop(q.mic_as);
    AAudioStream_close(q.mic_as);
    q.mic_as = NULL;
}
static void mic_try_open(u64 now) {
    q.mic_next_try = now + 2000000000ull;
    if (!ps2_android_permission("android.permission.RECORD_AUDIO", 0)) {
        if (!q.mic_said++) ps2_log("qrec: no RECORD_AUDIO permission yet: the microphone track is silent until it is granted "
                                   "(the headset's dialog, or adb shell pm grant com.kulakovri.ac0oculus android.permission.RECORD_AUDIO)");
        return;
    }
    AAudioStreamBuilder *b = NULL;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_NONE);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 1);
    AAudioStreamBuilder_setSampleRate(b, QA_RATE);
    if (q.mic_preset) AAudioStreamBuilder_setInputPreset(b, q.mic_preset);
    AAudioStreamBuilder_setDataCallback(b, mic_cb, NULL);
    AAudioStreamBuilder_setErrorCallback(b, mic_err, NULL);
    AAudioStream *as = NULL;
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &as);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) { ps2_log("qrec: the microphone could not be opened (%s): retried in 2 s", AAudio_convertResultToText(r)); return; }
    int rate = AAudioStream_getSampleRate(as), chn = AAudioStream_getChannelCount(as);
    if (rate != QA_RATE || chn != 1 || AAudioStream_getFormat(as) != AAUDIO_FORMAT_PCM_I16) {
        if (!q.mic_rate_bad++) ps2_log("qrec: the microphone gives %d Hz x %d (format %d), not 48000 x 1 s16: the track stays silent", rate, chn, (int)AAudioStream_getFormat(as));
        AAudioStream_close(as);
        q.mic_next_try = now + 60000000000ull;
        return;
    }
    __atomic_store_n(&q.micx.r, __atomic_load_n(&q.micx.w, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
    if ((r = AAudioStream_requestStart(as)) != AAUDIO_OK) { ps2_log("qrec: the microphone did not start (%s)", AAudio_convertResultToText(r)); AAudioStream_close(as); return; }
    q.mic_as = as;
    ps2_log("qrec: the microphone is recording: %d Hz mono, input preset %d (%s), burst %d frames", rate, (int)AAudioStream_getInputPreset(as),
            q.mic_preset ? "ACZ_QUEST_RECORD_MIC_PRESET" : "AAudio's default", AAudioStream_getFramesPerBurst(as));
}

/* ---- the encoders and the muxer ---- */
static void pend_free(qcodec *c) {
    for (qpend *p = c->ph, *n; p; p = n) { n = p->next; free(p); }
    c->ph = c->pt = NULL; c->pend_bytes = 0;
}
static void mux_write(qcodec *c, const unsigned char *d, const AMediaCodecBufferInfo *bi) {
    if (q.mux_started) {
        if (c->track < 0) return;
        if (AMediaMuxer_writeSampleData(q.mux, (size_t)c->track, d, bi) != AMEDIA_OK) c->errors++;
        c->out_bytes += (u64)bi->size; c->out_n++;
        return;
    }
    if (c->pend_bytes > (64u << 20)) { c->errors++; return; }
    qpend *p = (qpend *)malloc(sizeof(qpend) + (size_t)bi->size);
    if (!p) { c->errors++; return; }
    p->next = NULL; p->bi = *bi; p->bi.offset = 0;
    memcpy(p->d, d + bi->offset, (size_t)bi->size);
    if (c->pt) c->pt->next = p; else c->ph = p;
    c->pt = p; c->pend_bytes += (size_t)bi->size;
}
static qcodec *codec_at(int i) { return i == 0 ? &q.vc : i == 1 ? &q.game.cd : &q.micx.cd; }
/* every codec of the session has its output format: the tracks in a fixed order (video, game, mic), then the waiting
   samples; force (the session ends): the tracks that have one */
static void mux_try_start(int force) {
    if (q.mux_started || !q.mux) return;
    for (int i = 0; i < 3; i++) { qcodec *c = codec_at(i); if (c->on && !c->ofmt && !force) return; }
    int n = 0;
    for (int i = 0; i < 3; i++) {
        qcodec *c = codec_at(i);
        c->track = -1;
        if (!c->on || !c->ofmt) continue;
        ssize_t t = AMediaMuxer_addTrack(q.mux, c->ofmt);
        if (t < 0) { ps2_log("qrec: the muxer refused the %s track (%zd)", c->what, t); continue; }
        c->track = (int)t; n++;
    }
    if (!n) return;
    if (AMediaMuxer_start(q.mux) != AMEDIA_OK) { ps2_log("qrec: the muxer did not start"); return; }
    q.mux_started = 1; q.mux_tracks = n;
    for (int i = 0; i < 3; i++) {
        qcodec *c = codec_at(i);
        for (qpend *p = c->ph; p; p = p->next) mux_write(c, p->d, &p->bi);
        pend_free(c);
    }
}
static void lat_note_in(long long pts, u64 now) { q.lat_pts[q.lat_i % 64u] = pts; q.lat_t[q.lat_i % 64u] = now; q.lat_i++; }
static void lat_note_out(long long pts, u64 now) {
    for (u32 k = 0; k < 64u; k++)
        if (q.lat_pts[k] == pts && q.lat_t[k]) {
            double ms = (double)(now - q.lat_t[k]) / 1e6;
            q.lat_sum += ms; q.lat_n++; if (ms > q.lat_max) q.lat_max = ms;
            q.lat_t[k] = 0;
            return;
        }
}
static void drain(qcodec *c) {
    if (!c->c || c->eos_out) return;
    for (int guard = 0; guard < 64; guard++) {
        AMediaCodecBufferInfo bi; memset(&bi, 0, sizeof bi);
        ssize_t ox = AMediaCodec_dequeueOutputBuffer(c->c, &bi, 0);
        if (ox == AMEDIACODEC_INFO_TRY_AGAIN_LATER) break;
        if (ox == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            if (c->ofmt) AMediaFormat_delete(c->ofmt);
            c->ofmt = AMediaCodec_getOutputFormat(c->c);
            mux_try_start(0);
            continue;
        }
        if (ox == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (ox < 0) { c->errors++; break; }
        size_t sz = 0;
        uint8_t *d = AMediaCodec_getOutputBuffer(c->c, (size_t)ox, &sz);
        if (d && bi.size > 0 && !(bi.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG)) {
            if (c == &q.vc) lat_note_out(bi.presentationTimeUs, mono_ns());
            mux_write(c, d, &bi);
        }
        AMediaCodec_releaseOutputBuffer(c->c, (size_t)ox, false);
        if (bi.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) { c->eos_out = 1; break; }
    }
}
static void codec_free(qcodec *c) {
    if (c->c) { AMediaCodec_stop(c->c); AMediaCodec_delete(c->c); }
    if (c->ofmt) AMediaFormat_delete(c->ofmt);
    pend_free(c);
    const char *w = c->what;
    memset(c, 0, sizeof *c);
    c->what = w; c->track = -1;
}
static void codec_name(qcodec *c) {
    char *n = NULL;
    c->name[0] = 0;
    if (AMediaCodec_getName(c->c, &n) == AMEDIA_OK && n) { snprintf(c->name, sizeof c->name, "%s", n); AMediaCodec_releaseName(c->c, n); }
}
/* the video encoder: the wanted size, codec, NV12 (else I420), a profile; then fallbacks. 0 = running */
static int v_open(void) {
    static const u32 sizes[4][2] = { { 0, 0 }, { 1280, 960 }, { 1280, 720 }, { 960, 720 } };
    static const int cfs[2] = { 21, 19 };                /* COLOR_FormatYUV420SemiPlanar (NV12), COLOR_FormatYUV420Planar (I420) */
    for (int si = 0; si < 4; si++) {
        u32 w = si ? sizes[si][0] : q.cw, h = si ? sizes[si][1] : q.ch;
        if (q.vsize_ok && (w != q.vw || h != q.vh)) continue;   /* (the GPU's buffers have the first session's size) */
        if (si && w == q.cw && h == q.ch) continue;
        for (int mi = 0; mi < 2; mi++) {
            int hevc = mi ? !q.hevc : q.hevc;
            const char *mime = hevc ? "video/hevc" : "video/avc";
            for (int ci = 0; ci < 2; ci++) for (int pi = 0; pi < 2; pi++) {
                AMediaCodec *c = AMediaCodec_createEncoderByType(mime);
                if (!c) break;
                AMediaFormat *f = AMediaFormat_new();
                AMediaFormat_setString(f, "mime", mime);
                AMediaFormat_setInt32(f, "width", (int32_t)w);
                AMediaFormat_setInt32(f, "height", (int32_t)h);
                AMediaFormat_setInt32(f, "color-format", cfs[ci]);
                AMediaFormat_setInt32(f, "bitrate", q.kbps * 1000);
                AMediaFormat_setInt32(f, "bitrate-mode", q.brmode);     /* (ACZ_QUEST_RECORD_MODE: 1 VBR default, 2 CBR, 0 CQ) */
                if (q.qpmax > 0) AMediaFormat_setInt32(f, "video-qp-max", q.qpmax);
                AMediaFormat_setInt32(f, "frame-rate", q.fps);
                AMediaFormat_setInt32(f, "i-frame-interval", 1);
                AMediaFormat_setInt32(f, "color-range", 2);             /* limited (rq_rec.frag: BT.709 limited) */
                AMediaFormat_setInt32(f, "color-standard", 1);          /* BT.709 */
                AMediaFormat_setInt32(f, "color-transfer", 3);          /* SDR video */
                if (!pi) AMediaFormat_setInt32(f, "profile", hevc ? 1 : 8);   /* HEVC Main / AVC High */
                media_status_t r = AMediaCodec_configure(c, f, NULL, NULL, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
                AMediaFormat_delete(f);
                if (r != AMEDIA_OK || AMediaCodec_start(c) != AMEDIA_OK) { AMediaCodec_delete(c); continue; }
                q.vc.c = c; q.vc.on = 1; q.vc.track = -1;
                codec_name(&q.vc);
                int32_t stv = 0, slv = 0, cf = cfs[ci];
                AMediaFormat *in = AMediaCodec_getInputFormat(c);
                if (in) {
                    AMediaFormat_getInt32(in, "stride", &stv);
                    AMediaFormat_getInt32(in, "slice-height", &slv);
                    AMediaFormat_getInt32(in, "color-format", &cf);
                    AMediaFormat_delete(in);
                }
                q.v_stride = stv >= (int32_t)w ? stv : (int32_t)w;
                q.v_slice = slv >= (int32_t)h ? slv : (int32_t)h;
                q.v_cf = cfs[ci];
                q.v_size = (size_t)q.v_stride * (size_t)q.v_slice * 3u / 2u;
                q.vw = w; q.vh = h; q.vsize_ok = 1;
                ps2_log("qrec: video encoder %s (%s) %ux%u, %s, %d fps, %d kbit/s %s%s, keyframe every 1 s, profile %s; input stride %d, "
                        "slice height %d, colour format %d (asked %d)%s", q.vc.name, mime, w, h, cfs[ci] == 21 ? "NV12" : "I420", q.fps, q.kbps,
                        q.brmode == 2 ? "CBR" : q.brmode == 0 ? "CQ" : "VBR", q.qpmax > 0 ? " (QP max set)" : "",
                        pi ? "the encoder's default" : hevc ? "Main" : "High", stv, slv, (int)cf, cfs[ci],
                        si || mi ? " -- a fallback: the wanted configuration was refused" : "");
                return 0;
            }
        }
    }
    ps2_log("qrec: no video encoder accepted %ux%u (H.264 / HEVC, NV12 / I420, 4 sizes): the recordings have no video", q.cw, q.ch);
    return -1;
}
static int a_open(qtrack *a, int bitrate) {
    AMediaCodec *c = AMediaCodec_createEncoderByType("audio/mp4a-latm");
    if (!c) return -1;
    AMediaFormat *f = AMediaFormat_new();
    AMediaFormat_setString(f, "mime", "audio/mp4a-latm");
    AMediaFormat_setInt32(f, "sample-rate", a->rate);
    AMediaFormat_setInt32(f, "channel-count", a->ch);
    AMediaFormat_setInt32(f, "bitrate", bitrate);
    AMediaFormat_setInt32(f, "aac-profile", 2);                    /* AAC-LC */
    AMediaFormat_setInt32(f, "max-input-size", 16384);
    media_status_t r = AMediaCodec_configure(c, f, NULL, NULL, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(f);
    if (r != AMEDIA_OK || AMediaCodec_start(c) != AMEDIA_OK) { AMediaCodec_delete(c); ps2_log("qrec: no AAC encoder for the %s track", a->name); return -1; }
    a->cd.c = c; a->cd.on = 1; a->cd.track = -1;
    codec_name(&a->cd);
    return 0;
}
static void at_reset(qtrack *a) {
    __atomic_store_n(&a->r, __atomic_load_n(&a->w, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
    a->pos = 0; a->started = 0; a->refill = 0; a->corr = 0; a->ema = 0.0;
    a->dup = a->skip = a->sil = a->hard = a->last_t = 0; a->stage_n = 0; a->stage_over = 0; a->fed = 0;
    a->sq = 0.0; a->nsq = 0; a->peak = 0; a->ring_drop = 0;
}
/* frames onto the timeline (src NULL: silence); returns the frames placed */
static int at_put(qtrack *a, const short *src, int n) {
    if (n <= 0) return 0;
    if (a->stage_n + n > a->stage_cap) { a->stage_over += (u64)(a->stage_n + n - a->stage_cap); n = a->stage_cap - a->stage_n; if (n <= 0) return 0; }
    short *d = a->stage + (size_t)a->stage_n * (size_t)a->ch;
    size_t k = (size_t)n * (size_t)a->ch;
    if (src) {
        memcpy(d, src, k * sizeof(short));
        for (size_t i = 0; i < k; i++) { int v = src[i]; a->sq += (double)v * (double)v; if (v < 0) v = -v; if (v > a->peak) a->peak = v; }
        a->nsq += k;
    } else memset(d, 0, k * sizeof(short));
    a->stage_n += n; a->pos += n;
    return n;
}
static void at_silence(qtrack *a, long long n) {
    while (n > 0) { int k = n > 48000 ? 48000 : (int)n; int put = at_put(a, NULL, k); a->sil += (u64)put; if (put < k) return; n -= k; }
}
/* the ring's chunks onto the video's timeline (ps2_recexact.c's rule) */
static void at_pump(qtrack *a, u64 now) {
    u32 r = __atomic_load_n(&a->r, __ATOMIC_RELAXED), w = __atomic_load_n(&a->w, __ATOMIC_ACQUIRE);
    const double spn = (double)a->rate / 1e9;
    static short tmp[(QA_F + 8) * 2];
    while (r != w) {
        qa_chunk *c = &a->ring[r % QA_NCH];
        double exp_end = ((double)c->t - (double)q.t0) * spn;     /* where this chunk's last frame belongs */
        int n = c->n;
        const short *src = c->d;
        if (!a->started || a->refill) {
            if (exp_end < 0.0) { r++; continue; }               /* before the first frame */
            long long pad = (long long)(exp_end - n) - a->pos;
            if (pad > 0) at_silence(a, pad);
            else if (pad < 0) {                                 /* (its overlap with what is placed left out) */
                int cut = -pad >= n ? n : (int)-pad;
                a->skip += (u64)cut;
                if (cut == n) { r++; a->refill = 0; a->started = 1; continue; }
                src += cut * a->ch; n -= cut;
            }
            a->started = 1; a->refill = 0; a->ema = 0.0; a->corr = 0;
        }
        double e = (double)(a->pos + n) - exp_end;              /* + : the track is ahead of the clock */
        a->ema = a->ema * 0.98 + e * 0.02;
        if (a->ema < -0.1 * a->rate) {                          /* 100 ms behind (a stall): silence */
            long long gap = (long long)(-a->ema);
            at_silence(a, gap); a->ema += (double)gap; a->hard++;
        } else if (a->ema > 0.1 * a->rate) {                    /* 100 ms ahead: this chunk out */
            a->ema -= n; a->hard++; a->skip += (u64)n; r++; continue;
        }
        if (a->corr == 0 && a->ema > 0.010 * a->rate) a->corr = -1;
        else if (a->corr == 0 && a->ema < -0.010 * a->rate) a->corr = 1;
        else if (a->corr && a->ema > -0.003 * a->rate && a->ema < 0.003 * a->rate) a->corr = 0;
        int m = n;
        if (a->corr) {                                          /* 1 frame in 200 repeated / left out */
            m = 0;
            for (int i = 0; i < n; i++) {
                int k = (int)((a->pos + i) % 200);
                if (a->corr < 0 && k == 0) { a->skip++; continue; }
                for (int ch = 0; ch < a->ch; ch++) tmp[m * a->ch + ch] = src[i * a->ch + ch];
                m++;
                if (a->corr > 0 && k == 0) { for (int ch = 0; ch < a->ch; ch++) tmp[m * a->ch + ch] = src[i * a->ch + ch]; m++; a->dup++; }
            }
            src = tmp;
            a->ema += (double)(m - n);
        }
        at_put(a, src, m);
        a->last_t = c->t;
        r++;
    }
    __atomic_store_n(&a->r, r, __ATOMIC_RELEASE);
    /* nothing came (no device, a stall, the mic not open yet): silence up to 100 ms behind the clock, so the track goes on */
    if (now > q.t0) {
        long long want = (long long)(((double)now - (double)q.t0) * spn) - a->rate / 10;
        if ((!a->started || now - a->last_t > 200000000ull) && want > a->pos) {
            at_silence(a, want - a->pos);
            a->started = 1; a->refill = 1;
        }
    }
}
static void at_encode(qtrack *a) {
    while (a->stage_n > 0 && a->cd.c && !a->cd.eos_in) {
        ssize_t ix = AMediaCodec_dequeueInputBuffer(a->cd.c, 0);
        if (ix < 0) break;
        size_t cap = 0;
        uint8_t *p = AMediaCodec_getInputBuffer(a->cd.c, (size_t)ix, &cap);
        int fb = 2 * a->ch, k = (int)(cap / (size_t)fb);
        if (k > a->stage_n) k = a->stage_n;
        if (k > 4096) k = 4096;
        uint64_t pts = (uint64_t)(a->fed * 1000000ll / a->rate);
        if (!p || k <= 0) { AMediaCodec_queueInputBuffer(a->cd.c, (size_t)ix, 0, 0, pts, 0); a->cd.errors++; break; }
        memcpy(p, a->stage, (size_t)k * (size_t)fb);
        AMediaCodec_queueInputBuffer(a->cd.c, (size_t)ix, 0, (size_t)k * (size_t)fb, pts, 0);
        a->fed += k;
        memmove(a->stage, a->stage + (size_t)k * (size_t)a->ch, (size_t)(a->stage_n - k) * (size_t)fb);
        a->stage_n -= k;
    }
}
static void codec_eos(qcodec *c, uint64_t pts) {
    if (!c->c || c->eos_in) return;
    for (int i = 0; i < 25; i++) {
        ssize_t ix = AMediaCodec_dequeueInputBuffer(c->c, 20000);
        if (ix >= 0) { AMediaCodec_queueInputBuffer(c->c, (size_t)ix, 0, 0, pts, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM); c->eos_in = 1; return; }
        drain(c);
    }
    c->eos_in = 1; c->eos_out = 1;                      /* (no input buffer in 0.5 s: the stream ends where it is) */
}
/* one NV12 picture (W x H, rows of W bytes) into the encoder at the encoder's stride and slice height (I420: the chroma split);
   1 = queued, 0 = the encoder has no input buffer free (busy), -1 = left out (the buffer too small) */
static int v_put(const uint8_t *src, long long pts, int64_t wait_us) {
    ssize_t ix = AMediaCodec_dequeueInputBuffer(q.vc.c, wait_us);
    if (ix < 0) return 0;
    size_t cap = 0;
    uint8_t *dst = AMediaCodec_getInputBuffer(q.vc.c, (size_t)ix, &cap);
    const u32 W = q.vw, H = q.vh, S = (u32)q.v_stride, SH = (u32)q.v_slice;
    /* the bytes the picture really needs: up to the last chroma row's last pixel, without that row's trailing padding (the device,
       c2.qti.avc.encoder at 1440x1080: stride 1536, slice height 1088, a buffer of 2500512 bytes = 1536 x (1088 + 540) - 96) */
    size_t need = q.v_cf == 21 ? (size_t)S * SH + (size_t)S * (H / 2u - 1u) + W
                               : (size_t)S * SH + (size_t)(S / 2u) * (SH / 2u) + (size_t)(S / 2u) * (H / 2u - 1u) + W / 2u;
    size_t size = q.v_size <= cap ? q.v_size : cap;      /* (stride x slice height x 1.5 when it fits, else the whole buffer) */
    if (!dst || cap < need) {
        AMediaCodec_queueInputBuffer(q.vc.c, (size_t)ix, 0, 0, (uint64_t)(pts > 0 ? pts : 0), 0);
        if (!q.vc.errors++) ps2_log("qrec: the encoder's input buffer holds %zu bytes, a picture needs %zu: pictures left out", cap, need);
        return -1;
    }
    if (q.v_cf == 21) {
        if (S == W && SH == H) memcpy(dst, src, (size_t)W * H * 3u / 2u);
        else {
            for (u32 y = 0; y < H; y++) memcpy(dst + (size_t)y * S, src + (size_t)y * W, W);
            for (u32 y = 0; y < H / 2u; y++) memcpy(dst + (size_t)S * SH + (size_t)y * S, src + (size_t)W * H + (size_t)y * W, W);
        }
    } else {
        for (u32 y = 0; y < H; y++) memcpy(dst + (size_t)y * S, src + (size_t)y * W, W);
        uint8_t *u = dst + (size_t)S * SH, *v = u + (size_t)(S / 2u) * (SH / 2u);
        for (u32 y = 0; y < H / 2u; y++) {
            const uint8_t *uv = src + (size_t)W * H + (size_t)y * W;
            uint8_t *ur = u + (size_t)y * (S / 2u), *vr = v + (size_t)y * (S / 2u);
            for (u32 x = 0; x < W / 2u; x++) { ur[x] = uv[2u * x]; vr[x] = uv[2u * x + 1u]; }
        }
    }
    AMediaCodec_queueInputBuffer(q.vc.c, (size_t)ix, 0, size, (uint64_t)pts, 0);
    lat_note_in(pts, mono_ns());
    q.last_pts_us = (u64)pts;
    return 1;
}
/* the session's first video sample: a black picture at 0 s (the session's open), so the video track starts with the
   audio whatever the renderer shows first (the first real picture follows within a frame or two of the game's output) */
static void v_black(void) {
    static uint8_t *blk; static size_t blk_n;
    size_t n = (size_t)q.vw * q.vh * 3u / 2u;
    if (blk_n != n) { free(blk); blk = (uint8_t *)malloc(n); blk_n = blk ? n : 0; if (blk) { memset(blk, 16, (size_t)q.vw * q.vh); memset(blk + (size_t)q.vw * q.vh, 128, n - (size_t)q.vw * q.vh); } }
    if (!blk || v_put(blk, 0, 200000) != 1) ps2_log("qrec: the black first picture could not be queued: the video track may start late");
}
/* ACZ_QUEST_RECORD_VIDEO=cpu: one synthetic picture at the recording's rate, made on this thread, through the same copy into the
   encoder as a captured one (pts = now - the session's open); the encoder busy = the picture is dropped (counted) */
static void v_synthetic(u64 now) {
    if (!q.vc.c || q.vc.eos_in) return;
    u64 P = 1000000000ull / (u64)(q.fps > 0 ? q.fps : 30);
    if (!q.syn_next) q.syn_next = now;
    if (now < q.syn_next) return;
    q.syn_next = now - q.syn_next > P ? now + P : q.syn_next + P;
    static uint8_t *buf; static size_t bn; static u32 rng = 0x9E3779B9u;
    const u32 W = q.vw, H = q.vh;
    size_t n = (size_t)W * H * 3u / 2u;
    if (bn != n) { free(buf); buf = (uint8_t *)malloc(n); bn = buf ? n : 0; }
    if (!buf) return;
    u64 c0 = mono_ns();
    u32 f = (u32)q.syn_n;
    for (u32 y = 0; y < H; y++) {
        uint8_t *r = buf + (size_t)y * W;
        for (u32 x = 0; x < W; x++) r[x] = (uint8_t)(16u + (((x >> 1) + (y >> 1) + f * 6u) % 220u));
    }
    u32 bx = (f * 13u) % (W - 256u), by = (f * 7u) % (H - 256u);
    for (u32 y = 0; y < 256u; y++) {
        uint8_t *r = buf + (size_t)(by + y) * W + bx;
        for (u32 x = 0; x < 256u; x++) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; r[x] = (uint8_t)(16u + (rng & 0x7Fu) + 40u); }
    }
    for (u32 y = 0; y < H / 2u; y++) {
        uint8_t *r = buf + (size_t)W * H + (size_t)y * W;
        for (u32 x = 0; x < W / 2u; x++) { r[2u * x] = (uint8_t)(96u + ((x + f) & 63u)); r[2u * x + 1u] = (uint8_t)(96u + ((y + 2u * f) & 63u)); }
    }
    long long pts = (long long)((now - q.t0) / 1000ull);
    if (pts <= (long long)q.last_pts_us) pts = (long long)q.last_pts_us + 1;
    int r = v_put(buf, pts, 0);
    if (r == 1) { q.frames_sess++; q.v_in++; q.v_in_tot++; q.syn_n++; }
    else if (r == 0) { q.v_busy++; q.v_drop++; q.v_drop_tot++; }
    double ms = (double)(mono_ns() - c0) / 1e6;
    q.copy_ms += ms; q.copy_n++; if (ms > q.copy_max) q.copy_max = ms;
}
/* the oldest queued picture into the encoder; 1 = one taken off the queue (encoded or left out), 0 = none or the encoder busy */
static int v_feed_one(void) {
    pthread_mutex_lock(&q.mu);
    if (!q.qn) { pthread_mutex_unlock(&q.mu); return 0; }
    int s = q.q[q.qh];
    u64 t = q.slot_t[s];
    pthread_mutex_unlock(&q.mu);
    if (q.vc.c && !q.vc.eos_in && t > q.t0 + 1000000ull) {   /* (a picture published before the session opened: left out) */
        u64 c0 = mono_ns();
        long long pts = (long long)((t - q.t0) / 1000ull);
        if (pts <= (long long)q.last_pts_us) pts = (long long)q.last_pts_us + 1;
        int r = v_put(q.ptr[s], pts, 0);
        if (r == 0) { q.v_busy++; return 0; }        /* (the encoder is behind: the picture waits, the renderer drops) */
        if (r == 1) {
            if (!q.frames_sess) ps2_log("qrec: the first picture of %s at %.3f s", q.part, (double)pts / 1e6);
            q.frames_sess++; q.v_in++; q.v_in_tot++;
        }
        double ms = (double)(mono_ns() - c0) / 1e6;
        q.copy_ms += ms; q.copy_n++; if (ms > q.copy_max) q.copy_max = ms;
    }
    pthread_mutex_lock(&q.mu);
    q.qh = (q.qh + 1) % QR_MAXB; q.qn--; q.slot[s] = 0;
    pthread_mutex_unlock(&q.mu);
    return 1;
}
static double free_mb(void) {
    struct statvfs v;
    if (statvfs(q.dir, &v) != 0) return 1e12;
    return (double)v.f_bavail * (double)v.f_frsize / 1048576.0;
}
static int mkdirs(const char *p) {
    char b[700];
    snprintf(b, sizeof b, "%s", p);
    for (char *s = b + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(b, 0775); *s = '/'; }
    return mkdir(b, 0775) == 0 || errno == EEXIST ? 0 : -1;
}
static void level(qtrack *a, char *out, size_t cap) {
    if (!a->cd.on) { snprintf(out, cap, "off"); return; }
    if (!a->nsq) { snprintf(out, cap, "silent (no samples)"); return; }
    double rms = sqrt(a->sq / (double)a->nsq) / 32768.0, pk = (double)a->peak / 32768.0;
    snprintf(out, cap, "%.1f dBFS rms, %.1f peak", rms > 1e-7 ? 20.0 * log10(rms) : -140.0, pk > 1e-7 ? 20.0 * log10(pk) : -140.0);
    a->sq = 0.0; a->nsq = 0; a->peak = 0;
}
static void log_line(u64 now) {
    char g[64], m[64];
    level(&q.game, g, sizeof g); level(&q.micx, m, sizeof m);
    struct stat sb; double mb = q.fd >= 0 && fstat(q.fd, &sb) == 0 ? (double)sb.st_size / 1048576.0 : 0.0;
    ps2_log("qrec: 10 s: video %llu frames in (%llu in the session), %llu dropped (no free buffer; %llu in all), encoder busy %llu, "
            "pictures eye %llu / screen %llu / synthetic %llu; copy %.2f ms a frame (max %.2f), encoder in->out %.1f ms (max %.1f); game %s; mic %s%s; "
            "audio chunks lost game %llu / mic %llu; timeline game %llu repeated, %llu left out, %llu resyncs, %.1f s silence filled, "
            "mic %llu / %llu / %llu, %.1f s; file %.1f MB, %.1f GB free",
            q.v_in, q.frames_sess, q.v_drop, q.v_drop_tot, q.v_busy, (u64)__atomic_load_n(&q.cap_eye, __ATOMIC_RELAXED),
            (u64)__atomic_load_n(&q.cap_rt, __ATOMIC_RELAXED), q.vcpu ? q.v_in : 0ull, q.copy_n ? q.copy_ms / (double)q.copy_n : 0.0, q.copy_max,
            q.lat_n ? q.lat_sum / (double)q.lat_n : 0.0, q.lat_max, g, m, q.micx.cd.on ? (q.mic_as ? "" : " (not open)") : "",
            q.game.ring_drop, q.micx.ring_drop, q.game.dup, q.game.skip, q.game.hard, (double)q.game.sil / q.game.rate,
            q.micx.dup, q.micx.skip, q.micx.hard, (double)q.micx.sil / q.micx.rate, mb, free_mb() / 1024.0);
    q.v_in = q.v_drop = q.v_busy = 0; q.copy_ms = q.copy_max = 0.0; q.copy_n = 0; q.lat_sum = q.lat_max = 0.0; q.lat_n = 0;
    __atomic_store_n(&q.cap_eye, 0, __ATOMIC_RELAXED); __atomic_store_n(&q.cap_rt, 0, __ATOMIC_RELAXED);
    q.last_log = now;
}

/* ---- sessions ---- */
static void sess_open(void) {
    mkdirs(q.dir);
    double fm = free_mb();
    if (fm < (double)q.min_free_mb) {
        ps2_log("qrec: %.0f MB free in %s, under ACZ_QUEST_RECORD_MIN_FREE_MB %d: not recording (until the next resume)", fm, q.dir, q.min_free_mb);
        set_st(QS_HALT); return;
    }
    time_t now_s = time(NULL); struct tm tm; localtime_r(&now_s, &tm);
    snprintf(q.path, sizeof q.path, "%s/aczq_%04d-%02d-%02d_%02d-%02d-%02d.mp4", q.dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    snprintf(q.part, sizeof q.part, "%s.part", q.path);
    q.fd = open(q.part, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0644);
    if (q.fd < 0) { ps2_log("qrec: cannot create %s (%d): not recording (until the next resume)", q.part, errno); set_st(QS_HALT); return; }
    q.mux = AMediaMuxer_new(q.fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
    if (!q.mux) { ps2_log("qrec: no muxer"); close(q.fd); q.fd = -1; unlink(q.part); set_st(QS_HALT); return; }
    q.mux_started = 0; q.mux_tracks = 0;
    if (q.video && !q.vfail && v_open() != 0) q.vfail = 1;
    q.vfail_req = 0;
    at_reset(&q.game); at_reset(&q.micx);
    a_open(&q.game, 192000);
    if (q.mic) a_open(&q.micx, 96000);
    if (!q.vc.c && !q.game.cd.c && !q.micx.cd.c) {
        ps2_log("qrec: no encoder at all: not recording");
        AMediaMuxer_delete(q.mux); q.mux = NULL; close(q.fd); q.fd = -1; unlink(q.part);
        set_st(QS_HALT); return;
    }
    q.open_ns = mono_ns();
    q.t0 = q.open_ns;                                   /* (the video's and the audio's 0 s: the session's open) */
    q.last_pts_us = 0; q.frames_sess = 0;
    if (q.vc.c) v_black();
    {   struct timespec rt; clock_gettime(CLOCK_REALTIME, &rt); struct tm wt; localtime_r(&rt.tv_sec, &wt);
        ps2_log("qrec: %s: 0 s = wall %04d-%02d-%02d %02d:%02d:%02d.%03ld", q.part, wt.tm_year + 1900, wt.tm_mon + 1, wt.tm_mday, wt.tm_hour,
                wt.tm_min, wt.tm_sec, rt.tv_nsec / 1000000L); }
    q.v_in = q.v_drop = q.v_busy = 0; q.copy_ms = q.copy_max = 0.0; q.copy_n = 0; q.lat_sum = q.lat_max = 0.0; q.lat_n = 0;
    memset(q.lat_t, 0, sizeof q.lat_t);
    q.last_log = q.open_ns;
    q.mic_next_try = 0; q.mic_said = 0;
    pthread_mutex_lock(&q.mu);
    for (int i = 0; i < q.nb; i++) if (q.slot[i] == 2) q.slot[i] = 0;
    q.qn = 0; q.qh = 0;
    pthread_mutex_unlock(&q.mu);
    __atomic_store_n(&q.v_on, q.vc.c != NULL && !q.vcpu, __ATOMIC_RELEASE);   /* (=cpu: the renderer captures nothing) */
    q.syn_next = 0;
    set_st(QS_REC);
    ps2_log("qrec: recording %s: video %s, game audio %s, microphone %s (ACZ_QUEST_RECORD; %.1f GB free)", q.part,
            q.vc.c ? (q.vcpu ? "synthetic pictures (cpu)" : q.vc.name) : q.video ? "none (no encoder or no GPU capture)" : "off (ACZ_QUEST_RECORD_VIDEO=0)",
            q.game.cd.c ? q.game.cd.name : "none", q.micx.cd.c ? q.micx.cd.name : q.mic ? "none" : "off (ACZ_QUEST_RECORD_MIC=0)", fm / 1024.0);
}
static void sess_close(const char *why) {
    if (st() != QS_REC) return;
    __atomic_store_n(&q.v_on, 0, __ATOMIC_RELEASE);
    set_st(QS_CLOSING);
    u64 t_close = mono_ns();
    mic_close();
    for (u64 tend = mono_ns() + 300000000ull; q.qn && mono_ns() < tend;) {   /* (the pictures already captured, 300 ms at most) */
        if (!v_feed_one()) { drain(&q.vc); usleep(2000); }
        drain(&q.vc);
    }
    pthread_mutex_lock(&q.mu);
    for (int i = 0; i < q.nb; i++) if (q.slot[i] == 2) q.slot[i] = 0;
    q.qn = 0; q.qh = 0;
    pthread_mutex_unlock(&q.mu);
    if (q.game.cd.c) at_pump(&q.game, t_close);
    if (q.micx.cd.c) at_pump(&q.micx, t_close);
    for (u64 tend = mono_ns() + 200000000ull; (q.game.stage_n || q.micx.stage_n) && mono_ns() < tend;) {
        at_encode(&q.game); at_encode(&q.micx); drain(&q.game.cd); drain(&q.micx.cd); usleep(1000);
    }
    codec_eos(&q.vc, q.last_pts_us + 1);
    codec_eos(&q.game.cd, (uint64_t)(q.game.fed * 1000000ll / q.game.rate));
    codec_eos(&q.micx.cd, (uint64_t)(q.micx.fed * 1000000ll / q.micx.rate));
    for (u64 tend = mono_ns() + 2000000000ull; mono_ns() < tend;) {
        drain(&q.vc); drain(&q.game.cd); drain(&q.micx.cd);
        if ((!q.vc.c || q.vc.eos_out) && (!q.game.cd.c || q.game.cd.eos_out) && (!q.micx.cd.c || q.micx.cd.eos_out)) break;
        usleep(2000);
    }
    mux_try_start(1);                                   /* (a track that never produced a format is left out) */
    int ok = 0;
    double secs = (double)q.last_pts_us / 1e6;
    if (q.game.fed / q.game.rate > secs) secs = (double)q.game.fed / q.game.rate;
    int empty = q.vc.on && !q.frames_sess;               /* (video on and not one picture: the start's black picture only) */
    if (q.mux_started) ok = AMediaMuxer_stop(q.mux) == AMEDIA_OK;
    AMediaMuxer_delete(q.mux); q.mux = NULL;
    struct stat sb; double mb = fstat(q.fd, &sb) == 0 ? (double)sb.st_size / 1048576.0 : 0.0;
    close(q.fd); q.fd = -1;
    if (ok && empty) { unlink(q.part); ps2_log("qrec: %s: no picture in %.1f s (%s): no file", q.part, secs, why); }
    else if (ok && rename(q.part, q.path) == 0)
        ps2_log("qrec: finished %s (%s): %.1f s, %llu video frames (%llu dropped in all), %.1f MB, %d tracks; game %.1f s, mic %.1f s; "
                "closing took %.0f ms", q.path, why, secs, q.frames_sess, q.v_drop_tot, mb, q.mux_tracks, (double)q.game.fed / q.game.rate,
                (double)q.micx.fed / q.micx.rate, (double)(mono_ns() - t_close) / 1e6);
    else if (!q.mux_started) { unlink(q.part); ps2_log("qrec: %s: nothing was recorded (%s): no file", q.part, why); }
    else ps2_log("qrec: %s: the muxer did not finish (%s): the file may not play", q.part, why);
    codec_free(&q.vc); codec_free(&q.game.cd); codec_free(&q.micx.cd);
    q.t0 = 0;
    set_st(QS_IDLE);
}
static void *qthread(void *arg) {
    (void)arg;
    pthread_setname_np(pthread_self(), "qrec");
    for (;;) {
        pthread_mutex_lock(&q.mu);
        int fin = q.final, pz = q.paused, vf = q.vfail_req;
        char why[120]; snprintf(why, sizeof why, "%s", q.why);
        pthread_mutex_unlock(&q.mu);
        int s = st();
        if (fin) { if (s == QS_REC) sess_close(why); break; }
        if (s == QS_REC && pz) { sess_close("paused: the headset taken off or the app in the background"); continue; }
        if (s == QS_IDLE && !pz) {
            if (open_ok(mono_ns())) { q.open_wait_said = 0; sess_open(); continue; }
            if (!q.open_wait_said++) ps2_log("qrec: the next file waits for the XR session to show frames (VISIBLE / FOCUSED) for %llu ms",
                                             q.settle_ns / 1000000ull);
        }
        if (s == QS_REC) {
            u64 now = mono_ns();
            if (vf && !q.vfail) {
                q.vfail = 1;
                ps2_log("qrec: no video from the GPU (%s): %s", q.vfail_why, q.mux_started ? "the video track ends here" : "this file is audio only");
                __atomic_store_n(&q.v_on, 0, __ATOMIC_RELEASE);
                if (!q.mux_started) codec_free(&q.vc);
            }
            if (q.micx.cd.c) {
                if (__atomic_exchange_n(&q.mic_lost, 0, __ATOMIC_ACQ_REL)) { mic_close(); q.mic_next_try = now + 1000000000ull; }
                if (!q.mic_as && now >= q.mic_next_try) mic_try_open(now);
            }
            if (q.vcpu) v_synthetic(now);
            else for (int k = 0; k < 4 && v_feed_one(); k++) {}
            if (q.game.cd.c) at_pump(&q.game, now);
            if (q.micx.cd.c) at_pump(&q.micx, now);
            at_encode(&q.game); at_encode(&q.micx);
            drain(&q.vc); drain(&q.game.cd); drain(&q.micx.cd);
            if (now - q.last_log >= 10000000000ull) {
                log_line(now);
                if (free_mb() < (double)q.min_free_mb) {
                    ps2_log("qrec: under %d MB free: the recording stops (until the next resume)", q.min_free_mb);
                    sess_close("the storage is nearly full");
                    set_st(QS_HALT);
                    continue;
                }
            }
        }
        pthread_mutex_lock(&q.mu);
        if (!q.final && q.paused == pz && !q.qn) {
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            long add = st() == QS_REC ? 4000000L : 50000000L;
            ts.tv_nsec += add; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&q.cv, &q.mu, &ts);
        }
        pthread_mutex_unlock(&q.mu);
    }
    set_st(QS_DONE);
    pthread_mutex_lock(&q.mu);
    q.th_done = 1;
    pthread_cond_broadcast(&q.done_cv);
    pthread_mutex_unlock(&q.mu);
    return NULL;
}

/* ---- the platform's side (ps2_android.c) ---- */
int ps2_qrec_wanted(void) { cfg(); return q.on; }
int ps2_qrec_mic_wanted(void) { cfg(); return q.on && q.mic; }
void ps2_qrec_start(void) {
    cfg();
    if (!q.on || q.th_ok) return;
    set_st(QS_IDLE);
    if (pthread_create(&q.th, NULL, qthread, NULL) != 0) { ps2_log("qrec: no thread: the recorder is off"); set_st(QS_OFF); q.on = 0; return; }
    q.th_ok = 1;
    pthread_detach(q.th);
    ps2_log("qrec: the recorder is on (ACZ_QUEST_RECORD=1): %s, %ux%u at %d fps, %d kbit/s %s, game audio, %s; files in %s",
            !q.video ? "NO video (ACZ_QUEST_RECORD_VIDEO=0: the GPU untouched)"
                     : q.vcpu ? "video from SYNTHETIC CPU pictures (ACZ_QUEST_RECORD_VIDEO=cpu: the encoder runs, no GPU capture, the GPU untouched)" : "video", q.cw, q.ch, q.fps, q.kbps, q.hevc ? "HEVC" : "H.264",
            q.mic ? "microphone" : "no microphone (ACZ_QUEST_RECORD_MIC=0)", q.dir);
}
/* 1 = paused (APP_CMD_PAUSE: the file is finished), 0 = resumed (APP_CMD_RESUME: a new file starts) */
void ps2_qrec_pause(int paused) {
    if (!q.on || !q.th_ok) return;
    pthread_mutex_lock(&q.mu);
    q.paused = paused;
    if (!paused) __atomic_store_n(&q.resume_ns, mono_ns(), __ATOMIC_RELEASE);   /* (R-47: the next file settles after it) */
    if (!paused && st() == QS_HALT) set_st(QS_IDLE);
    pthread_cond_signal(&q.cv);
    pthread_mutex_unlock(&q.mu);
}
/* the end of the run: the session's file finished; waits up to 6 s; idempotent, any thread */
void ps2_qrec_stop(const char *why) {
    if (!q.on || !q.th_ok) return;
    pthread_mutex_lock(&q.mu);
    if (!q.final) { q.final = 1; snprintf(q.why, sizeof q.why, "%s", why ? why : "stop"); }
    pthread_cond_signal(&q.cv);
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 6;
    while (!q.th_done) if (pthread_cond_timedwait(&q.done_cv, &q.mu, &ts) == ETIMEDOUT) break;
    int done = q.th_done;
    pthread_mutex_unlock(&q.mu);
    if (!done) ps2_log("qrec: the recorder did not finish within 6 s (%s): the file may be a .part", why ? why : "stop");
}
