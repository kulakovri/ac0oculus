/* rq_cloud_far.glsl - the Quest eye pass's copy of cloud_far.glsl (rq_cloud_far.comp): the far march into the direction
   map, with the near / far cross-fade: the far march weighs a sample at distance t by w = smoothstep(R - W, R, t) (rq_far_w),
   R = cl.cfg.z (the near range), W = cl.shaft[1].z (the band, m; 0 = no band: the far march starts at R as before); the near
   march (rq_cloud_near.comp) weighs by 1 - w, so the two parts sum to the one volume over the band. Otherwise unchanged from
   cloud_far.glsl (S2; ps2_clouds_far.c records it). Set 1 is the far module's own. */
#ifndef RQ_CLOUD_FAR_GLSL
#define RQ_CLOUD_FAR_GLSL

float rq_far_w(float t) { return cl.shaft[1].z > 0.0 ? smoothstep(cl.cfg.z - cl.shaft[1].z, cl.cfg.z, t) : 1.0; }


/* Colour and extinction come from the shared core in cloud_common.glsl (cl_shade_s, cl_above, cl_cast_t,
   cl_extinction: the game's impostor model plus the cloud shadow map's cast shadow, identical in the near field, so the
   600 m hand-over has no seam). No multiple scattering, no powder, no silver lining. */
layout(set = 1, binding = 0, std140) uniform ClFarParams {
    vec4 mz;     /* x jitter amount (0 = none), y noise prefilter: x the step (0 = off), z the "above" probe every N fine
                    steps (reused in between), w 1 = half rate: tiles below the hard limit (2x the error) are marched
                    on even frames only, the must list every frame */
    vec4 spl;    /* cloud[] splat (S1: radius_m / height_m = the body's horizontal / vertical semi-axes; full to 0.45,
                    gone at 0.9 of them): x half-height / R when a cloud has no height_m, y extent below the centre
                    relative to above (< 1: flatter base), z full-body fraction (0.45), w cut-off fraction (0.9) */
    vec4 haze;   /* x thin-medium steps: in faint cloud step up to x times the step of stp.w optical depth (at most the
                    empty-space step; 0 = off), y opacity fade start (m), z fade end (m), w detail range (m) */
    vec4 stp;    /* x empty-space step factor, y max step (m), z min transmittance (early out), w max optical depth
                    per step */
    vec4 sch;    /* x error factor for tiles outside the view, y refresh-period factor outside the view,
                    z view margin (rad, head motion before the next refresh), w allowed error in 8-bit levels (the
                    contrast-aware limit: a smooth tile may move further than the pixel threshold) */
    vec4 fine;   /* x step inside cloud relative to the empty-space base step (< 1: denser sampling where it shows),
                    y splat: vertical optical depth through a cloud's centre (the density is scaled per cloud by its
                    height; the game's clouds are semi-transparent veils; 0 = full density), z splat: the envelope's
                    billows (0 = smooth ellipsoids; else a core 1 - 0.8 z wide plus w dome-topped puffs) */
    vec4 win;    /* xy: centre of the weather window relative to the centre eye (cloud space x, z; m); z: 1 = nothing
                    beyond the window (the cloud-list path: the game shows clouds to ~12 km), 0 = the procedural field
                    continues; w: weather texels per metre (512 / 20000) */
    vec4 ext;    /* x core fill for cumulus-like weather (C1 type >= 0.6, blended from 0.2): the splat's coverage R is
                    scaled by it, so the noise remap cuts holes inside the bodies (0 or 1 = off); y the radius of the
                    density's softening across the borders between clouds (PASS_WSOFT; weather texels, < 0.5 = off);
                    z 1 = the opacity-weighted splat (ACZ_VR_CLOUD_SPLAT_MIX; 0 = the strongest profile's cloud and
                    the union of all columns, as before); w 1 = a tile cap applies only to the tiles outside the view
                    (the governor's level 1 with ACZ_VR_GOV_FAR_MODE=1) */
    vec4 dbg;    /* x 1 = ACZ_DEBUG_CLOUD_FAR_STALE (PASS_TILES: each visible tile left alone is checked against a march
                    of its centre texel now); y 1 = ACZ_DEBUG_CLOUDS (the drawn tiles' staleness, cnt[11]); z 1 =
                    PS2_RQ_CLOUDS_FARNB (the empty tiles beside cloud drawn too); w the staging's slots (PS2_RQ_CLOUDS_FARSWAP_SLOTS,
                    R-70; 0 = 4096) */
    vec4 rq;     /* x 1 = PS2_RQ_CLOUDS_FARSWAP (sweeps from one origin, staged, copied in at once), y the longest sweep (frames),
                    z PS2_RQ_CLOUDS_COLD: a new generation's never-marched tiles in view marched a frame at most (0 = all at once,
                    as before; the ones outside the view then wait in the budgeted bucket 7), w PS2_RQ_CLOUDS_FARFAIR (R-70: 1 the
                    tiles the last sweep missed first, 2 also the stale tiles coming into view marched at once) */
    vec4 pred;   /* PS2_RQ_CLOUDS_FARLEAD (R-70): xyz the eye's motion per far frame (anchor frame, m, smoothed), w the lead in sweeps
                    (0 = off): a sweep's origin is put where the eye will be when the sweep is half shown */
    vec4 rq2;    /* (clouds6) x 1 = PS2_RQ_CLOUDS_FARONE: the never-marched and outside tiles marched from the shown sweep's origin
                    (rq_cloud_far.comp PASS_MARCH); y PS2_RQ_CLOUDS_FARCOARSE: the coarse level's first entry in the staging (0 =
                    off), z its chunks (PASS_COARSE, PASS_TILES), w 1 = PS2_RQ_CLOUDS_FARWARP_ONE (PASS_TILES writes the shown sweep's origin
                    into the draw list's spare entries) */
    vec4 rq3;    /* (clouds7, prompt 8 step 3) x 1 = PS2_RQ_CLOUDS_FARSHOWN: the parallax resolved per texel once a frame into the shown map
                    (PASS_RESOLVE) and the sweeps off (rq.x 0: the tiles re-marched by the travel rule, each from the eye); y the resolve's
                    steps (1-3) */
#if defined(CL_FAR_BAKE) || defined(CL_FAR_BAKE_P)
    /* (rq7_bake 2026-11-14, prompt 9 steps 2-3: PS2_RQ_CLOUDS_BAKE; declared only by the bake's modules, so every other module's
       SPIR-V stays as it was) */
    vec4 bk;     /* x 1 = the baked volumes are valid, y the bake's band base (m of altitude), z 1 / its thickness (m), w 1 = the distance
                    field steps the clear air (PS2_RQ_CLOUDS_BAKE_DF) */
    vec4 bk2;    /* the distance field: x a coarse cell along x / z (m), y across the band (m), z cells along x / z, w across the band */
    vec4 bk3;    /* x 1 = the detail octave on the baked density within haze.w (PS2_RQ_CLOUDS_BAKE_DETAIL), y the density volume's cell
                    (m), z 1 = "above" from the density volume at the probe point (PS2_RQ_CLOUDS_BAKE_ABOVE), w PS2_RQ_CLOUDS_ONE (rq8_onelayer:
                    1 the one layer from the eye, no near / far cross-fade; 2 also the static field's period torus) */
#endif
} fp;

