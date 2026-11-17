/* rq_order.inc -- the Quest frame recipe, section 4.3 of a private script (PS2_RQ_ORDER): the slot-0 draws
   of the eye pass reordered for early depth. Included by ps2_video.c after rq_video.inc, PS2_RQ_SUPPORT only.
   PS2_RQ_ORDER_LOG=1: one line per slot-0 draw of a dumped frame (census), and the alpha histogram of the terrain's textures. */
static int rqo_log_cfg(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_ORDER_LOG"); v = e && *e == '1'; }
    return v;
}
static void rqo_census(const u32 *order) {
    static u32 said;
    if (!xr_dump_now || perf_ab_pass || !rqo_log_cfg() || said >= 3u) return;
    said++;
    u32 hist[6]; memset(hist, 0, sizeof hist);
    u32 ntt = 0;
    ps2_log("rq-ord: list of %u draws: seq ii i | nat prog pass mode pad4 | n blend fix frag depth date xcls occl ck mfd blk | tex texrt self | z0 z1 | box",
            rep_ndraws);
    static u32 *seen;
    if (!seen) seen = (u32 *)calloc(VK_MAX_TEXTURES, sizeof *seen);
    for (u32 q = 0; q < rep_ndraws; q++) {
        u32 i = order ? order[q] : q;
        const vk_draw *d = &rep_draws[i];
        if (d->rt != 0u) continue;
        int mesh = d->native == PS2_VK_NATIVE_MESH && rep_mesh && d->mblock < rep_mesh->nblocks;
        u32 prog = mesh ? rep_mesh->blocks[d->mblock].prog : 0xFFu, pass = mesh ? rep_mesh->blocks[d->mblock].pass : 0xFFu;
        u32 mode = mesh ? rep_mesh->blocks[d->mblock].mode : 0u, pad4 = mesh ? rep_mesh->blocks[d->mblock].pad4 : 0u;
        float z0 = 1e9f, z1 = -1e9f, x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        if (!d->native && d->first + d->count <= rep_nverts)
            for (u32 j = d->first; j < d->first + d->count; j++) {
                const ps2_vk_vertex *v = &rep_verts[j];
                if (v->z < z0) z0 = v->z;
                if (v->z > z1) z1 = v->z;
                if (v->x < x0) x0 = v->x;
                if (v->x > x1) x1 = v->x;
                if (v->y < y0) y0 = v->y;
                if (v->y > y1) y1 = v->y;
            }
        else if (mesh && d->nbox[2] >= d->nbox[0]) { x0 = d->nbox[0]; y0 = d->nbox[1]; x1 = d->nbox[2]; y1 = d->nbox[3]; }
        ps2_log("rq-ord: %u %u %u | %u %u %u %u %08X | %u %03X %u %08X %05X %u %u %u %u %u %u | %d %u %u | %.6g %.6g | %.0f %.0f %.0f %.0f", d->seq, q, i, d->native, prog, pass, mode, pad4,
                d->count, d->blend, d->fix, d->frag_flags, d->depth, d->date, d->xcls, d->occl, d->cockpit, d->mfd, d->hudblk,
                (int)(s16)d->tex, d->tex_rt, d->tex_self, (double)z0, (double)z1, (double)x0, (double)x1, (double)y0, (double)y1);
        if (mesh && prog == 1u && pass == 0u && !d->tex_rt && d->tex != 0xFFFFu && d->tex < ntextures && textures[d->tex].rgba && seen[d->tex] != said) {
            vk_texture *t = &textures[d->tex];
            seen[d->tex] = said; ntt++;
            for (size_t k = 0; k < (size_t)t->w * t->h; k++) {
                u8 a = t->rgba[4u * k + 3u];
                hist[a == 0u ? 0 : a < 128u ? 1 : a <= 191u ? 2 : a < 255u ? 3 : 4]++;
            }
        }
    }
    {   /* the off-screen slots: who writes them, who reads them (item 4.3 c: the cloud impostor builds) */
        for (u32 r = 1; r < 64u; r++) {
            u32 nw = 0, nsp = 0, no = 0, nz = 0; int rd[6] = { -1, -1, -1, -1, -1, -1 }; u32 nr = 0; char t[160]; int w = 0; t[0] = 0;
            for (u32 q = 0; q < rep_ndraws; q++) {
                const vk_draw *d = &rep_draws[order ? order[q] : q];
                if (d->rt == r) { nw++; if (d->tex_rt && !d->tex_self && d->tex_rt != r + 1u) { int f = 0; for (u32 k = 0; k < nr; k++) if (rd[k] == (int)d->tex_rt - 1) f = 1; if (!f && nr < 6u) rd[nr++] = (int)d->tex_rt - 1; } }
                if (d->tex_rt == r + 1u) { if (cl_sprite(d)) nsp++; else if (d->rt != r) { no++; if (w < 140) w += snprintf(t + w, sizeof t - (size_t)w, " r%u/x%u", d->rt, d->xcls); } }
                if (d->zrt == r && d->rt != r) nz++;
            }
            if (nw || nsp || no) ps2_log("rq-ord: slot %u: %u draws into it (reading slots %d %d %d %d %d %d), read by %u cloud sprites and %u others:%s; as a depth target of %u", r, nw, rd[0], rd[1], rd[2], rd[3], rd[4], rd[5], nsp, no, t, nz);
        }
    }
    {   /* the terrain's first passes: wrap modes, tile regions (for the sampler's clamp) */
        u32 nsub = 0, exs = 0, nt = 0, nr[8]; memset(nr, 0, sizeof nr);
        for (u32 q = 0; q < rep_ndraws; q++) {
            const vk_draw *d = &rep_draws[order ? order[q] : q];
            if (d->rt != 0u || d->native != PS2_VK_NATIVE_MESH || !rep_mesh || d->mblock >= rep_mesh->nblocks || rep_mesh->blocks[d->mblock].prog != 1u || rep_mesh->blocks[d->mblock].pass != 0u || d->tex == 0xFFFFu || d->tex >= ntextures) continue;
            nt++; nr[(rep_mesh->blocks[d->mblock].pad1 >> 8) & 7u]++;
            u32 tw = textures[d->tex].w, th = textures[d->tex].h;
            if ((u32)d->maxu - d->minu + 1u != tw || (u32)d->maxv - d->minv + 1u != th) { nsub++; if (exs < 6u) { exs++; ps2_log("rq-ord: terrain region not the whole image: tex %ux%u tex_size %.0fx%.0f wrap 0x%X clamp %u..%u %u..%u", tw, th, (double)d->tex_w, (double)d->tex_h, d->wrap, d->minu, d->maxu, d->minv, d->maxv); } }
        }
        ps2_log("rq-ord: terrain first passes %u; regions not the whole image %u; pass bits 0:%u 1:%u 2:%u 3:%u 4:%u 5:%u 6:%u 7:%u", nt, nsub, nr[0], nr[1], nr[2], nr[3], nr[4], nr[5], nr[6], nr[7]);
    }
    ps2_log("rq-ord: terrain textures %u; texel alpha: 0: %u, 1..127: %u, 128..191 (fail): %u, 192..254: %u, 255: %u", ntt, hist[0], hist[1], hist[2], hist[3], hist[4]);
}

/* ---- PS2_RQ_ORDER (default off; section 4.3 of the plan): the slot-0 draws of the eye pass in the order a tile GPU wants, so that LRZ and
   early depth work. The list's slot-0 run is split into its head (everything up to the first HUD or game-cockpit draw) and tail; the head is sorted, stably,
   into groups, the tail stays as the game has it (HUD, markers, the game's own cockpit, the fade):
     RQO_OP  depth-writing, no blending and no alpha test that can fail: the terrain's patches (one merged draw per patch with the merged shader: no discard, no blend, the game's
             blend over the sky and the haze ring folded in), other opaque draws;
     RQO_AT  depth-writing with an alpha test that can fail: tree cards, ground objects;
     RQO_SKY the sky dome, at depth GEQUAL without a depth write;
     RQO_WAT terrain patches that take neither merged shader and whose texture has non-opaque texels (still blended over the sky, after the sky);
     RQO_BL  the rest of the head, in the game's order (sun, horizon band, effects, overlays).
   PS2_RQ_HAZE: the haze ring (8 DATE draws) is dropped and evaluated in the merged terrain shader (rq_haze_frame, the sky rings' table in a reserved mesh block). PS2_RQ_WATER: the shader
   water's patches take the merged water shader too. Opaque draws commute under the depth test; groups the depth test does not decide keep their order. */
