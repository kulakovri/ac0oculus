/* rq_thin.inc -- the far march's thin-medium step capped in metres (rq20_edges 2026-11-16, the user's F-2 "pattern" at the edge of far
   clouds, BUGS_RQ R-270). Included by ps2_clouds_far.c inside PS2_RQ_SUPPORT, just before rq_far_record, on a line that existed before
   (no PC line moves: ps2_vkcen.h puts __LINE__ into the PC objects).

   PS2_RQ_CLOUDS_THIN_MAX=M (metres; 0 = off, t13h; the Quest default in android/ps2_android.c): the far march's thin-medium step
   (ACZ_VR_CLOUD_FAR_THIN / PS2_RQ_CLOUDS_ONE_THIN: in faint cloud the step of 0.35 optical depth x the factor, up to the empty-space step
   = 3 fine steps) at most M metres, and never shorter than the fine step (rq_cloud_far.glsl cl_far_march, fp.sch.w -- the PC scheduler's
   field, which no rq shader read). From the eye the fine step is t x 0.045 (ACZ_VR_CLOUD_RQ_KSTEP x ACZ_VR_CLOUD_RQ_FINE 1.0), so beyond
   ~1.1 km the faint fringe of a body was sampled every 150-800 m, over the bake's 78 m cells: each map texel's one or two long samples
   (at its own white-noise jitter) hit or missed the fringe's wisps, a 0.4 deg texel apart, and the octahedral map's diagonal texels
   upsampled that into dark notches and a combed grain along the outline and over the body's face. With the cap the fringe is sampled
   at least every M metres near the eye and at the fine step from M / 0.045 on (3.3 km at 150 m); within M / 0.135 of the eye (1.1 km)
   3 fine steps are under M and nothing changes.
   [test] PS2_RQ_CLOUDS_THIN_MAX_AB=1 with PS2_RQ_ABSKIP=N: the cap is off in the left-out blocks of N eye-pass lists -- the `rq-ab:`
   lines give the frame's GPU time with and without it; with PS2_RQ_CLOUDS_FARTIME=F the far update's own time per arm
   (`clouds far rq time: ... PS2_RQ_CLOUDS_THIN_MAX`).
   [test] PS2_RQ_CLOUDS_TERM_ALT=K1,K2,.. (the cut of the frame by term): GS frames -- the eye dumps' numbers -- cycle through the
   march as configured (frame % (n + 1) == 0) and the n listed arms, each changing terms of the far march, so dumps of one run show each
   term's share a few fields apart (cross-run dumps are not one camera, R-135). Arms: 1 no per-texel jitter (fp.mz.x 0), 2 no
   thin-medium step (haze.x 0), 3 the step growth / 3 with 192 more steps, 4 the empty-space step x1 instead of x3 (stp.x; it also caps
   the thin step at one fine step), 5 no detail octave (bk3.x), 6 the converged march (2 + 4, the step growth / 4, 192 more steps),
   8 = 2 + 4, 9 the cap off (t13h), 20 <= K < 1000 the cap at K metres, K >= 1000 the cap at K % 1000 metres while the ray's
   transmittance is over (K / 1000) / 10 (5150: 150 m while T > 0.5). */
static float rq_thin_max(void) {
    static float v = -1.0f;
    if (v < 0.0f) {
        v = env_f("PS2_RQ_CLOUDS_THIN_MAX", 0.0f, 0.0f, 5000.0f);
        if (v > 0.0f) ps2_log("clouds far rq: the far march's thin-medium step at most %.0f m, never under the fine step (PS2_RQ_CLOUDS_THIN_MAX)", (double)v);
    }
    return v;
}
static int rq_thin_ab(void) {
    static int v = -1;
    if (v < 0) {
        v = env_f("PS2_RQ_CLOUDS_THIN_MAX_AB", 0.0f, 0.0f, 1.0f) > 0.5f && rq_thin_max() > 0.0f;
        if (v) ps2_log("clouds far rq: [test] PS2_RQ_CLOUDS_THIN_MAX_AB: the cap off in the left-out blocks of PS2_RQ_ABSKIP lists");
    }
    return v;
}
/* PS2_RQ_CLOUDS_THIN_MAX_T (0 = always): the cap only while the ray's transmittance is over this (fp.sch.x, the PC scheduler's other
   field): the fringe that shows is the first cloud a ray meets; behind that much cloud the long steps do not show and cost nothing more
   (the price of the cap is in faint veils seen through: M4, M11) */
