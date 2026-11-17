/* rq_passes.inc -- the frame's small passes (row G of plan 2; included by ps2_video.c, PS2_RQ_SUPPORT only). The off-screen runs of an eye list are
   30 render passes for 91 draws (M4): a pass is paid for its load, store and flushes whatever it holds. Each part has its own switch, default off, =alt
   every second list (the picture check):
   - PS2_RQ_PASSFIT=1: a run's render area is the bounds of its draws instead of the whole 512x448 target (the targets are not right-sized at upscale 1,
     rtz_active): load and store of what the run touches. Exact: outside the area nothing changes.
   - PS2_RQ_PASSZ=1: a run that does not test or write depth and whose depth nothing reads later neither loads nor stores it (DONT_CARE; core passes: fg_rpz).
   - PS2_RQ_PASSDROP=1: dead-pass elimination by version: walking the list backwards, a run into a slot is kept only if a kept draw reads what it leaves
     (the bounds read; an opaque rectangle covering them ends the older versions). The game's cloud sprites replaced by our clouds do not count as readers
     (the same drop as rqo_imp_mask, which works per slot and so keeps a slot reused later in the list). Slots read before their first write this frame are kept for
     the next list (the last list as the prediction). Never: RT_MFD, a slot in the hoist's snapshot mask, the displayed target (next switch).
   - PS2_RQ_NOCOMP=1: the displayed target's composite (rt1) when nothing in the list reads it; the eye path presents its own images (rq_eye_present).
   - PS2_RQ_BLIT=1: a run that is one opaque textured rectangle copying another target at a whole-pixel affine map (the glow chain's 2:1 steps, the sun probe's
     16 > 8 > 4 > 2 > 1) as vkCmdBlitImage, no render pass. A draw at the texel-centre of a 2:1 step and a linear blit read the same two texels.
   Log: rq-g: every 300 lists. Hooks: rqg_plan at the list's start, rqg_at in the draw loop, rqg_arm before select_target, rqpf (ps2_video.c) in the begins. */
#define RQG_RUNS 640u

typedef struct {
    u32 first, n;                     /* positions in the order */
    u16 rt, zrt;
    u8 live, blit, z_dc, z_touch, z_write;
    float box[4];                     /* the run's drawn bounds, game pixels */
    float nb[4];                      /* (log) what is read of it */
    u16 src;
    u8 linear, feed;                  /* (feed: the source is slot 0: the eye path reads rq_feed, the previous frame's copy) */
    s32 sr[4], dr[4];                 /* blit: source and destination x0, y0, x1, y1 */
} rqg_run;
static struct {
    int active, a_fit, a_z, a_drop, a_blit, a_comp;
    u32 alt, gen, nrun;
    u32 *map;                         /* per draw: (list stamp << 16) | (run + 1) */
    rqg_run run[RQG_RUNS];
    u64 s_lists, s_runs, s_dead, s_blit, s_zdc;
    double s_px, s_fit;
} rqg;

