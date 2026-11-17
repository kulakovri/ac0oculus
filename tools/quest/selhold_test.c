/* selhold_test.c -- the gate of BUGS_RQ R-76 (PS2_XR_SELECT_HOLD_MS): the View / Back filter of runtime/include/ps2_selhold.h
   on the PC, against the filter it replaces (ps2_video.c poll_input: counts of loop iterations, a tap under 30, the
   recentre at 60).

   Raw Back presses (and Start for the quit combo) on a millisecond time line go through the filter at a renderer-loop
   rate (60, 72, 90, 120 and 180 a second -- the six flights' logs counted 75-193 input polls a second, M18 169-193;
   several loop phases, and a jittered loop); the filter's output is published and
   latched once a field at 59.94 fields a second the way ps2_hle_pad.c does it (the game's state = the last publish OR
   every publish since the previous latch). The test counts the press edges the game sees in the latched state and
   the recentres.

   Expected (the prompt's gate): presses of 50, 150, 300, 600 and 900 ms: exactly one Select edge each and no recentre;
   a hold of 1.2 s: one recentre and no edge; Back with Start: neither. Also two taps 130 ms apart: two edges; three
   taps 90 ms apart: three edges (queued: each pulse waits for the gap after the one before). The device round of
   2026-11-11: presses of 1 and 5 ms (an adb `input gamepad keyevent` tap is down and up within ~1 ms) and ten 1 ms
   taps: one edge each, through the press counter (`presses`, read before the level as poll_input does); a 1 ms tap
   with Start held: neither.

   Build and run (Git Bash, MSYS2's gcc):
     export PATH=/c/msys64/ucrt64/bin:$PATH
     gcc -O1 -Wall -Wextra -Iruntime/include -o build/selhold_test.exe tools/quest/selhold_test.c && build/selhold_test.exe
   Exit 0 = every check passed. */
#include "ps2_selhold.h"

#include <stdio.h>
#include <string.h>

#define FIELD_NS 16683350ull                  /* 1 / 59.94 s */
#define HOLD_NS 1000000000ull                 /* PS2_XR_SELECT_HOLD_MS=1000 */

typedef struct { int t_on, t_off; } span;     /* ms; [on, off) */
typedef struct {
    const char *name;
    span back[10]; int nback;
    span start[1]; int nstart;
    int want_edges, want_rec;
} scenario;

static int in_spans(const span *s, int n, double t_ms) {
    for (int i = 0; i < n; i++) if (t_ms >= s[i].t_on && t_ms < s[i].t_off) return 1;
    return 0;
}
static unsigned downs(const span *s, int n, double t_ms) {   /* press edges so far (the pad layer's counter) */
    unsigned c = 0;
    for (int i = 0; i < n; i++) c += t_ms >= s[i].t_on;
    return c;
}

/* the old filter (ps2_video.c poll_input before R-76): counts of calls */
typedef struct { int held, pulse, combo; } oldsel;
static unsigned old_step(oldsel *o, int back, int start) {
    unsigned out = 0;
    if (back) {
        if (start) o->combo = 1;
        if (!o->combo && ++o->held == 60) out |= PS2_SELHOLD_RECENTRE;
    } else {
        if (o->held > 0 && o->held < 30 && !o->combo) o->pulse = 6;
        o->held = 0;
        o->combo = 0;
    }
    if (o->pulse > 0) { o->pulse--; out |= PS2_SELHOLD_SELECT; }
    return out;
}

typedef struct { int edges, rec, on_min, on_max; } result;

/* one run: loop rate hz, loop phase (0..1 of a loop period), jitter (0 or 1: +-35 % per interval, deterministic) */
static result run(const scenario *sc, double hz, double phase, int jitter, int use_old, int use_presses) {
    result r = { 0, 0, 1 << 30, 0 };
    ps2_selhold s; memset(&s, 0, sizeof s);
    oldsel o; memset(&o, 0, sizeof o);
    unsigned long long field = 0;             /* fields latched so far (ps2_kernel_vblank_count) */
    int poll = 0, seen = 0, host_prev = 0, run_len = 0;
    double loop_ns = 1e9 / hz;
    double t_loop = phase * loop_ns;          /* next loop call (ns) */
    double t_field = FIELD_NS * 0.37;         /* next field latch (ns): a phase of its own */
    unsigned lcg = 12345u, seen_downs = 0;
    const double end_ns = 3.0e9;
    while (t_loop < end_ns || t_field < end_ns) {
        if (t_field <= t_loop) {              /* the latch (ps2_pad_latch) */
            int host = poll | seen;
            seen = poll;
            field++;
            if (host && !host_prev) { r.edges++; run_len = 0; }
            if (host) run_len++;
            if (!host && host_prev) { if (run_len < r.on_min) r.on_min = run_len; if (run_len > r.on_max) r.on_max = run_len; }
            host_prev = host;
            t_field += FIELD_NS;
        } else {                              /* a renderer-loop iteration (poll_input) */
            double t_ms = t_loop / 1e6;      /* the counter is read first, the level 0.05 ms later (poll_input's order) */
            unsigned d = downs(sc->back, sc->nback, t_ms), presses = use_presses ? d - seen_downs : 0u;
            seen_downs = d;
            int back = in_spans(sc->back, sc->nback, t_ms + 0.05), start = in_spans(sc->start, sc->nstart, t_ms + 0.05);
            unsigned out = use_old ? old_step(&o, back, start)
                                   : ps2_selhold_step(&s, back, start, presses, (unsigned long long)t_loop, field, HOLD_NS, 4u, 3u);
            if (out & PS2_SELHOLD_RECENTRE) r.rec++;
            poll = (out & PS2_SELHOLD_SELECT) ? 1 : 0;
            seen |= poll;                     /* (ps2_pad_publish: pad_seen |= buttons) */
            double step = loop_ns;
            if (jitter) { lcg = lcg * 1103515245u + 12345u; step *= 0.65 + 0.7 * (double)((lcg >> 8) & 0xFFFF) / 65535.0; }
            t_loop += step;
        }
    }
    if (r.on_min == 1 << 30) r.on_min = 0;
    return r;
}

