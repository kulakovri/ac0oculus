/* rq_clouds.inc -- our volumetric clouds in the eye-sized pass, the Quest recipe (PS2_RQ_CLOUDS=1; a private script
   section 4.2; included by rq_video.inc, PS2_RQ_SUPPORT only). The clouds' compute (weather, shadow map, the far map with the travel rule
   and a tile budget, the near march) runs before the eye pass (rqcl_prepare); inside the pass one instanced draw over the tiles
   of the far map that hold cloud composites everything (rq_clouds_composite: rq_clouds.vert / rq_clouds.frag, mediump, at most 4
   texture reads, no attachment read). Replaces rql_cl_draw's canvases and full-screen layer (R-15, PS2_RQ_LAYERS). The scene depth
   this composite and the near march read is the feed's (rqcl_depth_source: the single place to swap in the quarter-size depth
   pre-pass of section 4.6). */
extern int ps2_clouds_rq_on(void);
extern int ps2_clouds_rq_compute(const ps2_cloud_pass *p);
extern VkDescriptorSet ps2_clouds_rq_set(int fi, uint32_t *near_div, uint32_t *near_img_w, uint32_t *near_img_h, uint32_t *near_lw, uint32_t *near_lh, int *near_drew);
extern VkDescriptorSetLayout ps2_clouds_common_set_layout(void);
extern void ps2_clouds_far_rq_buffers(VkBuffer *tiles, VkBuffer *draw, VkBuffer *list);
extern void ps2_clouds_far_rq_cam(float cam[3]);
extern void ps2_clouds_rq_prof(VkImageView *v, VkSampler *s);
extern int ps2_clouds_far_rq_shown(VkImageView *c, VkImageView *p);   /* (clouds7, PS2_RQ_CLOUDS_FARSHOWN) */
extern VkSampler ps2_clouds_rq_lin_sampler(void);
extern VkBuffer ps2_clouds_near_flag_buf(void);
extern void ps2_clouds_rq_tail(VkCommandBuffer cmd, int fi);

static struct {
    int tried, ok;
    VkShaderModule vs, fs, vn, fn, vne, fne;   /* the far tiles; the near cells: plain, -DEDGE */
    VkShaderModule fsd;                        /* (rq8_onelayer, PS2_RQ_CLOUDS_ONE: the far tiles with the fragment's own depth, rq_clouds_fd.frag) */
    VkShaderModule fss;                        /* (rq8_onelayer round 2, PS2_RQ_CLOUDS_ONE_FD=0: the front / back split without the depth write, rq_clouds_fs.frag) */
    VkDescriptorSetLayout dsl;
    VkDescriptorPool pool;
    VkDescriptorSet set[FRAMES_IN_FLIGHT];
    VkPipelineLayout layout;
    VkPipeline pipe, pn, pne; VkRenderPass rp_for;   /* far tiles, near cells plain, near cells with the scene inside the range */
    VkPipeline pipe_ns;                               /* (rq8_onelayer, [test] PS2_RQ_CLOUDS_ONE_ALT: the far tiles without SOFT, t12b's frames) */
    VkPipeline pipe_ab;                               /* (rq8_onelayer round 2, [test] PS2_RQ_CLOUDS_ONE_AB 1 / 4: the far tiles of the left-out blocks) */
    VkPipeline pipe_cut0;                             /* (rq10_artefacts, [test] PS2_RQ_ART_ALT with PS2_RQ_CLOUDS_SOFT_CUT: the far tiles without CUT) */
    VkPipeline pipe_cutalt;                           /* (rq13_cut, [test] PS2_RQ_CLOUDS_CUT_ALT: t13c's far tiles (SOFT) for the odd frames) */
    VkBuffer ubo; VkDeviceMemory ubo_mem; uint8_t *ubo_map;
    VkBuffer list, tiles, draw;
    /* this frame: the common set (the compute's), the near buffer's state */
    VkDescriptorSet cset; int near_on; float nuv[2]; float nsz[2]; float ndiv; u32 ncells;
} rqcl;

static int rqcl_want(void) { return ps2_clouds_rq_on(); }
/* [test] PS2_RQ_ART_ALT=1 (rq10_artefacts 2026-11-15, prompt 11 Part A): odd game frames (the XR seq of the list being drawn, the eye dumps'
   "game frame") draw without the artefact fixes switched on (PS2_RQ_CLOUDS_FOLD, PS2_RQ_CLOUDS_SOFT_CUT, PS2_RQ_CLOUDS_SUN_T, PS2_RQ_GSHADOW_FILTER),
   even ones with them: consecutive dumps of one run are a same-camera pair, a frame of motion apart (cross-run dumps are not one camera,
   R-135). For eye dumps only, never the headset. (Round 2) a mask instead of 1 flips only some: 2 FOLD, 4 SOFT_CUT, 8 SUN_T, 16 GSHADOW_FILTER */
static int rqart_alt(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_ART_ALT"); v = e && *e ? atoi(e) : 0; if (v == 1) v = 30; if (v < 0) v = 0;
                 if (v) ps2_log("rq art: [test] PS2_RQ_ART_ALT=%d: odd game frames without these artefact fixes (2 FOLD, 4 SOFT_CUT, 8 SUN_T, 16 GSHADOW_FILTER)", v); }
    return v;
}
static int rqart_off(int bit) { return (rqart_alt() & bit) && (cpit_seq & 1u); }
int ps2_rq_art_off_now(void) { return rqart_alt() && (cpit_seq & 1u); }
/* PS2_RQ_CLOUDS_FOLD (rq10_artefacts 2026-11-15, A1 of prompt 11, BUGS_RQ R-156; default 0, the Quest 1 since t13b; read once): the far
   tiles' per-corner parallax shift across the octahedral map's border. rq_clouds.vert shifts each corner's uv by the parallax between the
   tile's march origin and the eye (du = dir_uv(shifted) - dir_uv(d)) and dropped it only beyond 0.2 of the map. With ONE cloud layer every
   tile in view is marched from this frame's centre eye, so the shift is each eye's own half IPD (the near clouds' stereo: 3.2 cm at 10 m is
   0.3 texel); but a corner ON the map's outer border (the lower hemisphere's meridians x = 0 / z = 0) whose shifted direction crossed the
   meridian was read on the mirrored side, up to 100 texels away, and the tile's triangles stretched that content: the pale and dark wedges
   and sheets at aircraft (t13a's "square round the aircraft"; the user's 338 / 401 / 412 s). 1 = the shift capped at 4 texels (t13b);
   2 (round 2) = the shift on the corner's own face of the octahedron, continued linearly past its edges: no jump at the border at any map
   size and no cap, the stereo kept (rq_clouds.vert dir_uv_face, misc.w bits 6 and 7). Dropping the shift altogether would put the near
   clouds at infinity for the eyes */
static int rqcl_fold(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_CLOUDS_FOLD"); v = e && (*e == '1' || *e == '2') ? *e - '0' : 0;
                 ps2_log("rq clouds: PS2_RQ_CLOUDS_FOLD=%d: the far tiles' parallax shift %s", v, v == 2 ? "on the corner's own face (exact at the map's border)" : v ? "capped at 4 texels" : "dropped only beyond 0.2 of the map"); }
    return v;
}
/* PS2_RQ_CLOUDS_SOFT_CUT (rq10_artefacts 2026-11-15, A2 of prompt 11, BUGS_RQ R-157; default 0, read once; with SOFT, so with
   PS2_RQ_CLOUDS_ONE): rq_clouds.frag's constant CUT -- the depth texels nearer than the composite's own quad are left out of the SOFT fade
   (the pass's depth test cuts there, per sample), the others fade by their bilinear weights instead of the nearest texel's distance */
static int rqcl_cut(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_CLOUDS_SOFT_CUT"); v = e && *e == '1';
                 ps2_log("rq clouds: PS2_RQ_CLOUDS_SOFT_CUT=%d: the SOFT fade %s", v, v ? "by the depth texels behind the quad, bilinear (the pass's depth test cuts what stands in front)" : "by the nearest of the four depth texels"); }
    return v;
}
/* PS2_RQ_CLOUDS_SUN_T (rq10_artefacts 2026-11-15, A4 of prompt 11, BUGS_RQ R-159; default 0, read once; needs PS2_RQ_CLOUDS_FXFADE): the sun's
   glare veiled by our cloud between the sun and the eye. The game's flare, glow and ghosts (class 5, drawn after the cockpit over the HUD)
   and our canopy glass's glare knew only the in-cloud density (ACZ_IMM_CLOUD_SUN): under a cloud that hides the sun they shone in full
   (the user's M10 flight on t13a, 243-274 s: "under the cloud the sun shines, in the cloud not, out of it again"). The fade's compute
   (rq_cloud_fxfade.comp) reads the far map's transmittance in the sun's direction once a frame (six taps round it; -1 when the tile there
   was not marched from this frame's eye) into its table's header; the host reads it back FRAMES_IN_FLIGHT frames later (rqfx_dispatch),
   eases it (a third of the step a frame: ~4 frames) and scales the class-5 vertices' colour (rqsun_fade, after the in-cloud fade) and
   the glass's glare (rq_glass.inc rqgx_frame) by it */