enum { RQO_OP, RQO_AT, RQO_SKY, RQO_WAT, RQO_BL, RQO_TAIL, RQO_GN };
#define RQO_DROP 0x80u
#define RQO_NOBL 0x40u
#define RQO_SKYS 0x20u
#define RQO_MRG  0x10u
static struct {
    int on;                      /* this frame's order is in place */
    u32 *ord;
    u8 *grp;                     /* per draw (index): group | RQO_* flags */
    u8 *cls;                     /* per draw (index): the merged terrain's shader class (rqo_class) */
    u32 *tmp;
    u64 n_fr, n[RQO_GN + 1];
    u32 tail_ii;                 /* position of the tail's first draw */
    u64 skip_rt;                 /* the off-screen slots only the dropped cloud sprites read */
} rqo;
#define RQO_SKY_BLOCK (MESH_MAX_BLOCKS - 10u)
#define RQO_HZ_BIT 2097152u                    /* (mesh_pad bit 21: mesh.vert computes the sky / haze terms; rq_terrain.frag -DRQO reads them) */
static struct { int ok, hz, dome_covers; float tab[30][4]; u32 lay[8], nl; } rqoh;
static int rqo_cfg(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_ORDER"); v = e && *e == '1'; if (v) ps2_log("vk: PS2_RQ_ORDER=1: the eye pass's slot-0 draws in the tile GPU's order"); }
    return v;
}
static int rqo_hz_cfg(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_HAZE"); v = e && *e == '1'; if (v) ps2_log("vk: PS2_RQ_HAZE=1: the haze ring in closed form in the terrain shader"); }
    return v;
}
/* PS2_RQ_HAZE_RINGS (rq5_partb 2026-11-12, BUGS_RQ R-122, cross-reference R-110; default 1; the user's M2 flight of 11-12, 80-93 s and
   144-162 s: "something loads on the horizon ... this flashing horizon, at least do not make this white light"): the haze ring's closed
   form takes H, the sky's colour at a fragment's elevation, from a table of the game dome's rings (28 rows). rqo_rings clustered the
   dome's vertices by elevation in windows of 0.0015 rad counted from each cluster's first vertex; the vertices of one ring spread over
   0.0007-0.002 rad (the dome is not centred exactly on the frame's camera), so in some poses a ring came out as two clusters, the
   table's 28 rows were full before the dome's lower rings, and the rings below the horizon (-0.04 .. -0.22 rad on M2) fell off its
   end. A fragment below the last kept ring took that ring's colour: the pale horizon line (105, 121, 116 against the dome's
   84, 102, 98 there), and the far terrain beyond the ring's base (8 layers: H itself) turned into a pale slab cut by the terrain's
   stepped far edge; the split changes with the pose from list to list: the flashes. 1: a ring is a run of sorted elevations without a
   gap over RQO_RING_GAP (0.004 rad; real rings lie 0.02 rad and more apart), averaged; over 28 rings, the interior ring that the
   piecewise-linear colour misses least is merged away until 28 remain, so the horizon's rings never fall off. 0: the old clusters;
   2 [test]: the new rings on odd lists only (dumps of consecutive fields compare the two in one run). */
#define RQO_RING_GAP 0.004f
static int rqo_rings_link;                /* (this list: PS2_RQ_HAZE_RINGS's clusters) */
static int rqo_rings_cfg(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("PS2_RQ_HAZE_RINGS"); v = e && *e ? atoi(e) : 1;
        if (v < 0 || v > 2) v = 1;
        ps2_log("vk: PS2_RQ_HAZE_RINGS=%d: the sky table's rings %s (R-122)", v, v == 1 ? "by gaps, merged to fit" : v == 2 ? "by gaps on odd lists only [test]" : "in 0.0015 rad windows, cut at 28 (before R-122)");
    }
    return v;
}
static int rqo_wat_cfg(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_WATER"); v = e && *e == '1'; if (v) ps2_log("vk: PS2_RQ_WATER=1: the shader water in the merged terrain shader (rq_water.glsl), one draw per patch"); }
    return v;
}
static int rqo_valpha_one(const vk_draw *d) {                     /* the patch's vertex alpha is 0x80 at every vertex (the record's colour alpha) */
    for (u32 j = 0; j < d->count; j++) {
        const u8 *r = rep_mesh->recs + (size_t)(rep_mesh->idx[d->first + j] + d->mvbase) * MESH_REC_BYTES;
        s32 c3; memcpy(&c3, r + 44u, sizeof c3);
        if ((c3 & 255) != 128) return 0;
    }
    return 1;
}
static int rqo_flat_z0(const vk_draw *d) {                        /* a GS-path draw all at depth 0 (the far plane) */
    if (d->native || !d->count || d->first + d->count > rep_nverts) return 0;
    for (u32 j = d->first; j < d->first + d->count; j++) if (rep_verts[j].z != 0.0f) return 0;
    return 1;
}
#define RQO_RING_MAXV 8192
static int rqo_elcmp(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? 1 : (x > y ? -1 : 0); }
/* the draw's rings: per vertex the elevation of its view direction and its colour, clustered; returns the rings (el, r, g, b / 255), elevation descending */
static int rqo_rings(const vk_draw *d, const float R[9], const float f[2], const float c[2], float (*out)[4], int maxr) {
    static float tmp[RQO_RING_MAXV][4];
    u32 n = d->count > RQO_RING_MAXV ? RQO_RING_MAXV : d->count;
    if (!d->count || d->first + d->count > rep_nverts) return 0;
    float cx = c[0] - present_frame_off[0], cy = c[1] - present_frame_off[1];
    for (u32 q = 0; q < n; q++) {
        const ps2_vk_vertex *v = &rep_verts[d->first + q];
        float dx = (v->x - cx) / f[0], dy = -(v->y - cy) / f[1], dz = -1.0f;
        float wx = R[0] * dx + R[1] * dy + R[2] * dz, wy = R[3] * dx + R[4] * dy + R[5] * dz, wz = R[6] * dx + R[7] * dy + R[8] * dz;
        tmp[q][0] = atan2f(wy, sqrtf(wx * wx + wz * wz));
        tmp[q][1] = v->r / 255.0f; tmp[q][2] = v->g / 255.0f; tmp[q][3] = v->b / 255.0f;
    }
    qsort(tmp, n, sizeof tmp[0], rqo_elcmp);
    int m = 0;
    static float cl[64][4];                                       /* (PS2_RQ_HAZE_RINGS: all rings first, merged to maxr after) */
    int cap = rqo_rings_link ? 64 : maxr;
    float (*dst)[4] = rqo_rings_link ? cl : out;
    for (u32 q = 0; q < n;) {
        u32 j = q; float s[4] = { 0, 0, 0, 0 };
        while (j < n && (rqo_rings_link ? (j == q || tmp[j - 1][0] - tmp[j][0] < RQO_RING_GAP) : tmp[q][0] - tmp[j][0] < 0.0015f)) { for (int k = 0; k < 4; k++) s[k] += tmp[j][k]; j++; }
        if (m < cap) { for (int k = 0; k < 4; k++) dst[m][k] = s[k] / (float)(j - q); m++; }
        q = j;
    }
    if (rqo_rings_link) {
        while (m > maxr && m > 2) {                               /* (merge away the interior ring the linear colour between its neighbours misses least) */
            int best = 1; float bd = 1e30f;
            for (int r = 1; r < m - 1; r++) {
                float t = (cl[r][0] - cl[r + 1][0]) / fmaxf(cl[r - 1][0] - cl[r + 1][0], 1e-6f), dv = 0.0f;
                for (int k = 1; k < 4; k++) { float e = cl[r + 1][k] + t * (cl[r - 1][k] - cl[r + 1][k]) - cl[r][k]; dv += e * e; }
                if (dv < bd) { bd = dv; best = r; }
            }
            memmove(cl[best], cl[best + 1], (size_t)(m - best - 1) * sizeof cl[0]); m--;
        }
        if (m > maxr) m = maxr;
        memcpy(out, cl, (size_t)m * sizeof cl[0]);
    }
    return m;
}
/* PS2_RQ_TFADE (rq9_terrain 2026-11-15, prompt 10 step 5, BUGS_RQ R-152..R-155; the user's M10 flight of 11-14, 183-199 s: "make the draw
   distance a little longer; these mountains load so far away"; M2, 228-255 s: "the horizon looks good, it is behind the fog ... in mission 10
   the mountains vanished very unpleasantly"). How the 2006 terrain ends [measured, M10 tape, PS2_RQ_TFAR_LOG]: the game draws a 13 x 13
   window of 16384-unit blocks round the camera's block (0x15AA60), but its block and cell tests clip against the camera's far plane too
   (0x15A928: z >= w), which lies at the fog far in view depth: the terrain straight ahead ends at 51-56k units (the farthest patch's near
   side, median 53k), at the view's sides farther (88-115k). Every vertex is fogged linearly to the fog colour at fog far (GS fog: fog0 255,
   fog1 -0.00498 = 51200 units in M10) and the haze ring hides the end: 8 layers from fog far - 4096 to fog far (M10 47127..51370 units;
   4096 in every mission: 0x161EC0 with the constant at 0x3F81E8), each mixing 1/8 of the dome's colour H in. The window's edge is never
   seen (beyond the ring, inside its band of elevations, -7.5..+8.4 deg in M10; no patch beyond the ring rose above the band on the whole
   tape). What the user saw is the ring: a ridge at fog far is about the fog colour (M10 74..90, 85..96, 93..97), 25-30 levels darker than H,
   and loses that contrast in 8 steps over 4096 units (0.8 km, 3 s of flight), up to 7.5 levels in one frame (the closed form's ceil, as the
   game's 8 depth-tested layers): the ridge "vanishes". On M2's plain the same wall lies on flat, already fogged ground.
   The illusion: from PS2_RQ_TFADE_START x Fe to Fe the merged terrain fades into H (the dome's colour at the vertex's elevation, the colour
   the ring mixes in), at every elevation, as a smoothstep, and the ring's 8 layers become continuous (no step a frame). Fe = the ring's end
   (fog far) x PS2_RQ_TFADE_END. Defaults 0.75 and 1.0 (M10: from 38.5k to 51.4k units): up to 40k the ridges are as the game draws them (34
   of 36 levels), then they dissolve over 2.5 km at most 0.1 level a frame instead of 0.8 km in 7.5-level steps. A longer sight distance
   cannot come from here: the far plane ends the terrain at fog far, so [test] END above 1 (it also moves the ring's layers out) shows that
   plane's cut, a saw of cells straight ahead that turns with the head; step 5 b needs the game camera's far plane moved, with the price of
   the patches it adds (not built). Folded per vertex into the fog's terms (mesh.vert -DRQT, the sky table's row 1 z / w; the sign of the
   layers' base / step tells rq_terrain.frag the ring is continuous): no new varying, the same patches drawn, the fragment's work only in the
   ring's patches. 1 = on; 2 [debug] = the fade's weight drawn in red. Default off; the Quest default after the device rows. */