#if defined(CL_FAR_BAKE) || defined(CL_FAR_BAKE_P)
/* (rq8_onelayer, PS2_RQ_CLOUDS_ONE=2: the volumes over the cloud list's period, cl.wx its torus's mapping. The shape's base noise read with an
   integer number of its own periods over the list's period (Zero: 21.8 km / 4 km -> 5, a period of 4.37 km, 9 % larger features), from the
   torus coordinate: it repeats with the period, so the field has no seam half a period from the bake's eye. The bake and the march's near
   samples read it) */
vec4 bk_noise_base_p(vec3 p, float step_m) {
    vec4 sp = cl_shape_params();
    float P = 1.0 / cl.wx.z, nrep = max(floor(P / sp.x + 0.5), 1.0), pe = P / nrep;
    vec2 u = cl.wx.xy + p.xz * cl.wx.z;
    return textureLod(cl_noise_base, vec3(u.x * nrep, cl_alt(p) / pe, u.y * nrep), max(log2(max(step_m, 1.0e-3) * 0.5 * 128.0 / pe), 0.0));
}
#endif
struct ClFarP { vec3 ro; float tstart; float tmax; int nmax; float dtmin; float k; };
/* prof (ACZ_VR_CLOUD_FAR_SPLIT, cl_split 2026-10-30): x = the first distance with cloud (= first), y / z / w = where the
   accumulated transmittance falls to 0.8 / 0.5 / 0.2 (CL_DEPTH_NONE: never) -- cloud_dir_prof */
struct ClFarR { vec3 L; float T; float depth; float first; uint evals; vec4 prof; };
#ifdef CL_FAR_CHECK
/* [debug] PS2_RQ_CLOUDS_BAKE_COUNT=2 (rq7_bake): the baked against the procedural sample at every fine sample of the baked march: sums
   of the baked density, the procedural density (near: t < haze.w, far), their counts, |above| and |the cast shadow| differences */
float ck_db[2], ck_dp[2], ck_n[2], ck_ab, ck_su;
#endif

/* static per-texel jitter: white noise (an interleaved-gradient pattern beat against the step slices into diagonal
   moire); fixed per map texel, so it never flickers and is identical for both eyes */
float cl_far_jit(vec2 p) {
    uvec2 v = uvec2(ivec2(p)) * uvec2(1597334673u, 3812015801u);
    uint n = (v.x ^ v.y) * 1597334673u; n ^= n >> 16; n *= 2246822519u; n ^= n >> 13;
    return float(n) * (1.0 / 4294967296.0);
}