static struct { float t; u64 n, nbad; double sum; } rqsun = { 1.0f, 0, 0, 0.0 };
static int rqsun_cfg(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_CLOUDS_SUN_T"); v = e && *e == '1';
                 ps2_log("rq clouds: PS2_RQ_CLOUDS_SUN_T=%d: the sun's glare %s", v, v ? "x the far map's transmittance toward the sun (eased; with PS2_RQ_CLOUDS_FXFADE's compute)" : "by the in-cloud density only (ACZ_IMM_CLOUD_SUN)"); }
    return v;
}
/* (round 2, the review's note point 4) the far march marches the one tile that holds the sun every frame whatever the view (64 rays), so the
   sun's T is never a stale tile's: rq_cloud_far.comp PASS_TILES (fp.dbg.x, ps2_clouds_far.c's frame params) */
int ps2_rq_sun_tile(void) { return rqsun_cfg(); }
static float rqsun_k(void) { return rqsun_cfg() && !rqart_off(8) ? rqsun.t : 1.0f; }
/* PS2_RQ_GSHADOW_FILTER (rq10_artefacts 2026-11-15, A5 of prompt 11, BUGS_RQ R-160; default 0 = one read as before; metres, read once): the
   ground's cloud shadow per terrain vertex read as the centre and four taps this far along x and z (mesh.vert -DRQ gsh_t; the block's
   c[8] by ps2_video.c gsh_frame's hook), so one vertex in a gap of the map no longer lights its fan of triangles alone */
float ps2_rq_gsh_filter_m(void) {
    static float v = -1.0f;
    if (v < 0.0f) { const char *e = getenv("PS2_RQ_GSHADOW_FILTER"); v = e && *e ? (float)atof(e) : 0.0f; if (!(v >= 0.0f)) v = 0.0f; if (v > 1000.0f) v = 1000.0f;
                    ps2_log("rq clouds: PS2_RQ_GSHADOW_FILTER=%.0f: the ground's cloud shadow per vertex %s", (double)v, v > 0.0f ? "from five taps (the centre, +-reach along x and z)" : "from one read"); }
    return rqart_off(16) ? 0.0f : v;
}
/* the composite's switches of the 2026-11-10 cloud round (agent clouds3, PROMPT_2026-11-10_flight_bugs.md Part B), read once (default 0 each):
   bit 0 PS2_RQ_CLOUDS_NEAR_BILAT (B-3, R-53): the near buffer at depth edges weighted by depth agreement instead of one texel;
   bit 1 PS2_RQ_CLOUDS_SOFT (B-2, R-52): the far tiles at their nearest cloud, faded per pixel by the scene's distance inside the cloud;
   bit 2 PS2_RQ_CLOUDS_FARWARP (B-1, R-52): the far tiles' corners moved with their own content (rq_clouds.vert), not their uv;
   bit 3 PS2_RQ_CLOUDS_OPTICS (B-6, R-56): our halo / glory / spectre in both composites (a specialization constant: without it the
   pipelines carry none of the code);
   bit 4 PS2_RQ_CLOUDS_FARRESOLVE (clouds6 2026-11-11, A.4 piece 4, R-80): the far tiles drawn unmoved at their own directions with the
   parallax resolved per pixel (rq_clouds.frag rq_resolve, specialization constant 3), their empty neighbours drawn too; it retires
   FARWARP (ignored with it) */
/* PS2_RQ_CLOUDS_FARRESOLVE_STEPS (clouds6 2026-11-11, the device's price; 1-3, default 3): the per-pixel resolve's steps after its first
   guess (each one read of the depth data; a pixel stops early when the distance agrees) */
static int rqcl_res_steps(void) {
    static int n = -1;
    if (n < 0) { const char *e = getenv("PS2_RQ_CLOUDS_FARRESOLVE_STEPS"); n = e && *e ? atoi(e) : 3; if (n < 1) n = 1; if (n > 3) n = 3; }
    return n;
}
static int rqcl_flags(void) {
    static int f = -1;
    if (f < 0) {
        static const char *const nm[6] = { "PS2_RQ_CLOUDS_NEAR_BILAT", "PS2_RQ_CLOUDS_SOFT", "PS2_RQ_CLOUDS_FARWARP", "PS2_RQ_CLOUDS_OPTICS", "PS2_RQ_CLOUDS_FARRESOLVE",
                                            "PS2_RQ_CLOUDS_FARWARP_ONE" };   /* (bit 5, clouds6: rq_clouds.vert, every tile's offset from the shown sweep's origin) */
        f = 0;
        for (int i = 0; i < 6; i++) { const char *e = getenv(nm[i]); if (e && (*e == '1' || (i == 4 && *e == '2'))) f |= 1 << i; }   /* (FARRESOLVE=2: ps2_clouds_far.c) */
        if (f & 16) { f &= ~4; ps2_log("rq clouds: the far tiles' parallax resolved per pixel (PS2_RQ_CLOUDS_FARRESOLVE; FARWARP retired)"); }
        {   /* (rq8_onelayer, PS2_RQ_CLOUDS_ONE: the one layer starts at the eye, so the scene cuts it per pixel inside its span -- SOFT --, never at
               a quad's depth; every tile is marched from this frame's eye, so each tile's own offset is the eye's -- no shown sweep's origin.
               FARWARP off too: without the shown map its moved corners dropped whole tiles at random from frame to frame -- squares flickering
               in the overcast over M2's runway, a still camera (t12b with FARSWAP=0 FARSHOWN=0 the same; BUGS_RQ R-145); the uv shifted per
               corner keeps each eye's own offset) */
            extern int ps2_clouds_far_rq_one_cfg(void);
            if (ps2_clouds_far_rq_one_cfg()) { f |= 2; f &= ~(4 | 16 | 32); ps2_log("rq clouds: PS2_RQ_CLOUDS_ONE: SOFT on, FARWARP, FARRESOLVE and FARWARP_ONE off"); }
        }
        ps2_log("rq clouds: near buffer at depth edges %s (PS2_RQ_CLOUDS_NEAR_BILAT); far tiles against the scene %s (PS2_RQ_CLOUDS_SOFT); "
                "far tiles' parallax %s (PS2_RQ_CLOUDS_FARWARP)", (f & 1) ? "weighted by depth agreement" : "one texel where the four disagree",
                (f & 2) ? "faded per pixel inside the cloud's span" : "cut at the corners' first cloud", (f & 16) ? "resolved per pixel (FARRESOLVE)" : (f & 4) ? "the corners moved with their own texels" : "the uv shifted per corner");
        ps2_log("rq clouds: our optics (halo, glory, spectre) %s (PS2_RQ_CLOUDS_OPTICS)", (f & 8) ? "in the eye pass's composites" : "not drawn in the eye pass");
    }
    return f;
}

/* PS2_RQ_CLOUDS_FXFADE (agent clouds4 2026-11-11, R-55 / B-4; default 0, read once): the game's effects in the eye pass fade per vertex by our
   cloud in front of them (rq_fxfade.inc, rq_cloud_fxfade.comp): 1 = the PC's fade (ACZ_VR_CLOUD_FX_SOFT's range), 2 = [test] tinted by the fade */
static int rqfx_cfg(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_CLOUDS_FXFADE"); v = e ? atoi(e) : 0; if (v < 0 || v > 2) v = 0; }
    return v;
}
/* PS2_RQ_CLOUDS_FXAT (agent clouds4 2026-11-11, C4 of the user's M4 flight, R-67; default 0, read once): ACZ_VR_CLOUD_FX puts the cloud
   composite in front of the first effect after the list's last opaque world draw before the composite's anchor. In the eye pass the hoist
   has moved the sun probe in front of the scene, so a list without the game's cloud sprites anchors at the first HUD draw, and the game's
   own cockpit meshes (class 1, depth write), drawn after the world's effects and right before the HUD, kept the point behind those
   effects: they went under the composite and inside cloud the whole column covered them (M4 f4900: a burning enemy's fire and smoke trail
   gone; the user's M4 177-179.5 s: an enemy's trail gone at once). The 3D cockpit replaces those meshes and the eye pass never draws them:
   1 = they no longer hold the point back (ps2_video.c, the eye pass only) */