#define RQTD_FE_MAX 85000.0f
static struct { int on, alt; float start, end, fs, fe; } rqtd = { -1, 0, 0.0f, 0.0f, 0.0f, 0.0f };
static int rqtd_cfg(void) {
    if (rqtd.on < 0) {
        const char *e = getenv("PS2_RQ_TFADE"); rqtd.on = e && *e ? atoi(e) : 0;
        if (rqtd.on < 0 || rqtd.on > 2) rqtd.on = 0;
        e = getenv("PS2_RQ_TFADE_START"); rqtd.start = e && *e ? (float)atof(e) : 0.75f;
        if (!(rqtd.start >= 0.0f && rqtd.start <= 0.98f)) rqtd.start = 0.75f;
        e = getenv("PS2_RQ_TFADE_END"); rqtd.end = e && *e ? (float)atof(e) : 1.0f;
        if (!(rqtd.end >= 0.5f && rqtd.end <= 2.0f)) rqtd.end = 1.0f;
        e = getenv("PS2_RQ_TFADE_ALT"); rqtd.alt = e && *e == '1';  /* ([test] the fade on odd lists only: consecutive dumps of one run compare the two) */
        if (rqtd.on) ps2_log("vk: PS2_RQ_TFADE=%d: the far terrain fades into the sky's colour from %.2f to %.2f x the haze ring's end (fog far), at most %.0f units, the ring continuous%s%s",
                             rqtd.on, (double)(rqtd.start * rqtd.end), (double)rqtd.end, (double)RQTD_FE_MAX, rqtd.on == 2 ? " [debug: drawn red]" : "", rqtd.alt ? "; on odd lists only [test, PS2_RQ_TFADE_ALT]" : "");
    }
    return rqtd.on;
}
static float rqtd_fogzero(const u32 *order) {                     /* (no haze ring in the list: where the terrain's GS fog reaches the fog colour) */
    for (u32 q = 0; q < rep_ndraws; q++) {
        const vk_draw *d = &rep_draws[order ? order[q] : q];
        if (d->rt != 0u || d->native != PS2_VK_NATIVE_MESH || !rep_mesh || d->mblock >= rep_mesh->nblocks) continue;
        const ps2_vk_mesh_block *k = &rep_mesh->blocks[d->mblock];
        if (k->prog == 1u && k->fog1 < 0.0f && k->fog0 > 0.0f) return -k->fog0 / k->fog1;
    }
    return 0.0f;
}
static float rqtd_farplane(const u32 *order) {                   /* (the game camera's far plane, z = w, in view depth along its axis: the terrain's
                                                                   end straight ahead -- the fade must be complete there; 0 = not known) */
    for (u32 q = 0; q < rep_ndraws; q++) {
        const vk_draw *d = &rep_draws[order ? order[q] : q];
        if (d->rt != 0u || d->native != PS2_VK_NATIVE_MESH || !rep_mesh || d->mblock >= rep_mesh->nblocks) continue;
        const ps2_vk_mesh_block *k = &rep_mesh->blocks[d->mblock];
        if (k->prog != 1u || !(k->pad1 & 1u)) continue;
        float g = sqrtf(k->c[3] * k->c[3] + k->c[7] * k->c[7] + k->c[11] * k->c[11]);
        if (!(g > 0.0f)) return 0.0f;
        const float e[3] = { k->p[13], k->p[14], k->p[15] };
        float w0 = k->c[3] * e[0] + k->c[7] * e[1] + k->c[11] * e[2] + k->c[15], z0 = k->c[2] * e[0] + k->c[6] * e[1] + k->c[10] * e[2] + k->c[14];
        float zd = (k->c[2] * k->c[3] + k->c[6] * k->c[7] + k->c[10] * k->c[11]) / g;
        float fp = fabsf(g - zd) > 1e-12f ? (z0 - w0) / (g - zd) : 0.0f;
        return fp > 1000.0f && fp < 1e6f ? fp : 0.0f;
    }
    return 0.0f;
}
/* the frame's table: the dome's rings (elevation, colour); with PS2_RQ_HAZE the haze ring's layers. Written to the reserved block; rqoh.ok = the table is valid */
static int rqo_haze_frame(vk_frame *fr, const u32 *order, u32 seq) {
    rqoh.ok = 0; rqoh.hz = 0; rqoh.nl = 0; rqoh.dome_covers = 0;
    if (!rep_mesh || !fr->mcptr) return 0;
    float eye[3], R[9], f[2], c[2], sa, sc, smh;
    if (!seq) seq = present_xr_seq;                               /* (PS2_RQ=1: the frame's own sequence) */
    if (!seq || !ps2_xr_camera_of(seq, eye, R, f, c) || !ps2_xr_shift_of(seq, &sa, &sc, &smh) || sa == 0.0f || !(present_frame_zmax > 0.0f)) return 0;
    {   int rg = rqo_rings_cfg(); rqo_rings_link = rg == 1 || (rg == 2 && (seq & 1u)); }
    float K = smh / sa, zmax = present_frame_zmax;
    const vk_draw *dome = NULL;
    float h[8]; int have[8] = {0}; u32 nl = 0; float lcol[8][4] = {{0}};
    float bandlo = 1e9f, bandhi = -1e9f;
    for (u32 q = 0; q < rep_ndraws; q++) {
        u32 i = order ? order[q] : q;
        const vk_draw *d = &rep_draws[i];
        if (d->rt != 0u || d->native) continue;
        if (!dome && d->tex == 0xFFFFu && !d->tex_rt && (d->depth & 7u) == 5u && d->count >= 200u && rqo_flat_z0(d)) { dome = d; continue; }
        if (d->date && d->tex == 0xFFFFu && !d->tex_rt && d->blend == 0x164u && (d->depth & 7u) == 3u && fogr_flat_layer(d) && (d->fix & 15u) == 0u && d->fix < 128u && d->first < rep_nverts) {
            u32 l = d->fix / 16u;
            if (!have[l]) {
                float z = rep_verts[d->first].z;
                h[l] = K / (z * zmax + sc); have[l] = 1; nl++;
                lcol[l][0] = rep_verts[d->first].r; lcol[l][1] = rep_verts[d->first].g; lcol[l][2] = rep_verts[d->first].b; lcol[l][3] = (float)((d->frag_flags >> 23) & 1u);
                float rr[30][4];
                int nr = rqo_rings(d, R, f, c, rr, 30);
                for (int k = 0; k < nr; k++) { if (rr[k][0] < bandlo) bandlo = rr[k][0]; if (rr[k][0] > bandhi) bandhi = rr[k][0]; }
            }
            if (rqoh.nl < 8u) rqoh.lay[rqoh.nl++] = i;
        }
    }
    if (!dome) return 0;
    float rings[30][4];
    int nr = rqo_rings(dome, R, f, c, rings, 28);
    if (nr < 3) return 0;
    int hz = rqo_hz_cfg() && !perf_ab_flipped("rqhaze") && nl == 8u && rqoh.nl == 8u;
    float base = hz ? h[0] : 1e9f, step = hz ? (h[7] - h[0]) / 7.0f : 1.0f;
    {   static int alt = -1;                                      /* (PS2_RQ_HAZE_ALT=1 [test]: the closed form on odd lists only, the game's ring on even ones) */
        if (alt < 0) { const char *e = getenv("PS2_RQ_HAZE_ALT"); alt = e && *e == '1'; }
        if (alt && !(seq & 1u)) hz = 0;
    }
    if (!(step > 0.0f)) { hz = 0; base = 1e9f; step = 1.0f; }
    int tf_on = rqtd_cfg() && !(rqtd.alt && !(seq & 1u)) && !perf_ab_flipped("rqtfade");   /* PS2_RQ_TFADE (below; PS2_PERF_AB_FLIP=rqtfade: the re-draw
                                                                     without it); [test] _END above 1: the ring's layers move out with the fade's end */
    float tf_fe = 0.0f, tf_k = 1.0f;
    if (tf_on) {
        tf_fe = nl == 8u && h[7] > h[0] ? h[7] + (h[7] - h[0]) / 7.0f : rqtd_fogzero(order);
        tf_k = rqtd.end;
        if (tf_fe * tf_k > RQTD_FE_MAX) tf_k = RQTD_FE_MAX / fmaxf(tf_fe, 1.0f);
        if (hz && tf_k > 1.0f) { base *= tf_k; step *= tf_k; }
        tf_fe *= tf_k;
    }
    rqoh.tab[0][0] = base; rqoh.tab[0][1] = step; rqoh.tab[0][2] = (float)nr; rqoh.tab[0][3] = 0.0f;
    rqoh.tab[1][0] = hz ? bandlo : -10.0f; rqoh.tab[1][1] = hz ? bandhi : 10.0f; rqoh.tab[1][2] = rqoh.tab[1][3] = 0.0f;
    {   /* PS2_RQ_HAZE_LOG=from,to [debug] (R-122): the haze table of every list in that field window */
        static int lg = -1; static u32 lf, lt;
        if (lg < 0) { const char *e = getenv("PS2_RQ_HAZE_LOG"); lg = e && sscanf(e, "%u,%u", &lf, &lt) == 2; }
        if (lg && ps2_gs_frame_count >= lf && ps2_gs_frame_count <= lt)
            ps2_log("rq haze: seq %u field %u: layers %u / %u, hz %d, rings %s, band %.4f..%.4f rad, base %.0f step %.0f units, dome rings %d (lowest %.4f rad)",
                    seq, ps2_gs_frame_count, nl, rqoh.nl, hz, rqo_rings_link ? "by gaps" : "windows", (double)bandlo, (double)bandhi, (double)base, (double)step, nr, (double)rings[nr - 1][0]);
        if (lg && ps2_gs_frame_count >= lf && ps2_gs_frame_count <= lt) {
            char l[700]; int o = 0;
            for (u32 k = 0; k < 8u && o < 300; k++) o += snprintf(l + o, sizeof l - (size_t)o, " %.0f,%.0f,%.0f/%.0f", (double)lcol[k][0], (double)lcol[k][1], (double)lcol[k][2], (double)lcol[k][3]);
            o += snprintf(l + o, sizeof l - (size_t)o, " | dome");
            for (int r = 0; r < nr && o < 660; r++) if (rings[r][0] > bandlo - 0.06f && rings[r][0] < bandhi + 0.1f) o += snprintf(l + o, sizeof l - (size_t)o, " %.4f:%.0f,%.0f,%.0f", (double)rings[r][0], (double)(rings[r][1] * 255.0f), (double)(rings[r][2] * 255.0f), (double)(rings[r][3] * 255.0f));
            ps2_log("rq haze: seq %u layer rgb/datm%s", seq, l);
            {   static float all[64][4]; int na = rqo_rings(dome, R, f, c, all, 64);
                o = 0; for (int r = 0; r < na && o < 660; r++) o += snprintf(l + o, sizeof l - (size_t)o, " %.4f", (double)all[r][0]);
                ps2_log("rq haze: seq %u dome clusters %d (uncapped):%s", seq, na, l);
            }
        }
    }
    for (int r = 0; r < nr; r++) { rqoh.tab[2 + r][0] = rings[r][1]; rqoh.tab[2 + r][1] = rings[r][2]; rqoh.tab[2 + r][2] = rings[r][3]; rqoh.tab[2 + r][3] = rings[r][0]; }
    if (tf_on) {                                                  /* PS2_RQ_TFADE: where the far terrain fades into H (row 1's z, w: start, 1 / length; w < 0 = the debug tint) */
        float fe = tf_fe;
        if (rqtd.end <= 1.0f) { float fp = rqtd_farplane(order); if (fp > 0.0f && fe > fp) fe = fp; }   /* (never past the far plane: its cut must not show) */
        if (fe > 0.0f) {
            float fs = fe * rqtd.start;
            rqoh.tab[1][2] = fs; rqoh.tab[1][3] = (rqtd.on == 2 ? -1.0f : 1.0f) / fmaxf(fe - fs, 1.0f);
            rqtd.fs = fs; rqtd.fe = fe;
        }
    }
    memcpy((ps2_vk_mesh_block *)fr->mcptr + RQO_SKY_BLOCK, rqoh.tab, sizeof rqoh.tab);
    {   float b[4];                                                /* (the quad under the dome is covered when the dome's box is: a closed sphere of triangles around the eye) */
        rqoh.dome_covers = cl_draw_box(dome, b) && b[0] <= 0.0f && b[2] <= 0.0f && b[1] >= (float)rt_w && b[3] >= (float)rt_h;
    }
    rqoh.ok = 1; rqoh.hz = hz;
    {   static u32 nsaid; if (rqo_log_cfg() && !perf_ab_pass && !(nsaid++ % 300u)) {
        char l[512]; int o = 0;
        for (int r = 0; r < nr && o < 460; r++) o += snprintf(l + o, sizeof l - (size_t)o, " %.2f:%.0f,%.0f,%.0f", (double)(rings[r][0] * 57.29578f), (double)(rings[r][1] * 255.0f), (double)(rings[r][2] * 255.0f), (double)(rings[r][3] * 255.0f));
        ps2_log("rq haze: %s; base %.0f step %.1f band %.2f..%.2f deg, %d rings (el:rgb):%s", hz ? "closed form" : "off", (double)base, (double)step, (double)(bandlo * 57.29578f), (double)(bandhi * 57.29578f), nr, l);
        if (nl == 8u) ps2_log("rq haze: layers h %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f", (double)h[0], (double)h[1], (double)h[2], (double)h[3], (double)h[4], (double)h[5], (double)h[6], (double)h[7]);
    } }
    return 1;
}
/* the class of a merged terrain patch (PS2_RQ_OCLS): what its fragments need, known from the patch: bits 1 DET (detail passes 1 / 2), 2 L3 (the layer of pass 3 per fragment), 4 HZ (beyond the first haze layer), 8 ALPHA (a texture with
   non-opaque texels), 16 the shader water's patch beyond its fade (the plain shader), 32 L3V (the layer per vertex). */