static int rqg_sw(const char *n, int *v) {
    if (*v < 0) { const char *e = getenv(n); *v = !e ? 0 : *e == '1' ? 1 : !strcmp(e, "alt") ? 2 : 0; if (*v) ps2_log("vk: %s=%s", n, e); }
    return *v == 1 || (*v == 2 && (rqg.alt & 1u));
}
/* a draw that is one axis-aligned rectangle (any number of quads tiling it exactly) with s, t affine in x, y alone */
typedef struct { s32 dx0, dy0, dx1, dy1; float ks, sa, kt, ta; int bit25, point; } rqg_rc;
static float rqg_st(const vk_draw *d, const ps2_vk_vertex *v, int t) {
    float q = v->q > 1e-9f ? v->q : 1e-9f;
    if ((d->frag_flags >> 2) & 1u) return t ? v->t : v->s;
    return t ? v->t / q * d->tex_h : v->s / q * d->tex_w;
}
static int rqg_rect(const vk_draw *d, rqg_rc *R) {
    float b[4];
    if (d->native || d->topology != PS2_VK_TRIANGLES || d->count < 6u || (d->count % 3u) || d->first + d->count > rep_nverts) return 0;
    {   /* (the triangles tile their bounds exactly: strips of quads as the game cuts them) */
        double area = 0.0;
        b[0] = b[1] = 1e9f; b[2] = b[3] = -1e9f;
        for (u32 q = 0; q + 2u < d->count; q += 3u) {
            const ps2_vk_vertex *A = &rep_verts[d->first + q], *B = A + 1, *C = A + 2;
            double ax = B->x - A->x, ay = B->y - A->y, cx = C->x - A->x, cy = C->y - A->y;
            area += fabs(ax * cy - ay * cx) * 0.5;
            for (int k = 0; k < 3; k++) {
                const ps2_vk_vertex *v = k == 0 ? A : k == 1 ? B : C;
                if (v->x < b[0]) b[0] = v->x;
                if (v->y < b[1]) b[1] = v->y;
                if (v->x > b[2]) b[2] = v->x;
                if (v->y > b[3]) b[3] = v->y;
            }
        }
        double box = (double)(b[2] - b[0]) * (double)(b[3] - b[1]);
        if (box <= 0.0 || fabs(area - box) > 0.01 + box * 0.001) return 0;
    }
    const ps2_vk_vertex *V = &rep_verts[d->first];
    float ks = 0.0f, kt = 0.0f; int hx = 0, hy = 0;
    float s0 = rqg_st(d, &V[0], 0), t0 = rqg_st(d, &V[0], 1);
    for (u32 q = 0; q < d->count; q++) {
        if (fabsf(V[q].xr_dx) > 1e-5f || fabsf(V[q].q - V[0].q) > 1e-5f) return 0;
        if (!hx && fabsf(V[q].x - V[0].x) > 1e-3f) { ks = (rqg_st(d, &V[q], 0) - s0) / (V[q].x - V[0].x); hx = 1; }
        if (!hy && fabsf(V[q].y - V[0].y) > 1e-3f) { kt = (rqg_st(d, &V[q], 1) - t0) / (V[q].y - V[0].y); hy = 1; }
    }
    if (!hx || !hy) return 0;
    R->sa = s0 - ks * V[0].x; R->ta = t0 - kt * V[0].y; R->ks = ks; R->kt = kt;
    for (u32 q = 0; q < d->count; q++) {
        if (fabsf(rqg_st(d, &V[q], 0) - (R->sa + ks * V[q].x)) > 2e-2f || fabsf(rqg_st(d, &V[q], 1) - (R->ta + kt * V[q].y)) > 2e-2f) return 0;
    }
    R->dx0 = (s32)ceilf(b[0] - 0.5f); R->dx1 = (s32)ceilf(b[2] - 0.5f);    /* (the pixels whose centres the rectangle holds) */
    R->dy0 = (s32)ceilf(b[1] - 0.5f); R->dy1 = (s32)ceilf(b[3] - 0.5f);
    R->bit25 = (d->frag_flags >> 25) & 1u; R->point = (d->frag_flags >> 19) & 1u;
    return R->dx1 > R->dx0 && R->dy1 > R->dy0;
}
/* the texels a draw reads of its source, in target pixels: [b0, b2) x [b1, b3) */
static void rqg_read_box(const vk_draw *d, const vk_target *Y, float b[4]) {
    float W = (float)tew(Y), H = (float)teh(Y);
    rqg_rc R;
    if (tsc(Y) == 1u && rqg_rect(d, &R)) {
        float u0 = R.sa + R.ks * ((float)R.dx0 + 0.5f), u1 = R.sa + R.ks * ((float)R.dx1 - 0.5f);
        float v0 = R.ta + R.kt * ((float)R.dy0 + 0.5f), v1 = R.ta + R.kt * ((float)R.dy1 - 0.5f);
        if (u0 > u1) { float x = u0; u0 = u1; u1 = x; }
        if (v0 > v1) { float x = v0; v0 = v1; v1 = x; }
        if (R.point) { b[0] = floorf(u0 + 1.0f / 32.0f); b[2] = floorf(u1 + 1.0f / 32.0f) + 1.0f; b[1] = floorf(v0 + 1.0f / 32.0f); b[3] = floorf(v1 + 1.0f / 32.0f) + 1.0f; }
        else {
            float o = R.bit25 ? -0.5f : 0.0f;                  /* (the texels on either side of the sampling point) */
            b[0] = floorf(u0 + o); b[2] = floorf(u1 + o) + 2.0f; b[1] = floorf(v0 + o); b[3] = floorf(v1 + o) + 2.0f;
        }
    } else if (!d->native && d->count && d->first + d->count <= rep_nverts && tsc(Y) == 1u) {
        float s0 = 1e9f, s1 = -1e9f, t0 = 1e9f, t1 = -1e9f;
        for (u32 q = 0; q < d->count; q++) {
            const ps2_vk_vertex *v = &rep_verts[d->first + q];
            float s = rqg_st(d, v, 0), t = rqg_st(d, v, 1);
            if (s < s0) s0 = s; if (s > s1) s1 = s; if (t < t0) t0 = t; if (t > t1) t1 = t;
        }
        b[0] = floorf(s0) - 2.0f; b[2] = ceilf(s1) + 2.0f; b[1] = floorf(t0) - 2.0f; b[3] = ceilf(t1) + 2.0f;
    } else { b[0] = b[1] = 0.0f; b[2] = W; b[3] = H; }
    if (b[0] < 0.0f) b[0] = 0.0f;
    if (b[1] < 0.0f) b[1] = 0.0f;
    if (b[2] > W) b[2] = W;
    if (b[3] > H) b[3] = H;
    if (d->wrap & 0xFu) {                                       /* (a region clamp or repeat: the coordinates are moved by the wrap; the whole image, conservative) */
        u32 mu = d->wrap & 3u, mv = (d->wrap >> 2) & 3u;
        if (mu == 3u) { b[0] = 0.0f; b[2] = W; }
        if (mv == 3u) { b[1] = 0.0f; b[3] = H; }
        if (mu == 2u) { float r0 = (float)d->minu, r1 = (float)d->maxu + 1.0f; if (b[0] < r0) b[0] = r0; if (b[2] > r1) b[2] = r1; if (b[2] <= b[0]) { b[0] = r0; b[2] = r1; } }
        if (mv == 2u) { float r0 = (float)d->minv, r1 = (float)d->maxv + 1.0f; if (b[1] < r0) b[1] = r0; if (b[3] > r1) b[3] = r1; if (b[3] <= b[1]) { b[1] = r0; b[3] = r1; } }
    }
}
static int rqg_real_at(u32 f) { return ((f >> 3) & 1u) && ((f >> 4) & 7u) >= 2u; }     /* (an alpha test that looks at the alpha) */
static int rqg_z_use(const vk_draw *d) { return (d->depth & 3u) != 1u || ((d->depth >> 2) & 1u) || (d->depth & ((1u << 9) | (1u << 10) | (1u << 12) | (1u << 14) | (1u << 15))); }