static int rqfx_at_cfg(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_CLOUDS_FXAT"); v = e && *e == '1'; ps2_log("rq clouds: the effects before the game's own cockpit meshes %s (PS2_RQ_CLOUDS_FXAT)", v ? "after our clouds, with the rest" : "under our clouds' composite"); }
    return v;
}
static VkShaderModule rqfx_cs;                    /* (rq_cloud_fxfade.comp: rq_load_shaders) */
static void rqfx_dispatch(VkCommandBuffer cmd, vk_frame *fr);
static float rqfx_row(const vk_draw *e, int after);
static void rqfx_batch_n(u32 nv);

/* ---- PS2_RQ_CLOUDS_CUT (rq13_cut 2026-11-15, prompt 12 Part A, BUGS_RQ R-190; default 0, read once; needs PS2_RQ_CLOUDS_ONE) ----
   The user's t13c flights: "the stairs are still there" -- a ridge with cloud behind it ends the cloud in rectangular steps a few pixels
   high, an aircraft in cloud sits in a hole of big square pixels. The cause (the review, from the code): the composite cut the one layer
   at the scene by rqcl_depth_source, rq_feed.depth: 512 x 448 (one texel ~3.4 x 4.3 eye pixels), with PS2_RQ_ZPRE the world's native
   meshes only (no aircraft drawn by the GS path, no trees), and its SOFT fade by the nearest of four texels: every cut came out on that
   grid. With the switch every object applies the cloud in front of it itself, at its own exact distance (rq_cut.glsl): the merged
   terrain per fragment (rq_terrain.frag -DRQO: two reads of the far map), the other native meshes (aircraft, ground units) and the GS
   draws that write depth (trees, ground objects) per vertex (mesh.vert / gs.vert -DRQCUT, the cut_*.spv twins rq_load_shaders takes in
   place of the eye pass's originals); the composite draws its tiles at the far plane with the depth test, so only where nothing stands,
   and reads no scene depth (no SOFT). Every silhouette is the object's own, per sample of the 4x pass, and nothing lags a frame. The
   game's effects after the composite keep PS2_RQ_CLOUDS_FXFADE's per-vertex fade.
   1 = on, 2 = [test] the objects show the share of the ray's cloud in front of them as red (their own colour dropped).
   [test] PS2_RQ_CLOUDS_CUT_PARTS (default 15, the price rows by absence): 1 the terrain (native program 1), 2 the other native meshes,
   4 the GS draws, 8 the composite at the far plane without SOFT (without 8 the composite is t13c's: the objects' cloud doubles; with
   only 8 the objects take none); 16 (with 1) the merged terrain per vertex instead of per fragment (the cheaper trade: two map reads a vertex,
   the cloud in front interpolated over the patch's triangles); 32 (with 2, round 2) the other natives per vertex, round 1's way (round 2's
   default: per fragment -- no vertex texture read, the research's warning of direct mode) */
#define RQC_BLOCK (MESH_MAX_BLOCKS - 11u)       /* the reserved mesh block of rq_cut.glsl (next to RQO_SKY_BLOCK / RQ_LATE_BLOCK) */
#define RQC_TL_BIT 536870912u                   /* mesh_pad / mv_flags bit 29: the draw takes the cloud in front of it, T and L */
#define RQC_T_BIT 268435456u                    /* bit 28: its transmittance only (an additive or subtractive draw) */
#define RQC_FLAT_BIT 67108864u                  /* (round 2) bit 26, GS draws: an alpha-tested card -- one value, its provoking vertex's (rq_gs.frag) */
/* (round 2, prompt 13 / the research summary's upgrade 2, R-195) PS2_RQ_CLOUDS_CUT_SKIP (default 1 with the switch): the band test -- a triangle
   whose provoking vertex and the eye both lie under the cloud band's base (the eye by 100 m, the vertex by 100 m, the terrain's by 400 m: its
   triangles' own height) or both over its top runs none of the cloud code (mesh.vert / gs.vert rqc_need, a flat varying the fragment branches
   on); 0 = every flagged object evaluates the cloud */
static int rqcut_skip(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_CLOUDS_CUT_SKIP"); v = !(e && *e == '0'); }
    return v;
}
static int rqcut_off;                                   /* (the twins are not all there: off for the run, rqcut_load) */
static int rqcut_cfg(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_CLOUDS_CUT"); v = e && (*e == '1' || *e == '2') ? *e - '0' : 0; }
    return rqcut_off ? 0 : v;
}
static int rqcut_parts(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_CLOUDS_CUT_PARTS"); v = e && *e ? atoi(e) & 63 : 15; }
    return v;
}
/* the run's design: the switch with the one layer (its far map marched from this frame's eye: a direction from the eye reads it); known
   once the composite's set exists (rqcl_make) */
static int rqcut_live(void) {
    static int v = -1;
    if (v < 0 && rqcl.ok) {
        extern int ps2_clouds_far_rq_one_cfg(void);
        v = rqcut_cfg() && ps2_clouds_far_rq_one_cfg();
        if (rqcut_cfg())
            ps2_log("rq clouds: PS2_RQ_CLOUDS_CUT=%d: %s (parts %d: terrain %s, other native meshes %s, GS draws %s, the composite %s)", rqcut_cfg(),
                    !v ? "needs PS2_RQ_CLOUDS_ONE: off" : rqcut_cfg() == 2 ? "[test] the objects show the share of cloud in front of them as red"
                    : "every object applies the cloud in front of it at its own distance; the composite only where nothing stands",
                    rqcut_parts(), (rqcut_parts() & 1) ? ((rqcut_parts() & 16) ? "per vertex" : "per fragment") : "none", (rqcut_parts() & 2) ? ((rqcut_parts() & 32) ? "per vertex" : "per fragment") : "none",
                    (rqcut_parts() & 4) ? "per vertex" : "none", (rqcut_parts() & 8) ? "at the far plane, no scene depth read" : "t13c's (SOFT)");
    }
    return v > 0;
}
static int rqcut_comp(void) { return rqcut_live() && (rqcut_parts() & 8); }
/* [test] PS2_RQ_CLOUDS_CUT_ALT=N (with the switch): game frames (cpit_seq, the eye dumps' "game frame", as PS2_RQ_ART_ALT) in blocks of N, the
   odd blocks drawn t13c's way -- no object takes the cloud, the composite cuts it by the feed's depth (SOFT) --, the even ones the switch's:
   dumps one block apart are a same-camera pair, N frames of motion apart (N = 2 when each dump takes two frames). For eye dumps only */
static int rqcut_alt(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_CLOUDS_CUT_ALT"); v = e && *e ? atoi(e) : 0; if (v < 0 || v > 64) v = 0;
                 if (v) ps2_log("rq clouds: [test] PS2_RQ_CLOUDS_CUT_ALT=%d: odd blocks of game frames drawn t13c's way (no object takes the cloud, the composite's SOFT cut)", v); }
    return v;
}
static int rqcut_odd(void) { return rqcut_alt() && ((cpit_seq / (u32)rqcut_alt()) & 1u); }   /* (this frame t13c's) */
static void rqcut_block(vk_frame *fr);

/* the scene depth the near march and the composite read: rq_feed.depth, 1x, in the game's symmetric frame (one layer per eye, the mapping every consumer
   already has). Written by the eye pass's depth resolve one frame late (old path), or with PS2_RQ_ZPRE by rq_zpre.inc: THIS frame's world before the
   eye pass (+ the previous frame's cockpit coverage); the image, its layout and its mapping are the same in both cases, in SHADER_READ_ONLY layout */
static void rqcl_depth_source(ps2_cloud_pass *p) {
    p->depth = rq_feed.depth; p->depth_layout = &rq_feed.dlayout;
}