static int rqo_ocls_cfg(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_OCLS"); v = e && *e == '1' ? 1 : e && *e == '2' ? 2 : 0; if (v) ps2_log("vk: PS2_RQ_OCLS=1: the terrain's shader specialised per patch class"); }
    return v;
}
static float rqo_l3v_cfg(void) {
    static float v = -1.0f;
    if (v < 0.0f) { const char *e = getenv("PS2_RQ_L3V"); v = e ? (float)atof(e) : 0.0f; if (v > 0.0f) ps2_log("vk: PS2_RQ_L3V=%.0f: pass 3's layer per vertex beyond that distance (game units)", (double)v); }
    return v;
}
/* PS2_RQ_WFAR=<metres>: water patches whose nearest point is beyond that distance take the far class (bit 64): no wave reads (rq_water.glsl S_WF), pass 3's layer per vertex */
/* PS2_RQ_MAT=1: near-ground materials in the merged shader (class bit 128): the classification and fade per vertex, one array read per fragment, no second material; no PC twin in the pass */
static int rqo_mat_cfg(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_MAT"); v = e && *e == '1' && rnb.ok; if (v) ps2_log("vk: PS2_RQ_MAT=1: the near-ground materials in the merged terrain shader"); }
    return v;
}
static float rqo_wfar_cur = -1.0f, rqo_wfar_tgt = 0.0f;     /* units; the far class and the near class's mip bias follow rqo_wfar_cur */
static int rqo_wfar_noclass;                                /* (PS2_RQ_WFAR_NOCLASS=1 [test]: the near class's bias without the far class: the difference is the far class's error) */
static float rqo_wfar_cfg(void) {
    if (rqo_wfar_cur < 0.0f) {
        const char *e = getenv("PS2_RQ_WFAR"); float v = e ? (float)atof(e) * 6.0f : 0.0f;
        rqo_wfar_tgt = v; rqo_wfar_cur = v;
        e = getenv("PS2_RQ_WFAR_NOCLASS"); rqo_wfar_noclass = e && *e == '1';
        if (v > 0.0f) ps2_log("vk: PS2_RQ_WFAR=%.0f m: the waves resolve to the mean by that distance, the far water flat per vertex (%.0f units)", (double)v / 6.0, (double)v);
    }
    return rqo_wfar_cur;
}
/* the load lever (the frame controller's): where the waves resolve, in metres; moves slowly (at most 0.25 % a list) and switches nothing. No effect when PS2_RQ_WFAR is off at start. */
void ps2_rq_wfar_set(float metres) {
    (void)rqo_wfar_cfg();
    if (rqo_wfar_cur <= 0.0f) return;
    if (metres < 600.0f) metres = 600.0f;
    if (metres > 12000.0f) metres = 12000.0f;
    rqo_wfar_tgt = metres * 6.0f;
}
float ps2_rq_wfar_get(void) { return rqo_wfar_cfg() / 6.0f; }
static void rqo_wfar_step(void) {
    if (rqo_wfar_cfg() <= 0.0f) return;
    {   static int tn = -1; static float tv; static u32 tc;      /* (PS2_RQ_WFAR_TEST=<metres> [test]: the lever moved there after 600 lists) */
        if (tn < 0) { const char *e = getenv("PS2_RQ_WFAR_TEST"); tv = e ? (float)atof(e) : 0.0f; tn = tv > 0.0f; }
        if (tn && ++tc == 600u) { ps2_rq_wfar_set(tv); ps2_log("rq water: the lever moved to %.0f m", (double)tv); }
    }
    float d = rqo_wfar_tgt - rqo_wfar_cur, m = rqo_wfar_cur * 0.0025f;
    rqo_wfar_cur += d > m ? m : d < -m ? -m : d;
}
/* PS2_RQ_TFAR_LOG=N [debug] (rq9_terrain, R-152): every N lists one line about the far end of the merged terrain: the patches' extent around
   the eye (the game's 13 x 13 window of 16384-unit blocks), the farthest patch, the haze ring's layers and band, the fog's zero, and the
   patches wholly beyond the haze's last layer whose top rises above the band's top (the ring never hazes them: drawn as they are) */
