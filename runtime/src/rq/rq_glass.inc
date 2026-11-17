/* rq_glass.inc -- the readable canopy and the cracks in the Quest eye pass (R-57, R-58; the glass round of 2026-11-10, after
   the user's M9 flight: "there is no feeling of glass, and the feeling of glass is the main feature that must be there").
   Included by rq_cockpit.inc after the dry glass (PS2_RQ_SUPPORT only).

   What the PC's glass_light (cockpit.frag) gives the canopy and windscreen (the fx_glass meshes) is rebuilt here by a changed
   approach, not ported (its fragment stage over the canopy at eye size is 11-12 ms on the Adreno):
   - per VERTEX for this eye (rq_cockpit_gx.vert): the Fresnel rim x the outside's tone along the reflected ray
     (ACZ_VR_RQ_GLASS_FRESNEL), the interior's reflection read from the baked interior cube (gfx_refl_init) along the
     reflected ray with acrylic's Fresnel (ACZ_VR_RQ_GLASS_REFLECT), and the sun's forward-scatter and glare lobes;
   - the fragment stage of the triangles within 25 deg of the sun's image (list B, rq_cockpit_gxw.frag): ONE read of a
     tiling wear texture baked once (rqgx_wear_bake: hairlines, wiping strokes, polishing arcs, each texel with its line's
     direction), lit by glass_light's own rule (a line glints where its tangent is square to Q = S - D: arcs round the
     sun's image that slide with the head), and the glare's core from the interpolated eye ray (ACZ_VR_RQ_GLASS_GLARE);
   - the sunlit dust and water-spot rings at every angle as sprites placed once at load (rq_glass_dust.*, about 450 a
     square metre: what glass_light's gx_specks finds per pixel; ACZ_VR_RQ_GLASS_WEAR bit 1);
   - the cracks (rq_glass_crk.*, ACZ_VR_RQ_COCKPIT_CRACKS): the generator's segments as ribbons on the glass, the cores as
     small polar patches (the PC draws them only in its own cockpit pass, so the eye pass never showed a crack: F-4).
   Need: the CPU keeps a glass triangle only where one of its terms shows (the rim, the tint, the highlight, the interior's
   reflection read from a CPU copy of the cube, the near-sun list), with hysteresis (in at th, out below 0.6 th; the
   near-sun list in at 25 deg, out beyond 27 deg, where the GPU terms are already 0: no pops). With every switch 0 the
   dry glass is rq_cockpit_glass.* as before (the A/B). */
extern int ps2_crk_rq_export(float *seg, uint32_t segcap, uint32_t *nseg, float *core, uint32_t corecap, uint32_t *ncore);

#define RQGX_CN 32u                                   /* the CPU's interior cube for the need test: 32^2 a face */
#define RQGX_CORE_V (CRK_MAX_STARS * 216u * 3u)       /* core patch vertices (ps2_crk_rq_export: 216 triangles an impact) */
#define RQGX_SEG_B ((VkDeviceSize)CRK_MAX_SEGS * 64u)
#define RQGX_CORE_B ((VkDeviceSize)RQGX_CORE_V * 48u)
#define RQGX_IMAX 0.1f                                /* the wear texture's intensity unit (true width / texel width) */
#define RQGX_WN 1024u                                 /* the wear texture: 1024^2 RGBA8 over ACZ_VR_RQ_GLASS_TILE metres */

static struct {
    int init, tried, pok;
    float fresnel, reflect, glare, cracks, sig, density, tile;
    float cnight;                                     /* ACZ_VR_RQ_CRACK_NIGHT: the cockpit's night light on the cracks (D.2, R-86) */
    int wear;                                         /* ACZ_VR_RQ_GLASS_WEAR: bit 0 the dust sprites, bit 1 the near-sun scratches */
    int show;                                         /* [debug] ACZ_VR_RQ_GLASS_SHOW=1: the lists' coverage in colour */
    int rfs;                                          /* ACZ_VR_RQ_GLASS_REFLECT_FS=1: the interior read per fragment (rq_cockpit_gx*_rfs) */
    float rth;                                        /* ACZ_VR_RQ_GLASS_REFLECT_TH: the reflection's own need threshold (0: the glass's) */
    VkShaderModule vs, vsw, fs, fsw, dvs, dfs, cvs, cfs, vs_r, vsw_r, fs_r, fsw_r;
    VkDescriptorSetLayout dsl; VkPipelineLayout layout; VkDescriptorPool pool; VkDescriptorSet set[FRAMES_IN_FLIGHT];
    VkImageView set_cube[FRAMES_IN_FLIGHT]; VkBuffer set_sp[FRAMES_IN_FLIGHT]; int set_w[FRAMES_IN_FLIGHT];
    VkPipeline p_base, p_wear, p_dust, p_crk;
    VkImage wimg; VkDeviceMemory wmem; VkImageView wview; VkSampler wsmp, csmp; int wok, wtried;
    VkBuffer cb[FRAMES_IN_FLIGHT]; VkDeviceMemory cm[FRAMES_IN_FLIGHT]; void *cp[FRAMES_IN_FLIGHT];
    u32 cver[FRAMES_IN_FLIGHT], nseg[FRAMES_IN_FLIGHT], ncore[FRAMES_IN_FLIGHT]; int cok, ctried;
    float *cube; volatile u32 cube_gen; int cube_tried;   /* (lum, coverage) per texel of the coarse cube; a new bake bumps cube_gen */
    u32 set_gen[FRAMES_IN_FLIGHT];                    /* (the bake each slot's set holds: a view's handle may be reused by the next model's)*/
    u32 *tmp; u32 tcap;                               /* (rqgx_lists: the wet triangles' additive lists) */
    u32 fr_a, fr_b, fr_aw, fr_bw;                     /* this frame's lists (rqgx_lists) */
    int live;                                         /* this frame takes the new glass */
    u64 n_fr, n_a, n_b, n_w, n_sp, n_seg, n_crk_fr, ns_lists;
} rqgx;