static int rqcl_make(void) {
    if (rqcl.ok) return 1;
    if (rqcl.tried) return 0;
    rqcl.tried = 1;
    const u64 rqpw_t0 = vk_mono_ns();
    if (!rqcl.vs || !rqcl.fs || !rqcl.vn || !rqcl.fn || !rqcl.vne || !rqcl.fne) { ps2_log("vk: PS2_RQ_CLOUDS: shaders missing"); return 0; }
    ps2_clouds_far_rq_buffers(&rqcl.tiles, &rqcl.draw, &rqcl.list);
    if (!rqcl.tiles || !rqcl.draw) { ps2_log("vk: PS2_RQ_CLOUDS: the far module is off"); return 0; }
    VkDescriptorSetLayoutBinding b[7]; memset(b, 0, sizeof b);
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[1] = b[0]; b[1].binding = 1;
    b[2] = b[0]; b[2].binding = 2; b[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[3] = b[0]; b[3].binding = 3; b[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;   /* the far map's depth data (rq_clouds.vert) */
    b[4] = b[0]; b[4].binding = 4;                              /* the near cells' classes (rq_clouds_near.vert) */
    b[5] = b[3]; b[5].binding = 5; b[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;   /* (clouds7, PS2_RQ_CLOUDS_FARSHOWN: the shown map, rq_clouds.frag) */
    b[6] = b[5]; b[6].binding = 6;                                                    /* (its depth data) */
    VkDescriptorSetLayoutCreateInfo lc; memset(&lc, 0, sizeof lc);
    lc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; lc.bindingCount = 7; lc.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev, &lc, NULL, &rqcl.dsl) != VK_SUCCESS) return 0;
    VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * FRAMES_IN_FLIGHT }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, FRAMES_IN_FLIGHT }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * FRAMES_IN_FLIGHT } };
    VkDescriptorPoolCreateInfo pi; memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; pi.maxSets = FRAMES_IN_FLIGHT; pi.poolSizeCount = 3; pi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(dev, &pi, NULL, &rqcl.pool) != VK_SUCCESS) return 0;
    VkDescriptorSetLayout L[FRAMES_IN_FLIGHT];
    for (u32 k = 0; k < FRAMES_IN_FLIGHT; k++) L[k] = rqcl.dsl;
    VkDescriptorSetAllocateInfo ai; memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; ai.descriptorPool = rqcl.pool; ai.descriptorSetCount = FRAMES_IN_FLIGHT; ai.pSetLayouts = L;
    if (vkAllocateDescriptorSets(dev, &ai, rqcl.set) != VK_SUCCESS) return 0;
    /* the per-frame VS parameters (host-visible, 256 bytes per frame in flight) */
    {   VkBufferCreateInfo bi; memset(&bi, 0, sizeof bi);
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; bi.size = 256u * FRAMES_IN_FLIGHT; bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        if (vkCreateBuffer(dev, &bi, NULL, &rqcl.ubo) != VK_SUCCESS) return 0;
        VkMemoryRequirements r; vkGetBufferMemoryRequirements(dev, rqcl.ubo, &r);
        u32 mt = find_mem(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mt == 0xFFFFFFFFu) return 0;
        VkMemoryAllocateInfo mi; memset(&mi, 0, sizeof mi);
        mi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; mi.allocationSize = r.size; mi.memoryTypeIndex = mt;
        if (vkAllocateMemory(dev, &mi, NULL, &rqcl.ubo_mem) != VK_SUCCESS) return 0;
        vkBindBufferMemory(dev, rqcl.ubo, rqcl.ubo_mem, 0);
        void *m = NULL; vkMapMemory(dev, rqcl.ubo_mem, 0, VK_WHOLE_SIZE, 0, &m); rqcl.ubo_map = (uint8_t *)m;
    }
    for (u32 k = 0; k < FRAMES_IN_FLIGHT; k++) {
        VkDescriptorBufferInfo bi[4];
        VkDescriptorImageInfo ii; memset(&ii, 0, sizeof ii); ps2_clouds_rq_prof(&ii.imageView, &ii.sampler); ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        bi[0].buffer = rqcl.tiles; bi[0].offset = 0; bi[0].range = VK_WHOLE_SIZE;
        bi[1].buffer = rqcl.draw; bi[1].offset = 0; bi[1].range = VK_WHOLE_SIZE;
        bi[2].buffer = rqcl.ubo; bi[2].offset = 256u * k; bi[2].range = 256u;
        bi[3].buffer = ps2_clouds_near_flag_buf(); bi[3].offset = 0; bi[3].range = VK_WHOLE_SIZE;
        VkDescriptorImageInfo si[2]; memset(si, 0, sizeof si);
        ps2_clouds_far_rq_shown(&si[0].imageView, &si[1].imageView);
        si[0].sampler = si[1].sampler = ps2_clouds_rq_lin_sampler(); si[0].imageLayout = si[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet w[7]; memset(w, 0, sizeof w);
        for (u32 i = 0; i < 7; i++) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = rqcl.set[k]; w[i].dstBinding = i; w[i].descriptorCount = 1;
            w[i].descriptorType = b[i].descriptorType;
            if (i == 3) w[i].pImageInfo = &ii; else if (i >= 5) w[i].pImageInfo = &si[i - 5]; else w[i].pBufferInfo = &bi[i < 3 ? i : 3];
        }
        vkUpdateDescriptorSets(dev, 7, w, 0, NULL);
    }
    VkDescriptorSetLayout sl[2] = { ps2_clouds_common_set_layout(), rqcl.dsl };
    VkPushConstantRange pr; memset(&pr, 0, sizeof pr); pr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; pr.size = 64;   /* (rq8_onelayer: +dsplit) */
    VkPipelineLayoutCreateInfo pl; memset(&pl, 0, sizeof pl);
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pl.setLayoutCount = 2; pl.pSetLayouts = sl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev, &pl, NULL, &rqcl.layout) != VK_SUCCESS) return 0;
    rqcl.ok = 1;
    rqpw_made("the clouds' composite set, layout and buffer (rqcl_make)", rqpw_t0);
    ps2_log("rq clouds: PS2_RQ_CLOUDS: the far map with parallax, a tile budget and one composite in the eye pass");
    return 1;
}
static int rqcl_pipe(void) {
    extern int ps2_clouds_far_rq_one_alt(void);
    extern int ps2_clouds_far_rq_one_ab(void), ps2_clouds_far_rq_one_fd(void);
    int ab = ps2_clouds_far_rq_one_ab();
    if (rqcl.pipe && rqcl.pn && rqcl.pne && rqcl.rp_for == rqe.rp && (rqcl.pipe_ns || !ps2_clouds_far_rq_one_alt()) &&
        (rqcl.pipe_ab || (ab != 1 && ab != 4))) return 1;
    if (rqcl.pipe_ns) { vkDestroyPipeline(dev, rqcl.pipe_ns, NULL); rqcl.pipe_ns = VK_NULL_HANDLE; }
    if (rqcl.pipe_ab) { vkDestroyPipeline(dev, rqcl.pipe_ab, NULL); rqcl.pipe_ab = VK_NULL_HANDLE; }
    if (rqcl.pipe_cut0) { vkDestroyPipeline(dev, rqcl.pipe_cut0, NULL); rqcl.pipe_cut0 = VK_NULL_HANDLE; }
    if (rqcl.pipe_cutalt) { vkDestroyPipeline(dev, rqcl.pipe_cutalt, NULL); rqcl.pipe_cutalt = VK_NULL_HANDLE; }
    if (rqcl.pipe) { vkDestroyPipeline(dev, rqcl.pipe, NULL); rqcl.pipe = VK_NULL_HANDLE; }
    if (rqcl.pn) { vkDestroyPipeline(dev, rqcl.pn, NULL); rqcl.pn = VK_NULL_HANDLE; }
    if (rqcl.pne) { vkDestroyPipeline(dev, rqcl.pne, NULL); rqcl.pne = VK_NULL_HANDLE; }
    const u64 rqpw_t0 = vk_mono_ns();
    VkPipelineShaderStageCreateInfo st[2]; memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = rqcl.vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = rqcl.fs; st[1].pName = "main";
    {   extern int ps2_clouds_far_rq_one_cfg(void);          /* (PS2_RQ_CLOUDS_ONE: the front / back split; with _ONE_FD=1 the fragments carry their own first cloud's depth) */
        if (ps2_clouds_far_rq_one_cfg()) { VkShaderModule m = ps2_clouds_far_rq_one_fd() ? rqcl.fsd : rqcl.fss; if (m) st[1].module = m; } }
    VkPipelineVertexInputStateCreateInfo vin; memset(&vin, 0, sizeof vin); vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia; memset(&ia, 0, sizeof ia); ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vs; memset(&vs, 0, sizeof vs); vs.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vs.viewportCount = 1; vs.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs; memset(&rs, 0, sizeof rs); rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms; memset(&ms, 0, sizeof ms); ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = rq_samples();
    /* depth test GEQUAL (reversed depth, larger = nearer): the quad sits at its tile's nearest cloud */
    VkPipelineDepthStencilStateCreateInfo dss; memset(&dss, 0, sizeof dss); dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO; dss.depthTestEnable = VK_TRUE; dss.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    VkPipelineColorBlendAttachmentState cba; memset(&cba, 0, sizeof cba);
    cba.blendEnable = VK_TRUE; cba.colorBlendOp = VK_BLEND_OP_ADD; cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; cba.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    VkPipelineColorBlendStateCreateInfo cb; memset(&cb, 0, sizeof cb); cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cb.attachmentCount = 1; cb.pAttachments = &cba;
    static const VkDynamicState dy[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsi; memset(&dsi, 0, sizeof dsi); dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; dsi.dynamicStateCount = 2; dsi.pDynamicStates = dy;
    VkGraphicsPipelineCreateInfo gp; memset(&gp, 0, sizeof gp);
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia; gp.pViewportState = &vs;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &dss; gp.pColorBlendState = &cb; gp.pDynamicState = &dsi; gp.layout = rqcl.layout; gp.renderPass = rqe.rp;
    static int32_t spv[6]; static VkSpecializationMapEntry spm[6] = { { 0, 0, 4 }, { 1, 4, 4 }, { 2, 8, 4 }, { 3, 12, 4 }, { 4, 16, 4 }, { 5, 20, 4 } }; static VkSpecializationInfo spi_far, spi_near;
    spv[0] = ps2_clouds_rq_format2(); spv[1] = (rqcl_flags() & 8) ? 1 : 0; spv[2] = (rqcl_flags() & 2) ? 1 : 0;   /* (constant 0 T2: ACZ_VR_CLOUD_RQ_NEARFS=2, T in the depth image; 1 OPT: PS2_RQ_CLOUDS_OPTICS; 2 SOFT: PS2_RQ_CLOUDS_SOFT) */
    spv[3] = (rqcl_flags() & 16) ? rqcl_res_steps() : 0;          /* (constant 3 RES: PS2_RQ_CLOUDS_FARRESOLVE's steps, the far composite only) */
    {   VkImageView a, b2; spv[4] = ps2_clouds_far_rq_shown(&a, &b2) ? 1 : 0; }   /* (constant 4 SHOWN: clouds7's PS2_RQ_CLOUDS_FARSHOWN, the far composite only) */
    spv[5] = rqcl_cut() && spv[2] ? 1 : 0;                        /* (constant 5 CUT: rq10_artefacts' PS2_RQ_CLOUDS_SOFT_CUT, with SOFT) */
    const int32_t cut_s2 = spv[2], cut_s5 = spv[5]; const VkShaderModule cut_m = st[1].module;   /* (t13c's, for _CUT_ALT) */
    if (rqcut_comp()) { spv[2] = 0; spv[5] = 0; st[1].module = rqcl.fs; }   /* (rq13_cut, PS2_RQ_CLOUDS_CUT: no scene-depth read, the plain far tiles) */
    spi_far.mapEntryCount = 5; spi_far.pMapEntries = &spm[1]; spi_far.dataSize = 24; spi_far.pData = spv;
    spi_near.mapEntryCount = 2; spi_near.pMapEntries = spm; spi_near.dataSize = 12; spi_near.pData = spv;
    st[1].pSpecializationInfo = &spi_far;
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &rqcl.pipe) != VK_SUCCESS) { rqcl.pipe = VK_NULL_HANDLE; return 0; }
    if (rqcut_comp() && rqcut_alt()) {                            /* ([test] PS2_RQ_CLOUDS_CUT_ALT: t13c's far tiles for the odd frames) */
        int32_t c2 = spv[2], c5 = spv[5]; VkShaderModule cm = st[1].module;
        spv[2] = cut_s2; spv[5] = cut_s5; st[1].module = cut_m;
        if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &rqcl.pipe_cutalt) != VK_SUCCESS) rqcl.pipe_cutalt = VK_NULL_HANDLE;
        spv[2] = c2; spv[5] = c5; st[1].module = cm;
    }
    if (spv[5] && (rqart_alt() & 4)) {                                  /* ([test] PS2_RQ_ART_ALT: the frames without the fixes draw without CUT) */
        spv[5] = 0;
        if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &rqcl.pipe_cut0) != VK_SUCCESS) rqcl.pipe_cut0 = VK_NULL_HANDLE;
        spv[5] = 1;
    }
    if (ps2_clouds_far_rq_one_alt()) {                            /* (ONE_ALT: t12b's frames take the far tiles without SOFT) */
        int32_t sv = spv[2]; spv[2] = 0; st[1].module = rqcl.fs;
        if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &rqcl.pipe_ns) != VK_SUCCESS) rqcl.pipe_ns = VK_NULL_HANDLE;
        spv[2] = sv;
    }
    /* (round 2, [test] PS2_RQ_CLOUDS_ONE_AB with PS2_RQ_ABSKIP: the left-out blocks' far tiles -- 1 without the per-pixel scene fade (SOFT off,
       the plain fragment shader, the quad still at the tile's near bound), 4 with the fragment's own depth (rq_clouds_fd.frag) where the
       run's own draw without it, or without it where the run's draw with it) */
    if (ab == 1 || ab == 4) {
        int32_t sv = spv[2]; VkShaderModule m0 = st[1].module;
        if (ab == 1) { spv[2] = 0; st[1].module = rqcl.fs; }
        else st[1].module = ps2_clouds_far_rq_one_fd() ? rqcl.fss : rqcl.fsd;
        if (!st[1].module || vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &rqcl.pipe_ab) != VK_SUCCESS) rqcl.pipe_ab = VK_NULL_HANDLE;
        spv[2] = sv; st[1].module = m0;
    }
    st[1].pSpecializationInfo = &spi_near;
    st[0].module = rqcl.vn; st[1].module = rqcl.fn;
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &rqcl.pn) != VK_SUCCESS) { rqcl.pn = VK_NULL_HANDLE; return 0; }
    st[0].module = rqcl.vne; st[1].module = rqcl.fne;
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &rqcl.pne) != VK_SUCCESS) { rqcl.pne = VK_NULL_HANDLE; return 0; }
    rqcl.rp_for = rqe.rp;
    rqpw_made("the clouds' three composite pipelines (rqcl_pipe)", rqpw_t0);
    return 1;
}
/* the compute before the pass: the far update (usually already early in the command buffer), the near march; the composite's inputs
   ready. 1 = something to composite */