static struct { int on; u32 every, lists, n, nfar, nup, fc; float ext[4], dmax, upel, updmax, e[3], f0, f1, sec[16], ahead, zp[2]; } rqtf;
static int rqtf_cfg(void) {
    if (!rqtf.on) { const char *e = getenv("PS2_RQ_TFAR_LOG"); rqtf.every = e ? (u32)atoi(e) : 0u; rqtf.on = rqtf.every ? 1 : -1; }
    return rqtf.on > 0;
}
static void rqtf_reset(void) { memset(rqtf.sec, 0, sizeof rqtf.sec); rqtf.ahead = 0.0f; rqtf.ext[0] = rqtf.ext[2] = 1e30f; rqtf.ext[1] = rqtf.ext[3] = -1e30f; rqtf.dmax = 0.0f; rqtf.upel = -10.0f; rqtf.updmax = 0.0f; }
static void rqtf_note(const float mn[3], const float mx[3], const float e[3], float dmin, float dmax, const ps2_vk_mesh_block *k, u32 fogcol) {
    if (!rqtf.n) rqtf_reset();
    rqtf.n++;
    if (mn[0] - e[0] < rqtf.ext[0]) rqtf.ext[0] = mn[0] - e[0];
    if (mx[0] - e[0] > rqtf.ext[1]) rqtf.ext[1] = mx[0] - e[0];
    if (mn[2] - e[2] < rqtf.ext[2]) rqtf.ext[2] = mn[2] - e[2];
    if (mx[2] - e[2] > rqtf.ext[3]) rqtf.ext[3] = mx[2] - e[2];
    if (dmax > rqtf.dmax) rqtf.dmax = dmax;
    {   int sc = (int)floorf((atan2f((mn[2] + mx[2]) * 0.5f - e[2], (mn[0] + mx[0]) * 0.5f - e[0]) + 3.14159265f) * (16.0f / 6.2831853f)) & 15;   /* (per 22.5-degree sector of
            azimuth: the farthest patch's nearest point, the drawn region's edge there) */
        if (dmin > rqtf.sec[sc]) rqtf.sec[sc] = dmin; }
    {   float fx = k->c[3], fz = k->c[11], fl = sqrtf(fx * fx + fz * fz);   /* (the view's horizontal direction: the clip rows' w; the patches within
                                                                               10 degrees of it: the drawn region's edge straight ahead) */
        float cx = (mn[0] + mx[0]) * 0.5f - e[0], cz = (mn[2] + mx[2]) * 0.5f - e[2], cl = sqrtf(cx * cx + cz * cz);
        if (fl > 0.0f && cl > 0.0f && (fx * cx + fz * cz) / (fl * cl) > 0.9848f && dmin > rqtf.ahead) rqtf.ahead = dmin; }
    memcpy(rqtf.e, e, sizeof rqtf.e); rqtf.f0 = k->fog0; rqtf.f1 = k->fog1; rqtf.fc = fogcol;
    {   /* the game camera's clip planes in z along its view axis (the clip rows: z = c[2] x + c[6] y + c[10] z + c[14], w the same with 3, 7,
           11, 15; the block test 0x15A928 culls z >= w and z <= -w): the view depths where z = w and z = -w */
        float g = sqrtf(k->c[3] * k->c[3] + k->c[7] * k->c[7] + k->c[11] * k->c[11]);
        if (g > 0.0f) {
            float fv[3] = { k->c[3] / g, k->c[7] / g, k->c[11] / g };
            float w0 = k->c[3] * e[0] + k->c[7] * e[1] + k->c[11] * e[2] + k->c[15], z0 = k->c[2] * e[0] + k->c[6] * e[1] + k->c[10] * e[2] + k->c[14];
            float wd = g, zd = k->c[2] * fv[0] + k->c[6] * fv[1] + k->c[10] * fv[2];
            rqtf.zp[0] = fabsf(wd - zd) > 1e-12f ? (z0 - w0) / (wd - zd) : -1.0f;      /* (z = w) */
            rqtf.zp[1] = fabsf(wd + zd) > 1e-12f ? -(z0 + w0) / (wd + zd) : -1.0f;     /* (z = -w) */
        }
    }
    float hend = rqoh.hz ? rqoh.tab[0][0] + 7.0f * rqoh.tab[0][1] : 1e30f;
    if (dmin > hend) {
        rqtf.nfar++;
        float ix = mn[0] - e[0] > 0.0f ? mn[0] - e[0] : e[0] - mx[0] > 0.0f ? e[0] - mx[0] : 0.0f;
        float iz = mn[2] - e[2] > 0.0f ? mn[2] - e[2] : e[2] - mx[2] > 0.0f ? e[2] - mx[2] : 0.0f;
        float el = atan2f(mx[1] - e[1], fmaxf(sqrtf(ix * ix + iz * iz), 1.0f));
        if (el > rqoh.tab[1][1]) { rqtf.nup++; if (dmax > rqtf.updmax) rqtf.updmax = dmax; }
        if (el > rqtf.upel) rqtf.upel = el;
    }
}
static void rqtf_list(void) {
    if (!rqtf_cfg() || perf_ab_pass) return;
    if (++rqtf.lists % rqtf.every == 0u && rqtf.n) {
        float fz = rqtf.f1 < 0.0f ? -rqtf.f0 / rqtf.f1 : -1.0f, fs = rqtf.f1 < 0.0f ? (255.0f - rqtf.f0) / rqtf.f1 : -1.0f;
        ps2_log("rq tfar: field %u eye %.0f %.0f %.0f | %u patches, extent x %.0f..%.0f z %.0f..%.0f, farthest %.0f | haze %s base %.0f step %.1f (full at %.0f) band %.2f..%.2f deg | "
                "fog0 %.1f fog1 %.5f: fog from %.0f, full at %.0f | beyond the haze %u patches, %u rise above the band (top %.2f deg; farthest %.0f) fogcol %u,%u,%u | fade %.0f..%.0f",
                ps2_gs_frame_count, (double)rqtf.e[0], (double)rqtf.e[1], (double)rqtf.e[2], rqtf.n, (double)rqtf.ext[0], (double)rqtf.ext[1], (double)rqtf.ext[2], (double)rqtf.ext[3], (double)rqtf.dmax,
                rqoh.hz ? "on" : "off", (double)rqoh.tab[0][0], (double)rqoh.tab[0][1], (double)(rqoh.tab[0][0] + 7.0f * rqoh.tab[0][1]), (double)(rqoh.tab[1][0] * 57.29578f), (double)(rqoh.tab[1][1] * 57.29578f),
                (double)rqtf.f0, (double)rqtf.f1, (double)fs, (double)fz, rqtf.nfar, rqtf.nup, (double)(rqtf.upel * 57.29578f), (double)rqtf.updmax,
                rqtf.fc & 255u, (rqtf.fc >> 8) & 255u, (rqtf.fc >> 16) & 255u, (double)rqtd.fs, (double)rqtd.fe);
        char l[200]; int o = 0;
        for (int k = 0; k < 16; k++) o += snprintf(l + o, sizeof l - (size_t)o, " %.0f", (double)(rqtf.sec[k] * 0.001f));
        ps2_log("rq tfar: field %u sectors (azimuth from -180, 22.5 deg, the farthest patch's nearest point, k units):%s | ahead %.0f | clip z = w at view depth %.0f, z = -w at %.0f", ps2_gs_frame_count, l, (double)rqtf.ahead, (double)rqtf.zp[0], (double)rqtf.zp[1]);
    }
    rqtf.n = rqtf.nfar = rqtf.nup = 0;
}
static u8 rqo_class(const vk_draw *d, const ps2_vk_mesh_block *k, int wat, float water_end) {
    u32 rqf = (k->pad1 >> 8) & 7u;
    u8 c = 8u | ((rqf & 3u) ? 1u : 0u) | ((rqf & 4u) ? 2u : 0u) | 4u;
    if (!rqo_ocls_cfg()) return (u8)(c | 15u);
    if (d->tex != 0xFFFFu && rnb_tex_opaque(d->tex)) c &= (u8)~8u;
    float e[3] = { k->p[13], k->p[14], k->p[15] };
    int have_eye = (k->pad1 & 1u) != 0u;
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    for (u32 j = 0; j < d->count; j++) {
        float q[3]; memcpy(q, rep_mesh->recs + (size_t)(rep_mesh->idx[d->first + j] + d->mvbase) * MESH_REC_BYTES, sizeof q);
        for (int a = 0; a < 3; a++) { if (q[a] < mn[a]) mn[a] = q[a]; if (q[a] > mx[a]) mx[a] = q[a]; }
    }
    float dmin = 0.0f, dmax = 1e30f;
    if (have_eye) {
        float n2 = 0.0f, x2 = 0.0f;
        for (int a = 0; a < 3; a++) {
            float dl = mn[a] - e[a], dh = e[a] - mx[a], in = dl > 0.0f ? dl : dh > 0.0f ? dh : 0.0f, out = fabsf(dl) > fabsf(dh) ? fabsf(dl) : fabsf(dh);
            n2 += in * in; x2 += out * out;
        }
        dmin = sqrtf(n2); dmax = sqrtf(x2);
        if (rqtf_cfg()) rqtf_note(mn, mx, e, dmin, dmax, k, d->fogcol);
    }
    if (!(rqoh.hz && dmax > rqoh.tab[0][0] - 1024.0f)) c &= (u8)~4u;
    if (wat && have_eye && dmin > water_end + 64.0f) c |= 16u;
    if (wat && have_eye && rqm_cfg() && dmin < rqm_wnear) rqm_wnear = dmin;
    if (wat && !(c & 16u) && have_eye && rqo_wfar_cfg() > 0.0f && !rqo_wfar_noclass && dmin > rqo_wfar_cfg()) { static int lv = -1; if (lv < 0) { const char *e = getenv("PS2_RQ_WFAR_L3V"); lv = e && *e == '1'; } c |= 64u; if (lv && (c & 2u) && !(c & 1u)) c = (u8)((c & ~2u) | 32u); }
    if ((c & 2u) && !(wat && !(c & 16u))) { float l3 = rqo_l3v_cfg(); if (l3 > 0.0f && have_eye && dmin > l3) c = (u8)((c & ~2u) | 32u); }
    return c;
}
/* the group of a head draw (see above), the flags in *g, the class in *c */
static int rqo_group(const vk_draw *d, u32 wat_bit, u32 tmat_bit, u32 tp_bit, float water_end, u8 *g, u8 *c) {
    *g = 0; *c = 0;
    int mesh = d->native == PS2_VK_NATIVE_MESH && rep_mesh && d->mblock < rep_mesh->nblocks;
    int blend = (d->blend & 0x100u) != 0u, zw = (d->depth >> 2) & 1u, ztst = d->depth & 3u;
    u32 f = d->frag_flags;
    int ate = (f >> 3) & 1u, atst = (f >> 4) & 7u, afonly = (f >> 20) & 1u;
    if (mesh && rep_mesh->blocks[d->mblock].prog == 1u) {
        const ps2_vk_mesh_block *k = &rep_mesh->blocks[d->mblock];
        if (k->pass != 0u) return RQO_BL;                         /* (folded into pass 0, not drawn) */
        /* the shader variant this patch would take (as the draw loop's wat_v, det_v, tp_v) */
        int wat = wat_bit && (d->mcull & 2u) && k->e[7] > 0.5f;
        int det = tmat_bit && (d->mcull & 2u) && (k->pad4 & (PS2_TMAT_PAD4 | PS2_PROPS_PAD4)) == PS2_TMAT_PAD4;
        int tp = tp_bit && (d->mcull & 2u) && !(k->pad4 & PS2_PROPS_PAD4) && k->e[6] > 0.5f;
        int city = (k->pad4 & PS2_PROPS_PAD4) != 0u;
        int mat = det && rqo_mat_cfg();                           /* (PS2_RQ_MAT: the materials in the merged shader; on water patches none, their land texels lose them) */
        int plain = !wat && (!det || mat) && !tp && !city && rqt_lean_on && rqo_lean;
        int watm = wat && (!det || mat) && !tp && !city && rqo_wat_cfg() && !perf_ab_flipped("rqwater") && rqo_wat;
        if (rqm_cfg()) rqm_kind[plain ? 0 : watm ? 1 : (wat && det) ? 3 : det ? 2 : city ? 5 : tp ? 4 : wat ? 6 : 7]++;
        if (rqoh.ok && (plain || watm) && !d->tex_rt && d->tex != 0xFFFFu && d->tex < ntextures && textures[d->tex].set && (d->blend & 0xFFu) == 0x44u && ztst >= 2u && !(f & 3u) && ((f >> 17) & 1u) && rqo_valpha_one(d)) {
            if (afonly) { *g = RQO_DROP; return RQO_OP; }         /* (the alpha test's fail half: folded into the merged draw) */
            *g = RQO_NOBL | RQO_MRG;
            *c = rqo_class(d, k, wat, water_end);
            if (mat && !wat) *c |= 128u;
            if (rqm_cfg()) rqm_cls[*c]++;
            return RQO_OP;
        }
        int opaque = !d->tex_rt && d->tex != 0xFFFFu && rnb_tex_opaque(d->tex) && (d->blend & 0xFFu) == 0x44u && ztst >= 2u && rqo_valpha_one(d);
        if (opaque) {
            if (afonly) { *g = RQO_DROP; return RQO_OP; }         /* (no texel fails the test) */
            *g = RQO_NOBL;
            return RQO_OP;
        }
        return RQO_WAT;
    }
    if (!d->native && d->tex == 0xFFFFu && !d->tex_rt && ztst == 1u && rqo_flat_z0(d)) {
        if (zw && d->count >= 200u) { *g = RQO_SKYS; return RQO_SKY; }      /* the dome */
        if (!zw && !blend && d->count == 6u) { *g = rqoh.ok && rqoh.dome_covers ? (u8)(RQO_SKYS | RQO_DROP) : RQO_SKYS; return RQO_SKY; }   /* the quad under it: covered by the dome */
    }
    if (zw && !blend && ztst >= 2u) return (ate && atst != 1u) ? RQO_AT : RQO_OP;
    return RQO_BL;
}
/* the off-screen slots only the dropped cloud sprites read (the game's cloud impostors), else 0: their builds are left out too (item 4.3 c) */
static u64 rqo_imp_mask(const u32 *order) {
    u64 c = 0;
    for (u32 q = 0; q < rep_ndraws; q++) {
        const vk_draw *d = &rep_draws[order ? order[q] : q];
        if (d->tex_rt && d->tex_rt <= 64u && cl_sprite(d)) c |= 1ull << (d->tex_rt - 1u);
    }
    for (int changed = 1; changed && c;) {                       /* the slots those builds read (the puffs, the 2:1 shuffle's targets), transitively */
        changed = 0;
        for (u32 q = 0; q < rep_ndraws; q++) {
            const vk_draw *d = &rep_draws[order ? order[q] : q];
            if (d->rt == 0u || d->rt >= 64u || !(c & (1ull << d->rt)) || !d->tex_rt || d->tex_rt > 64u) continue;
            u64 s = 1ull << (d->tex_rt - 1u);
            if (!(c & s)) { c |= s; changed = 1; }
        }
    }
    for (int changed = 1; changed && c;) {                       /* a slot anything else reads stays, and with it what it is built from */
        changed = 0;
        for (u32 q = 0; q < rep_ndraws; q++) {
            const vk_draw *d = &rep_draws[order ? order[q] : q];
            if (!d->tex_rt || d->tex_rt > 64u || cl_sprite(d)) continue;
            u64 s = 1ull << (d->tex_rt - 1u);
            if ((c & s) && (d->rt >= 64u || !(c & (1ull << d->rt)))) { c &= ~s; changed = 1; }
        }
    }
    return c & ~1ull;
}
/* (R-60) the water's sun glitter and its edges, pushed in fxv.w (rq_water.glsl decodes: knee + 8 edge + 16 half-precision test):
   PS2_RQ_WGLINT_MAX=<G> (0.05..7.5; 0 = as before): the glitter rolls off softly towards G times its colour, G g / (G + g) in a form that stays
   finite in half precision, instead of the hard clamp min(g, 4) whose plateau is blown-out white specks;
   PS2_RQ_WATER_EDGE=1: the waves' slopes and the glitter's variance take the distance fade only, the water mask only the final mix (before: the
   mask's bilinear edge flattened the waves there into a mirror, whose glitter peaked in a rim along the mask's texel staircase);
   PS2_RQ_WFP16=1 [test]: the glitter's terms rounded to half precision with denormals flushed to zero (the Quest's mediump on the simulator) */