/* ---- procedural weather (the fallback without the game's cloud list): C1 coverage -> R coverage, G 0, B 1, A 1.
   World-anchored (full cloud-frame x/z), not periodic; the weather pass writes it into the window and the march uses
   it beyond the window. */
float cl_far_hash2(ivec2 c) {
    uvec2 v = uvec2(c) * uvec2(1597334673u, 3812015801u);
    uint n = (v.x ^ v.y) * 1597334673u;
    n ^= n >> 16; n *= 2246822519u; n ^= n >> 13;
    return float(n) * (1.0 / 4294967296.0);
}
float cl_far_vnoise(vec2 hi, vec2 lo, float cell) {      /* value noise at hi + lo (hi: multiples of 4096 m) */
    vec2 q = hi / cell + lo / cell, fl = floor(q), f = q - fl;
    ivec2 i = ivec2(fl);
    f = f * f * (3.0 - 2.0 * f);
    float a = cl_far_hash2(i), b = cl_far_hash2(i + ivec2(1, 0)), c = cl_far_hash2(i + ivec2(0, 1)),
          d = cl_far_hash2(i + ivec2(1, 1));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
vec4 cl_weather_proc(vec3 p_m, bool fine) {
    vec2 hi = cl_world_full_xz_hi(), lo = cl_world_full_xz_lo(p_m);
    float n = 0.48 * cl_far_vnoise(hi, lo, 4700.0) + 0.27 * cl_far_vnoise(hi, lo + 1234.5, 2300.0)
            + 0.15 * cl_far_vnoise(hi, lo - 777.0, 1100.0) + (fine ? 0.10 * cl_far_vnoise(hi, lo + 333.0, 470.0) : 0.05);
    float C = cl.band.z;
    /* fBm of value noise: mean 0.5, sd ~0.12; the threshold follows the coverage, overcast (C -> 1) covers all */
    float thr = mix(0.70, 0.30, C);
    float cov = smoothstep(thr - 0.05, thr + 0.10, n);
    cov = max(cov, smoothstep(0.85, 1.0, C));
    return vec4(cov, 0.0, 1.0, 1.0);
}
/* the weather for the far march: the window texture, beyond ~9.5 km from its centre the procedural field (or
   nothing on the cloud-list path); never the toroidal seam */
vec4 cl_far_weather(vec3 p_m) {
    vec2 r2 = abs(p_m.xz - fp.win.xy);
    float r = max(r2.x, r2.y);
    if (r < 9000.0) return cl_weather(p_m);
    vec4 far = fp.win.z > 0.5 ? vec4(0.0, 0.0, 1.0, 0.0) : cl_weather_proc(p_m, false);
    if (r >= 9800.0) return far;
    return mix(cl_weather(p_m), far, smoothstep(9000.0, 9800.0, r));
}
float cl_far_density(vec3 p, bool detail, float step_m) { return cl_density_ws(p, cl_far_weather(p), detail, step_m * fp.mz.y); }
#ifdef CL_FAR_STUB
/* [test] PS2_RQ_CLOUDS_FARSTUB (agent clouds6 2026-11-11, A.5 item 4 of PROMPT_2026-11-11_flights2_fixes.md; the variant
   rq_cloud_far_march_stub.comp only): the price of a cached sample. Every density the march takes (the empty-space sample,
   the entry's bisection, the fine sample) and the fine sample's light (the "above" probe and the cast shadow) come from ONE
   trilinear read of the light grid's texture (cl_lgrid_tex, ACZ_VR_CLOUD_LGRID: 160 x 32 x 160 RGBA8, repeat in x / z) at the
   sample point: r = the density (and "above"), g = the cast shadow. The picture is wrong (the grid's 6.4 km window repeats and
   holds no density); the march's structure, its steps and its shading are the real ones: the time per sample is what counts. */
vec4 rq_stub_read(vec3 p) {
    float hb = cl.band.x, ht = max(cl.band.y, hb + 1.0), w = cl.lgrid.w > 0.0 ? cl.lgrid.w : 1.0 / 6400.0;
    return textureLod(cl_lgrid_tex, vec3((cl.lgrid.y + p.x) * w, (cl_alt(p) - hb) / (ht - hb), (cl.lgrid.z + p.z) * w), 0.0);
}
float rq_stub_d(vec3 p) { return max(rq_stub_read(p).r - 0.05, 0.0); }
#endif
#ifdef CL_FAR_LG
/* ACZ_VR_CLOUD_LGRID_DENS (clouds6 2026-11-11, A.4 piece 5; the variant rq_cloud_far_march_lg.comp): a sample within the density
   grid's radius of use is ONE read of it (cloud_common.glsl cl_lgd: the density without the detail octave, the local height, "above",
   the cast shadow); beyond it, the real sample. The grid's trilinear density is not 0 in a thin halo round a cloud: under 0.02 (sqrt)
   it counts as empty, so the empty-space steps still skip the clear air. */
float rq_lgd_dens(vec4 g) { return g.b > 0.02 ? g.b * g.b : 0.0; }
float rq_lg_density(vec3 p, float step_m) { vec4 g; return cl_lgd_on() && cl_lgd(p, g) ? rq_lgd_dens(g) : cl_far_density(p, false, step_m); }
#endif
#ifdef CL_FAR_BAKE
/* PS2_RQ_CLOUDS_BAKE (rq7_bake 2026-11-14, prompt 9 steps 2-3; the variant rq_cloud_far_march_bake.comp; ps2_clouds_far.c bk_*): the
   far march's samples from small volumes baked once (rq_cloud_bake.comp) instead of the weather, the noise, the probe and the shadow map
   per sample. All three are world-anchored like the weather map: x / z = the weather map's own uv (cloud-frame x / z over 20 km, REPEAT,
   the same torus), y = the place in the bake's C1 band. bk_dens: R8, sqrt(c), c = cl_density_tx's coverage term before its weather density
   scale (without the detail octave, the noise prefiltered at the cell); bk_light: RGBA8 at a coarser grid, r = "above" (cl_above at the
   probe point), g = the cast shadow's transmittance at the probe point WITHOUT the shadow map's edge fade (applied here from this frame's
   eye), b = the local height in the column (cl_local_h), a = sqrt(the density scale wx.a); the density = c x wx.a, with the detail octave
   (fp.bk3.x, within haze.w) as cl_density_tx applies it, on c; bk_df: R8, the L-inf distance in coarse cells to the nearest coarse cell
   holding cloud (texelFetch). */
layout(set = 1, binding = 12) uniform sampler3D bk_dens;
layout(set = 1, binding = 13) uniform sampler3D bk_light;
layout(set = 1, binding = 14) uniform sampler3D bk_df;
vec3 bk_uvw(vec3 p, out float v) {
    v = (cl_alt(p) - fp.bk.y) * fp.bk.z;
    return vec3(cl.wx.x + p.x * cl.wx.z, v, cl.wx.y + p.z * cl.wx.z);
}
/* the coverage term c at p (cloud space): one trilinear read; the trilinear halo round a cloud (under 0.02 in sqrt) counts as empty, so
   the empty-space steps still skip clear air (the occupancy test: c > 0); the weather window's edge as cl_far_weather's (nothing beyond on
   the list path) */
float bk_density(vec3 p) {
    float v; vec3 uvw = bk_uvw(p, v);
    if (v <= 0.0 || v >= 1.0) return 0.0;
    float s = textureLod(bk_dens, uvw, 0.0).r;
    if (s < 0.02) return 0.0;
    float d = s * s;
    if (fp.win.z > 0.5 && fp.bk3.w < 1.5) { vec2 r2 = abs(p.xz - fp.win.xy); d *= 1.0 - smoothstep(9000.0, 9800.0, max(r2.x, r2.y)); }   /* (PS2_RQ_CLOUDS_ONE=2: the period's torus has no window) */
    return d;
}
/* (rq8_onelayer, PS2_RQ_CLOUDS_ONE: within fp.mz.w metres of the eye the coverage term c of a fine sample is computed from the weather and
   the base noise at the step's own detail instead of read from the 78 m density cells, blended over the last 30 % of that range. Seen
   from inside a body the baked cells fill its interior where the full noise leaves gaps of clear air: a veil over everything that the
   near march never drew (M10 f5954, PS2_RQ_CLOUDS_ONE_ALT). With =2 the base noise is the bake's own (periodic in the list's period),
   so a feature keeps its shape as it crosses the range) */
float bk_near_w(float t) { return fp.mz.w > 0.0 ? 1.0 - smoothstep(0.7 * fp.mz.w, fp.mz.w, t) : 0.0; }
float bk_coverage_proc(vec3 p, float step_m) {
    vec4 wx = cl_weather(p);
    if (!cl_in_column(p, wx)) return 0.0;
    vec4 b = fp.bk3.w > 1.5 ? bk_noise_base_p(p, step_m) : cl_noise_base_ws(p, step_m);
    float d = cl_density_tx(p, wx, b, vec4(0.0), false);
    return wx.a > 0.0 ? clamp(d / wx.a, 0.0, 1.0) : 0.0;
}
/* the density from c and the light texel g: the detail octave's erosion on c exactly as cl_density_tx (dn.x < 0: none), then the scale */
float bk_dens_full(float c, vec4 g, vec4 dn) {
    if (c <= 0.0) return 0.0;
    if (dn.x >= 0.0) {
        float dfbm = dn.r * 0.625 + dn.g * 0.25 + dn.b * 0.125;
        float m = mix(dfbm, 1.0 - dfbm, clamp(g.b * 4.0, 0.0, 1.0));
        c = clamp(cl_remap(c, m * cl_shape_params().z, 1.0, 0.0, 1.0), 0.0, 1.0);
    }
    return c * g.a * g.a;
}
vec4 bk_light_at(vec3 p) { float v; vec3 uvw = bk_uvw(p, v); uvw.y = clamp(v, 0.0, 1.0); return textureLod(bk_light, uvw, 0.0); }
/* the cast shadow: the baked transmittance at the probe point, faded by the shadow map's edge as seen from this frame's eye
   (cloud_shadow.glsl cl_shadow_od_map's weight); 1 without a map */
float bk_cast(vec3 p, float traw) {
    if (fp.bk3.w > 1.5) return traw;                  /* (PS2_RQ_CLOUDS_ONE=2: the bake marched it over the torus, no map edge) */
    if (cl.shd[0].w <= 0.0) return 1.0;
    vec3 q = cl_probe_pos(p);
    float alt = cl.shd[1].w + q.y;
    vec2 qb = q.xz - cl.shd[1].xy * (alt - cl.shd[1].z);
    float w = 1.0 - clamp((max(abs(qb.x), abs(qb.y)) - cl.shd[2].z) * abs(cl.shd[2].w), 0.0, 1.0);
    return mix(1.0, traw, w);
}
/* the distance field: how far the ray may go from p without any trilinear density read meeting cloud (m; 0 = near cloud: sample it).
   d = the L-inf distance (coarse cells) of p's cell to a cell holding cloud; every cell within d - 1 of p's is clear, and a read at q
   touches texels within one fine texel of q, so q within d - 2 cells of p's cell reads 0: the box of (d - 2) cells about p's cell */
float bk_df_skip(vec3 p, vec3 rd) {
    float v = (cl_alt(p) - fp.bk.y) * fp.bk.z;
    if (v < 0.0 || v >= 1.0) return 0.0;
    vec2 uv = fract(cl.wx.xy + p.xz * cl.wx.z);
    ivec3 c = ivec3(int(uv.x * fp.bk2.z), int(v * fp.bk2.w), int(uv.y * fp.bk2.z));
    c = clamp(c, ivec3(0), ivec3(int(fp.bk2.z) - 1, int(fp.bk2.w) - 1, int(fp.bk2.z) - 1));
    float d = floor(texelFetch(bk_df, c, 0).r * 255.0 + 0.5);
    if (d < 2.5) return 0.0;
    vec3 a = max(abs(rd), vec3(1.0e-6));
    return (d - 2.0) * min(min(fp.bk2.x / a.x, fp.bk2.y / a.y), fp.bk2.x / a.z);
}
/* (CL_FAR_COUNT: the empty-space density reads in evals bits 0-8, the distance-field reads in bits 9-15, the fine samples from bit 16) */
#define BK_EV_DF 0x200u
#endif

/* March one ray (cloud space) through the band between tstart and tmax. jit in 0..1 (static per map texel). */
#ifndef CL_FAR_SPEC
#define CL_FAR_SPEC 2           /* fine-step samples read ahead (1 = one at a time; 4 costs registers and wasted reads:
                                   3x slower at full load on the 3090 Ti) */
