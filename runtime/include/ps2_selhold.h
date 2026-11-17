/* ps2_selhold.h -- the View / Back (PS2 SELECT) filter of a VR run, timed by the clock (BUGS_RQ R-76, PS2_XR_SELECT_HOLD_MS).

   In VR, holding View recentres the view, so the game must never see a long Select: it gets Select only as a tap. The
   filter it replaces (ps2_video.c poll_input, the PC tree's block) counted calls of poll_input -- once a renderer-loop
   iteration, 75 to 193 a second in the user's flights of 2026-11-11, 169 to 193 in M18 --, sent a 6-call pulse on a
   release under 30 calls and recentred at 60: in M18 a press of 0.16 to 0.33 s sent nothing and one of ~0.33 s
   recentred (the user: "I press the weapon-change button and it does not work"). This one:
   - times the hold by the clock (now_ns): the recentre fires once a hold reaches hold_ns (1.0 s), and that hold sends
     no tap;
   - on the release, every other hold that is not the quit combo (Start seen during the hold: ps2_quit.c) queues one
     tap: there is no dead band between a tap and the recentre;
   - counts the tap in game fields (field = ps2_kernel_vblank_count()): Select is on for pulse_fields fields, then off
     for at least gap_fields before a queued next tap, so the game sees one press edge per tap at any loop rate (the
     latch, ps2_hle_pad.c, ORs every publish of a field into the next one: an "off" of 3 fields leaves at least one
     latched field without Select between two taps);
   - (2026-11-11, the device round) also takes `presses`, the press edges its source saw since the last call: the level
     is read once a call, so a press that starts and ends between two calls (an adb `input gamepad keyevent` tap is down
     and up within ~1 ms: 2 of 10 got through on the device) never showed in it. Such a press, with the level up and no
     hold running, queues one tap per press (as pad_seen in ps2_hle_pad.c keeps a press of less than a field). 0 =
     unknown, the level alone as before. The caller reads the counter before the level, so a press whose down edge
     lands between the two reads is seen as a hold in the next call, never twice.
   One function with its state in a struct, no globals: the PC tree's block can call the same function (the outside
   session asks the user about that port). The host test is tools/quest/selhold_test.c. */
#ifndef PS2_SELHOLD_H
#define PS2_SELHOLD_H

#define PS2_SELHOLD_SELECT   1u     /* send Select to the game in this call's pad state */
#define PS2_SELHOLD_RECENTRE 2u     /* recentre the view now (once per hold) */
#define PS2_SELHOLD_TAPPED   4u     /* a release queued a tap in this call (for the log) */
#define PS2_SELHOLD_COMBO    8u     /* a release ended a hold that was the quit combo (for the log) */

typedef struct {
    unsigned long long t0_ns;       /* the current hold began */
    unsigned long long held_ns;     /* the last finished hold's length (for the log) */
    unsigned long long pf;          /* the field the running pulse or gap began at */
    int down, combo, fired;         /* Back down; Start seen in this hold; the recentre fired in this hold (or the hold is void) */
    int phase;                      /* 0 idle, 1 Select on, 2 the gap after it */
    int pending;                    /* taps waiting for their pulse */
} ps2_selhold;

static inline unsigned ps2_selhold_step(ps2_selhold *s, int back, int start, unsigned presses, unsigned long long now_ns,
                                        unsigned long long field, unsigned long long hold_ns, unsigned pulse_fields,
                                        unsigned gap_fields) {
    unsigned out = 0;
    if (back) {
        if (!s->down) { s->down = 1; s->t0_ns = now_ns; s->combo = 0; s->fired = 0; }
        if (start) s->combo = 1;
        if (!s->combo && !s->fired && now_ns - s->t0_ns >= hold_ns) { s->fired = 1; out |= PS2_SELHOLD_RECENTRE; }
    } else if (s->down) {
        s->down = 0;
        s->held_ns = now_ns - s->t0_ns;
        if (s->combo) out |= PS2_SELHOLD_COMBO;
        else if (!s->fired) { if (s->pending < 3) s->pending++; out |= PS2_SELHOLD_TAPPED; }
    } else if (presses && !start) {                 /* pressed and released between two calls: one tap a press */
        s->held_ns = 0;
        for (unsigned k = 0; k < presses && s->pending < 3; k++) s->pending++;
        out |= PS2_SELHOLD_TAPPED;
    }
    if (s->phase == 1 && field - s->pf >= pulse_fields) { s->phase = 2; s->pf = field; }
    if (s->phase == 2 && field - s->pf >= gap_fields) s->phase = 0;
    if (s->phase == 0 && s->pending > 0) { s->pending--; s->phase = 1; s->pf = field; }
    if (s->phase == 1) out |= PS2_SELHOLD_SELECT;
    return out;
}
/* the controller was taken by a page (ps2_ui_blocks_game_input): a hold that began before it sends nothing on its
   release, and a tap not yet sent is dropped */
static inline void ps2_selhold_void(ps2_selhold *s) {
    if (s->down) s->fired = 1;
    s->phase = 0;
    s->pending = 0;
}

#endif