static void rqgx_cfg(void) {
    if (rqgx.init) return;
    rqgx.init = 1;
    const char *e;
    /* (FRESNEL default 0: the sky's tone in the rim is brighter than the PC's ambient stand-in, and over the world it feeds back
       through the scene light -- its bands are read from the eye image where the depth is the world's, glass included --,
       which raises the ambient and with it the cockpit and the reflection: M3 F-5E across +6.8 levels on the whole image
       with it and the reflection on, none with either off) */
    e = getenv("ACZ_VR_RQ_GLASS_FRESNEL"); rqgx.fresnel = e && *e ? (float)atof(e) : 0.0f;
    /* (REFLECT default 0.6: the PC's glass at the same frames, M4 F-15C without clouds, reflection alone mean +3.84 / +2.08 levels
       toward the sun / across; this one at 1.0 +5.68 / +3.56: the PC subtracts the rim it replaces also where a clear glass texel's
       own colour is dropped (alpha 0), so its reflection reads weaker) */
    e = getenv("ACZ_VR_RQ_GLASS_REFLECT"); rqgx.reflect = e && *e ? (float)atof(e) : 0.6f;
    e = getenv("ACZ_VR_RQ_GLASS_WEAR"); rqgx.wear = e && *e ? atoi(e) : 3;
    e = getenv("ACZ_VR_RQ_GLASS_GLARE"); rqgx.glare = e && *e ? (float)atof(e) : 1.0f;
    e = getenv("ACZ_VR_RQ_COCKPIT_CRACKS"); rqgx.cracks = e && *e ? (float)atof(e) : 1.0f;
    /* (CRACK_NIGHT, D.2 of the second flight round, R-86: the crack faces are lit by the scene's ambient and the sun only (cockpit.frag's
       crk_glass, kept), so at night (M11 141 s) a hit left its rings -- the core has a light floor -- and no rays. Lit from within
       instead: a face that turns the eye's ray down into the cockpit mirrors the panel the night instrument lighting lights, and the
       crushed glass scatters it; this strength x the night level x the light's tint, 0 by day. 0: as before) */
    e = getenv("ACZ_VR_RQ_CRACK_NIGHT"); rqgx.cnight = e && *e ? (float)atof(e) : 0.5f;
    e = getenv("ACZ_VR_RQ_GLASS_SIG"); rqgx.sig = e && *e ? (float)atof(e) : 0.03f;
    e = getenv("ACZ_VR_RQ_GLASS_DUST_DENSITY"); rqgx.density = e && *e ? (float)atof(e) : 450.0f;
    e = getenv("ACZ_VR_RQ_GLASS_TILE"); rqgx.tile = e && *e ? (float)atof(e) : 0.3f;
    e = getenv("ACZ_VR_RQ_GLASS_SHOW"); rqgx.show = e && *e == '1';
    e = getenv("ACZ_VR_RQ_GLASS_REFLECT_FS"); rqgx.rfs = !(e && *e == '0');
    /* (REFLECT_TH default 0.006, 1.5 levels: the PC's reflection over the F-15C is a wash of 2-4 levels (across: 45 % of the image over
       2, 17 % over 4); with the glass's 0.012 and the strength 0.6 the need test dropped most of it, 6.8 / 1.5 % left) */
    e = getenv("ACZ_VR_RQ_GLASS_REFLECT_TH"); rqgx.rth = e && *e ? (float)atof(e) : 0.006f;   /* (0: ACZ_VR_RQ_COCKPIT_GLASS_TH) */
    if (!(rqgx.fresnel >= 0.0f)) rqgx.fresnel = 0.0f;
    if (rqgx.fresnel > 1.0f) rqgx.fresnel = 1.0f;
    if (!(rqgx.reflect >= 0.0f)) rqgx.reflect = 0.0f;
    if (rqgx.reflect > 4.0f) rqgx.reflect = 4.0f;
    if (!(rqgx.glare >= 0.0f)) rqgx.glare = 0.0f;
    if (rqgx.glare > 4.0f) rqgx.glare = 4.0f;
    if (!(rqgx.cracks >= 0.0f)) rqgx.cracks = 0.0f;
    if (rqgx.cracks > 2.0f) rqgx.cracks = 2.0f;
    if (!(rqgx.cnight >= 0.0f)) rqgx.cnight = 0.0f;
    if (rqgx.cnight > 2.0f) rqgx.cnight = 2.0f;
    if (!(rqgx.sig >= 0.005f)) rqgx.sig = 0.005f;
    if (rqgx.sig > 0.2f) rqgx.sig = 0.2f;
    if (!(rqgx.density >= 0.0f)) rqgx.density = 0.0f;
    if (rqgx.density > 4000.0f) rqgx.density = 4000.0f;
    if (!(rqgx.tile >= 0.05f)) rqgx.tile = 0.05f;
    if (rqgx.tile > 2.0f) rqgx.tile = 2.0f;
    rqgx.wear &= 3;
    ps2_log("cockpit material: the readable glass: Fresnel x sky %.2f, interior reflection %.2f, wear %d (1 dust, 2 scratches; sigma %.3f rad, "
            "%.0f specks a m2, tile %.2f m), glare %.2f; cracks in the eye pass %.2f, their night light %.2f (ACZ_VR_RQ_GLASS_*, ACZ_VR_RQ_COCKPIT_CRACKS, ACZ_VR_RQ_CRACK_NIGHT)",
            (double)rqgx.fresnel, (double)rqgx.reflect, rqgx.wear, (double)rqgx.sig, (double)rqgx.density, (double)rqgx.tile,
            (double)rqgx.glare, (double)rqgx.cracks, (double)rqgx.cnight);
}
static int rqgx_on(void) { rqgx_cfg(); return rqgx.fresnel > 0.0f || rqgx.reflect > 0.0f || rqgx.wear || rqgx.glare > 0.0f || rqgx.show; }

static void rqgx_load_shaders(const char *base) {
    rqgx_cfg();
    static const struct { VkShaderModule *m; const char *n; } S[] = {
        { &rqgx.vs, "rq_cockpit_gx.vert" }, { &rqgx.vsw, "rq_cockpit_gxw.vert" }, { &rqgx.fs, "rq_cockpit_gx.frag" }, { &rqgx.fsw, "rq_cockpit_gxw.frag" },
        { &rqgx.dvs, "rq_glass_dust.vert" }, { &rqgx.dfs, "rq_glass_dust.frag" }, { &rqgx.cvs, "rq_glass_crk.vert" }, { &rqgx.cfs, "rq_glass_crk.frag" },
        { &rqgx.vs_r, "rq_cockpit_gx_rfs.vert" }, { &rqgx.vsw_r, "rq_cockpit_gxw_rfs.vert" }, { &rqgx.fs_r, "rq_cockpit_gx_rfs.frag" }, { &rqgx.fsw_r, "rq_cockpit_gxw_rfs.frag" } };
    char spv[512];
    for (u32 i = 0; i < sizeof S / sizeof S[0]; i++) {
        snprintf(spv, sizeof spv, "%s/%s.spv", base, S[i].n);
        *S[i].m = load_spv(spv);
        if (!*S[i].m) ps2_log("vk: the readable glass: %s missing", spv);
    }
}

/* ---- the wear texture (once): hairlines, wiping strokes and polishing arcs as glass_light's layers, each texel with its
   line's direction as the doubled angle (r = 0.5 + 0.5 I cos 2phi, g = 0.5 + 0.5 I sin 2phi, b = I in RQGX_IMAX): its mips
   average directions as they should, and their length against I says how coherent a texel's directions are ---- */
