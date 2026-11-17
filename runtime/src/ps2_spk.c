/* ps2_spk.c -- the speaker profile: ENHANCEMENTS > SOUND > QUEST SPEAKERS (menu agent rq2_menu, 2026-11-09; docs/SETTINGS.md
   "The Quest", NOTES_RQ.md 2026-11-10 "speakers").

   The user plays on the Meta Quest 3's own speakers, not on headphones (2026-10-24). They give little below about 120-150 Hz,
   rattle when pushed at 40-60 Hz and leak between the ears (docs/research/2026-10-24/spatial-audio.md section 2.7): the
   engine's body, the G-suit's whump and the explosions' thump are lost. This stage shapes the final mix for them, on the audio
   thread, after every layer and right before the chunk goes to the device (ps2_audio.c audio_cb), so the WAV taps hear what
   the speakers get. Three stages, one strength:
   1. Psychoacoustic bass. The band below ACZ_AUDIO_SPEAKERS_XO (120 Hz; mono, Linkwitz-Riley 24 dB/oct) is divided by its own
      envelope (the RMS of 40 ms, x sqrt 2: the amplitude of a sine) and fed to the Chebyshev polynomials T2 and T3, which turn
      a sine of any level into exactly its 2nd and 3rd harmonic; multiplied by the envelope again, the harmonics follow the
      bass's level linearly (no squaring of the dynamics). Band-passed to XO .. 4 x XO and added to both ears at
      ACZ_AUDIO_SPEAKERS_HARM (0.35 of the fundamental, about -9 dB; the 3rd at half of that). The ear hears the missing
      fundamental from them ("virtual bass", the research's rank 2 item).
   2. The sub-bass the drivers cannot move goes (ACZ_AUDIO_SPEAKERS_HP, 50 Hz, 12 dB/oct): it only costs headroom and buzzes.
   3. Gentle loudness compensation: a low shelf of ACZ_AUDIO_SPEAKERS_SHELF dB (2) below 250 Hz, where the speakers still play.
   Then a peak limiter at ACZ_AUDIO_SPEAKERS_CEIL dBFS (-1): instant attack, 120 ms release, from the louder channel. It acts
   only where the shaped mix would clip, so the 2006 mix keeps its dynamics and its balance above the bass.
   The dry chunk and the shaped one are blended by the strength (ACZ_AUDIO_SPEAKERS: 1 ON, 0.5 SUBTLE, 0 OFF; built-in 1; the Quest
   build only, or a PC build with PS2_QUEST_SETTINGS=ON), smoothed over 20 ms, so a change on the page does not click. At 0 the stage returns at once and
   its filters are reset: OFF is the dry mix, bit for bit.
   ACZ_DEBUG_SPEAKERS=N: every N s of audio a log line with the stage's CPU time per second of audio, the limiter's deepest gain
   reduction and the share of samples it touched. tools/spk_test.c runs the same code on a WAV file (CPU, band levels). */
#include "ps2_runtime.h"
#include "ps2_settings.h"                    /* ps2_audio_speakers_set */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SPK_BUILTIN 1.0f                      /* on unless the row or the switch says otherwise (compiled only for the Quest: CMakeLists.txt) */
#define SPK_RATE 48000.0f
#define SPK_2PI 6.283185307179586

typedef struct { float b0, b1, b2, a1, a2, z1, z2; } spk_bq;
static inline float bq_run(spk_bq *q, float x) {         /* transposed direct form II */
    float y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}
static void bq_set(spk_bq *q, double b0, double b1, double b2, double a0, double a1, double a2) {
    q->b0 = (float)(b0 / a0); q->b1 = (float)(b1 / a0); q->b2 = (float)(b2 / a0);
    q->a1 = (float)(a1 / a0); q->a2 = (float)(a2 / a0);
    q->z1 = q->z2 = 0.0f;
}
/* RBJ's cookbook: 2nd-order low / high pass at f (Q 0.7071: Butterworth), low shelf of g dB (slope 1) */
static void bq_lp(spk_bq *q, double f) {
    double w = SPK_2PI * f / SPK_RATE, c = cos(w), al = sin(w) / (2.0 * 0.70710678);
    bq_set(q, (1.0 - c) * 0.5, 1.0 - c, (1.0 - c) * 0.5, 1.0 + al, -2.0 * c, 1.0 - al);
}
static void bq_hp(spk_bq *q, double f) {
    double w = SPK_2PI * f / SPK_RATE, c = cos(w), al = sin(w) / (2.0 * 0.70710678);
    bq_set(q, (1.0 + c) * 0.5, -(1.0 + c), (1.0 + c) * 0.5, 1.0 + al, -2.0 * c, 1.0 - al);
}
static void bq_lshelf(spk_bq *q, double f, double g) {
    double A = pow(10.0, g / 40.0), w = SPK_2PI * f / SPK_RATE, c = cos(w), al = sin(w) / 2.0 * sqrt(2.0), sa = 2.0 * sqrt(A) * al;
    bq_set(q, A * ((A + 1.0) - (A - 1.0) * c + sa), 2.0 * A * ((A - 1.0) - (A + 1.0) * c), A * ((A + 1.0) - (A - 1.0) * c - sa),
           (A + 1.0) + (A - 1.0) * c + sa, -2.0 * ((A - 1.0) + (A + 1.0) * c), (A + 1.0) + (A - 1.0) * c - sa);
}