#endif
ClFarR cl_far_march(ClFarP P, vec3 rd, float jit) {
    ClFarR r = ClFarR(vec3(0.0), 1.0, CL_DEPTH_NONE, CL_DEPTH_NONE, 0u, vec4(CL_DEPTH_NONE));
    float hb = cl.band.x, ht = cl.band.y, y0 = cl_alt(P.ro);
    float t0 = P.tstart, t1 = P.tmax;
    if (abs(rd.y) < 1e-6) {
        if (y0 <= hb || y0 >= ht) return r;
    } else {
        float ta = (hb - y0) / rd.y, tb = (ht - y0) / rd.y;
        t0 = max(t0, min(ta, tb)); t1 = min(t1, max(ta, tb));
    }
    if (t1 <= t0) return r;
    float sig0 = cl_extinction();
    float dtmax = fp.stp.y, coarse = fp.stp.x;
    float wsum = 0.0, dsum = 0.0;
    jit = 0.5 + (jit - 0.5) * fp.mz.x;
    float t = t0 + jit * clamp(t0 * P.k, P.dtmin, dtmax);
    float tempty = t0;          /* last position known to be empty */
    int zrun = 3;               /* >= 3: empty-space (coarse) stepping */
    int nl = 0; float above = 0.0, sun_t = 1.0;
    int lstep = max(int(fp.mz.z), 1);
    int i = 0;
    bool done = false;
    float dte = -1.0;           /* > 0: the step of the sample right after an entry (from the step before the entry) */
    float dthin = 0.0;          /* the last in-cloud step when it was a thin-medium one (empty samples keep it) */
#ifdef CL_FAR_BAKE
    float tsp = -1.0;           /* (rq8_onelayer, PS2_RQ_CLOUDS_ONE: the transmittance where the ray passes fp.sch.y, the front's end) */
#endif
    while (!done && i < P.nmax && t < t1) {
#ifdef CL_FAR_BAKE
        if (tsp < 0.0 && t >= fp.sch.y && fp.bk3.w > 0.5) tsp = r.T;
#endif
        if (zrun >= 3) {
            dthin = 0.0;
            float dt = clamp(t * P.k, P.dtmin, dtmax);
#ifdef CL_FAR_STUB
            float dc = rq_stub_d(P.ro + rd * t); r.evals++;
#elif defined(CL_FAR_LG)
            float dc = rq_lg_density(P.ro + rd * t, dt); r.evals++;
#elif defined(CL_FAR_BAKE)
            if (fp.bk.w > 0.5) {               /* (PS2_RQ_CLOUDS_BAKE_DF: clear air by the distance field, no density read) */
                float sk = bk_df_skip(P.ro + rd * t, rd); r.evals += BK_EV_DF;
                if (sk > 0.0) { tempty = t + sk; t += max(sk, dt * coarse); i++; continue; }
            }
            float dc = bk_density(P.ro + rd * t); r.evals++;
#else
            float dc = cl_far_density(P.ro + rd * t, false, dt); r.evals++;
#endif
            if (dc <= 0.0) { tempty = t; t += dt * coarse; i++; continue; }
            /* hit after empty space: bisect the entry twice between the last empty sample and here, then march
               finely from there (removes the slice pattern at silhouettes); that fine sample is this iteration's */
            float ta = tempty, tb = t;
            for (int b = 0; b < 2; b++) {
                float tm = 0.5 * (ta + tb);
#ifdef CL_FAR_STUB
                if (rq_stub_d(P.ro + rd * tm) > 0.0) tb = tm; else ta = tm;
#elif defined(CL_FAR_BAKE)
                if (bk_density(P.ro + rd * tm) > 0.0) tb = tm; else ta = tm;
#elif defined(CL_FAR_LG)
                if (rq_lg_density(P.ro + rd * tm, dt) > 0.0) tb = tm; else ta = tm;
#else
                if (cl_far_density(P.ro + rd * tm, false, dt) > 0.0) tb = tm; else ta = tm;
#endif
                r.evals++;
            }
            zrun = 0;
            dte = max(dt * fp.fine.x, P.dtmin * 0.5);
            t = ta + jit * dte;                            /* from the last empty point, jittered by one fine step */
        }
        /* fine stepping, CL_FAR_SPEC samples read ahead: their positions are fixed unless a sample's optical depth caps
           its step or three empty samples end the fine mode. All their reads go out together (the weather, both noise
           texels, the probe's weather and base noise), then they are integrated in order; where a guess fails the
           loop re-speculates from the true position. Same samples, same order as one-at-a-time stepping: the near
           tiles are latency-bound (~2 us of dependent reads per step with the GPU mostly idle). */
        float ts[CL_FAR_SPEC], ds[CL_FAR_SPEC];
#ifdef CL_FAR_STUB
        vec4 G[CL_FAR_SPEC];
#elif defined(CL_FAR_BAKE)
        float BD[CL_FAR_SPEC]; vec4 BL[CL_FAR_SPEC], BX[CL_FAR_SPEC];
#else
        vec4 W[CL_FAR_SPEC], B[CL_FAR_SPEC], D[CL_FAR_SPEC], WQ[CL_FAR_SPEC], BQ[CL_FAR_SPEC];
#endif
#ifdef CL_FAR_LG
        vec4 GL[CL_FAR_SPEC]; bool cj[CL_FAR_SPEC];
#endif
        float tt = t;
        [[unroll]] for (int j = 0; j < CL_FAR_SPEC; j++) {
            float dj = (j == 0 && dte > 0.0) ? dte : max(clamp(tt * P.k, P.dtmin, dtmax) * fp.fine.x, P.dtmin * 0.5);
            ts[j] = tt; ds[j] = dj; tt += dj;
        }
        dte = -1.0;
#ifdef CL_FAR_STUB
        [[unroll]] for (int j = 0; j < CL_FAR_SPEC; j++) G[j] = rq_stub_read(P.ro + rd * ts[j]);
#elif defined(CL_FAR_BAKE)
        [[unroll]] for (int j = 0; j < CL_FAR_SPEC; j++) {
            vec3 pj = P.ro + rd * ts[j];
            BD[j] = bk_density(pj);
            {   float wn = bk_near_w(ts[j]);              /* (PS2_RQ_CLOUDS_ONE: the full noise near the eye) */
                if (wn > 0.0) BD[j] = mix(BD[j], bk_coverage_proc(pj, ds[j] * fp.mz.y), wn); }
            BL[j] = bk_light_at(pj);
            BX[j] = fp.bk3.x > 0.5 && ts[j] < fp.haze.w ? cl_noise_detail_ws(pj, ds[j] * fp.mz.y) : vec4(-1.0);
        }
#else
#ifdef CL_FAR_LG
        [[unroll]] for (int j = 0; j < CL_FAR_SPEC; j++) cj[j] = cl_lgd_on() && cl_lgd(P.ro + rd * ts[j], GL[j]);
#endif
        [[unroll]] for (int j = 0; j < CL_FAR_SPEC; j++) {
#ifdef CL_FAR_LG
            if (cj[j]) {                           /* (a cached sample: no other read but the detail octave's, ACZ_VR_CLOUD_LGRID_DETAIL_M) */
                D[j] = ts[j] < fp.haze.w && cl_lgd_det_on(ts[j]) ? cl_noise_detail_ws(P.ro + rd * ts[j], ds[j] * fp.mz.y) : vec4(-1.0);
                continue;
            }
#endif
            vec3 pj = P.ro + rd * ts[j], qj = cl_probe_pos(pj);
            float lj = ds[j] * fp.mz.y;
            W[j] = cl_far_weather(pj); B[j] = cl_noise_base_ws(pj, lj);
            D[j] = ts[j] < fp.haze.w ? cl_noise_detail_ws(pj, lj) : vec4(0.0);
            WQ[j] = cl_far_weather(qj); BQ[j] = cl_noise_base_ws(qj, lj);
        }
#endif
        [[unroll]] for (int j = 0; j < CL_FAR_SPEC; j++) {
            /* sample 0 belongs to the iteration the while condition (or the entry) admitted */
            if (j > 0 && (done || zrun >= 3 || i >= P.nmax || !(t < t1) || t != ts[j])) break;
#ifdef CL_FAR_BAKE
            if (tsp < 0.0 && t >= fp.sch.y && fp.bk3.w > 0.5) tsp = r.T;
#endif
            float dt = ds[j];
            vec3 p = P.ro + rd * t;
#ifdef CL_FAR_STUB
            float d = max(G[j].r - 0.05, 0.0); r.evals += 0x10000u;
#elif defined(CL_FAR_BAKE)
            /* (PS2_RQ_CLOUDS_BAKE_DETAIL: the detail octave on c within haze.w, as cl_density_tx) */
            float d = bk_dens_full(BD[j], BL[j], BX[j]); r.evals += 0x10000u;
#elif defined(CL_FAR_LG)
            bool det = t < fp.haze.w;
            float d = cj[j] ? (D[j].x >= 0.0 ? cl_lgd_erode(GL[j], rq_lgd_dens(GL[j]), D[j]) : rq_lgd_dens(GL[j]))
                            : cl_density_tx(p, W[j], B[j], D[j], det); r.evals += 0x10000u;
#else
            bool det = t < fp.haze.w;
            float d = cl_density_tx(p, W[j], B[j], D[j], det); r.evals += 0x10000u;
#endif
            if (d > 0.0) {
                zrun = 0;
                r.first = min(r.first, t);
                float fade = 1.0 - smoothstep(fp.haze.y, fp.haze.z, t);
#ifdef CL_FAR_BAKE
                /* (rq8_onelayer, PS2_RQ_CLOUDS_ONE, fp.bk3.w: the one layer from the eye, no near march to cross-fade with) */
                float sg = d * sig0 * fade * (fp.bk3.w > 0.5 ? 1.0 : rq_far_w(t));
#else
                float sg = d * sig0 * fade * rq_far_w(t);
#endif
#ifdef CL_FAR_STUB
                above = G[j].r; sun_t = G[j].g;   /* (the cached light: the same read) */
                vec3 c = cl_shade_s(p, rd, d, clamp(cl_height_frac(p), 0.0, 1.0), above, sun_t);
#elif defined(CL_FAR_BAKE)
                above = BL[j].r; sun_t = bk_cast(p, BL[j].g);   /* (the baked light and height: one read) */
                /* (PS2_RQ_CLOUDS_BAKE_ABOVE, fp.bk3.z: "above" from the density volume at the probe point -- its 78 m cells, not the light
                   grid's 156 m --, the scale of this sample's body) */
                if (fp.bk3.z > 0.5) above = cl_above_d(bk_density(cl_probe_pos(p)) * BL[j].a * BL[j].a);
                vec3 c = cl_shade_s(p, rd, d, BL[j].b, above, sun_t);
#ifdef CL_FAR_CHECK
                {   bool dt2 = t < fp.haze.w; int k = dt2 ? 0 : 1; float lj = ds[j] * fp.mz.y;
                    vec4 Wc = cl_far_weather(p);
                    float dp = cl_density_tx(p, Wc, cl_noise_base_ws(p, lj), dt2 ? cl_noise_detail_ws(p, lj) : vec4(0.0), dt2);
                    vec3 qp = cl_probe_pos(p);
                    float ap = cl_above_d(cl_density_tx(qp, cl_far_weather(qp), cl_noise_base_ws(qp, lj), vec4(0.0), false));
                    ck_db[k] += d; ck_dp[k] += dp; ck_n[k] += 1.0; ck_ab += abs(above - ap); ck_su += abs(sun_t - cl_cast_t(qp));
                }
#endif
#else
#ifdef CL_FAR_LG
                float hlj;
                if (cj[j]) { above = GL[j].r; sun_t = GL[j].g; hlj = GL[j].a; }   /* (the cached light and height: the same read) */
                else {
#endif
                if (nl++ % lstep == 0) {          /* the one probe: cloud above (+ the cast shadow from there on) */
                    vec3 qp = cl_probe_pos(p);
                    above = cl_above_d(cl_density_tx(qp, WQ[j], BQ[j], vec4(0.0), false));
                    sun_t = cl_cast_t(qp);
#ifndef CL_FAR_COUNT
                    r.evals += 0x10000u;
#endif
                }
#ifdef CL_FAR_LG
                hlj = cl_local_h(p, W[j]);
                }
                vec3 c = cl_shade_s(p, rd, d, hlj, above, sun_t);
#else
                vec3 c = cl_shade_s(p, rd, d, cl_local_h(p, W[j]), above, sun_t);
#endif
#endif
                float dto = fp.stp.w / max(sg, 1e-6);                      /* the step of stp.w optical depth */
                if (dto < dt) dt = dto;                                     /* at most stp.w optical depth per step */
                else if (fp.haze.x > 0.0) { /* faint cloud: longer steps, so a large faint cloud does not use up the step
                                               budget before the clouds behind it (up to the empty-space step) */
                    float dtl = clamp(t * P.k, P.dtmin, dtmax) * coarse;
                    /* (rq20_edges 2026-11-16, PS2_RQ_CLOUDS_THIN_MAX, BUGS_RQ R-270; fp.sch.w, a field the rq path never read: the PC
                       scheduler's) the thin step at most sch.w metres, never shorter than the fine step: from the eye the fine step is
                       t x 0.045, so beyond ~1.1 km the thin step grew to 3 fine steps -- 150-800 m at 1.1-6 km, over the bake's 78 m cells
                       and the fringe's wisps -- and each map texel's few long samples (at its own jitter) hit or missed the faint
                       fringe: the far clouds' "pattern", dark notches and a combed grain along the outline and over the body. Near the
                       eye (3 fine steps under sch.w) nothing changes */
                    /* (PS2_RQ_CLOUDS_THIN_MAX_T, fp.sch.x -- the PC scheduler's too: the cap only while the ray's transmittance is
                       over it; behind that much cloud the long steps do not show. 0 = always) */
                    if (fp.sch.w > 0.0 && r.T > fp.sch.x) dtl = min(dtl, max(ds[j], fp.sch.w));
                    dt = min(max(dt, fp.haze.x * dto), dtl);
                }
                dthin = fp.haze.x > 0.0 && dto >= ds[j] ? dt : 0.0;
                float tr = exp(-sg * dt);
                float w = r.T * (1.0 - tr);
                r.L += w * c; dsum += w * t; wsum += w;
                float tp = r.T;
                r.T *= tr;
                /* the opacity profile: where the transmittance crosses 0.8 / 0.5 / 0.2 inside this step (exactly, by the
                   step's exponential) */
                if (tp > 0.2 && r.T <= 0.8) {
                    const vec3 th = vec3(0.8, 0.5, 0.2);
                    [[unroll]] for (int q = 0; q < 3; q++)
                        if (tp > th[q] && r.T <= th[q]) r.prof[q + 1] = t + min(log(tp / th[q]) / max(sg, 1e-9), dt);
                }
                if (r.T < fp.stp.z) {           /* early out: the rest (below the limit) at this sample; the profile's
                                                   thresholds still above it end here */
                    [[unroll]] for (int q = 1; q < 4; q++) r.prof[q] = min(r.prof[q], t + dt);
                    r.L += r.T * c; dsum += r.T * t; wsum += r.T; r.T = 0.0; done = true;
                }
            } else {
                zrun++;
                tempty = t;
                dt = max(dt, dthin);    /* a hole in a faint cloud: keep its long step (the fine one crawls) */
            }
            if (!done) t += dt;
            i++;
        }
    }
    if (wsum > 1e-4 && r.T < 0.99) r.depth = dsum / wsum;
#ifdef CL_FAR_BAKE
    /* (rq8_onelayer, PS2_RQ_CLOUDS_ONE: the opacity of the ray's FRONT part, up to fp.sch.y (1500 m), in prof.z: the composite fades the front
       and the back by the scene's distance apart, so a wisp near the eye no longer spreads a far cloud's opacity over the terrain in front of
       it, and the haze around the aircraft stays in front of the terrain) */
    if (fp.bk3.w > 0.5) r.prof.z = tsp < 0.0 ? r.T : tsp;
#endif
    r.prof.x = r.first;
    return r;
}

#endif