static float rqo_wglint_word(void) {
    static float v = -1.0f;
    if (v < 0.0f) {
        const char *e = getenv("PS2_RQ_WGLINT_MAX"); float g = e && *e ? (float)atof(e) : 0.0f;
        if (!(g > 0.0f)) g = 0.0f; else if (g < 0.05f) g = 0.05f; else if (g > 7.5f) g = 7.5f;
        e = getenv("PS2_RQ_WATER_EDGE"); int edge = e && *e == '1';
        e = getenv("PS2_RQ_WFP16"); int h = e && *e == '1';
        v = g + (edge ? 8.0f : 0.0f) + (h ? 16.0f : 0.0f);
        if (v > 0.0f) ps2_log("vk: the eye pass's water: glitter %s %.2f, edges %s%s (PS2_RQ_WGLINT_MAX, PS2_RQ_WATER_EDGE%s)", g > 0.0f ? "rolls off towards" : "clamped at", g > 0.0f ? (double)g : 4.0,
                              edge ? "by the distance fade only" : "as before", h ? ", half precision emulated" : "", h ? ", PS2_RQ_WFP16" : "");
    }
    return v;
}
/* the frame's constants of the merged water, as push constants (rq_water.glsl) */
static void rqo_water_push(vk_frame *fr, gs_push *pc) {
    /* PS2_RQ_MAP_HOST: the block's host copy (wat_frame's, with rqo_apply's p[0]): this runs per merged terrain / water draw, and each
       call read about 25 scattered floats out of the uncached arena -- most of the render thread's time on the simulator (P-5) */
    const ps2_vk_mesh_block *w = rqmh_now && rqmh_wat_fr == fr ? &rqmh_wat : (const ps2_vk_mesh_block *)fr->mcptr + MESH_WATER_BLOCK;
    rqmh_wpush++;
    memcpy(pc->rp, w->r, 8u * sizeof(float));                      /* RA, RB */
    pc->rp[8] = w->m[6]; pc->rp[9] = w->m[14]; pc->rp[10] = w->c[7]; pc->rp[11] = w->r[8];
    pc->rp[12] = w->c[4]; pc->rp[13] = w->c[5]; pc->rp[14] = w->c[6];
    pc->rp[15] = w->c[3] > 0.0f && (w->c[4] > 0.0f || w->c[5] > 0.0f || w->c[6] > 0.0f) ? 1.0f : 0.0f;
    memcpy(pc->rp2, &w->c[8], 4u * sizeof(float));
    memcpy(pc->soft, &w->c[12], 4u * sizeof(float));
    memcpy(pc->fxv, &w->l[8], 3u * sizeof(float)); pc->fxv[3] = rqo_wglint_word();   /* (R-60: 0 = as before) */
}
static const u32 *rqo_apply(vk_frame *fr, const u32 *order, u32 wat_bit, u32 tmat_bit, u32 tp_bit, int cld) {
    u32 n = rep_ndraws;
    if (!n || n > VK_MAX_DRAWS || !rep_mesh) return order;
    if (!rqo.ord) { rqo.ord = (u32 *)malloc((size_t)VK_MAX_DRAWS * sizeof *rqo.ord); rqo.grp = (u8 *)malloc(VK_MAX_DRAWS); rqo.cls = (u8 *)calloc(VK_MAX_DRAWS, 1); rqo.tmp = (u32 *)malloc((size_t)VK_MAX_DRAWS * sizeof *rqo.tmp); if (!rqo.ord || !rqo.grp || !rqo.cls || !rqo.tmp) return order; }
    float water_end = 1e30f;
    if (wat_bit && fr->mcptr) {
        ps2_vk_mesh_block *wb = (ps2_vk_mesh_block *)fr->mcptr + MESH_WATER_BLOCK;
        const ps2_vk_mesh_block *wr = rqmh_now && rqmh_wat_fr == fr ? &rqmh_wat : wb;   /* (PS2_RQ_MAP_HOST: l[] from the host copy) */
        if (wr->l[1] > 0.0f) water_end = wr->l[0] + 1.0f / wr->l[1];
        rqo_wfar_step(); wb->p[0] = rqo_wfar_cfg();
        if (rqmh_wat_fr == fr) rqmh_wat.p[0] = rqo_wfar_cfg();   /* (the host copy stays the arena's bytes) */
    }
    u32 a = n, b = 0;
    for (u32 q = 0; q < n; q++) { u32 i = order ? order[q] : q; if (rep_draws[i].rt == 0u) { if (a == n) a = q; b = q + 1u; } }
    if (a >= b) return order;
    for (u32 q = a; q < b; q++) if (rep_draws[order ? order[q] : q].rt != 0u) return order;      /* (not one run: as it was) */
    u32 t = b;                                                     /* the tail: from the first HUD or game-cockpit draw on */
    for (u32 q = a; q < b; q++) { const vk_draw *d = &rep_draws[order ? order[q] : q]; if (d->xcls == 2u || d->cockpit) { t = q; break; } }
    u32 *o = rqo.ord;
    u32 cnt[RQO_GN]; memset(cnt, 0, sizeof cnt);
    for (u32 q = a; q < t; q++) {
        u32 i = order ? order[q] : q;
        u8 fl;
        u8 cl;
        u32 g = (u32)rqo_group(&rep_draws[i], wat_bit, tmat_bit, tp_bit, water_end, &fl, &cl);
        rqo.grp[i] = (u8)(g | fl);
        rqo.cls[i] = cl;
        cnt[g]++;
    }
    if (rqoh.ok && rqoh.hz) for (u32 k = 0; k < rqoh.nl; k++) rqo.grp[rqoh.lay[k]] |= RQO_DROP;      /* (the haze ring: in the merged shader) */
    u32 start[RQO_GN], w = a;
    for (u32 g = 0; g < RQO_GN; g++) { start[g] = w; w += cnt[g]; }
    for (u32 q = 0; q < a; q++) o[q] = order ? order[q] : q;
    for (u32 q = a; q < t; q++) { u32 i = order ? order[q] : q; o[start[rqo.grp[i] & 15u]++] = i; }
    if (rqo_ocls_cfg() == 1 && cnt[RQO_OP] > 1u) {                      /* the opaque group by shader class: they commute under the depth test, and each class is one pipeline */
        u32 nkey[129], p0 = a; memset(nkey, 0, sizeof nkey);
#define RQO_SKEY(i) ((rqo.grp[i] & RQO_MRG) ? 1u + (rqo.cls[i] & 15u) + ((rqo.cls[i] & 48u) ? 16u : 0u) + ((rqo.cls[i] & 64u) ? 32u : 0u) + ((rqo.cls[i] & 128u) ? 64u : 0u) : 0u)
        for (u32 q = p0; q < p0 + cnt[RQO_OP]; q++) nkey[RQO_SKEY(o[q])]++;
        u32 sum = 0; for (u32 k = 0; k < 129u; k++) { u32 c = nkey[k]; nkey[k] = sum; sum += c; }
        for (u32 q = p0; q < p0 + cnt[RQO_OP]; q++) { u32 i = o[q]; rqo.tmp[nkey[RQO_SKEY(i)]++] = i; }
#undef RQO_SKEY
        memcpy(o + p0, rqo.tmp, cnt[RQO_OP] * sizeof *o);
    }
    for (u32 q = t; q < n; q++) {
        u32 i = order ? order[q] : q;
        o[q] = i;
        const vk_draw *d = &rep_draws[i];
        if (d->rt != 0u) continue;
        u8 fl = 0;
        if (d->xcls == 3u && !d->native && d->tex == 0xFFFFu && !d->tex_rt && (d->blend & 0x1FFu) == 0x144u && d->first + d->count <= rep_nverts) {   /* (the fade quad at alpha 0: nothing to draw) */
            int zero = 1;
            for (u32 j = d->first; j < d->first + d->count; j++) if (rep_verts[j].a != 0.0f) { zero = 0; break; }
            if (zero) fl = RQO_DROP;
        }
        rqo.grp[i] = (u8)(RQO_TAIL | fl);
    }
    rqo.skip_rt = cld ? rqo_imp_mask(o) : 0u;
    rqtf_list();
    rqo.on = 1; rqo.tail_ii = t;
    rqo.n_fr++;
    for (u32 g = 0; g < RQO_GN; g++) rqo.n[g] += cnt[g];
    rqo.n[RQO_TAIL] += b - t;
    if (!(rqo.n_fr % 1200u))
        ps2_log("rq order: %llu frames; draws a frame: opaque %.1f, alpha-tested %.1f, sky %.1f, non-opaque terrain %.1f, blended %.1f, tail %.1f; game's cloud impostor slots left out 0x%llX", (unsigned long long)rqo.n_fr,
                (double)rqo.n[RQO_OP] / 1200.0, (double)rqo.n[RQO_AT] / 1200.0, (double)rqo.n[RQO_SKY] / 1200.0, (double)rqo.n[RQO_WAT] / 1200.0, (double)rqo.n[RQO_BL] / 1200.0, (double)rqo.n[RQO_TAIL] / 1200.0, (unsigned long long)rqo.skip_rt), memset(rqo.n, 0, sizeof rqo.n);
    return o;
}
static inline void rqo_key(u32 i, u32 *dkey, u32 *bkey) {
    u8 g = rqo.grp[i];
    if (g & RQO_NOBL) *bkey &= ~0x100u;
    if (g & RQO_SKYS) *dkey = (*dkey & ~7u) | 2u;                 /* (GEQUAL, no depth write) */
}