static int rqcl_prepare(VkCommandBuffer cmd, vk_frame *fr) {
    ps2_cloud_pass p; memset(&p, 0, sizeof p);
    p.cmd = cmd; p.frame_index = (int)(fr - frames); p.cmd_id = cl_cmd_serial;
    p.color = rts[0].color; p.color_view = rts[0].cview; p.color_layout = rts[0].layout;     /* (no composite: not used) */
    rqcl_depth_source(&p);
    p.layers = rt_layers; p.eye = -1; p.view = &cl_view;
    p.rq_compute = 1;
    if (!ps2_clouds_rq_compute(&p)) return 0;
    uint32_t div = 0, iw = 0, ih = 0, lw = 0, lh = 0; int drew = 0;
    rqcl.cset = ps2_clouds_rq_set((int)(fr - frames), &div, &iw, &ih, &lw, &lh, &drew);
    if (!rqcl.cset) return 0;
    rqcl.near_on = drew; rqcl.ndiv = (float)div;
    rqcl.nuv[0] = 1.0f / ((float)div * (float)iw); rqcl.nuv[1] = 1.0f / ((float)div * (float)ih);
    rqcl.nsz[0] = (float)lw; rqcl.nsz[1] = (float)lh;
    /* the composite's vertex shader parameters (rq_clouds.vert, std140) */
    float cam[3]; ps2_clouds_far_rq_cam(cam);
    float *u = (float *)(rqcl.ubo_map + 256u * (uint32_t)(fr - frames));
    float *const um = u, uh[56];                                  /* (PS2_RQ_MAP_HOST: filled here, then [0, 20) and [26, 56) in two copies:
                                                                     u[19] read u[18] back from the uncached buffer) */
    if (rqmh_now) u = uh;
    const ps2_cloud_view *v = &cl_view;
    u[0] = v->cam_x[0]; u[1] = v->cam_x[1]; u[2] = v->cam_x[2]; u[3] = 0;
    u[4] = v->cam_y[0]; u[5] = v->cam_y[1]; u[6] = v->cam_y[2]; u[7] = 0;
    u[8] = v->cam_z[0]; u[9] = v->cam_z[1]; u[10] = v->cam_z[2]; u[11] = 0;
    u[12] = v->proj[0]; u[13] = v->proj[1]; u[14] = v->proj[2]; u[15] = v->proj[3];
    u[16] = (float)v->target_w; u[17] = (float)v->target_h;
    u[18] = (float)ps2_clouds_rq_map_n(); u[19] = 8.0f / u[18];
    u[26] = 0; u[27] = 0;                                         /* (u[20..25], the crop: at the draw, rq_clouds_composite) */
    for (int e = 0; e < 2; e++) {
        u[28 + 4 * e] = v->eye_m[e][0]; u[29 + 4 * e] = v->eye_m[e][1]; u[30 + 4 * e] = v->eye_m[e][2]; u[31 + 4 * e] = 0;
    }
    u[36] = cam[0]; u[37] = cam[1]; u[38] = cam[2]; u[39] = 0;
    u[40] = v->depth_a; u[41] = v->depth_c; u[42] = v->depth_mh; u[43] = v->depth_zmax;
    u[44] = ps2_clouds_units_per_m(); u[45] = u[46] = u[47] = 0;
    {   /* [debug] PS2_RQ_CLOUDS_FARZ (rq_clouds.vert): the far tiles at a constant depth instead of their corners' first cloud distance:
           1 = in front of the whole scene (no depth cut by terrain; the Quest cockpit's range still cuts), 2 = behind it (over the sky only) */
        static int fz = -1; if (fz < 0) { const char *e = getenv("PS2_RQ_CLOUDS_FARZ"); fz = e ? atoi(e) : 0; if (fz) ps2_log("rq clouds: [debug] PS2_RQ_CLOUDS_FARZ=%d", fz); }
        u[45] = (float)fz; }
    u[47] = (float)(((rqcl_flags() >> 2) & 1) | (rqcl_flags() & 2) | ((rqcl_flags() >> 2) & 4) | ((rqcl_flags() >> 2) & 8));   /* (rq_clouds.vert misc.w: bit 0 FARWARP, bit 1 SOFT, bit 2 FARRESOLVE, bit 3 FARWARP_ONE) */
    {   VkImageView a, b2; if (ps2_clouds_far_rq_shown(&a, &b2)) u[47] = (float)(((int)u[47] & 2) | 16); }   /* (clouds7: bit 4 FARSHOWN, with SOFT kept) */
    {   extern int ps2_clouds_far_rq_one(void), ps2_clouds_far_rq_one_alt(void);   /* (rq8_onelayer: bit 5 PS2_RQ_CLOUDS_ONE with _ONE_FD=1, the quad at the near plane) */
        extern int ps2_clouds_far_rq_one_fd(void), ps2_clouds_far_rq_one_ab(void), ps2_rq_ab_left_out(void);
        int fd = ps2_clouds_far_rq_one_fd();
        if (ps2_clouds_far_rq_one_ab() == 4 && rqcl.pipe_ab && ps2_rq_ab_left_out()) fd = !fd;   /* (_ONE_AB=4: the left-out blocks the other way) */
        if (ps2_clouds_far_rq_one() && fd) u[47] = (float)((int)u[47] | 32);
        else if (ps2_clouds_far_rq_one_alt()) u[47] = (float)((int)u[47] & ~2); }   /* (ONE_ALT's t12b frames: no SOFT) */
    if (rqcl_fold() && !rqart_off(2)) u[47] = (float)((int)u[47] | (rqcl_fold() == 2 ? 128 : 64));   /* (rq10_artefacts, PS2_RQ_CLOUDS_FOLD: misc.w bit 6 the cap, 7 the face) */
    if (rqcut_comp() && !rqcut_odd()) u[47] = (float)((int)u[47] | 256);   /* (rq13_cut, PS2_RQ_CLOUDS_CUT: misc.w bit 8, the far tiles at the far plane) */
    rqcut_block(fr);                                              /* (PS2_RQ_CLOUDS_CUT: the objects' block, rq_cut.glsl) */
    {   u32 cols = (lw + 7u) / 8u, rows = (lh + 7u) / 8u;             /* the near cells (rq_clouds_near.vert) */
        rqcl.ncells = cols * rows;
        u[48] = (float)cols; u[49] = (float)rows; u[50] = (float)(8u * div); u[51] = 0;
        u[52] = rqcl.nuv[0]; u[53] = rqcl.nuv[1]; u[54] = 0;
        u[55] = 4000.0f; }                                            /* (rq_clouds.vert nuv.w: FARWARP's distance where a corner has no cloud near) */
    if (u != um) { memcpy(um, u, 20u * sizeof(float)); memcpy(um + 26, u + 26, 30u * sizeof(float)); }
    return 1;
}
/* inside the eye pass, at the place of the old layer draw */
static int rqcl_skip;   /* (PS2_RQ_SKIPC=2 / 3 in its left-out blocks, rq_video.inc rql_cl_draw: 1 = no far tiles, 2 = no near cells) */
static void rq_clouds_composite(VkCommandBuffer cmd, vk_frame *fr) {
    if (!rqcl_pipe() || !rqcl.cset) return;
    VkViewport vp; memset(&vp, 0, sizeof vp); vp.width = (float)rqe.w; vp.height = (float)rqe.h; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    /* dynamic resolution: every draw reaches the centred rectangle through rqe.xf (the per-view crop scaled about the image's
       centre, xf[0..3] x vw / W, xf[4..5] x vh / H); the viewport stays the whole image. The scale is read back from xf against
       the crop it was made from, so this stays right whatever owns the rectangle. The vertex shader takes the crop from here, the
       fragment shader undoes it the same way (rq_clouds.frag), so the depth and near-buffer lookups follow the rectangle. */
    float sx = rqe.xf[0] * (rqe.crop[0][1] - rqe.crop[0][0]), sy = rqe.xf[4] * (rqe.crop[0][3] - rqe.crop[0][2]);
    if (!(sx > 0.0f) || sx > 1.0f) sx = 1.0f;
    if (!(sy > 0.0f) || sy > 1.0f) sy = 1.0f;
    u32 vw = (u32)ceilf(sx * (float)rqe.w), vh = (u32)ceilf(sy * (float)rqe.h);
    if (vw > rqe.w) vw = rqe.w;
    if (vh > rqe.h) vh = rqe.h;
    VkRect2D sc; memset(&sc, 0, sizeof sc); sc.offset.x = (s32)((rqe.w - vw) / 2u); sc.offset.y = (s32)((rqe.h - vh) / 2u); sc.extent.width = vw; sc.extent.height = vh;
    vkCmdSetScissor(cmd, 0, 1, &sc);
    {   float *u = (float *)(rqcl.ubo_map + 256u * (uint32_t)(fr - frames));
        memcpy(&u[20], rqe.xf, 4 * sizeof(float)); u[24] = rqe.xf[4]; u[25] = rqe.xf[5]; }
    static int dbg = -1; if (dbg < 0) { const char *e = getenv("PS2_RQ_CLOUDS_VIEW"); dbg = e ? atoi(e) : 0; }   /* [debug] 1 far only, 2 near only, 3 far tile ids (flat colours; the B.5 gate's tile borders),
                                                                   4 (clouds6) neither composite drawn, the marches run: the scene without our clouds, the A.7 gate's mask base */
    {   float *u = (float *)(rqcl.ubo_map + 256u * (uint32_t)(fr - frames)); u[46] = (float)dbg; }
    VkDescriptorSet s[2] = { rqcl.cset, rqcl.set[fr - frames] };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqcl.layout, 0, 2, s, 0, NULL);
    float pc[16];
    memcpy(pc, rqe.xf, 8 * sizeof(float));                        /* rqx, rqy, rqsz */
    pc[8] = rqcl.nuv[0]; pc[9] = rqcl.nuv[1]; pc[10] = rqcl.nsz[0]; pc[11] = rqcl.nsz[1]; pc[12] = rqcl.ndiv;
    pc[13] = rqz_cfg() ? 1.0f : 0.0f;                             /* (PS2_RQ_ZPRE, rq_clouds_near.frag -DEDGE: the depth's cockpit band is not an occluder) */
    pc[14] = (float)rqcl_flags();                                 /* (bit 0 PS2_RQ_CLOUDS_NEAR_BILAT: rq_clouds_near.frag -DEDGE) */
    {   extern float ps2_clouds_far_rq_split(void); pc[15] = ps2_clouds_far_rq_split(); }   /* (rq8_onelayer, PS2_RQ_CLOUDS_ONE: the front part's end, rq_clouds_fd.frag) */
    vkCmdPushConstants(cmd, rqcl.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    /* the far part first (one read a fragment; the tile quads' depth cuts the mountains), then the near cells over it (dst = L + dst T):
       the plain ones, then those with the scene inside the near range */
    VkPipeline fp_ = rqcl.pipe;
    {   extern int ps2_clouds_far_rq_one(void), ps2_clouds_far_rq_one_alt(void);   /* ([test] PS2_RQ_CLOUDS_ONE_ALT: t12b's frames without SOFT) */
        if (rqcl.pipe_ns && ps2_clouds_far_rq_one_alt() && !ps2_clouds_far_rq_one()) fp_ = rqcl.pipe_ns;
        extern int ps2_clouds_far_rq_one_ab(void), ps2_rq_ab_left_out(void);
        if (rqcl.pipe_ab && ps2_clouds_far_rq_one() && ps2_rq_ab_left_out()) fp_ = rqcl.pipe_ab; }   /* (round 2, [test] PS2_RQ_CLOUDS_ONE_AB 1 / 4) */
    if (rqcl.pipe_cut0 && rqart_off(4)) fp_ = rqcl.pipe_cut0;   /* ([test] PS2_RQ_ART_ALT: this frame without the fixes) */
    if (rqcl.pipe_cutalt && rqcut_odd()) fp_ = rqcl.pipe_cutalt;  /* ([test] PS2_RQ_CLOUDS_CUT_ALT: this frame t13c's) */
    if (dbg != 2 && dbg != 4 && rqcl_skip != 1) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fp_); vkCmdDrawIndirect(cmd, rqcl.list, 64, 1, 16); }
    if (rqcl.near_on && dbg != 1 && dbg != 3 && dbg != 4 && rqcl.ncells && rqcl_skip != 2) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqcl.pn); vkCmdDraw(cmd, 6, rqcl.ncells, 0, 0);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqcl.pne); vkCmdDraw(cmd, 6, rqcl.ncells, 0, 0);
    }
}