/* a live run of one draw: can it be a blit? */
static void rqg_try_blit(rqg_run *r, const vk_draw *d, int alpha_needed) {
    if (d->native || !d->tex_rt || d->tex_rt > RT_SLOTS || d->tex_depth || d->tex_self || d->date || d->mfd || d->cockpit || (u32)(d->tex_rt - 1u) == r->rt) return;
    vk_target *s = d->tex_rt == 1u ? &rq_feed : &rts[d->tex_rt - 1u], *t = &rts[r->rt];
    u32 f = d->frag_flags;
    if (d->tex_rt == 1u && !rq_feed_valid) return;
    if (!s->created || tsc(s) != 1u || tsc(t) != 1u || (d->blend & 0x100u) || !(d->depth & 8u) || (d->depth & 0x1E0u) || rqg_z_use(d) || (f & (1u << 16)) || (f & 3u) > 1u || rqg_real_at(f) || ((f >> 24) & 1u)) return;
    if ((f >> 18) & 1u && ((f >> 3) & 1u)) return;              /* (AFAIL keep: a failing test masks) */
    rqg_rc R;
    if (!rqg_rect(d, &R)) return;
    if (R.dx0 < d->scissor[0] || R.dy0 < d->scissor[2] || R.dx1 > d->scissor[1] + 1 || R.dy1 > d->scissor[3] + 1) return;
    if (R.dx0 < 0 || R.dy0 < 0 || R.dx1 > (s32)tew(t) || R.dy1 > (s32)teh(t)) return;
    const ps2_vk_vertex *V = &rep_verts[d->first];
    int neutral = (f & 3u) == 1u;
    if (!neutral) { neutral = 1; for (u32 q = 0; q < d->count; q++) if (fabsf(V[q].r - 128.0f) > 0.5f || fabsf(V[q].g - 128.0f) > 0.5f || fabsf(V[q].b - 128.0f) > 0.5f) { neutral = 0; break; } }
    if (!neutral) return;
    int wa = ((d->depth >> 4) & 1u) != 0u;
    if (wa) { if (!((f >> 17) & 1u) || (((f & 3u) == 0u) && fabsf(V[0].a - 128.0f) > 0.5f)) return; if ((f >> 21) & 1u) return; }
    else if (alpha_needed) return;                               /* (a blit writes alpha too) */
    if (R.point && (fabsf(R.ks - 1.0f) > 1e-3f || fabsf(R.kt - 1.0f) > 1e-3f)) return;
    float o = R.point || R.bit25 ? 0.0f : 0.5f;
    float S0 = R.sa + R.ks * (float)R.dx0 + o, S1 = R.sa + R.ks * (float)R.dx1 + o, T0 = R.ta + R.kt * (float)R.dy0 + o, T1 = R.ta + R.kt * (float)R.dy1 + o;
    float E[4] = { S0, T0, S1, T1 };
    for (int k = 0; k < 4; k++) { if (fabsf(E[k] - roundf(E[k])) > 1e-2f) return; E[k] = roundf(E[k]); }
    if (E[0] < 0.0f || E[1] < 0.0f || E[2] > (float)tew(s) || E[3] > (float)teh(s) || E[0] >= E[2] || E[1] >= E[3]) return;
    u32 mu = d->wrap & 3u, mv = (d->wrap >> 2) & 3u;
    if (mu == 3u || mv == 3u) return;
    if (mu == 2u && ((s32)d->minu > (s32)E[0] || (s32)d->maxu + 1 < (s32)E[2])) return;      /* (the region's clamp would bite) */
    if (mv == 2u && ((s32)d->minv > (s32)E[1] || (s32)d->maxv + 1 < (s32)E[3])) return;
    r->blit = 1; r->src = (u16)(d->tex_rt - 1u); r->linear = R.point ? 0 : 1; r->feed = d->tex_rt == 1u;
    r->sr[0] = (s32)E[0]; r->sr[1] = (s32)E[1]; r->sr[2] = (s32)E[2]; r->sr[3] = (s32)E[3];
    r->dr[0] = R.dx0; r->dr[1] = R.dy0; r->dr[2] = R.dx1; r->dr[3] = R.dy1;
}