/* ---- PS2_RQ_TREE_AA [test] (rq14_small 2026-11-15, prompt 12 Part B, BUGS_RQ R-185): the game's tree cards in the eye pass ----
   The game draws a tree as a card (two triangles, STQ) that samples one of its small tree impostors (a 128 x 128 slot it renders each
   frame from the crossed-card model), bilinear (TEX1 MMAG linear), CLAMP, with no mips (a render target has one level), alpha-tested
   GREATER 0x20 with AFAIL KEEP (a discard) on the near path, blended and tested GREATER 0 on the far, fading path. In the eye a card is
   4-6 game pixels wide = 16-33 eye pixels on the device: the 116-texel impostor is read at 4-8 texels an eye pixel, one bilinear tap
   (2 x 2 texels) a pixel, and the discard decides each pixel whole, so the 4x samples give the cut-out's edge nothing. Bits:
   1 = the read box-filtered over the pixel's footprint (up to 4 x 4 bilinear taps from the screen derivatives of the texel coordinate,
   colour weighted by alpha): a prefiltered read, what a mip chain would give; 2 = the near path's alpha test as coverage of the 4
   samples (gl_SampleMask from the alpha sharpened by its screen derivative, (a - ref) / fwidth(a) + 0.5) instead of a discard: alpha to
   coverage, with the stored alpha left as the game's FBA writes it. 3 = both. A card: a GS draw of slot 0 that reads an off-screen slot
   (not the feed, not depth, no DATE), alpha test GEQUAL / GREATER, STQ, bilinear, a depth write, discard on fail. rq_gs_tree.frag
   (rq_gs.frag -DTREE_AA) through get_pipeline_ex's mesh bit 3 on a GS draw (key bit 55), the mode in push flags.w. 4 = the cards keep
   their own x: vk_sprite (ps2_gs.c) moves a sprite's left and right edges to whole game pixels -- exact at 1x, in the eye a card's sides
   cut or widened by up to half a game pixel (~2 eye pixels), another amount every frame (with PS2_RQ=eye). Default 0.
   8 (R-241, with 1 / 2): the Adreno-safe modules -- on the device the cards of rq_gs_tree.frag drew invisible (both paths), the
   simulator draws them. The far (blended) cards and, without 2, the near ones take rq_gs_tree_nm.frag (the box read, no sample mask
   named anywhere: the game's alpha test as a discard); with 2 the near cards (their blend off) take rq_gs_tree_sd.frag (the box read
   and the coverage mask, but no second blend source: the mask and the dual-source output are not written together), through
   get_pipeline_ex's mesh bit 4 (key bit 56, unused for GS draws). 11 = all of it: the far cards then show whether the box read draws
   on the device, the near cards whether the sample mask does without the second source.
   16 (R-242, rq_treemip.inc; with 8 and 2: `26`): each impostor the list's cards read gets a premultiplied mip chain, built in one compute
   dispatch where the eye pass begins, and the cards take ONE trilinear tap from it instead of the box read (flags.w bit 2); 11's look at
   about the game's read's price. PS2_RQ_TREE_AA_AB=M [test]: mode M in PS2_RQ_ABSKIP's left-out blocks (keep bit 8 alike: the modules
   are chosen by the main mode). */