/* ---- PS2_RQ_CLOUDS_CUT (see rqcut_cfg): the objects' block, the draws' bits, the eye pass's shader twins, the far map in the mesh set ---- */
static int rqfx_far_plane(const vk_draw *d);            /* (rq_fxfade.inc) */
static struct { u64 frames, tl_n, t_n, tl_g, t_g, g_nz, g_far, n_skip, n_flat, n_band, n_below, n_above; float eye_y, base, top; } rqcs;   /* the census, `rq cut:` every 600 eye frames */
/* rqcl_prepare: the block of rq_cut.glsl (this frame's view: the camera's axes in cloud space, the projection, the GS depth's constants,
   units a metre, the map's used size, the split) in the frame's mesh blocks; the census line */
static void rqcut_block(vk_frame *fr) {
    if (!rqcut_live() || !fr->mcptr) return;
    extern float ps2_clouds_far_rq_split(void);
    const ps2_cloud_view *v = &cl_view;
    float q[7][4]; memset(q, 0, sizeof q);
    for (int i = 0; i < 3; i++) { q[0][i] = v->cam_x[i]; q[1][i] = v->cam_y[i]; q[2][i] = v->cam_z[i]; }
    q[0][3] = ps2_clouds_units_per_m(); q[1][3] = (float)ps2_clouds_rq_map_n(); q[2][3] = ps2_clouds_far_rq_split();
    memcpy(q[3], v->proj, sizeof q[3]);
    q[4][0] = v->depth_a; q[4][1] = v->depth_c; q[4][2] = v->depth_mh; q[4][3] = v->depth_zmax;
    q[5][0] = (float)rqcut_cfg(); q[5][1] = (float)rqcut_parts();
    {   ps2_cloud_env env; memset(&env, 0, sizeof env);         /* (round 2: the band test's q6 -- the eye's altitude, the band, on) */
        if (rqcut_skip() && ps2_cloud_env_read(&env) && (env.valid & CL_ENV_PROFILE) && env.top_m > env.base_m) {
            q[6][0] = (float)v->origin_m[1]; q[6][1] = env.base_m; q[6][2] = env.top_m; q[6][3] = 1.0f;
            rqcs.eye_y = q[6][0]; rqcs.base = env.base_m; rqcs.top = env.top_m; rqcs.n_band++;
        } }
    {   /* [test] PS2_RQ_CLOUDS_CUT_MODEL=1: the share in front as SOFT's own (linear in the opacity: T_d = 1 - k (1 - T)) instead of Beer-Lambert
           inside each span -- the look against t13c's composite where its coarse depth was right (an object's inside, far from any edge) */
        static int m = -1; if (m < 0) { const char *e = getenv("PS2_RQ_CLOUDS_CUT_MODEL"); m = e && *e == '1'; if (m) ps2_log("rq clouds: [test] PS2_RQ_CLOUDS_CUT_MODEL=1: the objects' share linear in the opacity (SOFT's)"); }
        q[5][2] = (float)m; }
    memcpy((ps2_vk_mesh_block *)fr->mcptr + RQC_BLOCK, q, sizeof q);
    if (q[6][3] > 0.5f) { if (q[6][0] < q[6][1] - 100.0f) rqcs.n_below++; else if (q[6][0] > q[6][2] + 100.0f) rqcs.n_above++; }
    if (!(++rqcs.frames % 600u)) {
        double n = 600.0;
        ps2_log("rq cut: %llu eye frames: a frame %.1f native draws with T and L (%.1f T only), %.1f GS draws with T and L (%.1f T only); "
                "left as they were: %.1f GS world draws without a depth write before the composite, %.1f at the far plane, %.1f native (programs 4, 5)",
                (unsigned long long)rqcs.frames, (double)rqcs.tl_n / n, (double)rqcs.t_n / n, (double)rqcs.tl_g / n, (double)rqcs.t_g / n,
                (double)rqcs.g_nz / n, (double)rqcs.g_far / n, (double)rqcs.n_skip / n);
        ps2_log("rq cut: the band test (PS2_RQ_CLOUDS_CUT_SKIP): %llu of 600 frames with the band, the eye under its base by 100 m in %llu, over its top in %llu "
                "(now: eye %.0f m, band %.0f .. %.0f m); %.1f GS cards a frame flat", (unsigned long long)rqcs.n_band, (unsigned long long)rqcs.n_below,
                (unsigned long long)rqcs.n_above, (double)rqcs.eye_y, (double)rqcs.base, (double)rqcs.top, (double)rqcs.n_flat / n);
        rqcs.tl_n = rqcs.t_n = rqcs.tl_g = rqcs.t_g = rqcs.g_nz = rqcs.g_far = rqcs.n_skip = rqcs.n_flat = rqcs.n_band = rqcs.n_below = rqcs.n_above = 0;
    }
}
/* ps2_video.c render_list (the hook on the line before rqo_pm's): the draw's bits in mesh_pad / mv_flags. A draw of the open eye pass into
   slot 0, this frame with the clouds' compute: native meshes of programs 0-3 (the terrain by part 1, the others by part 2), and GS world
   draws that write depth and do not stand at the far plane (part 4). T and L where the draw's blend gives the source its share
   (no blend; (Cs - Cd) C + Cd; (Cd - Cs) C + Cs), T only for the rest (additive, subtractive: the light belongs to what lies under them).
   The GS draws without a depth write keep what they had: the effects after the composite PS2_RQ_CLOUDS_FXFADE's fade, those before it
   the composite where nothing stands */