int main(void) {
    static const scenario sc[] = {
        { "press 50 ms",   { { 500, 550 } }, 1, { { 0, 0 } }, 0, 1, 0 },
        { "press 150 ms",  { { 500, 650 } }, 1, { { 0, 0 } }, 0, 1, 0 },
        { "press 300 ms",  { { 500, 800 } }, 1, { { 0, 0 } }, 0, 1, 0 },
        { "press 600 ms",  { { 500, 1100 } }, 1, { { 0, 0 } }, 0, 1, 0 },
        { "press 900 ms",  { { 500, 1400 } }, 1, { { 0, 0 } }, 0, 1, 0 },
        { "hold 1.2 s",    { { 500, 1700 } }, 1, { { 0, 0 } }, 0, 0, 1 },
        { "Back+Start 300 ms", { { 500, 800 } }, 1, { { 550, 900 } }, 1, 0, 0 },
        { "Back+Start 1.5 s",  { { 500, 2000 } }, 1, { { 600, 2100 } }, 1, 0, 0 },
        { "two taps 80 ms, 130 ms apart", { { 500, 580 }, { 710, 790 } }, 2, { { 0, 0 } }, 0, 2, 0 },
        { "three taps 40 ms, 90 ms apart", { { 500, 540 }, { 590, 630 }, { 680, 720 } }, 3, { { 0, 0 } }, 0, 3, 0 },
        { "press 1 ms (an adb tap)", { { 500, 501 } }, 1, { { 0, 0 } }, 0, 1, 0 },
        { "press 5 ms",    { { 500, 505 } }, 1, { { 0, 0 } }, 0, 1, 0 },
        { "ten 1 ms taps, 200 ms apart", { { 500, 501 }, { 700, 701 }, { 900, 901 }, { 1100, 1101 }, { 1300, 1301 },
          { 1500, 1501 }, { 1700, 1701 }, { 1900, 1901 }, { 2100, 2101 }, { 2300, 2301 } }, 10, { { 0, 0 } }, 0, 10, 0 },
        { "1 ms tap with Start held", { { 500, 501 } }, 1, { { 400, 700 } }, 1, 0, 0 },
    };
    static const double rates[] = { 60.0, 72.0, 90.0, 120.0, 180.0 };   /* (M18's log: 169-193 polls a second) */
    static const double phases[] = { 0.0, 0.31, 0.77 };
    int nsc = (int)(sizeof sc / sizeof sc[0]), fails = 0, runs = 0;
    printf("selhold_test: the new filter (hold 1000 ms by the clock, Select 4 fields, gap 3) against the old one (call counts)\n");
    printf("%-30s %6s | new: edges rec (fields on)        | level only | old: edges rec\n", "case", "loop/s");
    for (int i = 0; i < nsc; i++) {
        for (size_t k = 0; k < sizeof rates / sizeof rates[0]; k++) {
            int e_min = 1 << 30, e_max = -1, r_min = 1 << 30, r_max = -1, on_lo = 1 << 30, on_hi = 0, l_min = 1 << 30, l_max = -1;
            for (int j = 0; j < (int)(sizeof phases / sizeof phases[0]) + 1; j++) {   /* the phases, then a jittered loop */
                result r = run(&sc[i], rates[k], j < 3 ? phases[j] : 0.5, j == 3, 0, 1);
                result lv = run(&sc[i], rates[k], j < 3 ? phases[j] : 0.5, j == 3, 0, 0);   /* the level alone (2df1d44) */
                if (lv.edges < l_min) l_min = lv.edges;
                if (lv.edges > l_max) l_max = lv.edges;
                runs++;
                if (r.edges != sc[i].want_edges || r.rec != sc[i].want_rec) {
                    fails++;
                    printf("FAIL %s at %.0f/s (%s): %d edges, %d recentres; want %d, %d\n", sc[i].name, rates[k],
                           j < 3 ? "phase" : "jitter", r.edges, r.rec, sc[i].want_edges, sc[i].want_rec);
                }
                if (r.edges < e_min) e_min = r.edges;
                if (r.edges > e_max) e_max = r.edges;
                if (r.rec < r_min) r_min = r.rec;
                if (r.rec > r_max) r_max = r.rec;
                if (r.edges && r.on_min < on_lo) on_lo = r.on_min;
                if (r.on_max > on_hi) on_hi = r.on_max;
            }
            result o = run(&sc[i], rates[k], 0.0, 0, 1, 0);
            char on[24] = "-";
            if (e_max > 0) snprintf(on, sizeof on, "%d-%d", on_lo, on_hi);
            printf("%-30s %6.0f | %d-%d edges, %d-%d rec (%-5s)   %s | %2d-%-2d      | %d edges, %d rec%s\n", sc[i].name,
                   rates[k], e_min, e_max, r_min, r_max, on, (e_min == sc[i].want_edges && e_max == sc[i].want_edges
                   && r_min == sc[i].want_rec && r_max == sc[i].want_rec) ? "ok  " : "FAIL", l_min, l_max, o.edges, o.rec,
                   (o.edges != sc[i].want_edges || o.rec != sc[i].want_rec) ? "  (old: wrong)" : "");
        }
    }
    printf("selhold_test: %d runs, %d failed\n", runs, fails);
    return fails ? 1 : 0;
}