static float rqgx_rnd(u32 *s) { u32 x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x ? x : 0x9E3779B9u; return (float)(*s >> 8) * (1.0f / 16777216.0f); }
static void rqgx_put(float *acc, float x, float y, float I, float ph) {
    float tex = rqgx.tile / (float)RQGX_WN;
    int ix = (int)floorf(x / tex), iy = (int)floorf(y / tex);
    ix %= (int)RQGX_WN; iy %= (int)RQGX_WN; if (ix < 0) ix += (int)RQGX_WN; if (iy < 0) iy += (int)RQGX_WN;
    float *a = acc + ((size_t)iy * RQGX_WN + (u32)ix) * 3u;
    if (I > a[2]) { a[0] = I * cosf(2.0f * ph); a[1] = I * sinf(2.0f * ph); a[2] = I; }
}
static void rqgx_line(float *acc, float cx, float cy, float phi, float L, float w, float kb) {
    float tex = rqgx.tile / (float)RQGX_WN, tx = cosf(phi), ty = sinf(phi), nx = -ty, ny = tx;
    float I = w / tex / RQGX_IMAX; if (I > 1.0f) I = 1.0f;
    int steps = (int)(2.0f * L / (0.5f * tex)) + 1;
    for (int k = 0; k <= steps; k++) {
        float al = -L + 2.0f * L * (float)k / (float)steps, ac = kb * al * al;
        float ends = 1.0f - rqw_ss(0.5f * L, L, fabsf(al));
        if (ends <= 0.0f) continue;
        rqgx_put(acc, cx + tx * al + nx * ac, cy + ty * al + ny * ac, I * ends, phi + atanf(2.0f * kb * al));
    }
}
static void rqgx_wear_bake(vk_frame *st) {
    if (rqgx.wtried) return;
    rqgx.wtried = 1;
    u64 t0 = vk_mono_ns();
    const u32 N = RQGX_WN;
    float T = rqgx.tile;
    float *acc = (float *)calloc((size_t)N * N * 3u, sizeof(float));
    u8 *px = (u8 *)malloc((size_t)N * N * 4u);
    if (!acc || !px) { free(acc); free(px); return; }
    u32 rs = 0x6A09E667u, nl = 0;
    /* glass_light's gx_hairs layers: cell, probability, lines a cell (all directions), true width, bend (1/m), shortest */
    static const float H[4][7] = { { 0.022f, 0.9f, 20.0f, 4e-6f, 1e-5f, 4.0f, 0.3f }, { 0.027f, 0.85f, 16.0f, 4e-6f, 1e-5f, 4.0f, 0.3f },
                                   { 0.045f, 0.8f, 12.0f, 8e-6f, 2e-5f, 3.0f, 0.4f }, { 0.15f, 0.6f, 6.0f, 1e-5f, 2.5e-5f, 5.0f, 0.6f } };
    for (int l = 0; l < 4; l++) {
        float s = H[l][0], rho = 0.35f * s;
        int nc = (int)ceilf(T / s);
        for (int cy = 0; cy < nc; cy++) for (int cx = 0; cx < nc; cx++) {
            if (rqgx_rnd(&rs) >= H[l][1]) continue;
            float ox = ((float)cx + 0.5f + (rqgx_rnd(&rs) - 0.5f) * 0.3f) * T / (float)nc, oy = ((float)cy + 0.5f + (rqgx_rnd(&rs) - 0.5f) * 0.3f) * T / (float)nc;
            float ph0 = rqgx_rnd(&rs) * 3.14159265f;
            int nor = (int)H[l][2];
            for (int k = 0; k < nor; k++) {
                float ph = ph0 + (float)k * 3.14159265f / (float)nor;
                float al = (rqgx_rnd(&rs) - 0.5f) * 0.7f * rho, ac = (rqgx_rnd(&rs) - 0.5f) * 1.3f * rho;
                float L = rho * (H[l][6] + (1.0f - H[l][6]) * rqgx_rnd(&rs));
                float w = H[l][3] + (H[l][4] - H[l][3]) * rqgx_rnd(&rs);
                float kb = (rqgx_rnd(&rs) - 0.5f) * H[l][5];
                rqgx_line(acc, ox + cosf(ph) * al - sinf(ph) * ac, oy + sinf(ph) * al + cosf(ph) * ac, ph, L, w, kb);
                nl++;
            }
        }
    }
    /* gx_swirl: polishing arcs (cell 0.11 m, 30 %): rings 0.3-0.7 mm apart are finer than a texel, so their mean (0.55 w / dr,
       x 0.6) with the ring's tangent as the direction; sectors present or not as the PC's */
    {   float s = 0.11f, rho = 0.35f * s, tex = T / (float)N;
        int nc = (int)ceilf(T / s);
        for (int cy = 0; cy < nc; cy++) for (int cx = 0; cx < nc; cx++) {
            if (rqgx_rnd(&rs) >= 0.3f) continue;
            float ox = ((float)cx + 0.5f + (rqgx_rnd(&rs) - 0.5f) * 0.3f) * T / (float)nc, oy = ((float)cy + 0.5f + (rqgx_rnd(&rs) - 0.5f) * 0.3f) * T / (float)nc;
            float dr = 3e-4f + 4e-4f * rqgx_rnd(&rs), wr = 4e-6f + 6e-6f * rqgx_rnd(&rs), a0 = rqgx_rnd(&rs) * 6.2831853f;
            float Im = 0.6f * 0.55f * wr / dr / RQGX_IMAX;
            u32 sec = 0; for (int b = 0; b < 8; b++) if (rqgx_rnd(&rs) >= 0.45f) sec |= 1u << b;
            int r0 = (int)floorf((ox - rho) / tex), r1 = (int)ceilf((ox + rho) / tex), q0 = (int)floorf((oy - rho) / tex), q1 = (int)ceilf((oy + rho) / tex);
            for (int y = q0; y <= q1; y++) for (int x = r0; x <= r1; x++) {
                float dx = ((float)x + 0.5f) * tex - ox, dy = ((float)y + 0.5f) * tex - oy, rr = sqrtf(dx * dx + dy * dy);
                if (rr >= rho || rr < 2e-4f) continue;
                float ang = atan2f(dy, dx);
                int b = (int)floorf((ang - a0) * 1.2732395f); b = ((b % 8) + 8) % 8;
                if (!((sec >> b) & 1u)) continue;
                float ends = (1.0f - rqw_ss(0.5f * rho, rho, rr)) * rqw_ss(0.0f, 0.15f * rho, rr);
                rqgx_put(acc, ((float)x + 0.5f) * tex, ((float)y + 0.5f) * tex, Im * ends, ang + 1.5707963f);
            }
        }
    }
    u32 lit = 0;
    for (size_t i = 0; i < (size_t)N * N; i++) {
        const float *a = acc + i * 3u;
        px[i * 4u] = rqw_u8(0.5f + 0.5f * a[0]); px[i * 4u + 1u] = rqw_u8(0.5f + 0.5f * a[1]); px[i * 4u + 2u] = rqw_u8(a[2]); px[i * 4u + 3u] = 255u;
        lit += a[2] > 0.004f;
    }
    int ok = rqw_image_to(px, N, N, st, &rqgx.wimg, &rqgx.wmem, &rqgx.wview);
    free(acc); free(px);
    if (!ok) { ps2_log("cockpit material: the readable glass's wear texture could not be made"); return; }
    VkSamplerCreateInfo ms; memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO; ms.magFilter = VK_FILTER_LINEAR; ms.minFilter = VK_FILTER_LINEAR; ms.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    ms.addressModeU = ms.addressModeV = ms.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT; ms.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(dev, &ms, NULL, &rqgx.wsmp) != VK_SUCCESS) return;
    ms.addressModeU = ms.addressModeV = ms.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; ms.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; ms.maxLod = 0.0f;
    if (vkCreateSampler(dev, &ms, NULL, &rqgx.csmp) != VK_SUCCESS) return;
    rqgx.wok = 1;
    ps2_log("cockpit material: the readable glass's wear texture baked in %.0f ms: %u lines and the polishing arcs over %.2f m, %.1f %% of the texels lit",
            (double)(vk_mono_ns() - t0) * 1e-6, nl, (double)T, 100.0 * (double)lit / (double)((size_t)N * N));
}

/* ---- the dust and the water-spot rings: placed once per model on the fx_glass meshes (about rqgx.density a square metre:
   glass_light's speck cells, 0.022 m at 20 %, as the glass crosses them), kept in a storage buffer ---- */
typedef struct { float p[4], n[4], k[4]; } rqgx_speck;
static void rqgx_specks_build(rqc_model *q, const cpit_model *m) {
    if (!(rqgx.wear & 1) || rqgx.density <= 0.0f || q->gxsb) return;
    u32 cap = 0, n = 0;
    double area = 0.0;
    for (int pass = 0; pass < 2; pass++) {
        rqgx_speck *sp = NULL;
        if (pass) {
            cap = n + 16u; n = 0;
            if (make_buffer((VkDeviceSize)cap * sizeof(rqgx_speck), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            &q->gxsb, &q->gxsm, &q->gxsp) != 0) { q->gxsb = VK_NULL_HANDLE; return; }
            sp = (rqgx_speck *)q->gxsp;
        }
        for (u32 g = 0; g < q->ng; g++) {
            const rqc_gmesh *gm = &q->gm[g];
            const cpit_mesh *me = &m->mesh[gm->mesh];
            if (!(me->flags & CPIT_F_GLASSFX)) continue;
            const u32 *ix = q->gix + gm->ifirst;
            for (u32 t = 0; t < gm->icount / 3u; t++, ix += 3) {
                const float *a = q->gv + 6u * (ix[0] - q->gvx0), *b = q->gv + 6u * (ix[1] - q->gvx0), *c = q->gv + 6u * (ix[2] - q->gvx0);
                float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
                float cr[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
                float cl = sqrtf(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]), A = 0.5f * cl;
                if (cl < 1e-12f) continue;
                if (!pass) area += A;
                u32 rs = (gm->mesh + 1u) * 0x9E3779B9u ^ (t + 1u) * 0x85EBCA6Bu;
                float want = A * rqgx.density;
                int k = (int)want; if (rqgx_rnd(&rs) < want - (float)k) k++;
                for (int j = 0; j < k; j++) {
                    float u = rqgx_rnd(&rs), v = rqgx_rnd(&rs);
                    if (u + v > 1.0f) { u = 1.0f - u; v = 1.0f - v; }
                    float kind = rqgx_rnd(&rs), z = rqgx_rnd(&rs), z2 = rqgx_rnd(&rs);
                    if (!pass) { n++; continue; }
                    if (n >= cap) break;
                    rqgx_speck *s = &sp[n++];
                    for (int d = 0; d < 3; d++) { s->p[d] = a[d] + u * e1[d] + v * e2[d]; s->n[d] = cr[d] / cl; }
                    s->p[3] = (float)gm->mesh; s->n[3] = 0.0f;
                    if (kind < 0.9f) { s->k[0] = 5e-5f + 2.5e-4f * z * z; s->k[1] = 0.0f; }
                    else { s->k[0] = 7e-4f + 1.3e-3f * z; s->k[1] = 4e-5f + 6e-5f * z2; }
                    s->k[2] = kind; s->k[3] = 0.0f;
                }
            }
        }
    }
    q->ngxs = n;
    ps2_log("cockpit material: the readable glass: %u dust specks and water-spot rings on %.2f m2 of canopy and windscreen", n, area);
}