u32 ps2_rq_cut_bits(const void *dv) {
    const vk_draw *d = (const vk_draw *)dv;
    if (!rq_open || !rql.cl_ok || !rqcut_live() || rqcut_odd() || d->rt != 0u || d->cockpit) return 0u;
    u32 b = d->blend, a = b & 3u, bb = (b >> 2) & 3u, dd = (b >> 6) & 3u;
    int share = !(b & 0x100u) || (a == 0u && bb == 1u && dd == 1u) || (a == 1u && bb == 0u && dd == 0u);
    if (d->native == PS2_VK_NATIVE_MESH) {
        if (!rep_mesh || d->mblock >= rep_mesh->nblocks) return 0u;
        u32 prog = rep_mesh->blocks[d->mblock].prog;
        if (prog >= 4u) { rqcs.n_skip++; return 0u; }
        if (!(rqcut_parts() & (prog == 1u ? 1 : 2))) return 0u;
        if (share) rqcs.tl_n++; else rqcs.t_n++;
        return share ? RQC_TL_BIT : RQC_T_BIT;
    }
    if (!(rqcut_parts() & 4) || d->xcls != 1u) return 0u;
    if (!((d->depth >> 2) & 1u)) { if (!rql.cl_drawn) rqcs.g_nz++; return 0u; }
    if (rqfx_far_plane(d)) { rqcs.g_far++; return 0u; }
    if (share) rqcs.tl_g++; else rqcs.t_g++;
    {   u32 f = d->frag_flags, card = ((f >> 3) & 1u) && ((f >> 4) & 7u) != 1u;   /* (round 2: an alpha test that can fail -- a card: one value, flat) */
        if (card) rqcs.n_flat++;
        return (share ? RQC_TL_BIT : RQC_T_BIT) | (card ? RQC_FLAT_BIT : 0u); }
}
/* the mesh fragment shaders of the eye pass's non-terrain native meshes (the PC's mesh_mv.frag / mesh_b_mv.frag, shared with the old path):
   their twins with the vertex's cloud applied, only behind the twins of the eye pass's mesh vertex shaders (ps2_video.c get_pipeline_ex,
   the hook on the PS2_RQ_GS_NOSPEC line) */
static struct { int on; VkShaderModule mfs, mbfs, vs[4]; } rqcm;
VkShaderModule ps2_rq_cut_fs(VkShaderModule vs, VkShaderModule fs) {
    if (!rqcm.on || !vs || (vs != rqcm.vs[0] && vs != rqcm.vs[1] && vs != rqcm.vs[2] && vs != rqcm.vs[3])) return fs;
    if (fs == mesh_fs && rqcm.mfs) return rqcm.mfs;
    if (fs == rnb.fs[0] && rqcm.mbfs) return rqcm.mbfs;
    return fs;
}
/* rq_load_shaders: with the switch the twins cut_<name>.spv (rq_cut.glsl, CMakeLists.txt) in place of the eye pass's modules -- all of them
   or none (a fragment twin reads what only the vertex twins write): a missing twin leaves the switch off for the run */
