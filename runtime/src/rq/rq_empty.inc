/* rq_empty.inc -- PS2_RQ_EMPTY_HOLD (rq22_blink 2026-11-17, BUGS_RQ R-290; included by rq_video.inc, PS2_RQ_SUPPORT only; ps2_video.c
   calls it through PS2_RQ_EMPTY_BEGIN / PS2_RQ_EMPTY_KEEP of ps2_vk.h and on the lines of rq_kind_count, each on the line it extends,
   so the PC objects keep their line numbers).

   Default 1; `0` = as before. A list without draws right after an eye-pass list changes nothing. Such a list is a field in which the
   game finished no frame: a game frame over two fields, as the device's GPU-bound frames make them (kills and explosions in cloud at
   the dynamic scale's floor; PS2_TEST_EE_STALL makes them on the simulator). The headset already keeps the previous images for it
   (ps2_video.c xr_hold: "kept the previous stereo images (list of 0 draws, scene frame 0)"), but with no tag of its own (cpit_seq 0)
   it was counted a static frame (PS2_RQ_KINDS: no head pose):
   - PS2_RQ_KINDS_SEED (rq_seed_slot0, R-51) blitted each eye's crop of the feed stretched over the whole of slot 0 -- right for the
     static screens' readers of slot 0, wrong here;
   - the frame's end copied slot 0 back into the feed (rq_feed_update);
   - the next eye frame's readers of the feed -- the wet canopy's world tap (rq_cockpit.inc rqw_draw), the game's glow (its 2:1 copy
     of slot 0), the scene light -- read the eye's own picture as the game's frame: a darker translucent copy of the whole picture over
     the glass, magnified by 1 / crop (1.24 x 1.19 on the Quest 3's left eye) about the eye's outer bottom corner, for one frame, a
     fainter one in the frame after (the user's "blinking" of the 11-17 flights: M1, ~150 such lists, every detected blink within 32
     fields of one);
   - the cockpit screens' plan (ACZ_VR_MFD_DUAL) went off for that list (no cockpit draws in it) and on again: the scope black or both
     screens on the radar for a frame, and the game's radar / big-map alternation restarted (zero_vr.c `both maps off / on`).
   Now such a list leaves slot 0, the feed, the eye -> static bookkeeping (rqk_prev_e: a real static frame after it still seeds) and the
   screens' plan as they are. A list with draws is never taken (a cut-in's frozen background reads slot 0 in a list of its own).
   Lines `rq frames: an empty list at field F right after an eye-pass list (N): held ...` (the first 8, then every 100th) and
   `empty lists after an eye frame N (held M)` in rq_kind_count's 600-list line.
   [test] PS2_RQ_EMPTY_DUMP=K: the first K such lists ask for an eye dump of the next frame (ps2_vk_eyedump_req), hold on or off: the
   frame that showed the copy, on the device too. */
static int rqk_prev_e;                         /* (tentative: ps2_video.c's own definition follows, after rq_seed_slot0) */
static int rq_empty_list;                      /* this list has no draws and follows an eye-pass list (the list's own drawing) */
static int rq_empty_now;                       /* ... and PS2_RQ_EMPTY_HOLD holds it */
static u64 rq_empty_n, rq_held_n;
static int rq_empty_cfg(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("PS2_RQ_EMPTY_HOLD"); v = !(e && *e == '0');
        ps2_log("vk: PS2_RQ_EMPTY_HOLD=%d: a list without draws right after an eye-pass list %s", v,
                v ? "changes nothing (no seed of slot 0, no feed update, the screens' plan kept)" : "is a static frame (slot 0 seeded, the feed updated from it; as before)");
    }
    return v;
}
/* render_list_body's start (PS2_RQ_EMPTY_BEGIN): the list's own drawing decides, not an A/B re-draw, a bench repeat, the clean window's
   tail or the cinematic view's pass */
static void rq_empty_begin(void) {
    if (perf_ab_pass || bench_pass || cc_extra || xr_second_pass || rcl_tail || cine_t) return;
    rq_empty_list = rq_on() == 3 && !rep_ndraws && rqk_prev_e && rq_kinds();
    rq_empty_now = rq_empty_list && rq_empty_cfg();
}
/* rq_kind_count, for such a list: counted, logged, and with PS2_RQ_EMPTY_DUMP the next frame dumped */
static void rq_empty_note(void) {
    static int dump_k = -1;
    if (dump_k < 0) { const char *e = getenv("PS2_RQ_EMPTY_DUMP"); dump_k = e && *e ? atoi(e) : 0; if (dump_k < 0) dump_k = 0; }
    rq_empty_n++;
    if (rq_empty_now) rq_held_n++;
    if (rq_empty_n <= 8u || !(rq_empty_n % 100u))
        ps2_log("rq frames: an empty list at field %u right after an eye-pass list (%llu): %s (PS2_RQ_EMPTY_HOLD)", ps2_gs_frame_count, (unsigned long long)rq_empty_n,
                rq_empty_now ? "held: slot 0, the feed and the screens' plan unchanged" : "a static frame: slot 0 seeded, the feed from it");
    if (dump_k > 0) { dump_k--; ps2_vk_eyedump_req = 1; ps2_log("rq frames: [test] PS2_RQ_EMPTY_DUMP: the next frame dumped (field %u)", ps2_gs_frame_count); }
}