static struct {
    int init;
    float s;                                  /* the strength in effect (smoothed toward the page's) */
    float xo, harm, hp, shelf, ceil;          /* the switches */
    float k_s, k_env, kr_lim;                 /* per-sample coefficients: strength 20 ms, envelope 40 ms, limiter release */
    spk_bq lp1, lp2;                          /* the bass band, mono (two Butterworth = Linkwitz-Riley) */
    spk_bq hh, hl;                            /* the harmonics' band: XO .. 4 XO */
    spk_bq sub[2], shl[2];                    /* per ear: the sub-bass cut, the low shelf */
    float ms;                                 /* the bass band's mean square */
    float g;                                  /* the limiter's gain reduction (1 - gain; 0 = none) */
    int dbg;                                  /* ACZ_DEBUG_SPEAKERS: seconds between the lines, 0 none */
    double t_acc;                             /* the stage's wall time in this window, s */
    unsigned long long n_acc, n_lim;          /* frames in this window, frames the limiter touched */
    float g_min;
} spk;
static volatile int spk_req = -1;             /* the page's strength in thousandths (ps2_audio_speakers_set), -1 none */

static float envf(const char *n, float d, float lo, float hi) {
    const char *e = getenv(n);
    float v = e && *e ? (float)atof(e) : d;
    return v < lo ? lo : v > hi ? hi : v;
}
static void spk_filters(void) {
    bq_lp(&spk.lp1, spk.xo); bq_lp(&spk.lp2, spk.xo);
    bq_hp(&spk.hh, spk.xo); bq_lp(&spk.hl, 4.0 * spk.xo);
    for (int c = 0; c < 2; c++) {
        if (spk.hp > 0.0f) bq_hp(&spk.sub[c], spk.hp); else bq_set(&spk.sub[c], 1, 0, 0, 1, 0, 0);
        bq_lshelf(&spk.shl[c], 250.0, spk.shelf);
    }
    spk.ms = 0.0f;
    spk.g = 0.0f;
}
static void spk_init(void) {
    spk.init = 1;
    float s = envf("ACZ_AUDIO_SPEAKERS", SPK_BUILTIN, 0.0f, 1.5f);
    spk.xo = envf("ACZ_AUDIO_SPEAKERS_XO", 120.0f, 60.0f, 250.0f);
    spk.harm = envf("ACZ_AUDIO_SPEAKERS_HARM", 0.35f, 0.0f, 1.5f);
    spk.hp = envf("ACZ_AUDIO_SPEAKERS_HP", 50.0f, 0.0f, 120.0f);
    if (spk.hp > 0.0f && spk.hp < 20.0f) spk.hp = 20.0f;
    spk.shelf = envf("ACZ_AUDIO_SPEAKERS_SHELF", 2.0f, 0.0f, 6.0f);
    spk.ceil = powf(10.0f, envf("ACZ_AUDIO_SPEAKERS_CEIL", -1.0f, -6.0f, 0.0f) / 20.0f);
    spk.dbg = (int)envf("ACZ_DEBUG_SPEAKERS", 0.0f, 0.0f, 3600.0f);
    spk.k_s = 1.0f - expf(-1.0f / (0.020f * SPK_RATE));
    spk.k_env = 1.0f - expf(-1.0f / (0.040f * SPK_RATE));
    spk.kr_lim = expf(-1.0f / (0.120f * SPK_RATE));       /* (the reduction's decay per sample) */
    spk.g_min = 1.0f;
    spk_filters();
    if (__atomic_load_n(&spk_req, __ATOMIC_ACQUIRE) < 0) __atomic_store_n(&spk_req, (int)lrintf(s * 1000.0f), __ATOMIC_RELEASE);
    spk.s = s;
    ps2_log("audio: speaker profile (QUEST SPEAKERS, ACZ_AUDIO_SPEAKERS) %s, strength %.2f: the bass below %.0f Hz also as its 2nd "
            "and 3rd harmonics at %.2f, %s, %+.1f dB below 250 Hz, peaks held at %.1f dBFS", s > 0.0f ? "on" : "off", (double)s,
            (double)spk.xo, (double)spk.harm, spk.hp > 0.0f ? "the sub-bass cut below the cut-off" : "no sub-bass cut",
            (double)spk.shelf, 20.0 * log10((double)spk.ceil));
    if (spk.hp > 0.0f) ps2_log("audio: speaker profile: sub-bass cut at %.0f Hz (ACZ_AUDIO_SPEAKERS_HP)", (double)spk.hp);
}