static void rqcut_load(const char *base) {
    if (!rqcut_cfg()) return;
    static const struct { VkShaderModule *m; const char *n; } S[] = {
        { &rqv_gs, "gs_rq_mv.vert" }, { &rqv_mesh, "mesh_rq_mv.vert" }, { &rqv_mesh_zc, "mesh_zc_rq_mv.vert" }, { &rqv_b, "mesh_rq_b_mv.vert" },
        { &rqv_b_zc, "mesh_zc_rq_b_mv.vert" }, { &rqf_gl, "rq_gs.frag" }, { &rqf_gt, "rq_gs_tree.frag" }, { &rqf_gt_nm, "rq_gs_tree_nm.frag" }, { &rqf_gt_sd, "rq_gs_tree_sd.frag" }, { &rqf_gt_nmm, "rq_gs_tree_nm_mip.frag" }, { &rqf_gt_sdm, "rq_gs_tree_sd_mip.frag" }, { &rqf_gd, "rq_gs_date.frag" }, { &rqf_gf, "rq_gs_flat.frag" },
        { &rqf_gd_df[0], "rq_gs_date_df0.frag" }, { &rqf_gd_df[1], "rq_gs_date_df1.frag" },
        { &rqt_v[0], "mesh_rqt_mv.vert" }, { &rqt_v[1], "mesh_zc_rqt_mv.vert" }, { &rqt_v[2], "mesh_rq_rqt_mv.vert" }, { &rqt_v[3], "mesh_zc_rq_rqt_mv.vert" },
        { &rqt_bv[0], "mesh_rqt_b_mv.vert" }, { &rqt_bv[1], "mesh_zc_rqt_b_mv.vert" }, { &rqt_bv[2], "mesh_rq_rqt_b_mv.vert" }, { &rqt_bv[3], "mesh_zc_rq_rqt_b_mv.vert" },
        { &rqt_lean, "rq_terrain_mv.frag" }, { &rqt_leanb, "rq_terrain_b_mv.frag" }, { &rqo_lean, "rq_terrain_o_mv.frag" }, { &rqo_leanb, "rq_terrain_o_b_mv.frag" },
        { &rqo_wat, "rq_water_o_mv.frag" }, { &rqo_watb, "rq_water_o_b_mv.frag" },
        { &rqt_f[0], "mesh_rqt_mv.frag" }, { &rqt_f[1], "mesh_wat_rqt_mv.frag" }, { &rqt_f[2], "mesh_det_rqt_mv.frag" }, { &rqt_f[3], "mesh_watdet_rqt_mv.frag" },
        { &rqt_f[4], "mesh_tp_rqt_mv.frag" }, { &rqt_f[5], "mesh_wattp_rqt_mv.frag" }, { &rqt_f[6], "mesh_dettp_rqt_mv.frag" }, { &rqt_f[7], "mesh_watdettp_rqt_mv.frag" },
        { &rqt_f[8], "mesh_city_rqt_mv.frag" },
        { &rqt_bf[0], "mesh_rqt_b_mv.frag" }, { &rqt_bf[1], "mesh_wat_rqt_b_mv.frag" }, { &rqt_bf[2], "mesh_det_rqt_b_mv.frag" }, { &rqt_bf[3], "mesh_watdet_rqt_b_mv.frag" },
        { &rqt_bf[4], "mesh_tp_rqt_b_mv.frag" }, { &rqt_bf[5], "mesh_wattp_rqt_b_mv.frag" }, { &rqt_bf[6], "mesh_dettp_rqt_b_mv.frag" }, { &rqt_bf[7], "mesh_watdettp_rqt_b_mv.frag" },
        { &rqt_bf[8], "mesh_city_rqt_b_mv.frag" },
    };
    enum { NS = sizeof S / sizeof S[0] };
    VkShaderModule got[NS]; char spv[512]; int n = 0, miss = 0;
    for (u32 i = 0; i < NS; i++) {
        snprintf(spv, sizeof spv, "%s/cut_%s.spv", base, S[i].n);
        got[i] = load_spv(spv);
        if (got[i]) n++; else { miss++; ps2_log("rq clouds: PS2_RQ_CLOUDS_CUT: %s missing", spv); }
    }
    snprintf(spv, sizeof spv, "%s/cut_mesh_mv.frag.spv", base); rqcm.mfs = load_spv(spv);
    snprintf(spv, sizeof spv, "%s/cut_mesh_b_mv.frag.spv", base); rqcm.mbfs = load_spv(spv);
    if (miss || !rqcm.mfs || !rqcm.mbfs) {
        for (u32 i = 0; i < NS; i++) if (got[i]) vkDestroyShaderModule(dev, got[i], NULL);
        if (rqcm.mfs) vkDestroyShaderModule(dev, rqcm.mfs, NULL);
        if (rqcm.mbfs) vkDestroyShaderModule(dev, rqcm.mbfs, NULL);
        rqcm.mfs = rqcm.mbfs = VK_NULL_HANDLE; rqcut_off = 1;
        ps2_log("rq clouds: PS2_RQ_CLOUDS_CUT: %d of the eye pass's shader twins missing: off for this run", miss + !rqcm.mfs + !rqcm.mbfs);
        return;
    }
    for (u32 i = 0; i < NS; i++) { *S[i].m = got[i]; rqm_name(got[i], S[i].n); }
    rqm_name(rqcm.mfs, "mesh_mv.frag (cut)"); rqm_name(rqcm.mbfs, "mesh_b_mv.frag (cut)");
    rqcm.vs[0] = rqv_mesh; rqcm.vs[1] = rqv_mesh_zc; rqcm.vs[2] = rqv_b; rqcm.vs[3] = rqv_b_zc; rqcm.on = 1;
    ps2_log("rq clouds: PS2_RQ_CLOUDS_CUT: the eye pass's %d shader twins and the native meshes' two fragment twins loaded", n);
}
/* the mesh set (ps2_video.c mesh_slayout, the hooks PS2_RQ_MSET_*): bindings 9 (the far map) and 10 (its profile), linear, clamp, for the
   vertex and fragment stages, only with the switch */
int ps2_rq_mset_extra(void) { return rqcut_cfg() != 0; }
void ps2_rq_mset_bind(VkDescriptorSetLayoutBinding *mb, uint32_t *count) {
    if (!rqcut_cfg()) return;
    for (int k = 0; k < 2; k++) {
        VkDescriptorSetLayoutBinding *b = &mb[*count]; memset(b, 0, sizeof *b);
        b->binding = 9u + (u32)k; b->descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b->descriptorCount = 1;
        b->stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        (*count)++;
    }
}
/* frame_cmd_begin (the slot's last use is done; nothing of this command buffer is recorded yet): the slot's mesh set takes the far map's
   views once they exist (they are made once, by the clouds' first frame) */
static VkImageView rqcut_bound[FRAMES_IN_FLIGHT][2];
void ps2_rq_cut_frame(VkDescriptorSet set, int fi) {
    if (!rqcut_cfg() || fi < 0 || fi >= (int)FRAMES_IN_FLIGHT || !set) return;
    VkImageView cv = VK_NULL_HANDLE, pv = VK_NULL_HANDLE; VkSampler cs = VK_NULL_HANDLE, ps = VK_NULL_HANDLE; float sc = 1.0f;
    extern int ps2_clouds_dir_hook(VkImageView *view, VkSampler *samp, float *uv_scale);
    if (!ps2_clouds_dir_hook(&cv, &cs, &sc) || !cv) return;
    ps2_clouds_rq_prof(&pv, &ps);
    if (!pv) return;
    if (rqcut_bound[fi][0] == cv && rqcut_bound[fi][1] == pv) return;
    VkDescriptorImageInfo ii[2]; memset(ii, 0, sizeof ii);
    ii[0].sampler = cs; ii[0].imageView = cv; ii[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ii[1].sampler = ps2_clouds_rq_lin_sampler(); ii[1].imageView = pv; ii[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet w[2]; memset(w, 0, sizeof w);
    for (int k = 0; k < 2; k++) {
        w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[k].dstSet = set; w[k].dstBinding = 9u + (u32)k; w[k].descriptorCount = 1;
        w[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[k].pImageInfo = &ii[k];
    }
    vkUpdateDescriptorSets(dev, 2, w, 0, NULL);
    rqcut_bound[fi][0] = cv; rqcut_bound[fi][1] = pv;
    ps2_log("rq clouds: PS2_RQ_CLOUDS_CUT: frame slot %d's mesh set takes the far map (bindings 9, 10)", fi);
}