static u64 rqtree_n[2];                    /* (cards drawn: near path, far path) */
static int rqtree_cfg(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("PS2_RQ_TREE_AA");
        v = e && *e >= '0' && *e <= '9' ? atoi(e) & 31 : 0;
        if (v) ps2_log("vk: PS2_RQ_TREE_AA=%d: the tree cards' impostor read %s%s%s", v, (v & 1) ? "box-filtered over the pixel's footprint" : "as the game's",
                       (v & 2) ? ", the near path's alpha test as sample coverage (alpha to coverage)" : ", the alpha test a discard",
                       (v & 4) ? ", the cards' x not moved to whole game pixels (ps2_gs.c vk_sprite)" : "");
        if (v & 16) ps2_log("vk: PS2_RQ_TREE_AA=%d (R-242): the cards read a premultiplied mip chain of their impostor, one trilinear tap (rq_treemip.inc)%s", v, (v & 8) ? "" : " -- without bit 8 the near cards take rq_gs_tree.frag, invisible on the device (R-241)");
        if (v & 8) ps2_log("vk: PS2_RQ_TREE_AA=%d (R-241): the Adreno-safe modules -- far cards%s rq_gs_tree_nm (no sample mask), near cards %s", v,
                           (v & 2) ? "" : " and near cards", (v & 2) ? "rq_gs_tree_sd (the coverage mask without the second blend source)" : "the same");
    }
    return v;
}
static int rqtree_ab(void) {                   /* PS2_RQ_TREE_AA_AB=M [test]: the mode in PS2_RQ_ABSKIP's left-out blocks (-1: none) */
    static int v = -2;
    if (v == -2) { const char *e = getenv("PS2_RQ_TREE_AA_AB"); v = e && *e >= '0' && *e <= '9' ? atoi(e) & 31 : -1;
                   if (v >= 0) ps2_log("vk: PS2_RQ_TREE_AA_AB=%d [test]: the tree cards' mode in the left-out blocks of PS2_RQ_ABSKIP=%d (`rq-ab:` with = PS2_RQ_TREE_AA=%d)", v, rqab_get(), rqtree_cfg()); }
    return v;
}
static int rqtree_eff(void) { int a = rqtree_ab(); return a >= 0 && rqab_get() > 0 && rqab_active() ? a : rqtree_cfg(); }   /* (this list's mode) */
static int rqtree_any(void) { int a = rqtree_ab(); return rqtree_cfg() | (a > 0 ? a : 0); }
static VkShaderModule rqtree_mod(u32 mesh) {   /* (pipe_build's module for a tree card; mesh 128 = get_pipeline_ex's 16: a near card of 8 | 2) */
    int a = rqtree_any(), mip = (a & 16) && !(a & 1);   /* (bit 16 without the box read: the lean twins -DTREE_MIP, no box code) */
    if (!(a & 8)) return rqf_gt;
    if ((mesh & 128u) && rqf_gt_sd) return mip && rqf_gt_sdm ? rqf_gt_sdm : rqf_gt_sd;
    return mip && rqf_gt_nmm ? rqf_gt_nmm : rqf_gt_nm ? rqf_gt_nm : rqf_gt;
}
static int rqo_tree_card(const vk_draw *d, u32 dkey) {
    u32 f = d->frag_flags, at = (f >> 4) & 7u;
    if (d->native || d->rt != 0u || !d->tex_rt || d->tex_rt <= 1u || d->tex_rt > RT_SLOTS || d->tex_depth || d->tex_self || d->date) return 0;
    if (!((f >> 3) & 1u) || (at != 5u && at != 6u) || ((f >> 2) & 1u) || ((f >> 19) & 1u) || !((f >> 18) & 1u)) return 0;   /* (ate, GEQUAL / GREATER, STQ, bilinear, discard) */
    if (((f >> 20) & 1u) || ((f >> 24) & 1u) || ((f >> 30) & 1u) || (f >> 31)) return 0;   /* (no FB-only test, shuffle, merge, marker) */
    return ((dkey >> 2) & 1u) != 0u;                                                          /* (a depth write) */
}