static float rq_thin_t(void) {
    static float v = -1.0f;
    if (v < 0.0f) {
        v = env_f("PS2_RQ_CLOUDS_THIN_MAX_T", 0.0f, 0.0f, 1.0f);
        if (v > 0.0f && rq_thin_max() > 0.0f) ps2_log("clouds far rq: the thin step's cap only while the ray's transmittance is over %.2f (PS2_RQ_CLOUDS_THIN_MAX_T)", (double)v);
    }
    return v;
}
/* the FARTIME arm of this frame: 1 = the cap on, 0 = off (only with PS2_RQ_CLOUDS_THIN_MAX_AB) */
static int rq_thin_arm(void) { extern int ps2_rq_ab_left_out(void); return rq_thin_ab() ? !ps2_rq_ab_left_out() : 0; }
/* this frame's thin cap and the term cut's arm into the slot's params and the push constants */
static void rq_thin_frame(int fi, far_push *pc) {
    far_params *tp = slot_params(fi);
    tp->sch[3] = rq_thin_ab() && !rq_thin_arm() ? 0.0f : rq_thin_max();
    tp->sch[0] = rq_thin_t();
    static int nt_ = -1, arm_[8];
    if (nt_ < 0) {
        nt_ = 0;
        const char *s = getenv("PS2_RQ_CLOUDS_TERM_ALT");
        for (char *q; s && *s && nt_ < 8; s = *q ? q + 1 : q) { long k = strtol(s, &q, 10); if (q == s) break; arm_[nt_++] = (int)k; }
        if (nt_) ps2_log("clouds far rq: [test] PS2_RQ_CLOUDS_TERM_ALT: GS frames cycle the configured march and %d arms (the first %d)", nt_, arm_[0]);
    }
    if (!nt_) return;
    extern u32 ps2_gs_frame_count;
    int a = (int)(ps2_gs_frame_count % (u32)(nt_ + 1)), term = a ? arm_[a - 1] : 0;
    if (ps2_gs_frame_count % 300u <= (u32)nt_)
        ps2_log("clouds far rq: [test] TERM_ALT: GS frame %u field %llu arm %d (term %d)", (unsigned)ps2_gs_frame_count,
                (unsigned long long)ps2_kernel_vblank_count(), a, term);
    if (term == 1) tp->mz[0] = 0.0f;
    if (term == 2 || term == 6 || term == 8) tp->haze[0] = 0.0f;
    if (term == 3) { pc->march[2] *= 1.0f / 3.0f; pc->march[3] += 192.0f; }
    if (term == 4 || term == 6 || term == 8) tp->stp[0] = 1.0f;
    if (term == 5) tp->bk3[0] = 0.0f;
    if (term == 6) { pc->march[2] *= 0.25f; pc->march[3] += 192.0f; }
    if (term == 9) tp->sch[3] = 0.0f;
    if (term >= 20 && term < 1000) { tp->sch[3] = (float)term; tp->sch[0] = 0.0f; }
    if (term >= 1000) { tp->sch[3] = (float)(term % 1000); tp->sch[0] = 0.1f * (float)(term / 1000); }   /* (5150: 150 m while T > 0.5) */
}
/* the FARTIME line per arm (PS2_RQ_CLOUDS_THIN_MAX_AB) */
static void rq_thin_log(const double tms[2], const double tmm[2], const uint32_t cnt[2]) {
    ps2_log("clouds far rq time: far update %.3f ms (the march %.3f) with PS2_RQ_CLOUDS_THIN_MAX %.0f m, T > %.2f (%u frames), %.3f ms (%.3f) without "
            "(%u): the cap costs %.3f ms", tms[1] / cnt[1], tmm[1] / cnt[1], (double)rq_thin_max(), (double)rq_thin_t(), cnt[1], tms[0] / cnt[0], tmm[0] / cnt[0],
            cnt[0], tms[1] / cnt[1] - tms[0] / cnt[0]);
}