/* the page (renderer thread): the new strength, in effect within 20 ms */
void ps2_audio_speakers_set(float s) {
    if (s < 0.0f) s = 0.0f;
    if (s > 1.5f) s = 1.5f;
    __atomic_store_n(&spk_req, (int)lrintf(s * 1000.0f), __ATOMIC_RELEASE);
    ps2_log("audio: speaker profile -> %.2f", (double)s);
}

static inline s16 spk_s16(float v) {
    float t = v * 32768.0f;
    t = t > 32767.0f ? 32767.0f : t < -32768.0f ? -32768.0f : t;
    return (s16)lrintf(t);
}

/* the audio thread: one chunk of interleaved stereo s16, in place */
void ps2_spk_process(s16 *buf, int frames) {
    if (!spk.init) spk_init();
    int req = __atomic_load_n(&spk_req, __ATOMIC_ACQUIRE);
    float tgt = (float)(req < 0 ? 0 : req) * 0.001f;
    if (tgt <= 0.0f && spk.s <= 0.0f) return;                 /* OFF: the dry mix, bit for bit */
    double t0 = spk.dbg ? ps2_wall_seconds() : 0.0;
    const float in = 1.0f / 32768.0f, ceil = spk.ceil, harm = spk.harm;
    unsigned lim = 0;
    for (int i = 0; i < frames; i++) {
        float s = spk.s;
        if (s != tgt) { s += (tgt - s) * spk.k_s; if (fabsf(tgt - s) < 1e-4f) s = tgt; spk.s = s; }
        float l = (float)buf[2 * i] * in, r = (float)buf[2 * i + 1] * in;
        /* 1. the bass band and its harmonics */
        float b = bq_run(&spk.lp2, bq_run(&spk.lp1, 0.5f * (l + r)));
        spk.ms += (b * b - spk.ms) * spk.k_env;
        float env = sqrtf(2.0f * spk.ms), h = 0.0f;
        if (env > 1e-4f) {                                    /* (below -80 dBFS: nothing to lift) */
            float x = b / env;
            x = x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x;
            float x2 = x * x;
            h = ((2.0f * x2 - 1.0f) + 0.5f * x * (4.0f * x2 - 3.0f)) * env;   /* T2 + T3 / 2 */
        }
        h = bq_run(&spk.hl, bq_run(&spk.hh, h)) * harm;
        /* 2, 3. the sub-bass out, the low shelf; then the blend with the dry mix */
        float wl = bq_run(&spk.shl[0], bq_run(&spk.sub[0], l)) + h;
        float wr = bq_run(&spk.shl[1], bq_run(&spk.sub[1], r)) + h;
        float ol = l + s * (wl - l), orr = r + s * (wr - r);
        /* the limiter: instant attack, slow release */
        float pk = fabsf(ol) > fabsf(orr) ? fabsf(ol) : fabsf(orr), want = pk > ceil ? ceil / pk : 1.0f;
        /* (kept as the reduction 1 - gain: a gain creeping up to 1 in float steps stalls about 2e-4 under it for good) */
        spk.g = spk.g < 1e-6f ? 0.0f : spk.g * spk.kr_lim;
        if (1.0f - want > spk.g) spk.g = 1.0f - want;
        float g = 1.0f - spk.g;
        if (spk.g > 0.0f) { lim++; if (g < spk.g_min) spk.g_min = g; }
        buf[2 * i] = spk_s16(ol * g);
        buf[2 * i + 1] = spk_s16(orr * g);
    }
    if (spk.s <= 0.0f && tgt <= 0.0f) spk_filters();          /* (faded out: the next ON starts from rest) */
    if (spk.dbg) {
        spk.t_acc += ps2_wall_seconds() - t0;
        spk.n_acc += (unsigned long long)frames;
        spk.n_lim += lim;
        if (spk.n_acc >= (unsigned long long)spk.dbg * 48000ull) {
            double secs = (double)spk.n_acc / SPK_RATE;
            ps2_log("audio: speaker profile: %.1f us of CPU per second of audio (%.4f %% of a core), strength %.2f, limiter "
                    "on %.2f %% of the samples, deepest %.2f dB", spk.t_acc / secs * 1e6, spk.t_acc / secs * 100.0, (double)spk.s,
                    100.0 * (double)spk.n_lim / (double)spk.n_acc, 20.0 * log10((double)spk.g_min));
            spk.t_acc = 0.0; spk.n_acc = spk.n_lim = 0; spk.g_min = 1.0f;
        }
    }
}