/* ---- the interior cube on the CPU (once per bake): what the glass reflects, for the need test ---- */
typedef struct { VkImage img; VkBuffer buf; u32 n; } rqgx_rb;
static void rqgx_rb_rec(VkCommandBuffer cmd, void *a) {
    rqgx_rb *r = (rqgx_rb *)a;
    VkImageMemoryBarrier2 b; VkDependencyInfo di;
    barrier_fill(&b, r->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 6;
    memset(&di, 0, sizeof di); di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO; di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    VkBufferImageCopy rg; memset(&rg, 0, sizeof rg);
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rg.imageSubresource.layerCount = 6;
    rg.imageExtent.width = rg.imageExtent.height = r->n; rg.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd, r->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r->buf, 1, &rg);
    barrier_fill(&b, r->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 6;
    vkCmdPipelineBarrier2(cmd, &di);
}
/* the need test's reflection bound per glass triangle (once per bake): the brightest texel of the coarse cube along the
   reflected rays of the corners and the centre, seen from the design eye and 8 cm aside in each direction (the head's
   usual range), as the shaders read it (parallax sphere 0.7 m) */
static void rqgx_cube_at(const float *d, float *out);
static void rqgx_rl_make(float *cube, rqc_model *q) {
    if (!q || !q->gxtc || !q->ngtri) return;
    u8 *rl = (u8 *)calloc(q->ngtri, 1);
    if (!rl) return;
    static const float eo[5][3] = { { 0, 0, 0 }, { 0.08f, 0, 0 }, { -0.08f, 0, 0 }, { 0, 0.08f, 0 }, { 0, -0.08f, 0 } };
    float R0 = 0.7f, *keep = rqgx.cube;
    rqgx.cube = cube;
    u32 nz = 0;
    for (u32 t = 0; t < q->ngtri; t++) {
        const float *tc = q->gxtc + (size_t)t * 6u, *nn0 = tc + 3;
        float lmax = 0.0f;
        for (int j = 0; j < 4; j++) {
            const float *pp = j < 3 ? q->gv + 6u * (q->gix[t * 3u + (u32)j] - q->gvx0) : tc;
            for (int e = 0; e < 5; e++) {
                float D[3] = { pp[0] - eo[e][0], pp[1] - eo[e][1], pp[2] - eo[e][2] };
                float dl = sqrtf(D[0] * D[0] + D[1] * D[1] + D[2] * D[2]);
                if (dl < 1e-5f) continue;
                D[0] /= dl; D[1] /= dl; D[2] /= dl;
                float nn[3] = { nn0[0], nn0[1], nn0[2] }, dn = D[0] * nn[0] + D[1] * nn[1] + D[2] * nn[2];
                if (dn > 0.0f) { nn[0] = -nn[0]; nn[1] = -nn[1]; nn[2] = -nn[2]; dn = -dn; }     /* (n faces the eye) */
                float Rr[3] = { D[0] - 2.0f * dn * nn[0], D[1] - 2.0f * dn * nn[1], D[2] - 2.0f * dn * nn[2] };
                float bq = pp[0] * Rr[0] + pp[1] * Rr[1] + pp[2] * Rr[2], cq = pp[0] * pp[0] + pp[1] * pp[1] + pp[2] * pp[2] - R0 * R0, disc = bq * bq - cq;
                float tt = disc > 0.0f ? (cq > 0.0f ? -bq - sqrtf(disc) : -bq + sqrtf(disc)) : -1.0f;
                float dir[3] = { tt > 0.0f ? pp[0] + tt * Rr[0] : Rr[0], tt > 0.0f ? pp[1] + tt * Rr[1] : Rr[1], tt > 0.0f ? pp[2] + tt * Rr[2] : Rr[2] }, ic[2];
                rqgx_cube_at(dir, ic);
                if (ic[0] > lmax) lmax = ic[0];
            }
        }
        int v = (int)(lmax * 255.0f + 0.999f);
        rl[t] = (u8)(v > 255 ? 255 : v); nz += rl[t] > 0;
    }
    rqgx.cube = keep;
    u8 *old = q->gxrl;
    __atomic_store_n(&q->gxrl, rl, __ATOMIC_RELEASE);
    (void)old;                                         /* (a model's first bake only: nothing to free while the renderer may read it) */
    ps2_log("cockpit material: the readable glass: the reflection's bound made for %u glass triangles (%u can show some interior)", q->ngtri, nz);
}
/* (on the cockpit loader's thread right after gfx_refl_init's bake: ps2_vk.h PS2_RQ_REFL_HOOK; a submission from the renderer's
   frame would wait for the GPU lock the frame holds) */
void ps2_rq_glass_cube_read(void *model) {
    cpit_model *lm = (cpit_model *)model;          /* (the model being loaded: `cpit` here is the active one) */
    if (!lm || !rqc_cfg() || !rqgx_on() || rqgx.reflect <= 0.0f) return;
    if (!lm->rf_ok || !lm->rf_img) return;
    u32 N = (u32)gfx.rf_n, C = RQGX_CN;
    if (N < C || N % C) { ps2_log("cockpit material: the readable glass: interior cube %u^2 not a multiple of %u: the reflection drawn by the rim test only", N, C); rqgx.cube = NULL; return; }
    VkBuffer buf; VkDeviceMemory mem; void *p = NULL;
    if (make_buffer((VkDeviceSize)N * N * 24u, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &buf, &mem, &p) != 0) return;
    rqgx_rb r = { lm->rf_img, buf, N };
    if (cpit_submit_once(rqgx_rb_rec, &r) == 0) {
        static float cb[2][6u * RQGX_CN * RQGX_CN * 2u]; static int cbi;   /* (filled aside, then published: the renderer may read the other) */
        float *c = cb[cbi ^= 1];
        {
            const u8 *s = (const u8 *)p;
            u32 k = N / C;
            double lsum = 0.0, asum = 0.0;
            for (u32 f = 0; f < 6u; f++) for (u32 y = 0; y < C; y++) for (u32 x = 0; x < C; x++) {
                float l = 0.0f, a = 0.0f;                 /* (the brightest texel of the block: a thin bright edge keeps its bound) */
                for (u32 yy = 0; yy < k; yy++) for (u32 xx = 0; xx < k; xx++) {
                    const u8 *t = s + (((size_t)f * N + y * k + yy) * N + x * k + xx) * 4u;
                    float lt = (0.299f * (float)t[0] + 0.587f * (float)t[1] + 0.114f * (float)t[2]) * (1.0f / 255.0f);
                    if (lt > l) l = lt;
                    a += (float)t[3] * (1.0f / 255.0f);
                }
                float *o = c + (((size_t)f * C + y) * C + x) * 2u;
                o[0] = l; o[1] = a / (float)(k * k);
                lsum += o[0]; asum += o[1];
            }
            __atomic_store_n(&rqgx.cube, c, __ATOMIC_RELEASE);
            __atomic_add_fetch(&rqgx.cube_gen, 1u, __ATOMIC_RELEASE);
            rqgx_rl_make(c, lm->rqc);
            ps2_log("cockpit material: the readable glass: the interior cube read back for the need test (%u^2 -> %u^2, the blocks' brightest; mean of that %.3f, coverage %.2f)",
                    N, C, lsum / (6.0 * C * C), asum / (6.0 * C * C));
        }
    }
    vkDestroyBuffer(dev, buf, NULL); vkFreeMemory(dev, mem, NULL);
}
/* a cockpit-frame direction into the coarse cube (Vulkan's face rule): out[0] luminance, out[1] coverage */
static void rqgx_cube_at(const float *d, float *out) {
    float ax = fabsf(d[0]), ay = fabsf(d[1]), az = fabsf(d[2]), ma, sc, tc; u32 f;
    if (ax >= ay && ax >= az) { ma = ax; f = d[0] > 0.0f ? 0u : 1u; sc = d[0] > 0.0f ? -d[2] : d[2]; tc = -d[1]; }
    else if (ay >= az) { ma = ay; f = d[1] > 0.0f ? 2u : 3u; sc = d[0]; tc = d[1] > 0.0f ? d[2] : -d[2]; }
    else { ma = az; f = d[2] > 0.0f ? 4u : 5u; sc = d[2] > 0.0f ? d[0] : -d[0]; tc = -d[1]; }
    if (ma < 1e-6f) { out[0] = out[1] = 0.0f; return; }
    int x = (int)((sc / ma * 0.5f + 0.5f) * (float)RQGX_CN), y = (int)((tc / ma * 0.5f + 0.5f) * (float)RQGX_CN);
    x = x < 0 ? 0 : x >= (int)RQGX_CN ? (int)RQGX_CN - 1 : x; y = y < 0 ? 0 : y >= (int)RQGX_CN ? (int)RQGX_CN - 1 : y;
    const float *c = rqgx.cube + (((size_t)f * RQGX_CN + (u32)y) * RQGX_CN + (u32)x) * 2u;
    out[0] = c[0]; out[1] = c[1];
}

/* ---- pipelines (with the eye pass's, rqc_pipes) ---- */
static void rqgx_pipes(void) {
    if (rqgx.tried) return;
    rqgx.tried = 1;
    if (!rqgx.vs || !rqgx.vsw || !rqgx.fs || !rqgx.fsw || !rqc.texl || !rqc.fl) return;
    VkDescriptorSetLayoutBinding b[5]; memset(b, 0, sizeof b);
    VkDescriptorBindingFlags bf[5];
    for (u32 k = 0; k < 5u; k++) { b[k].binding = k; b[k].descriptorCount = 1; bf[k] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT; }
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;   /* the interior cube */
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;    /* the wear */
    for (u32 k = 2; k < 5u; k++) { b[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[k].stageFlags = VK_SHADER_STAGE_VERTEX_BIT; }  /* specks, segments, cores */
    VkDescriptorSetLayoutBindingFlagsCreateInfo bi; memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO; bi.bindingCount = 5; bi.pBindingFlags = bf;
    VkDescriptorSetLayoutCreateInfo lc; memset(&lc, 0, sizeof lc);
    lc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; lc.pNext = &bi; lc.bindingCount = 5; lc.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev, &lc, NULL, &rqgx.dsl) != VK_SUCCESS) { rqgx.dsl = VK_NULL_HANDLE; return; }
    VkDescriptorSetLayout sl[4] = { rqc.texl, deslayout, rqc.fl, rqgx.dsl };
    VkPushConstantRange pr; memset(&pr, 0, sizeof pr); pr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT; pr.size = 16;
    VkPipelineLayoutCreateInfo pl; memset(&pl, 0, sizeof pl);
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pl.setLayoutCount = 4; pl.pSetLayouts = sl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev, &pl, NULL, &rqgx.layout) != VK_SUCCESS) { rqgx.layout = VK_NULL_HANDLE; return; }
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2u * FRAMES_IN_FLIGHT }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3u * FRAMES_IN_FLIGHT } };
    VkDescriptorPoolCreateInfo pc; memset(&pc, 0, sizeof pc); pc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; pc.maxSets = FRAMES_IN_FLIGHT; pc.poolSizeCount = 2; pc.pPoolSizes = ps;
    if (vkCreateDescriptorPool(dev, &pc, NULL, &rqgx.pool) != VK_SUCCESS) { rqgx.pool = VK_NULL_HANDLE; return; }
    for (u32 k = 0; k < FRAMES_IN_FLIGHT; k++) {
        VkDescriptorSetAllocateInfo ai; memset(&ai, 0, sizeof ai); ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; ai.descriptorPool = rqgx.pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &rqgx.dsl;
        if (vkAllocateDescriptorSets(dev, &ai, &rqgx.set[k]) != VK_SUCCESS) return;
    }
    /* the states: as the dry glass (premultiplied, no depth write, RGB only, GEQUAL against the cockpit's depth) */
    VkPipelineColorBlendAttachmentState gb; memset(&gb, 0, sizeof gb);
    gb.blendEnable = VK_TRUE; gb.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; gb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; gb.colorBlendOp = VK_BLEND_OP_ADD;
    gb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; gb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; gb.alphaBlendOp = VK_BLEND_OP_ADD;
    gb.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    VkPipelineColorBlendStateCreateInfo cb; memset(&cb, 0, sizeof cb); cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cb.attachmentCount = 1; cb.pAttachments = &gb;
    VkPipelineDepthStencilStateCreateInfo dss; memset(&dss, 0, sizeof dss); dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    dss.depthTestEnable = VK_TRUE; dss.depthWriteEnable = VK_FALSE; dss.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; dss.maxDepthBounds = 1.0f;
    VkPipelineRasterizationStateCreateInfo rs; memset(&rs, 0, sizeof rs); rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkVertexInputBindingDescription bd[2]; VkVertexInputAttributeDescription at[5];
    VkPipelineVertexInputStateCreateInfo vin = rqc_vin(&bd[0], at);
    memset(&bd[1], 0, sizeof bd[1]); bd[1].binding = 1; bd[1].stride = 20u;
    memset(&at[3], 0, 2u * sizeof at[3]); at[3].location = 3; at[3].binding = 1; at[3].format = VK_FORMAT_R32G32_SFLOAT;
    at[4].location = 4; at[4].binding = 1; at[4].format = VK_FORMAT_R32G32B32_SFLOAT; at[4].offset = 8u;
    vin.vertexBindingDescriptionCount = 2; vin.pVertexBindingDescriptions = bd; vin.vertexAttributeDescriptionCount = 5; vin.pVertexAttributeDescriptions = at;
    VkPipelineVertexInputStateCreateInfo vnone; memset(&vnone, 0, sizeof vnone); vnone.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineShaderStageCreateInfo st[2]; memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].pName = "main"; st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].pName = "main";
    VkPipelineLayout keep = rqc.layout;
    rqc.layout = rqgx.layout;
    int rf = rqgx.rfs && rqgx.vs_r && rqgx.vsw_r && rqgx.fs_r && rqgx.fsw_r;
    st[0].module = rf ? rqgx.vs_r : rqgx.vs; st[1].module = rf ? rqgx.fs_r : rqgx.fs; rqgx.p_base = rqc_mk(st, 2, &vin, &rs, &dss, &cb);
    st[0].module = rf ? rqgx.vsw_r : rqgx.vsw; st[1].module = rf ? rqgx.fsw_r : rqgx.fsw; rqgx.p_wear = rqc_mk(st, 2, &vin, &rs, &dss, &cb);
    if (rqgx.rfs) ps2_log("cockpit material: the readable glass reads the interior per fragment (ACZ_VR_RQ_GLASS_REFLECT_FS)%s", rf ? "" : ": its stages missing, per vertex");
    if (rqgx.dvs && rqgx.dfs) { st[0].module = rqgx.dvs; st[1].module = rqgx.dfs; rqgx.p_dust = rqc_mk(st, 2, &vnone, &rs, &dss, &cb); }
    if (rqgx.cvs && rqgx.cfs) { st[0].module = rqgx.cvs; st[1].module = rqgx.cfs; rqgx.p_crk = rqc_mk(st, 2, &vnone, &rs, &dss, &cb); }
    rqc.layout = keep;
    for (u32 k = 0; k < FRAMES_IN_FLIGHT; k++)
        if (make_buffer(RQGX_SEG_B + RQGX_CORE_B, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &rqgx.cb[k], &rqgx.cm[k], &rqgx.cp[k]) != 0) { rqgx.cb[k] = VK_NULL_HANDLE; }
    rqgx.cok = rqgx.cb[0] != VK_NULL_HANDLE;
    for (u32 k = 0; k < FRAMES_IN_FLIGHT; k++) {           /* (the crack buffers never change: written once) */
        if (!rqgx.cb[k]) { rqgx.cok = 0; continue; }
        VkDescriptorBufferInfo di[2] = { { rqgx.cb[k], 0, RQGX_SEG_B }, { rqgx.cb[k], RQGX_SEG_B, RQGX_CORE_B } };
        VkWriteDescriptorSet w[2]; memset(w, 0, sizeof w);
        for (u32 j = 0; j < 2u; j++) { w[j].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[j].dstSet = rqgx.set[k]; w[j].dstBinding = 3u + j; w[j].descriptorCount = 1; w[j].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[j].pBufferInfo = &di[j]; }
        vkUpdateDescriptorSets(dev, 2, w, 0, NULL);
    }
    rqgx.pok = rqgx.p_base && rqgx.p_wear;
    ps2_log("vk: the readable glass's pipelines: base %d, near sun %d, dust %d, cracks %d", rqgx.p_base != VK_NULL_HANDLE, rqgx.p_wear != VK_NULL_HANDLE,
            rqgx.p_dust != VK_NULL_HANDLE, rqgx.p_crk != VK_NULL_HANDLE);
}