static void rqg_plan(const u32 *order, int cld) {
    static int c_fit = -1, c_z = -1, c_drop = -1, c_blit = -1, c_comp = -1;
    rqg.alt++;
    rqg.active = 0; rqpf.armed = 0;
    int fit = rqg_sw("PS2_RQ_PASSFIT", &c_fit), zdc = rqg_sw("PS2_RQ_PASSZ", &c_z), drop = rqg_sw("PS2_RQ_PASSDROP", &c_drop), blit = rqg_sw("PS2_RQ_BLIT", &c_blit), comp = rqg_sw("PS2_RQ_NOCOMP", &c_comp);
    if (!(fit || zdc || drop || blit || comp) || rep_ndraws > VK_MAX_DRAWS) return;
    if (!rqg.map) { rqg.map = (u32 *)calloc(VK_MAX_DRAWS, sizeof(u32)); if (!rqg.map) return; }
    rqg.gen = (rqg.gen + 1u) & 0xFFFFu;
    if (!rqg.gen) { memset(rqg.map, 0, VK_MAX_DRAWS * sizeof(u32)); rqg.gen = 1u; }
    u32 nr = 0, n = rep_ndraws, s0first = n, last_q = ~0u;
    rqg_run *cur = NULL;
    for (u32 q = 0; q < n; q++) {
        u32 i = order ? order[q] : q;
        const vk_draw *d = &rep_draws[i];
        if (d->rt == 0u && s0first == n) s0first = q;
        if (d->rt == 0u || d->rt >= RT_SLOTS || !rts[d->rt].created) { cur = NULL; continue; }
        u32 z = d->zrt < RT_SLOTS && rts[d->zrt].created ? d->zrt : d->rt;
        if (!cur || cur->rt != d->rt || cur->zrt != z || q != last_q + 1u) {
            if (nr >= RQG_RUNS) return;                     /* (too many: nothing changes this list) */
            cur = &rqg.run[nr++];
            memset(cur, 0, sizeof *cur);
            cur->first = q; cur->rt = (u16)d->rt; cur->zrt = (u16)z; cur->box[0] = cur->box[1] = 1e9f; cur->box[2] = cur->box[3] = -1e9f;
        }
        cur->n++; last_q = q;
        rqg.map[i] = (rqg.gen << 16) | nr;
        if (rqg_z_use(d)) cur->z_touch = 1;
        if ((d->depth >> 2) & 1u || (d->depth & ((1u << 9) | (1u << 10) | (1u << 12)))) cur->z_write = 1;
        float b[4];
        if (!rqh_box(d, b)) { b[0] = b[1] = 0.0f; b[2] = (float)tew(&rts[d->rt]); b[3] = (float)teh(&rts[d->rt]); }
        if (b[0] < (float)d->scissor[0]) b[0] = (float)d->scissor[0];
        if (b[1] < (float)d->scissor[2]) b[1] = (float)d->scissor[2];
        if (b[2] > (float)(d->scissor[1] + 1)) b[2] = (float)(d->scissor[1] + 1);
        if (b[3] > (float)(d->scissor[3] + 1)) b[3] = (float)(d->scissor[3] + 1);
        if (b[0] < cur->box[0]) cur->box[0] = b[0];
        if (b[1] < cur->box[1]) cur->box[1] = b[1];
        if (b[2] > cur->box[2]) cur->box[2] = b[2];
        if (b[3] > cur->box[3]) cur->box[3] = b[3];
    }
    rqg.nrun = nr;
    /* what the next list reads before writing it (this list as the prediction): kept at the end of this one */
    static u8 wr[RT_SLOTS], wz[RT_SLOTS], nc[RT_SLOTS], na[RT_SLOTS], nz[RT_SLOTS];
    static float nb[RT_SLOTS][4];
    memset(wr, 0, sizeof wr); memset(wz, 0, sizeof wz); memset(nc, 0, sizeof nc); memset(na, 0, sizeof na); memset(nz, 0, sizeof nz);
    for (u32 q = 0; q < n; q++) {
        u32 i = order ? order[q] : q;
        const vk_draw *d = &rep_draws[i];
        if (d->tex_rt && d->tex_rt <= RT_SLOTS && !(cld && cl_sprite(d))) {
            u32 Y = d->tex_rt - 1u;
            if (d->tex_depth) { if (!wz[Y]) nz[Y] = 1; }
            else if (!wr[Y]) { nc[Y] = 1; na[Y] = 1; nb[Y][0] = nb[Y][1] = 0.0f; nb[Y][2] = (float)tew(&rts[Y]); nb[Y][3] = (float)teh(&rts[Y]); }
        }
        if (d->rt < RT_SLOTS) {
            wr[d->rt] = 1;
            u32 z = d->zrt < RT_SLOTS && rts[d->zrt].created ? d->zrt : d->rt;
            if ((d->depth & 3u) != 1u && !wz[z]) nz[z] = 1;           /* (a test against last frame's depth) */
            if ((d->depth >> 2) & 1u || (d->depth & ((1u << 9) | (1u << 10) | (1u << 12)))) wz[z] = 1;
        }
    }
    /* backwards over the list */
    for (u32 q = n; q-- > 0; ) {
        u32 i = order ? order[q] : q;
        const vk_draw *d = &rep_draws[i];
        u32 m = rqg.map[i];
        rqg_run *r = (m >> 16) == rqg.gen && (m & 0xFFFFu) ? &rqg.run[(m & 0xFFFFu) - 1u] : NULL;
        if (!r) {                                               /* (slot 0 and what is not ours: its reads) */
            if (d->tex_rt && d->tex_rt <= RT_SLOTS && !(cld && cl_sprite(d))) {
                u32 Y = d->tex_rt - 1u;
                if (d->tex_depth) nz[Y] = 1;
                else {
                    float b[4]; rqg_read_box(d, &rts[Y], b);
                    if (!nc[Y]) { memcpy(nb[Y], b, sizeof b); nc[Y] = 1; }
                    else { if (b[0] < nb[Y][0]) nb[Y][0] = b[0]; if (b[1] < nb[Y][1]) nb[Y][1] = b[1]; if (b[2] > nb[Y][2]) nb[Y][2] = b[2]; if (b[3] > nb[Y][3]) nb[Y][3] = b[3]; }
                    if (((d->frag_flags >> 17) & 1u) && ((d->blend & 0x100u) ? ((d->blend >> 4) & 3u) == 0u : 0) ) na[Y] = 1;
                    if (((d->frag_flags >> 17) & 1u) && rqg_real_at(d->frag_flags)) na[Y] = 1;
                }
            }
            continue;
        }
        q = r->first;                                           /* (the whole run, at once) */
        u32 X = r->rt;
        int prot = X == RT_MFD || ((rq_snap_mask >> X) & 1u) || (X == present_rt && !comp);
        int on = comp && X == present_rt ? 1 : drop && !prot;
        if (nc[X]) memcpy(r->nb, nb[X], sizeof r->nb); else { r->nb[0] = r->nb[1] = r->nb[2] = r->nb[3] = -1.0f; }
        r->live = 1;
        if (on) {
            int ov = nc[X] && r->box[0] < nb[X][2] && nb[X][0] < r->box[2] && r->box[1] < nb[X][3] && nb[X][1] < r->box[3];
            if (!ov && !(r->z_write && nz[r->zrt])) r->live = 0;
        }
        if (!r->live) { rqg.s_dead++; continue; }
        if (zdc && !r->z_touch && !nz[r->zrt] && r->zrt != 0u) { r->z_dc = 1; rqg.s_zdc++; }
        /* an opaque rectangle covering what is read ends the older versions */
        for (u32 k = r->first; k < r->first + r->n; k++) {
            const vk_draw *e = &rep_draws[order ? order[k] : k];
            rqg_rc R;
            if ((e->blend & 0x100u) || !(e->depth & 8u) || (e->depth & 0xE0u) || e->date || (e->depth & 3u) != 1u || rqg_real_at(e->frag_flags) || e->tex_self || ((e->frag_flags >> 24) & 1u) || !rqg_rect(e, &R) || !nc[X]) continue;
            if ((float)R.dx0 <= nb[X][0] && (float)R.dx1 >= nb[X][2] && (float)R.dy0 <= nb[X][1] && (float)R.dy1 >= nb[X][3]) {
                nc[X] = 0;
                if (((e->depth >> 4) & 1u) && !((e->depth >> 8) & 1u)) na[X] = 0;
            }
        }
        if (blit && r->n == 1u && r->first < s0first && !fg_keep_now) rqg_try_blit(r, &rep_draws[order ? order[r->first] : r->first], na[X]);
        if (r->blit) rqg.s_blit++;
        /* its own reads, and its depth */
        for (u32 k = r->first; k < r->first + r->n; k++) {
            const vk_draw *e = &rep_draws[order ? order[k] : k];
            if ((e->depth & 3u) != 1u) nz[r->zrt] = 1;
            if (!e->tex_rt || e->tex_rt > RT_SLOTS || (cld && cl_sprite(e))) continue;
            u32 Y = e->tex_rt - 1u;
            if (e->tex_depth) { nz[Y] = 1; continue; }
            float b[4]; rqg_read_box(e, &rts[Y], b);
            if (!nc[Y]) { memcpy(nb[Y], b, sizeof b); nc[Y] = 1; }
            else { if (b[0] < nb[Y][0]) nb[Y][0] = b[0]; if (b[1] < nb[Y][1]) nb[Y][1] = b[1]; if (b[2] > nb[Y][2]) nb[Y][2] = b[2]; if (b[3] > nb[Y][3]) nb[Y][3] = b[3]; }
            int wa_ = ((e->depth >> 4) & 1u) && na[X];        /* (its alpha output is needed, and it takes it from the texture) */
            if (((e->frag_flags >> 17) & 1u) && (((e->blend & 0x100u) ? ((e->blend >> 4) & 3u) == 0u : 0) || wa_ || rqg_real_at(e->frag_flags))) na[Y] = 1;
        }
    }
    if (xr_dump_now && getenv("PS2_RQ_LIST")) ps2_log("rq-g-alt: field %u parity %u (the =alt switches are on when 1)", ps2_gs_frame_count, rqg.alt & 1u);
    rqg.a_fit = fit; rqg.a_z = zdc; rqg.a_drop = drop; rqg.a_blit = blit; rqg.a_comp = comp;
    rqg.active = 1;
    {   rqg.s_lists++; rqg.s_runs += nr;
        for (u32 k = 0; k < nr; k++) {
            const rqg_run *r = &rqg.run[k];
            if (!r->live || r->blit) continue;
            double w = (double)tew(&rts[r->rt]) * (double)teh(&rts[r->rt]);
            double bx = fit && r->box[2] > r->box[0] && r->box[3] > r->box[1] ? (double)(r->box[2] - r->box[0]) * (double)(r->box[3] - r->box[1]) : w;
            rqg.s_px += w; rqg.s_fit += bx < w ? bx : w;
        }
        if (!(rqg.s_lists % 300u)) {
            double m = (double)rqg.s_lists;
            ps2_log("rq-g: %llu lists: %.1f off-screen runs: %.1f dropped, %.1f by blit, %.1f without their depth; the areas of what remains %.3f Mpx -> %.3f Mpx a list",
                    (unsigned long long)rqg.s_lists, (double)rqg.s_runs / m, (double)rqg.s_dead / m, (double)rqg.s_blit / m, (double)rqg.s_zdc / m, rqg.s_px / m / 1e6, rqg.s_fit / m / 1e6);
        }
    }
    if (xr_dump_now && getenv("PS2_RQ_LIST")) {
        for (u32 k = 0; k < nr; k++) {
            const rqg_run *r = &rqg.run[k];
            ps2_log("rq-g-run: [%u] rt%u z rt%u %u draws: %s%s%s box %.0f,%.0f..%.0f,%.0f need %.0f,%.0f..%.0f,%.0f", r->first, r->rt, r->zrt, r->n, r->live ? "live" : "DEAD", r->blit ? " BLIT" : "", r->z_dc ? " noz" : "", (double)r->box[0], (double)r->box[1], (double)r->box[2], (double)r->box[3], (double)r->nb[0], (double)r->nb[1], (double)r->nb[2], (double)r->nb[3]);
        }
    }
}
/* the draw loop: 0 the draw goes its way, 1 left out, 2 done by a blit (the loop's target state is stale) */
static int rqg_at(VkCommandBuffer cmd, u32 i, int in_eye_pass) {
    if (!rqg.active) return 0;
    u32 m = rqg.map[i];
    if ((m >> 16) != rqg.gen || !(m & 0xFFFFu)) return 0;
    const rqg_run *r = &rqg.run[(m & 0xFFFFu) - 1u];
    if (!r->live) return 1;
    if (!r->blit || in_eye_pass) return 0;
    vk_target *s = r->feed ? &rq_feed : &rts[r->src], *t = &rts[r->rt];
    if (!t->drawn || t->layout == VK_IMAGE_LAYOUT_UNDEFINED || s->layout == VK_IMAGE_LAYOUT_UNDEFINED) return 0;   /* (the first use clears the whole target: the normal way) */
    if (rendering_open) { vkCmdEndRendering(cmd); rendering_open = 0; }
    VkImageMemoryBarrier2 b[2];
    for (int k = 0; k < 2; k++) {
        vk_target *v = k ? t : s;
        int rd = v->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier_fill(&b[k], v->color, VK_IMAGE_ASPECT_COLOR_BIT, v->layout, k ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     rd ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, rd ? VK_ACCESS_2_SHADER_READ_BIT : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, k ? VK_ACCESS_2_TRANSFER_WRITE_BIT : VK_ACCESS_2_TRANSFER_READ_BIT);
    }
    barrier_flush(cmd, b, 2);
    VkImageBlit rg[2];
    u32 nl = t->mono ? 1u : rt_layers;
    if (nl > 2u) nl = 2u;
    for (u32 l = 0; l < nl; l++) {
        memset(&rg[l], 0, sizeof rg[l]);
        rg[l].srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rg[l].srcSubresource.baseArrayLayer = s->mono ? 0u : l; rg[l].srcSubresource.layerCount = 1;
        rg[l].dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rg[l].dstSubresource.baseArrayLayer = l; rg[l].dstSubresource.layerCount = 1;
        rg[l].srcOffsets[0].x = r->sr[0]; rg[l].srcOffsets[0].y = r->sr[1]; rg[l].srcOffsets[1].x = r->sr[2]; rg[l].srcOffsets[1].y = r->sr[3]; rg[l].srcOffsets[1].z = 1;
        rg[l].dstOffsets[0].x = r->dr[0]; rg[l].dstOffsets[0].y = r->dr[1]; rg[l].dstOffsets[1].x = r->dr[2]; rg[l].dstOffsets[1].y = r->dr[3]; rg[l].dstOffsets[1].z = 1;
    }
    vkCmdBlitImage(cmd, s->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nl, rg, r->linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    for (int k = 0; k < 2; k++) {
        vk_target *v = k ? t : s;
        barrier_fill(&b[k], v->color, VK_IMAGE_ASPECT_COLOR_BIT, k ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, k ? VK_ACCESS_2_TRANSFER_WRITE_BIT : VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        v->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    barrier_flush(cmd, b, 2);
    t->drawn = 1; t->lr_dirty = 1; t->snap_valid = 0; t->snap_alpha_valid = 0;
    return 2;
}
/* before select_target of an off-screen run: its render area and depth ops for the next begin */
static void rqg_arm(u32 i) {
    if (!rqg.active || !(rqg.a_fit || rqg.a_z)) return;
    u32 m = rqg.map[i];
    if ((m >> 16) != rqg.gen || !(m & 0xFFFFu)) return;
    const rqg_run *r = &rqg.run[(m & 0xFFFFu) - 1u];
    if (!r->live || r->rt == 0u) return;
    if (!rqg.a_fit && !r->z_dc) return;
    vk_target *t = &rts[r->rt];
    s32 sc = (s32)tsc(t);
    s32 W = (s32)(tew(t) * tsc(t)), H = (s32)(teh(t) * tsc(t));
    s32 x0 = 0, y0 = 0, x1 = W, y1 = H;
    if (rqg.a_fit && r->box[2] > r->box[0] && r->box[3] > r->box[1]) {
        x0 = (s32)floorf(r->box[0]) * sc; y0 = (s32)floorf(r->box[1]) * sc;
        x1 = (s32)ceilf(r->box[2]) * sc; y1 = (s32)ceilf(r->box[3]) * sc;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > W) x1 = W;
        if (y1 > H) y1 = H;
        if (x1 <= x0 || y1 <= y0) { x0 = 0; y0 = 0; x1 = W; y1 = H; }
    }
    rqpf.armed = 1; rqpf.z_dc = r->z_dc; rqpf.x0 = x0; rqpf.y0 = y0; rqpf.x1 = x1; rqpf.y1 = y1;
}