/* ---- each frame (rqc_frame_begin, F filled): glass_light's inputs (gfx_frame, as cpit_draw: the sun from C1, its colour x
   day x the cloud's shadow; also the cracks' sun), the slot's set, the cracks' buffer ---- */
static void rqgx_frame(rqc_frame *F, vk_frame *fr, int lit) {
    rqgx.live = 0;
    memset(F->gx, 0, sizeof F->gx);
    int slot = (int)(fr - frames);
    if (!rqgx.pok || slot < 0 || slot >= (int)FRAMES_IN_FLIGHT) return;
    if (lit && !cine_t) {
        gfx_frame(fr, 0, cloud_sun());
        const ps2_vk_mesh_block *k = (const ps2_vk_mesh_block *)fr->mcptr + MESH_CPIT_BLOCK;
        ps2_vk_mesh_block kh;                         /* (PS2_RQ_MAP_HOST: e[] and p[] as gfx_frame left them, read once from the mapped arena) */
        if (rqc.fh_on[slot]) { memcpy(kh.e, k->e, sizeof kh.e); memcpy(kh.p, k->p, sizeof kh.p); k = &kh; }
        F->gx[0] = k->e[0]; F->gx[1] = k->e[1]; F->gx[2] = k->e[2];
        F->gx[3] = (rqgx.wear & 2) ? k->e[3] : 0.0f;
        F->gx[4] = k->e[4]; F->gx[5] = k->e[5]; F->gx[6] = k->e[6];
        F->gx[7] = k->e[7] * rqgx.glare * rqsun_k();   /* (rq10_artefacts round 2, PS2_RQ_CLOUDS_SUN_T: the glare under a cloud that hides the sun) */
        F->gx[8] = cpit.rf_ok ? k->e[8] * rqgx.reflect : 0.0f;
        F->gx[9] = rqgx.fresnel;
        F->gx[10] = (rqgx.wear & 1) ? k->p[13] : 0.0f;
        F->gx[11] = k->e[11] > 0.0f ? k->e[11] : 0.7f;
        F->gx[12] = k->p[12];
    } else { F->gx[9] = rqgx.fresnel; F->gx[11] = 0.7f; }
    {   /* water on the glass fills the scratches and covers the dust (glass_light's 1 - 0.85 wash; here the canopy's water as a whole) */
        float wt = cx_now() ? cx_b + cx_r : canopy_v[0];
        wt = wt < 0.0f ? 0.0f : wt > 1.0f ? 1.0f : wt;
        F->gx[3] *= 1.0f - 0.85f * wt; F->gx[10] *= 1.0f - 0.85f * wt;
    }
    F->gx[13] = 1.0f / (rqgx.sig * rqgx.sig);
    F->gx[14] = 1.0f / rqgx.tile;
    F->gx[16] = slot >= 0 && crk.ptr[slot] ? ((const float *)crk.ptr[slot])[0] : 0.0f;    /* (the cracks' clock: crk_frame's header this frame) */
    F->gx[18] = rqgx.cracks;
    F->gx[21] = rqgx.cnight;                                                               /* (ACZ_VR_RQ_CRACK_NIGHT: x F->night, rq_glass_crk.vert) */
    F->gx[20] = rqgx.show ? 1.0f : 0.0f;
    F->gx[19] = 0.5f * F->proj[0] * F->misc[0] * F->xfx[0] * F->eyesize[0];               /* (the eye's focal length in its pixels) */
    if (!(F->gx[19] > 1.0f)) F->gx[19] = 1.0f;
    /* the slot's set: the interior cube, the wear, this model's specks (written only when they change: the slot's last use is done) */
    rqc_model *q = cpit.rqc;
    VkImageView cv = cpit.rf_ok ? cpit.rf_cube : VK_NULL_HANDLE;
    VkBuffer sb = q ? q->gxsb : VK_NULL_HANDLE;
    VkWriteDescriptorSet w[3]; VkDescriptorImageInfo ii[2]; VkDescriptorBufferInfo bi; u32 nw = 0;
    memset(w, 0, sizeof w); memset(ii, 0, sizeof ii); memset(&bi, 0, sizeof bi);
    if (cv && (rqgx.set_cube[slot] != cv || rqgx.set_gen[slot] != rqgx.cube_gen) && rqgx.csmp) {
        ii[0].sampler = rqgx.csmp; ii[0].imageView = cv; ii[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[nw].dstBinding = 0; w[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[nw].pImageInfo = &ii[0]; nw++;
        rqgx.set_cube[slot] = cv; rqgx.set_gen[slot] = rqgx.cube_gen;
    }
    if (rqgx.wok && !rqgx.set_w[slot]) {
        ii[1].sampler = rqgx.wsmp; ii[1].imageView = rqgx.wview; ii[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[nw].dstBinding = 1; w[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[nw].pImageInfo = &ii[1]; nw++;
        rqgx.set_w[slot] = 1;
    }
    if (sb && rqgx.set_sp[slot] != sb) {
        bi.buffer = sb; bi.range = VK_WHOLE_SIZE;
        w[nw].dstBinding = 2; w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[nw].pBufferInfo = &bi; nw++;
        rqgx.set_sp[slot] = sb;
    }
    for (u32 j = 0; j < nw; j++) { w[j].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[j].dstSet = rqgx.set[slot]; w[j].descriptorCount = 1; }
    if (nw) vkUpdateDescriptorSets(dev, nw, w, 0, NULL);
    if (!cv) F->gx[8] = 0.0f;
    if (!rqgx.wok) { F->gx[3] = 0.0f; }
    /* the cracks: this slot's copy when the generator's version moved */
    if (rqgx.cok && rqgx.cracks > 0.0f && rqgx.p_crk && crk.n > 0 && cpit.crk) {
        u32 ver = ps2_crk_gpu_version();
        if (rqgx.cver[slot] != ver) {
            u32 ns = 0, nc = 0;
            if (ps2_crk_rq_export((float *)rqgx.cp[slot], CRK_MAX_SEGS, &ns, (float *)((u8 *)rqgx.cp[slot] + RQGX_SEG_B), RQGX_CORE_V, &nc)) {
                rqgx.cver[slot] = ver; rqgx.nseg[slot] = ns; rqgx.ncore[slot] = nc;
                static u32 nlog;
                if (nlog < 24u) { nlog++; ps2_log("cockpit material: cracks in the eye pass: %u segments, %u core vertices (version %u, slot %d)", ns, nc, ver, slot); }
            }
        }
    } else if (crk.n <= 0) { rqgx.nseg[slot] = rqgx.ncore[slot] = 0; rqgx.cver[slot] = 0; }
    rqgx.live = rqgx_on();
}

/* ---- the lists: the dry glass by need with the new terms (A: per-vertex terms, B: near the sun's image, with the wear read);
   the triangles the lean wet canopy takes get the new terms alone (Aw, Bw: additive, after the water). Indices relative to the
   glass's first vertex (the second vertex stream, gvb, starts there). Returns the triangles written. ---- */
static u32 rqgx_lists(rqc_model *q, const rqc_frame *F, u32 *out, int skip_wet) {
    /* (F is the frame's mapped uniform buffer: uncached on this GPU's host-visible memory, so every field the loop needs is
       copied out once -- reading F->head per triangle cost 0.5 us a triangle, 10 ms a frame for the F-15C on the simulator) */
    float th = rqc_glass_th();
    const float head[3] = { F->head[0], F->head[1], F->head[2] };
    float sun[3] = { F->l[0], F->l[4], F->l[8] };
    {   int s = (int)(F->pack[0] + 0.5f);
        sun[0] = F->l[s]; sun[1] = F->l[4 + s]; sun[2] = F->l[8 + s];
        float len = sqrtf(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
        if (len > 1e-5f) { sun[0] /= len; sun[1] /= len; sun[2] /= len; }
    }
    float amb = (F->r[12] + F->r[13] + F->r[14]) * 0.33f;
    float scl = 0.0f; for (int j = 0; j < 3; j++) { float s = F->r[j] + F->r[4 + j] + F->r[8 + j]; scl += (s < 1.5f ? s : 1.5f) * 0.333f; }
    float lv = amb + 0.2f * scl; lv = lv < 0.03f ? 0.03f : lv > 1.2f ? 1.2f : lv;
    const float S[3] = { F->gx[0], F->gx[1], F->gx[2] };   /* the sun (C1) in the cockpit frame */
    float g1 = F->gx[4] + F->gx[5] + F->gx[6], sstr = F->gx[3], gstr = F->gx[7], rw = F->gx[8], R0 = F->gx[11];
    int nearsun = g1 > 0.0f && (sstr > 0.0f || gstr > 0.0f);
    int refl = rw > 0.0f && q->gxrl != NULL;
    float rk = rqgx.rth > 0.0f ? th / rqgx.rth : 1.0f;     /* (ACZ_VR_RQ_GLASS_REFLECT_TH: the reflection kept from its own level) */
    u32 nA = 0, nB = 0, nAw = 0, nBw = 0;
    u32 cap = q->ngtri;
    if (rqgx.tcap < cap) {
        u32 *t = (u32 *)realloc(rqgx.tmp, (size_t)cap * 4u * 3u * sizeof(u32)); if (!t) return 0; rqgx.tmp = t; rqgx.tcap = cap;
    }
    /* (every list is built in host memory and copied once: the index buffer is mapped write-combined / uncached memory, where
       one store a word cost 8.8 ms a frame for the F-15C's 18.8k glass triangles on the simulator) */
    u32 *tmp = rqgx.tmp;                             /* the wet triangles' additive lists: Aw at 0, Bw at tcap; rqgx_wet_copy */
    u32 *oA = tmp + 6u * rqgx.tcap, *oB = tmp + 9u * rqgx.tcap;   /* A and B (B moved right after A when copied) */
    u32 tri_i = 0;
    for (u32 g = 0; g < q->ng; g++) {
        const rqc_gmesh *gm = &q->gm[g];
        const cpit_mesh *m = &cpit.mesh[gm->mesh];
        const u32 *ix = q->gix + gm->ifirst;
        u32 ntm = gm->icount / 3u;
        if (cpit_mesh_hidden(m)) { for (u32 t = 0; t < ntm; t++) q->gxst[tri_i + t] = 0; tri_i += ntm; continue; }
        int fx = (m->flags & CPIT_F_GLASSFX) != 0;
        int outer_skip = skip_wet == 1 && cpit_glass_outer(m);
        const rqc_row *row = &(rqmh_now && q->rowsh ? q->rowsh : (const rqc_row *)q->rowsp)[gm->mesh];   /* (PS2_RQ_MAP_HOST: the host copy) */
        float ca = row->col[3], spk = row->sp[0], shi = row->sp[1];
        int wm = skip_wet == 2 && rqw_mesh(m);
        for (u32 t = 0; t < ntm; t++, ix += 3, tri_i++) {
            u8 *stt = &q->gxst[tri_i];
            const float *tc = q->gxtc + (size_t)tri_i * 6u;
            float c[3] = { tc[0], tc[1], tc[2] }, nn[3] = { tc[3], tc[4], tc[5] };
            int wet = outer_skip;
            if (!wet && wm) wet = rqw_tri_need(c, rqc_wet_th());
            float V[3] = { head[0] - c[0], head[1] - c[1], head[2] - c[2] };
            float vl = sqrtf(V[0] * V[0] + V[1] * V[1] + V[2] * V[2]);
            if (vl > 1e-5f) { V[0] /= vl; V[1] /= vl; V[2] /= vl; }
            float nv = nn[0] * V[0] + nn[1] * V[1] + nn[2] * V[2];
            if (nv < 0.0f) { nn[0] = -nn[0]; nn[1] = -nn[1]; nn[2] = -nn[2]; nv = -nv; }
            float f = 1.0f - nv; f = f * f * f * f * f;
            float wbest = 0.0f;                        /* the strongest term at the centre, in display units */
            int b = 0;
            if (!wet) {
                if (ca >= th) wbest = 1.0f;
                float wr = f * 0.35f * (amb + 0.5f);
                if (wr > wbest) wbest = wr;
                if (spk > 0.0f && wbest < th) {
                    float H[3] = { sun[0] + V[0], sun[1] + V[1], sun[2] + V[2] };
                    float hl = sqrtf(H[0] * H[0] + H[1] * H[1] + H[2] * H[2]);
                    if (hl > 1e-5f) { float nh = fabsf(nn[0] * H[0] + nn[1] * H[1] + nn[2] * H[2]) / hl; float s = 2.0f * spk * powf(nh, shi > 1.0f ? shi : 1.0f); if (s > wbest) wbest = s; }
                }
            }
            if (fx) {
                if (refl && wbest < th) {               /* the interior's reflection: the brightest it can show from the head's range (made once) */
                    float fr = 0.04f + 0.96f * f; fr = fr < 0.15f ? fr : 0.15f;
                    float w = (float)q->gxrl[tri_i] * (1.0f / 255.0f) * fr * lv * rw * rk;
                    if (w > wbest) wbest = w;
                }
                if (nearsun) {                          /* within 25 deg of the sun's image (out beyond 27: the GPU's lobes are 0 from 23) */
                    float D[3] = { -V[0], -V[1], -V[2] };
                    float cs = D[0] * S[0] + D[1] * S[1] + D[2] * S[2];
                    float behind = -(S[0] * nn[0] + S[1] * nn[1] + S[2] * nn[2]);
                    float lim = (*stt & 2u) ? 0.8910f : 0.9063f;   /* cos 27 / cos 25 deg */
                    if (behind > 0.0f && cs > lim) b = 1;
                }
            }
            int keep = b || wbest >= th || ((*stt & 1u) && wbest >= 0.6f * th);
            *stt = (u8)((keep ? 1u : 0u) | (b ? 2u : 0u));
            if (!keep) continue;
            u32 i0 = ix[0] - q->gvx0, i1 = ix[1] - q->gvx0, i2 = ix[2] - q->gvx0;
            if (!wet) {
                u32 *o = b ? oB + 3u * nB++ : oA + 3u * nA++;
                o[0] = i0; o[1] = i1; o[2] = i2;
            } else if (fx && skip_wet == 2) {          /* (wet, the lean canopy's: only the new terms, additive, after the water) */
                u32 *o = b ? tmp + 3u * (rqgx.tcap + nBw++) : tmp + 3u * nAw++;
                o[0] = i0; o[1] = i1; o[2] = i2;
            }
        }
    }
    memcpy(out, oA, (size_t)nA * 3u * sizeof(u32));                 /* A, then B right behind it */
    memcpy(out + 3u * nA, oB, (size_t)nB * 3u * sizeof(u32));
    rqgx.fr_a = nA; rqgx.fr_b = nB; rqgx.fr_aw = nAw; rqgx.fr_bw = nBw;
    rqgx.n_a += nA; rqgx.n_b += nB; rqgx.n_w += nAw + nBw;
    return nA + nB;
}
/* the wet triangles' additive lists (rqgx_lists) into the index buffer at out (after the lean wet list): Aw, then Bw */
static u32 rqgx_wet_copy(u32 *out) {
    u32 nAw = rqgx.fr_aw, nBw = rqgx.fr_bw;
    if (!rqgx.tmp) return 0;
    memcpy(out, rqgx.tmp, (size_t)nAw * 3u * sizeof(u32));
    memcpy(out + 3u * nAw, rqgx.tmp + 3u * rqgx.tcap, (size_t)nBw * 3u * sizeof(u32));
    return nAw + nBw;
}

/* ---- the draws (rq_cockpit_glass) ---- */
static void rqgx_bind(VkCommandBuffer cmd, rqc_model *q, u32 slot, int vb) {
    rqc_set_view(cmd);
    VkDescriptorSet sets[4] = { q->texset, cpit.sm_ok ? cpit.sm_set : white_set, q->fset[slot], rqgx.set[slot] };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqgx.layout, 0, 4, sets, 0, NULL);
    if (vb) {
        VkDeviceSize o0 = (VkDeviceSize)q->gvx0 * sizeof(rqc_vtx), o1 = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &q->vb, &o0);
        vkCmdBindVertexBuffers(cmd, 1, 1, &q->gvb, &o1);
        vkCmdBindIndexBuffer(cmd, q->gib[slot], 0, VK_INDEX_TYPE_UINT32);
    }
}
/* the dry glass by need with the new terms: list A (base) and B (near the sun); returns the triangles written at gip[slot] */
static u32 rqgx_draw_glass(VkCommandBuffer cmd, vk_frame *fr, int skip_wet) {
    rqc_model *q = cpit.rqc;
    if (!q || !q->ok || !q->ng || !q->gvb || !q->gxst) return 0;
    u32 slot = (u32)(fr - frames);
    const rqc_frame *F = rqc_F(q, slot);
    u64 tl0 = vk_mono_ns();
    u32 ntri = rqgx_lists(q, F, (u32 *)q->gip[slot], skip_wet);
    rqgx.ns_lists += vk_mono_ns() - tl0;
    rqc.n_gtris += ntri; rqc.n_gfr++; rqgx.n_fr++;
    if (!ntri) return 0;
    rqgx_bind(cmd, q, slot, 1);
    float pk[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    vkCmdPushConstants(cmd, rqgx.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pk, pk);
    if (rqgx.fr_a) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqgx.p_base); vkCmdDrawIndexed(cmd, rqgx.fr_a * 3u, 1, 0, 0, 0); rqc.fr_pipes++; rqc.fr_draws++; }
    if (rqgx.fr_b) {
        if (rqgx.show) { pk[1] = 1.0f; vkCmdPushConstants(cmd, rqgx.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pk, pk); }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqgx.p_wear); vkCmdDrawIndexed(cmd, rqgx.fr_b * 3u, 1, rqgx.fr_a * 3u, 0, 0); rqc.fr_pipes++; rqc.fr_draws++; }
    return ntri;
}
/* the wet triangles' new terms alone (additive, over the water: the inner face's reflection and wear lie nearer the eye) at fi */
static u32 rqgx_draw_wet_extra(VkCommandBuffer cmd, vk_frame *fr, u32 fi) {
    rqc_model *q = cpit.rqc;
    if (!q || !q->ok || !q->gvb || !(rqgx.fr_aw + rqgx.fr_bw)) return 0;
    u32 slot = (u32)(fr - frames);
    u32 n = rqgx_wet_copy((u32 *)q->gip[slot] + (size_t)fi * 3u);
    if (!n) return 0;
    rqgx_bind(cmd, q, slot, 1);
    float pk[4] = { 1.0f, 2.0f, 0.0f, 0.0f };
    vkCmdPushConstants(cmd, rqgx.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pk, pk);
    if (rqgx.fr_aw) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqgx.p_base); vkCmdDrawIndexed(cmd, rqgx.fr_aw * 3u, 1, fi * 3u, 0, 0); rqc.fr_pipes++; rqc.fr_draws++; }
    if (rqgx.fr_bw) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqgx.p_wear); vkCmdDrawIndexed(cmd, rqgx.fr_bw * 3u, 1, (fi + rqgx.fr_aw) * 3u, 0, 0); rqc.fr_pipes++; rqc.fr_draws++; }
    return n;
}
/* the dust specks and rings (all of the model's; the vertex stage drops the unlit ones) */
static void rqgx_draw_dust(VkCommandBuffer cmd, vk_frame *fr) {
    rqc_model *q = cpit.rqc;
    if (!q || !q->ok || !q->gxsb || !q->ngxs || !rqgx.p_dust || !(rqgx.wear & 1)) return;
    u32 slot = (u32)(fr - frames);
    const rqc_frame *F = rqc_F(q, slot);
    if (F->gx[10] <= 0.0f && F->gx[3] <= 0.0f) return;
    if (F->gx[4] + F->gx[5] + F->gx[6] <= 0.0f) return;          /* (no sunlight: nothing lit) */
    rqgx_bind(cmd, q, slot, 0);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqgx.p_dust);
    vkCmdDraw(cmd, q->ngxs * 6u, 1, 0, 0);
    rqc.fr_pipes++; rqc.fr_draws++; rqgx.n_sp++;
}
/* the cracks: ribbons, then the cores */
static void rqgx_draw_cracks(VkCommandBuffer cmd, vk_frame *fr) {
    rqc_model *q = cpit.rqc;
    if (!q || !q->ok || !rqgx.p_crk || !rqgx.cok || rqgx.cracks <= 0.0f || crk.n <= 0 || ps2_crk_ab_off() || perf_ab_flipped("cracks")) return;
    /* [test] ACZ_VR_RQ_CRACK_AB=1 (R-86): the cracks only on odd game fields, so dumps at consecutive fields give the crack layer's own
       pixels in one run (D.2's gate) */
    static int ab = -1;
    if (ab < 0) { const char *e = getenv("ACZ_VR_RQ_CRACK_AB"); ab = e && *e == '1'; if (ab) ps2_log("cockpit material: [test] the cracks only on odd game fields (ACZ_VR_RQ_CRACK_AB)"); }
    extern u32 ps2_gs_frame_count;
    if (ab && !(ps2_gs_frame_count & 1u)) return;
    u32 slot = (u32)(fr - frames);
    if (!rqgx.cver[slot] || !(rqgx.nseg[slot] + rqgx.ncore[slot])) return;
    rqgx_bind(cmd, q, slot, 0);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqgx.p_crk);
    float pk[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (rqgx.nseg[slot]) {
        vkCmdPushConstants(cmd, rqgx.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pk, pk);
        vkCmdDraw(cmd, rqgx.nseg[slot] * 6u, 1, 0, 0); rqc.fr_draws++;
    }
    if (rqgx.ncore[slot]) {
        pk[0] = 1.0f;
        vkCmdPushConstants(cmd, rqgx.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pk, pk);
        vkCmdDraw(cmd, rqgx.ncore[slot], 1, 0, 0); rqc.fr_draws++;
    }
    rqc.fr_pipes++; rqgx.n_crk_fr++; rqgx.n_seg += rqgx.nseg[slot];
}
/* every 240 frames with rq_cockpit_glass's lines */
static void rqgx_log(void) {
    if (!rqgx.n_fr) return;
    ps2_log("cockpit material: the readable glass: %llu frames (the need test %.3f ms a frame on the CPU, %u glass triangles): list A %.0f, near the sun %.0f, the wet glass's own terms %.0f triangles a frame; dust drawn in %llu frames; cracks drawn in %llu frames (%.0f segments a frame)",
            (unsigned long long)rqgx.n_fr, (double)rqgx.ns_lists * 1e-6 / (double)rqgx.n_fr, cpit.rqc ? cpit.rqc->ngtri : 0u, (double)rqgx.n_a / (double)rqgx.n_fr, (double)rqgx.n_b / (double)rqgx.n_fr, (double)rqgx.n_w / (double)rqgx.n_fr,
            (unsigned long long)rqgx.n_sp, (unsigned long long)rqgx.n_crk_fr, rqgx.n_crk_fr ? (double)rqgx.n_seg / (double)rqgx.n_crk_fr : 0.0);
}
