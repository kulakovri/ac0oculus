/* rq_video.inc -- the native Quest renderer, stage R1 step 3 (NOTES_RQ.md; included by ps2_video.c, PS2_RQ_SUPPORT only).
   PS2_RQ=eye: a head-tracked flight frame draws slot 0's whole content (one run, fg_hoist with the slot-0 reads fed by
   rq_feed) into ONE core render pass at the eye swapchain's size: 2 layers (multiview 0b11), 4x MSAA colour and depth,
   resolved into the swapchain image (a UNORM view of chain 6) and a single-sample depth. The game's symmetric frame is
   mapped to each eye by the per-view crop in the vertex shaders (-DRQ, pc.rq[]). After the pass rq_ds draws the 1x feed
   (colour and depth) from the eye images for the next frame's reads of slot 0. A frame the hoist or the swapchain cannot
   serve falls back to the old path. */
#include "rq_clouds.inc"
extern int ps2_xr_rq_size(uint32_t *w, uint32_t *h);
extern int ps2_xr_rq_acquire(u32 seq, uint32_t *idx, VkImage *img, VkImageView *view, float crop[2][4]);
extern int ps2_xr_rq_fdm(void);
extern VkImageView ps2_xr_rq_fdm_view(uint32_t idx);
extern int ps2_xr_rq_sc_sub(void);              /* (chain 6 is subsampled: PS2_XR_SC_SUBSAMPLED, note 5 step B) */
static VkShaderModule rqtp_fs;                     /* (PS2_RQ_TESTPAT) */
static VkShaderModule rqz_am_fs;                   /* (PS2_RQ_ZPRE_AMASK: rq_amask.frag) */
static u32 rqtp_seq;
extern void ps2_xr_rq_present(u32 seq);
extern void ps2_xr_rq_rect(int32_t x, int32_t y, int32_t w, int32_t h);
/* [test] PS2_XR_DUMP_AT="a[-b[/s]],..." (rq10_artefacts 2026-11-15; ps2_vk.h PS2_RQ_DUMPAT_*): eye dumps at a list of fields and ranges
   (every s fields, default 1; at most 64 entries) instead of PS2_XR_DUMP_EYES / _COUNT / _EVERY -- the first frame at or after each listed
   field. The next field due after `after`: -1 without the list, LONG_MAX when none is left */
/* PS2_RQ_RTFIT_1X (rq13_cut round 4, BUGS_RQ R-123; default 1, `0` = as before): create_targets gives each slot its extent for the new
   size before it sets the new scale, so rtz_resize_extent tested the OLD one. Going from 2x to 1x (PS2_RQ_FLAT_SCALE: the menus and static
   screens at 2x, the flight at 1x) the slots kept extents fitted to the 2x session's draws, and at 1x PS2_PERF_RT_FIT does not run
   (rtz_active: rt_scale > 1), so they never grew back. M18's V2 cutscene (the user's W-6) reads slot 6 -- fitted to 128 x 96 in the
   boot's menus -- through gs.frag's normalised path: its coordinates up to 256 / 128 = 2.0 under a repeat sampler, the 2 x 2 tiles of a
   half-size picture, dark (the writes beyond the extent clipped). Here: at a new scale of 1 every slot full size */
void ps2_rq_rtz_new_scale(unsigned s, unsigned short *ew, unsigned short *eh) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("PS2_RQ_RTFIT_1X"); on = !(e && *e == '0'); }
    static int nlog;
    if (on && s <= 1u && (*ew || *eh)) {
        if (nlog++ < 24) ps2_log("vk: PS2_RQ_RTFIT_1X: a slot's extent %ux%u fitted at the old scale dropped -- full size at 1x (R-123)", (unsigned)*ew, (unsigned)*eh);
        *ew = *eh = 0;
    }
}
long ps2_rq_dumpat_last = -1;
volatile int ps2_vk_eyedump_req;                   /* ([test] ACZ_TEST_PAD_ON's DUMP, zero_vr.c: one dump at the next frame; ps2_vk.h) */
#include "rq_empty.inc"                            /* (PS2_RQ_EMPTY_HOLD, R-290: an empty list after an eye-pass list changes nothing) */
long ps2_rq_dumpat_next(long after) {
    static int n = -1; static long a[64], b[64], s[64];
    if (n < 0) {
        n = 0;
        const char *e = getenv("PS2_XR_DUMP_AT");
        for (const char *p = e; p && *p && n < 64; ) {
            char *q; long x = strtol(p, &q, 10), y = x, st = 1;
            if (q == p) break;
            if (*q == '-') { y = strtol(q + 1, &q, 10); }
            if (*q == '/') { st = strtol(q + 1, &q, 10); }
            if (st < 1) st = 1;
            if (y < x) y = x;
            a[n] = x; b[n] = y; s[n] = st; n++;
            if (*q != ',' && *q != '+') break;                         /* ('+' too: step1.sh's rows split their variables at commas) */
            p = q + 1;
        }
        if (n) ps2_log("eye dumps: [test] PS2_XR_DUMP_AT: %d entries, the first at field %ld", n, a[0]);
    }
    if (!n) return -1;
    long best = LONG_MAX;
    for (int i = 0; i < n; i++) {
        long f = a[i];
        if (f <= after) f = a[i] + ((after - a[i]) / s[i] + 1) * s[i];
        if (f <= b[i] && f < best) best = f;
    }
    return best;
}

/* ---- PS2_RQ_MODLOG [test]: per-list draw counts of the eye pass by fragment module ---- */
#define RQM_PIPES 8192u
#define RQM_MODS 200u
static struct { VkPipeline p; VkShaderModule m; } rqm_p[RQM_PIPES];
static u32 rqm_np;
static struct { VkShaderModule m; const char *n; u64 draws, verts; } rqm_mod[RQM_MODS];
static u32 rqm_nm;
static u64 rqm_other_d, rqm_other_v;
static u64 rqm_cls[256];                          /* the merged patches by class byte (rq_order.inc rqo_class) */
static float rqm_wnear = 1e30f;                   /* the nearest water patch of the window (units) */
static u64 rqm_kind[8];                           /* the terrain's first passes by what rqo_group chose: plain, water merged, materials, water + materials, terrain pack, city, water unmerged, other */
static int rqm_cfg(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_MODLOG"); v = e && *e == '1'; }
    return v;
}
static void rqm_name(VkShaderModule m, const char *n) { if (m && rqm_nm < RQM_MODS) { rqm_mod[rqm_nm].m = m; rqm_mod[rqm_nm].n = n; rqm_nm++; } }
static void rqm_pipe(VkPipeline p, VkShaderModule m) {
    if (!rqm_cfg() || !p) return;
    u32 i = __atomic_fetch_add(&rqm_np, 1u, __ATOMIC_ACQ_REL);
    if (i < RQM_PIPES) { rqm_p[i].m = m; __atomic_store_n(&rqm_p[i].p, p, __ATOMIC_RELEASE); }
}
static void rqm_draw(VkPipeline p, u32 verts) {
    if (!rqm_cfg()) return;
    static VkPipeline last; static int last_j = -2;
    if (p != last) {
        u32 n = __atomic_load_n(&rqm_np, __ATOMIC_ACQUIRE); VkShaderModule m = VK_NULL_HANDLE;
        if (n > RQM_PIPES) n = RQM_PIPES;
        for (u32 i = 0; i < n; i++) if (__atomic_load_n(&rqm_p[i].p, __ATOMIC_ACQUIRE) == p) { m = rqm_p[i].m; break; }
        last_j = -1;
        if (m) for (u32 j = 0; j < rqm_nm; j++) if (rqm_mod[j].m == m) { last_j = (int)j; break; }
        last = p;
    }
    if (last_j >= 0) { rqm_mod[last_j].draws++; rqm_mod[last_j].verts += verts; }
    else { rqm_other_d++; rqm_other_v += verts; }
}
static void rqm_list(void) {    /* once per list: every 600 lists the per-list averages, the heaviest module first */
    static u32 n;
    if (!rqm_cfg() || ++n < 600u) return;
    for (;;) {
        int b = -1; u64 bv = 0;
        for (u32 j = 0; j < rqm_nm; j++) if (rqm_mod[j].draws && rqm_mod[j].verts >= bv) { b = (int)j; bv = rqm_mod[j].verts; }
        if (b < 0) break;
        ps2_log("rq-mod: %-28s %7.1f draws %9.1f vertices a list", rqm_mod[b].n, (double)rqm_mod[b].draws / (double)n, (double)rqm_mod[b].verts / (double)n);
        rqm_mod[b].draws = 0; rqm_mod[b].verts = 0;
    }
    if (rqm_other_d) ps2_log("rq-mod: %-28s %7.1f draws %9.1f vertices a list", "(not an rq module: a PC shader)", (double)rqm_other_d / (double)n, (double)rqm_other_v / (double)n);
    ps2_log("rq-mod: terrain patches a list: plain %.1f, water merged %.1f, materials (PC twin) %.1f, water + materials (PC twin) %.1f, terrain pack %.1f, city %.1f, water unmerged %.1f, other %.1f", (double)rqm_kind[0] / (double)n, (double)rqm_kind[1] / (double)n, (double)rqm_kind[2] / (double)n, (double)rqm_kind[3] / (double)n, (double)rqm_kind[4] / (double)n, (double)rqm_kind[5] / (double)n, (double)rqm_kind[6] / (double)n, (double)rqm_kind[7] / (double)n);
    memset(rqm_kind, 0, sizeof rqm_kind);
    for (int t = 0; t < 8; t++) {                 /* (bits: 1 detail, 2 layer, 4 haze, 8 non-opaque texels, 16 water beyond its fade, 32 layer per vertex, 64 far water, 128 materials) */
        int b = -1; u64 bv = 0;
        for (int c = 0; c < 256; c++) if (rqm_cls[c] > bv) { b = c; bv = rqm_cls[c]; }
        if (b < 0) break;
        ps2_log("rq-mod: merged class 0x%02X: %.1f patches a list", (unsigned)b, (double)bv / (double)n);
        rqm_cls[b] = 0;
    }
    memset(rqm_cls, 0, sizeof rqm_cls);
    if (rqm_wnear < 1e29f) ps2_log("rq-mod: nearest water patch of the window %.0f m", (double)rqm_wnear / 6.0);
    rqm_wnear = 1e30f;
    rqm_other_d = 0; rqm_other_v = 0; n = 0;
}
/* PS2_RQ_MP=1 (default off): the relaxed-precision twins of the eye pass's small shaders (-DMP) */
static void rqm_load(const char *base) {
    static const struct { VkShaderModule *m; const char *n; } S[] = { { &rqf_gl, "rq_gs_mp.frag" }, { &rqf_gd, "rq_gs_date_mp.frag" }, { &rqf_gf, "rq_gs_flat_mp.frag" },
        { &rqf_lc, "rq_layer_cl_mp.frag" }, { &rqf_lp, "rq_layer_cp_mp.frag" } };
    const char *e = getenv("PS2_RQ_MP");
    char spv[512];
    if (!e || *e != '1') return;
    ps2_log("vk: PS2_RQ_MP=1: the eye pass's small shaders with relaxed precision");
    for (u32 i = 0; i < sizeof S / sizeof S[0]; i++) {
        snprintf(spv, sizeof spv, "%s/%s.spv", base, S[i].n);
        VkShaderModule m = load_spv(spv);
        if (m) *S[i].m = m; else ps2_log("vk: PS2_RQ_MP: %s missing", spv);
    }
}
static VkShaderModule rqtm_cs;                     /* (rq_treemip.comp: PS2_RQ_TREE_AA bit 16, rq_treemip.inc) */
static void rqtm_build(VkCommandBuffer cmd);       /* (rq_treemip.inc: the tree impostors' mip chains, before the eye pass) */
static void rq_load_shaders(const char *base) {
    static const struct { VkShaderModule *m; const char *n; } S[] = {
        { &rqv_gs, "gs_rq_mv.vert" }, { &rqv_mesh, "mesh_rq_mv.vert" }, { &rqv_mesh_zc, "mesh_zc_rq_mv.vert" },
        { &rqv_b, "mesh_rq_b_mv.vert" }, { &rqv_b_zc, "mesh_zc_rq_b_mv.vert" },
        { &rqf_gs, "gs_rq_mv.frag" }, { &rqf_lr, "gs_rq_mv_lr.frag" }, { &rqv_ds, "rq_ds.vert" }, { &rqf_ds, "rq_ds.frag" }, { &rqf_gr, "rq_gs_rt.frag" }, { &rqf_gy, "rq_gs_sky.frag" }, { &rqf_gt, "rq_gs_tree.frag" }, { &rqf_gt_nm, "rq_gs_tree_nm.frag" }, { &rqf_gt_sd, "rq_gs_tree_sd.frag" }, { &rqf_gt_nmm, "rq_gs_tree_nm_mip.frag" }, { &rqf_gt_sdm, "rq_gs_tree_sd_mip.frag" }, { &rqh_pvs, "rq_hud_p.vert" }, { &rqh_pfs, "rq_hud_p.frag" }, { &rqcl.vs, "rq_clouds.vert" }, { &rqcl.fs, "rq_clouds.frag" }, { &rqcl.fsd, "rq_clouds_fd.frag" }, { &rqcl.fss, "rq_clouds_fs.frag" }, { &rqcl.vn, "rq_clouds_near.vert" }, { &rqcl.fn, "rq_clouds_near.frag" }, { &rqcl.vne, "rq_clouds_near_e.vert" }, { &rqcl.fne, "rq_clouds_near_e.frag" },  { &rqf_gl, "rq_gs.frag" }, { &rqf_gd, "rq_gs_date.frag" }, { &rqf_gf, "rq_gs_flat.frag" }, { &rqf_lc, "rq_layer_cl.frag" }, { &rqf_lp, "rq_layer_cp.frag" }, { &rqh_vs, "rq_hud.vert" }, { &rqh_fs, "rq_hud.frag" }, { &rqz_vs, "rq_zpre.vert" }, { &rqz_ck_fs, "rq_zpre_ck.frag" }, { &rqz_ds_nz, "rq_ds_nz.frag" }, { &rqtp_fs, "rq_testpat.frag" }, { &rqz_am_fs, "rq_amask.frag" }, { &rqf_lr_ns, "gs_rq_mv_lr_ns.frag" }, { &rqf_gs_ns, "gs_rq_mv_ns.frag" }, { &rqf_lr_df[0], "gs_rq_mv_lr_df0.frag" }, { &rqf_lr_df[1], "gs_rq_mv_lr_df1.frag" }, { &rqf_gd_df[0], "rq_gs_date_df0.frag" }, { &rqf_gd_df[1], "rq_gs_date_df1.frag" },
        { &rqfx_cs, "rq_cloud_fxfade.comp" }, { &rqtm_cs, "rq_treemip.comp" },
        { &rqt_v[0], "mesh_rqt_mv.vert" }, { &rqt_v[1], "mesh_zc_rqt_mv.vert" }, { &rqt_v[2], "mesh_rq_rqt_mv.vert" }, { &rqt_v[3], "mesh_zc_rq_rqt_mv.vert" },
        { &rqt_f[0], "mesh_rqt_mv.frag" }, { &rqt_f[1], "mesh_wat_rqt_mv.frag" }, { &rqt_f[2], "mesh_det_rqt_mv.frag" }, { &rqt_f[3], "mesh_watdet_rqt_mv.frag" },
        { &rqt_f[4], "mesh_tp_rqt_mv.frag" }, { &rqt_f[5], "mesh_wattp_rqt_mv.frag" }, { &rqt_f[6], "mesh_dettp_rqt_mv.frag" }, { &rqt_f[7], "mesh_watdettp_rqt_mv.frag" },
        { &rqt_f[8], "mesh_city_rqt_mv.frag" }, { &rqt_lean, "rq_terrain_mv.frag" }, { &rqt_leanb, "rq_terrain_b_mv.frag" }, { &rqo_lean, "rq_terrain_o_mv.frag" }, { &rqo_leanb, "rq_terrain_o_b_mv.frag" }, { &rqo_wat, "rq_water_o_mv.frag" }, { &rqo_watb, "rq_water_o_b_mv.frag" },
        { &rqt_bv[0], "mesh_rqt_b_mv.vert" }, { &rqt_bv[1], "mesh_zc_rqt_b_mv.vert" }, { &rqt_bv[2], "mesh_rq_rqt_b_mv.vert" }, { &rqt_bv[3], "mesh_zc_rq_rqt_b_mv.vert" },
        { &rqt_bf[0], "mesh_rqt_b_mv.frag" }, { &rqt_bf[1], "mesh_wat_rqt_b_mv.frag" }, { &rqt_bf[2], "mesh_det_rqt_b_mv.frag" }, { &rqt_bf[3], "mesh_watdet_rqt_b_mv.frag" },
        { &rqt_bf[4], "mesh_tp_rqt_b_mv.frag" }, { &rqt_bf[5], "mesh_wattp_rqt_b_mv.frag" }, { &rqt_bf[6], "mesh_dettp_rqt_b_mv.frag" }, { &rqt_bf[7], "mesh_watdettp_rqt_b_mv.frag" },
        { &rqt_bf[8], "mesh_city_rqt_b_mv.frag" },
    };
    char spv[512];
    if (!vk_mv) return;
    for (u32 i = 0; i < sizeof S / sizeof S[0]; i++) {
        snprintf(spv, sizeof spv, "%s/%s.spv", base, S[i].n);
        *S[i].m = load_spv(spv);
        if (!*S[i].m) ps2_log("vk: PS2_RQ: %s missing", spv);
        rqm_name(*S[i].m, S[i].n);
    }
    rqc_load_shaders(base);
    rqb_load_shaders(base);
    rqm_load(base);
    rqcut_load(base);                                   /* (rq13_cut, PS2_RQ_CLOUDS_CUT: the eye pass's twins, rq_clouds.inc) */
}
static int rq_mk_img(u32 w, u32 h, VkFormat f, VkImageUsageFlags u, VkSampleCountFlagBits s, VkImageAspectFlags asp,
                     VkImage *im, VkDeviceMemory *m, VkImageView *v, int lazy) {
    VkImageCreateInfo ii; memset(&ii, 0, sizeof ii);
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = f;
    {   /* PS2_RQ_FDM_PLAIN4X=1 [test] (note 6 E3a, 2026-11-10): the 4x colour and depth of the foveated pass WITHOUT the subsampled
           flag (fragmentDensityMapNonSubsampledImages is 1): the in-pass subpassLoad then reads a plain attachment; a side experiment */
        static int p4 = -1;
        if (p4 < 0) { const char *e = getenv("PS2_RQ_FDM_PLAIN4X"); p4 = e && *e == '1'; if (p4) ps2_log("vk: PS2_RQ_FDM_PLAIN4X=1: the foveated pass's 4x attachments plain (note 6 E3a test)"); }
        if (rqe.fdm && s != VK_SAMPLE_COUNT_1_BIT && !p4) ii.flags = VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT;   /* (the 4x attachments of a pass with a density map) */
    }
    ii.extent.width = w; ii.extent.height = h; ii.extent.depth = 1; ii.mipLevels = 1; ii.arrayLayers = 2;
    ii.samples = s; ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.usage = u; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &ii, NULL, im) != VK_SUCCESS) return -1;
    VkMemoryRequirements r; vkGetImageMemoryRequirements(dev, *im, &r);
    u32 mt = lazy ? find_mem(r.memoryTypeBits, VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) : 0xFFFFFFFFu;
    if (mt == 0xFFFFFFFFu) mt = find_mem(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == 0xFFFFFFFFu) return -1;
    VkMemoryAllocateInfo ai; memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = r.size; ai.memoryTypeIndex = mt;
    if (vkAllocateMemory(dev, &ai, NULL, m) != VK_SUCCESS) return -1;
    if (vkBindImageMemory(dev, *im, *m, 0) != VK_SUCCESS) return -1;
    VkImageViewCreateInfo vi; memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = *im; vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; vi.format = f;
    vi.subresourceRange.aspectMask = asp; vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 2;
    if (vkCreateImageView(dev, &vi, NULL, v) != VK_SUCCESS) return -1;
    return 0;
}
/* ---- PS2_RQ_FDM_OWN (R-45, default 1 with PS2_RQ_FDM=1): our own density map, chain 6 plain ----
   With the runtime's map (XR_FB_foveation_vulkan, PS2_RQ_FDM_OWN=0) chain 6 is created with the density-map flag and its
   images are subsampled: a bin rendered at a lower density stores its reduced content in part of the bin and the rest keeps
   old bytes. The compositor decodes that layout; our own reads of the image do not (device, M1 field 9000, 2026-11-10: the
   1x feed rq_ds makes through the immutable subsampled sampler shows the same stale rectangles as the raw swapchain dump,
   mean difference 1.9 levels): the feed, and through it the wet canopy's world tap, the cockpit mark of PS2_RQ_ZPRE, the
   scene light and the game's slot-0 reads, take those bytes; the _sc dumps (a raw copy) show them too. Here the chain is
   plain (no density-map flag) and the pass's density map is ours: the 4x attachments stay subsampled (transient, never
   read outside the pass), the swapchain image is a non-subsampled attachment (fragmentDensityMapNonSubsampledImages: the
   driver writes every pixel at full size), so every reader -- the compositor, the feed, the recorder, the dumps -- sees an
   ordinary image. The map follows PS2_XR_FOVEATION (low / medium / high / high_top, PS2_XR_FOVEATION_OFFSET) in each eye's
   tangent space, per axis: full density inside a1 of the half-field, half to a2, a quarter beyond (high_top: the full area
   moved up by the offset). Made once, uploaded before the first pass that uses it. */
/* (R-45 round 3) memory for our own images that live beyond one pass (the density maps, the dump's decode image): the driver's wish for a
   dedicated allocation honoured (VkMemoryDedicatedRequirements -> VkMemoryDedicatedAllocateInfo, always given), a device-local type that is not
   lazily allocated, preferably not host-visible. Our density maps were bound with find_mem's first device-local type and no dedicated
   allocation: with them as the pass's density map the device logged GPU page faults (kgsl "write translation fault", UCHE, at 0 + 0x100-
   multiples, the first 2-3 frames of the map's use; the runtime's maps never), i.e. per-bin writes of the driver through a missing pointer */
/* PS2_RQ_FDM_SAMPLED=1 [test] (R-45 round 3): our density maps also with SAMPLED usage (a guess at what the runtime's maps have; the faults' A/B) */
static int rq_fdm_sampled(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_FDM_SAMPLED"); v = e && *e == '1'; if (v) ps2_log("vk: PS2_RQ_FDM_SAMPLED=1: our density maps with SAMPLED usage"); }
    return v;
}
static int rq_img_mem(VkImage img, VkDeviceMemory *mem, const char *what) {
    VkMemoryDedicatedRequirements dq; memset(&dq, 0, sizeof dq); dq.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
    VkMemoryRequirements2 r2; memset(&r2, 0, sizeof r2); r2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2; r2.pNext = &dq;
    VkImageMemoryRequirementsInfo2 ri; memset(&ri, 0, sizeof ri); ri.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2; ri.image = img;
    vkGetImageMemoryRequirements2(dev, &ri, &r2);
    u32 bits = r2.memoryRequirements.memoryTypeBits, mt = 0xFFFFFFFFu;
    for (int pass = 0; pass < 2 && mt == 0xFFFFFFFFu; pass++)
        for (u32 i = 0; i < memprops.memoryTypeCount && mt == 0xFFFFFFFFu; i++) {
            VkMemoryPropertyFlags f = memprops.memoryTypes[i].propertyFlags;
            if ((bits & (1u << i)) && (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && !(f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)
                && (pass || !(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))) mt = i;
        }
    if (mt == 0xFFFFFFFFu) { ps2_log("vk: %s: no device-local memory type that is not lazily allocated (bits 0x%X)", what, bits); return 0; }
    VkMemoryDedicatedAllocateInfo da; memset(&da, 0, sizeof da); da.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO; da.image = img;
    VkMemoryAllocateInfo ai; memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.pNext = &da; ai.allocationSize = r2.memoryRequirements.size; ai.memoryTypeIndex = mt;
    if (vkAllocateMemory(dev, &ai, NULL, mem) != VK_SUCCESS) { *mem = VK_NULL_HANDLE; return 0; }
    if (vkBindImageMemory(dev, img, *mem, 0) != VK_SUCCESS) return 0;
    ps2_log("vk: %s: %llu bytes in memory type %u (flags 0x%X; the type find_mem picks: %u), dedicated allocation (the driver: %s)", what,
            (unsigned long long)r2.memoryRequirements.size, mt, (unsigned)memprops.memoryTypes[mt].propertyFlags,
            find_mem(bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), dq.requiresDedicatedAllocation ? "required" : dq.prefersDedicatedAllocation ? "preferred" : "not asked");
    return 1;
}
extern int ps2_xr_rq_fdm_own(float *voff_deg);
extern void ps2_xr_rq_fov_tan(int eye, float t[4]);
static struct { int on, level, due, logged; float voff, tan[2][4]; u32 fw, fh, tx, ty;
                VkImage img; VkDeviceMemory mem; VkImageView view; VkBuffer buf; VkDeviceMemory bmem; void *bptr; } rqf;
static int rqf_make(u32 w, u32 h) {
    float voff = 0.0f;
    int lv = ps2_xr_rq_fdm_own(&voff);
    if (lv <= 0) return 0;
    VkPhysicalDeviceFragmentDensityMapPropertiesEXT fp; memset(&fp, 0, sizeof fp);
    fp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_PROPERTIES_EXT;
    VkPhysicalDeviceProperties2 p2; memset(&p2, 0, sizeof p2);
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2; p2.pNext = &fp;
    vkGetPhysicalDeviceProperties2(phys, &p2);
    u32 tx = fp.maxFragmentDensityTexelSize.width, ty = fp.maxFragmentDensityTexelSize.height;
    if (fp.minFragmentDensityTexelSize.width <= 32u && tx >= 32u) tx = 32u;       /* (the runtime's own map: 53 x 55 texels for 1680 x 1760, 32 px) */
    if (fp.minFragmentDensityTexelSize.height <= 32u && ty >= 32u) ty = 32u;
    if (!tx || !ty) { ps2_log("vk: PS2_RQ_FDM_OWN: no density texel size: the runtime's map"); return 0; }
    rqf.fw = (w + tx - 1u) / tx; rqf.fh = (h + ty - 1u) / ty; rqf.tx = tx; rqf.ty = ty;
    VkImageCreateInfo ii; memset(&ii, 0, sizeof ii);
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8_UNORM;
    ii.extent.width = rqf.fw; ii.extent.height = rqf.fh; ii.extent.depth = 1; ii.mipLevels = 1; ii.arrayLayers = 2;
    ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (rq_fdm_sampled() ? VK_IMAGE_USAGE_SAMPLED_BIT : 0u); ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &ii, NULL, &rqf.img) != VK_SUCCESS) { rqf.img = VK_NULL_HANDLE; return 0; }
    if (!rq_img_mem(rqf.img, &rqf.mem, "PS2_RQ_FDM_OWN: the density map")) return 0;
    VkImageViewCreateInfo vi; memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = rqf.img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; vi.format = VK_FORMAT_R8G8_UNORM;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 2;
    if (vkCreateImageView(dev, &vi, NULL, &rqf.view) != VK_SUCCESS) { rqf.view = VK_NULL_HANDLE; return 0; }
    if (make_buffer((VkDeviceSize)rqf.fw * rqf.fh * 2u * 2u, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &rqf.buf, &rqf.bmem, &rqf.bptr) != 0 || !rqf.bptr) return 0;
    rqf.level = lv; rqf.voff = voff; rqf.on = 1;
    ps2_log("vk: PS2_RQ_FDM_OWN=1: our own density map %ux%u x 2 (texel %ux%u; the device's %ux%u..%ux%u), level %d, offset %.1f deg; chain 6 %s (R-45)",
            rqf.fw, rqf.fh, tx, ty, fp.minFragmentDensityTexelSize.width, fp.minFragmentDensityTexelSize.height,
            fp.maxFragmentDensityTexelSize.width, fp.maxFragmentDensityTexelSize.height, lv, (double)voff, ps2_xr_rq_fdm() ? "subsampled (PS2_RQ_FDM_OWN=2)" : "plain");
    return 1;
}
/* the map's values from each eye's tangents (ps2_xr_rq_acquire's crop of the symmetric frame); 1 = new values to upload */
static int rqf_fill(void) {
    float t[2][4];
    for (int e = 0; e < 2; e++) ps2_xr_rq_fov_tan(e, t[e]);
    if (!(t[0][1] > t[0][0]) || !(t[0][2] > t[0][3]) || !(t[1][1] > t[1][0]) || !(t[1][2] > t[1][3])) return 0;
    if (rqf.logged) {                                       /* (a changed field of view: once more, after the queue is idle) */
        int same = 1;
        for (int e = 0; e < 2; e++) for (int k = 0; k < 4; k++) if (fabsf(t[e][k] - rqf.tan[e][k]) > 1e-3f) same = 0;
        if (same) return 0;
        vkDeviceWaitIdle(dev);
    }
    memcpy(rqf.tan, t, sizeof t);
    static const float A[5][2] = { { 1e9f, 1e9f }, { 0.72f, 1e9f }, { 0.55f, 0.85f }, { 0.42f, 0.70f }, { 0.42f, 0.70f } };   /* (off, low, medium, high, high_top) */
    int lv = rqf.level < 1 ? 1 : rqf.level > 4 ? 4 : rqf.level;
    float a1 = A[lv][0], a2 = A[lv][1], sh = tanf(rqf.voff * 0.0174533f);
    u8 *p = (u8 *)rqf.bptr;
    double sum[2] = { 0.0, 0.0 };
    for (int e = 0; e < 2; e++) {
        float l = t[e][0], r = t[e][1], u = t[e][2], d = t[e][3];
        for (u32 j = 0; j < rqf.fh; j++) for (u32 i = 0; i < rqf.fw; i++) {
            float fx = ((float)i + 0.5f) * (float)rqf.tx / (float)rqe.w, fy = ((float)j + 0.5f) * (float)rqf.ty / (float)rqe.h;
            if (fx > 1.0f) fx = 1.0f;
            if (fy > 1.0f) fy = 1.0f;
            float x = l + fx * (r - l), y = u - fy * (u - d) - sh;                 /* (the tangents; y up, the high-detail area moved up by the offset) */
            float un = x >= 0.0f ? x / r : x / -l, vn = y >= 0.0f ? y / (u - sh > 0.05f ? u - sh : 0.05f) : y / -(d - sh);
            float ax = fabsf(un), ay = fabsf(vn);
            float dx = ax < a1 ? 1.0f : ax < a2 ? 0.5f : 0.25f, dy = ay < a1 ? 1.0f : ay < a2 ? 0.5f : 0.25f;
            u8 *q = p + (((size_t)e * rqf.fh + j) * rqf.fw + i) * 2u;
            q[0] = dx >= 1.0f ? 255u : dx >= 0.5f ? 128u : 64u; q[1] = dy >= 1.0f ? 255u : dy >= 0.5f ? 128u : 64u;
            sum[e] += (double)(dx * dy);
        }
    }
    double n = (double)rqf.fw * (double)rqf.fh;
    ps2_log("vk: PS2_RQ_FDM_OWN: the density map made for the eyes' fields (L %.2f..%.2f x %.2f..%.2f, R %.2f..%.2f x %.2f..%.2f tan): shaded share %.2f / %.2f of the pixels",
            (double)t[0][0], (double)t[0][1], (double)t[0][3], (double)t[0][2], (double)t[1][0], (double)t[1][1], (double)t[1][3], (double)t[1][2], sum[0] / n, sum[1] / n);
    rqf.logged = 1;
    return 1;
}
/* before the pass (no rendering open): the map's bytes into its image, FRAGMENT_DENSITY_MAP_OPTIMAL after */
static void rqf_upload(VkCommandBuffer cmd) {
    if (!rqf.on || !rqf.due) return;
    rqf.due = 0;
    VkImageMemoryBarrier2 b;
    barrier_fill(&b, rqf.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_FRAGMENT_DENSITY_PROCESS_BIT_EXT, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    barrier_flush(cmd, &b, 1);
    VkBufferImageCopy rc; memset(&rc, 0, sizeof rc);
    rc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rc.imageSubresource.layerCount = 2;
    rc.imageExtent.width = rqf.fw; rc.imageExtent.height = rqf.fh; rc.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd, rqf.buf, rqf.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rc);
    barrier_fill(&b, rqf.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_DENSITY_PROCESS_BIT_EXT, VK_ACCESS_2_FRAGMENT_DENSITY_MAP_READ_BIT_EXT);
    barrier_flush(cmd, &b, 1);
}
/* PS2_RQ_FDM_UPLOAD (R-48, outside note 5 step F, 2026-11-10; default 1): our density maps are uploaded and waited for in
   their own submission BEFORE the first pass that uses them is recorded. Without the DYNAMIC / DEFERRED view flags the
   driver may read a density map on the host at vkCmdBeginRenderPass (record time); uploaded in the same command buffer
   the map held nothing then (the page faults at address 0 in the first 2-3 frames, R-48). `0`: the upload recorded into
   the frame's command buffer as before. */
static int rq_fdm_upload_first(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_FDM_UPLOAD"); v = !(e && *e == '0'); }
    return v;
}
static int rq_oneshot(void (*rec)(VkCommandBuffer), const char *what) {
    VkCommandBufferAllocateInfo ai; memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; ai.commandPool = cmdpool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VkCommandBuffer c = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(dev, &ai, &c) != VK_SUCCESS) return 0;
    VkCommandBufferBeginInfo bi; memset(&bi, 0, sizeof bi); bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    int ok = 0;
    if (vkBeginCommandBuffer(c, &bi) == VK_SUCCESS) {
        rec(c);
        if (vkEndCommandBuffer(c) == VK_SUCCESS) {
            VkSubmitInfo si; memset(&si, 0, sizeof si); si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &c;
            ok = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle(queue) == VK_SUCCESS;
        }
    }
    vkFreeCommandBuffers(dev, cmdpool, 1, &c);
    ps2_log("vk: PS2_RQ_FDM_UPLOAD=1: %s uploaded and waited for before the first pass that uses it was recorded (%s)", what, ok ? "ok" : "FAILED");
    return ok;
}
/* the density map the eye pass of swapchain image idx uses: ours (PS2_RQ_FDM_OWN=1 / 2) or the runtime's */
static VkImageView rq_pass_fdm(u32 idx) { return rqf.on ? rqf.view : ps2_xr_rq_fdm_view(idx); }
/* ---- PS2_RQ_FEED_FDM, PS2_RQ_SC_DECODE (R-45, round 2): reading the runtime's subsampled chain 6 from inside a render pass that has a
   density map. A subsampled image is defined only when sampled with a subsampled sampler from a fragment shader of a render pass with a
   fragment density map; the feed pass (rq_ds) had none, so it read the raw layout (stale rectangles in the feed, R-45). Both readers below
   use the feed's pipeline (rq_ds -DNOZ, PS2_RQ_ZPRE's colour-only feed pass) in a twin of its render pass with a density map as attachment
   1; their targets are plain images (fragmentDensityMapNonSubsampledImages). Only with the runtime's map (PS2_RQ_FDM_OWN=0):
   - PS2_RQ_FEED_FDM=1: the 1x feed pass with a map of full density everywhere; =2: the runtime's map of this frame's image (its top-left
     corner then sets the feed pass's own density: a coarser feed) [test, default off until the device says which one decodes];
   - PS2_RQ_SC_DECODE=1 (default; dump frames of PS2_XR_DUMP_SWAPCHAIN=1 only): the whole eye image, 1:1, through the same pass at the
     eye's size with the runtime's map of this image (the post-processing case the extension is made for; =2: the full-density map), into a
     plain 2-layer image that the _sc dump copies instead of the swapchain image; =0: the raw copy as before. */
static struct { int tried, ok, up_due, dfresh; u32 fw, fh;
                VkRenderPass rp; VkPipeline pipe;
                VkImage one; VkDeviceMemory onem; VkImageView onev; VkBuffer buf; VkDeviceMemory bmem; void *bptr;
                VkFramebuffer ffb[RQ_MAX_IMG + 1]; VkImageView ffb_c[RQ_MAX_IMG + 1], ffb_d[RQ_MAX_IMG + 1];
                VkImage dimg; VkDeviceMemory dmem; VkImageView dview;
                VkFramebuffer dfb[RQ_MAX_IMG + 1]; VkImageView dfb_c[RQ_MAX_IMG + 1], dfb_d[RQ_MAX_IMG + 1]; } rqx;
static int rqx_feed_cfg(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_FEED_FDM"); v = e && (*e == '1' || *e == '2') ? *e - '0' : 0; }
    return v;
}
static int rqx_dec_cfg(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_SC_DECODE"); v = e && (*e == '0' || *e == '2') ? *e - '0' : 1; }
    return v;
}
/* ---- PS2_RQ_FEED_DECODE (R-45 round 4; [test] default 0: reverted 05:40 on R-45a's evidence, open for R-45b / c, note 5): the stale rectangles reach the PICTURE through the feed.
   The hunt's _sc dumps (decoded through a pass with the runtime's map) show blocks of an older world behind a current cockpit, most
   in the right eye (M2, M3, M5, M6): the feed read the subsampled chain 6 raw (old bytes in the low-density bins) and the game's
   slot-0 reads (its full-screen effects that draw the last frame back) blend that feed into the world before the cockpit is drawn.
   Here every frame first runs SC_DECODE's pass (the eye image 1:1 at the eye's size with the runtime's map of this image, into the
   plain 2-layer image rqx.dimg), then the feed pass and the recorder read that plain image instead of the swapchain image: through
   a second set of the feed's layout (its immutable sampler is the subsampled one; outside a pass with a density map it reads a plain
   image as a plain sampler does -- the device's dumps are the check). =0: the raw read as before (PS2_RQ_FEED_FDM still applies). */
static struct { int tried; VkDescriptorPool pool; VkDescriptorSet set; VkImageView b0, b1; VkDescriptorSet cur; u64 n; } rqfd;
static int rqfd_cfg(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_FEED_DECODE"); v = e && *e == '1'; }   /* ([test] since 2026-11-10 note 5: default off) */
    return v;
}
/* the set the feed pass and the recorder read this frame's eye image through: the decoded plain image's (PS2_RQ_FEED_DECODE) or the
   swapchain image's own */
static VkDescriptorSet rq_eye_src_set(void) { return rqfd.cur ? rqfd.cur : rqe.dsset[rqe.idx]; }
static int rqx_make(void) {
    if (rqx.ok) return 1;
    if (rqx.tried) return 0;
    rqx.tried = 1;
    if (!rqe.ok || !rqe.fdm || !ps2_xr_rq_sc_sub() || !rqz_cfg() || !rqv_ds || !rqz_ds_nz) { ps2_log("vk: PS2_RQ_FEED_FDM / PS2_RQ_SC_DECODE: need the subsampled chain 6 and PS2_RQ_ZPRE: off"); return 0; }
    PFN_vkCreateRenderPass2 crp2 = (PFN_vkCreateRenderPass2)vkGetDeviceProcAddr(dev, "vkCreateRenderPass2");
    if (!crp2) return 0;
    VkAttachmentDescription2 da[2]; memset(da, 0, sizeof da);
    for (int i = 0; i < 2; i++) da[i].sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
    da[0].format = VK_FORMAT_R8G8B8A8_UNORM; da[0].samples = VK_SAMPLE_COUNT_1_BIT; da[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; da[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; da[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    da[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; da[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    da[1].format = VK_FORMAT_R8G8_UNORM; da[1].samples = VK_SAMPLE_COUNT_1_BIT; da[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; da[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    da[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; da[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    da[1].initialLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT; da[1].finalLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
    VkAttachmentReference2 dc; memset(&dc, 0, sizeof dc);
    dc.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2; dc.attachment = 0; dc.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; dc.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    VkSubpassDescription2 ds; memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2; ds.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; ds.viewMask = 3; ds.colorAttachmentCount = 1; ds.pColorAttachments = &dc;
    VkSubpassDependency2 dd[2]; memset(dd, 0, sizeof dd);
    for (int i = 0; i < 2; i++) dd[i].sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
    dd[0].srcSubpass = VK_SUBPASS_EXTERNAL; dd[0].dstSubpass = 0; dd[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; dd[0].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dd[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; dd[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dd[1].srcSubpass = 0; dd[1].dstSubpass = VK_SUBPASS_EXTERNAL; dd[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dd[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dd[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; dd[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    uint32_t cm = 3;
    VkRenderPassFragmentDensityMapCreateInfoEXT fdmi; memset(&fdmi, 0, sizeof fdmi);
    fdmi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT;
    fdmi.fragmentDensityMapAttachment.attachment = 1; fdmi.fragmentDensityMapAttachment.layout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
    VkRenderPassCreateInfo2 rd; memset(&rd, 0, sizeof rd);
    rd.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2; rd.pNext = &fdmi; rd.attachmentCount = 2; rd.pAttachments = da; rd.subpassCount = 1; rd.pSubpasses = &ds;
    rd.dependencyCount = 2; rd.pDependencies = dd; rd.correlatedViewMaskCount = 1; rd.pCorrelatedViewMasks = &cm;
    if (crp2(dev, &rd, NULL, &rqx.rp) != VK_SUCCESS) { rqx.rp = VK_NULL_HANDLE; ps2_log("vk: PS2_RQ_FEED_FDM: the pass could not be made"); return 0; }
    /* the feed's pipeline (rq_ds.vert, rq_ds_nz.frag, the feed's layout with its immutable subsampled sampler) for this pass */
    VkPipelineShaderStageCreateInfo st[2]; memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = rqv_ds; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = rqz_ds_nz; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin; memset(&vin, 0, sizeof vin); vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia; memset(&ia, 0, sizeof ia); ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vs; memset(&vs, 0, sizeof vs); vs.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vs.viewportCount = 1; vs.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs; memset(&rs, 0, sizeof rs); rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms; memset(&ms, 0, sizeof ms); ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo dss; memset(&dss, 0, sizeof dss); dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    VkPipelineColorBlendAttachmentState cba; memset(&cba, 0, sizeof cba); cba.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo cb; memset(&cb, 0, sizeof cb); cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cb.attachmentCount = 1; cb.pAttachments = &cba;
    static const VkDynamicState dy[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsi; memset(&dsi, 0, sizeof dsi); dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; dsi.dynamicStateCount = 2; dsi.pDynamicStates = dy;
    VkGraphicsPipelineCreateInfo gp; memset(&gp, 0, sizeof gp);
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia; gp.pViewportState = &vs;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &dss; gp.pColorBlendState = &cb; gp.pDynamicState = &dsi; gp.layout = rqe.dslo; gp.renderPass = rqx.rp;
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &rqx.pipe) != VK_SUCCESS) { rqx.pipe = VK_NULL_HANDLE; ps2_log("vk: PS2_RQ_FEED_FDM: the pipeline could not be made"); return 0; }
    /* the full-density map (R8G8 255, 255), 32 px texels, the eye's size: covers the eye image and the feed */
    rqx.fw = (rqe.w + 31u) / 32u; rqx.fh = (rqe.h + 31u) / 32u;
    VkImageCreateInfo ii; memset(&ii, 0, sizeof ii);
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8_UNORM;
    ii.extent.width = rqx.fw; ii.extent.height = rqx.fh; ii.extent.depth = 1; ii.mipLevels = 1; ii.arrayLayers = 2;
    ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (rq_fdm_sampled() ? VK_IMAGE_USAGE_SAMPLED_BIT : 0u); ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &ii, NULL, &rqx.one) != VK_SUCCESS) { rqx.one = VK_NULL_HANDLE; return 0; }
    if (!rq_img_mem(rqx.one, &rqx.onem, "PS2_RQ_FEED_FDM: the full-density map")) return 0;
    VkImageViewCreateInfo vi; memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = rqx.one; vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; vi.format = VK_FORMAT_R8G8_UNORM;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 2;
    if (vkCreateImageView(dev, &vi, NULL, &rqx.onev) != VK_SUCCESS) { rqx.onev = VK_NULL_HANDLE; return 0; }
    VkDeviceSize bs = (VkDeviceSize)rqx.fw * rqx.fh * 2u * 2u;
    if (make_buffer(bs, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &rqx.buf, &rqx.bmem, &rqx.bptr) != 0 || !rqx.bptr) return 0;
    memset(rqx.bptr, 255, (size_t)bs);
    rqx.up_due = 1; rqx.ok = 1;
    ps2_log("vk: PS2_RQ_FEED_FDM=%d, PS2_RQ_SC_DECODE=%d: the feed pass's twin with a density map made (full-density map %ux%u x 2)", rqx_feed_cfg(), rqx_dec_cfg(), rqx.fw, rqx.fh);
    return 1;
}
/* outside any pass: the full-density map's one upload */
static void rqx_upload(VkCommandBuffer cmd) {
    if (!rqx.ok || !rqx.up_due) return;
    rqx.up_due = 0;
    VkImageMemoryBarrier2 b;
    barrier_fill(&b, rqx.one, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_FRAGMENT_DENSITY_PROCESS_BIT_EXT, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    barrier_flush(cmd, &b, 1);
    VkBufferImageCopy rc; memset(&rc, 0, sizeof rc);
    rc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rc.imageSubresource.layerCount = 2;
    rc.imageExtent.width = rqx.fw; rc.imageExtent.height = rqx.fh; rc.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd, rqx.buf, rqx.one, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rc);
    barrier_fill(&b, rqx.one, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_DENSITY_PROCESS_BIT_EXT, VK_ACCESS_2_FRAGMENT_DENSITY_MAP_READ_BIT_EXT);
    barrier_flush(cmd, &b, 1);
}
/* a framebuffer of the twin pass (colour view c, density map view d, w x h), cached in one slot; made again when its views change */
static VkFramebuffer rqx_fb(VkFramebuffer *fb, VkImageView *fc, VkImageView *fd, VkImageView c, VkImageView d, u32 w, u32 h) {
    if (*fb && *fc == c && *fd == d) return *fb;
    if (*fb) { vkDeviceWaitIdle(dev); vkDestroyFramebuffer(dev, *fb, NULL); *fb = VK_NULL_HANDLE; }
    VkImageView av[2] = { c, d };
    VkFramebufferCreateInfo ci; memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO; ci.renderPass = rqx.rp; ci.attachmentCount = 2; ci.pAttachments = av; ci.width = w; ci.height = h; ci.layers = 1;
    if (vkCreateFramebuffer(dev, &ci, NULL, fb) != VK_SUCCESS) { *fb = VK_NULL_HANDLE; return VK_NULL_HANDLE; }
    *fc = c; *fd = d;
    return *fb;
}
/* rq_ds's draw in the twin pass (p: its 8 push constants) */
static void rqx_draw(VkCommandBuffer cmd, VkFramebuffer fb, u32 w, u32 h, const float *p) {
    VkRenderPassBeginInfo bi; memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; bi.renderPass = rqx.rp; bi.framebuffer = fb; bi.renderArea.extent.width = w; bi.renderArea.extent.height = h;
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp; memset(&vp, 0, sizeof vp); vp.width = (float)w; vp.height = (float)h; vp.maxDepth = 1.0f;
    VkRect2D sc; memset(&sc, 0, sizeof sc); sc.extent.width = w; sc.extent.height = h;
    vkCmdSetViewport(cmd, 0, 1, &vp); vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqx.pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqe.dslo, 0, 1, &rqe.dsset[rqe.idx], 0, NULL);
    vkCmdPushConstants(cmd, rqe.dslo, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8u * (u32)sizeof(float), p);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}
/* rq_eye_after: the feed's framebuffer in the twin pass (PS2_RQ_FEED_FDM), or 0 = the feed pass as before */
static VkFramebuffer rqx_feed_fb(vk_target *f, u32 w, u32 h) {
    int m = rqx_feed_cfg();
    if (!m || !rqe.fdm || !ps2_xr_rq_sc_sub() || !rqz_cfg() || !rqx_make()) return VK_NULL_HANDLE;
    VkImageView d = m == 2 ? rq_pass_fdm(rqe.idx) : rqx.onev;
    u32 k = m == 2 ? rqe.idx : RQ_MAX_IMG;
    return d ? rqx_fb(&rqx.ffb[k], &rqx.ffb_c[k], &rqx.ffb_d[k], f->cview, d, w, h) : VK_NULL_HANDLE;
}
/* rq_eye_after, the swapchain image SHADER_READ_ONLY: the whole eye image 1:1 into the plain image the _sc dump copies (dump frames;
   every frame with PS2_RQ_FEED_DECODE, before the feed pass, which then reads it: feed = 1) */
static void rqx_decode_any(VkCommandBuffer cmd, int feed);
static void rqx_decode(VkCommandBuffer cmd) {
    extern int ps2_xr_dump_swapchain_on(void);
    rqx.dfresh = 0;
    if (!xr_dump_now || !ps2_xr_dump_swapchain_on() || !rqx_dec_cfg()) return;
    rqx_decode_any(cmd, 0);
}
static void rqx_decode_any(VkCommandBuffer cmd, int feed) {
    if (!rqe.fdm || !ps2_xr_rq_sc_sub() || !rqz_cfg() || !rqx_make()) return;
    { if (rqx.up_due && rq_fdm_upload_first()) rq_oneshot(rqx_upload, "the twin passes' full-density map"); rqx_upload(cmd); }
    if (!rqx.dimg) {
        VkImageCreateInfo ii; memset(&ii, 0, sizeof ii);
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent.width = rqe.w; ii.extent.height = rqe.h; ii.extent.depth = 1; ii.mipLevels = 1; ii.arrayLayers = 2;
        ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(dev, &ii, NULL, &rqx.dimg) != VK_SUCCESS) { rqx.dimg = VK_NULL_HANDLE; return; }
        if (!rq_img_mem(rqx.dimg, &rqx.dmem, "PS2_RQ_SC_DECODE: the decode image")) { vkDestroyImage(dev, rqx.dimg, NULL); rqx.dimg = VK_NULL_HANDLE; return; }
        VkImageViewCreateInfo vi; memset(&vi, 0, sizeof vi);
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = rqx.dimg; vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 2;
        if (vkCreateImageView(dev, &vi, NULL, &rqx.dview) != VK_SUCCESS) rqx.dview = VK_NULL_HANDLE;
    }
    if (!rqx.dview) return;
    int full = !feed && rqx_dec_cfg() == 2;                /* (the feed's decode: always the runtime's map of this image) */
    VkImageView d = full ? rqx.onev : rq_pass_fdm(rqe.idx);
    u32 k = full ? RQ_MAX_IMG : rqe.idx;
    VkFramebuffer fb = d ? rqx_fb(&rqx.dfb[k], &rqx.dfb_c[k], &rqx.dfb_d[k], rqx.dview, d, rqe.w, rqe.h) : VK_NULL_HANDLE;
    if (!fb) return;
    float p[8] = { 0.0f, (float)rqe.w, 0.0f, (float)rqe.w, 0.0f, (float)rqe.h, 1.0f, 1.0f };   /* (rq_ds: the eye image 1:1; no fade mark, no depth view) */
    rqx_draw(cmd, fb, rqe.w, rqe.h, p);
    rqx.dfresh = 1;
    if (!feed) ps2_log("vk: PS2_RQ_SC_DECODE=%d: this dump's eye images read through a pass with %s (the _sc files)", rqx_dec_cfg(), full ? "a full-density map" : "the runtime's map");
}
/* PS2_RQ_FEED_DECODE: this frame's eye image decoded (rqx_decode_any, feed = 1) and the second set pointed at it; 0 = not on / failed
   (the feed then reads the swapchain image as before) */
static int rqfd_frame(VkCommandBuffer cmd) {
    rqfd.cur = VK_NULL_HANDLE;
    if (!rqfd_cfg() || rqf.on || !rqe.fdm || !ps2_xr_rq_sc_sub() || !rqz_cfg()) return 0;
    if (!rqfd.set) {
        if (rqfd.tried) return 0;
        rqfd.tried = 1;
        VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 };
        VkDescriptorPoolCreateInfo pi; memset(&pi, 0, sizeof pi);
        pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; pi.maxSets = 1; pi.poolSizeCount = 1; pi.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(dev, &pi, NULL, &rqfd.pool) != VK_SUCCESS) { rqfd.pool = VK_NULL_HANDLE; ps2_log("vk: PS2_RQ_FEED_DECODE: no pool: off"); return 0; }
        VkDescriptorSetAllocateInfo ai; memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; ai.descriptorPool = rqfd.pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &rqe.dsdsl;
        if (vkAllocateDescriptorSets(dev, &ai, &rqfd.set) != VK_SUCCESS) { rqfd.set = VK_NULL_HANDLE; ps2_log("vk: PS2_RQ_FEED_DECODE: no set: off"); return 0; }
    }
    rqx.dfresh = 0;
    rqx_decode_any(cmd, 1);
    if (!rqx.dfresh) return 0;
    VkImageView v1 = rqz_cfg() ? rq_feed.dview : rqe.rdv;
    if (rqfd.b0 != rqx.dview || rqfd.b1 != v1) {             /* (once: the decode image and the feed's views are made once; the set is in no
                                                               pending command buffer before its first use) */
        VkDescriptorImageInfo ii[2]; memset(ii, 0, sizeof ii);
        ii[0].sampler = rqe.smp_sub ? rqe.smp_sub : sampler; ii[0].imageView = rqx.dview; ii[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ii[1].sampler = sampler_point; ii[1].imageView = v1; ii[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet wr[2]; memset(wr, 0, sizeof wr);
        for (int i = 0; i < 2; i++) { wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[i].dstSet = rqfd.set; wr[i].dstBinding = (uint32_t)i; wr[i].descriptorCount = 1; wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[i].pImageInfo = &ii[i]; }
        if (rqfd.b0) vkDeviceWaitIdle(dev);                  /* (a later change: the set may be in a pending command buffer) */
        vkUpdateDescriptorSets(dev, 2, wr, 0, NULL);
        rqfd.b0 = rqx.dview; rqfd.b1 = v1;
        ps2_log("vk: PS2_RQ_FEED_DECODE=1: the eye image decoded every frame (a %ux%u x 2 pass with the runtime's map); the feed and the recorder read the plain image (R-45)", rqe.w, rqe.h);
    }
    rqfd.cur = rqfd.set; rqfd.n++;
    return 1;
}
/* scdump_record (ps2_video.c): the plain image this frame's _sc dump copies instead of the swapchain image (layer = eye), or 0 */
static VkImage rqx_dump_image(void) { return rqx.dfresh ? rqx.dimg : VK_NULL_HANDLE; }
/* the pass, the images at the eye's size, the feed's pass and pipeline: made once at the swapchain's size */
/* outside note 5, step 0 (2026-11-10): the device's density-map facts, once into the log (PS2_RQ_FDM=1) */
static void rq_fdm_facts(void) {
    static int done;
    if (done || !rq_fdm_dev) return;
    done = 1;
    int has2 = vk_dev_has_ext("VK_EXT_fragment_density_map2");
    VkPhysicalDeviceFragmentDensityMapFeaturesEXT f1; memset(&f1, 0, sizeof f1); f1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT;
    VkPhysicalDeviceFragmentDensityMap2FeaturesEXT f2x; memset(&f2x, 0, sizeof f2x); f2x.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_2_FEATURES_EXT;
    VkPhysicalDeviceFeatures2 ff; memset(&ff, 0, sizeof ff); ff.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2; ff.pNext = &f1;
    if (has2) f1.pNext = &f2x;
    vkGetPhysicalDeviceFeatures2(phys, &ff);
    VkPhysicalDeviceFragmentDensityMapPropertiesEXT p1; memset(&p1, 0, sizeof p1); p1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_PROPERTIES_EXT;
    VkPhysicalDeviceFragmentDensityMap2PropertiesEXT p2x; memset(&p2x, 0, sizeof p2x); p2x.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_2_PROPERTIES_EXT;
    VkPhysicalDeviceProperties2 pp; memset(&pp, 0, sizeof pp); pp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2; pp.pNext = &p1;
    if (has2) p1.pNext = &p2x;
    vkGetPhysicalDeviceProperties2(phys, &pp);
    ps2_log("vk: FDM facts (note 5 step 0): features fragmentDensityMap %u, Dynamic %u, NonSubsampledImages %u; VK_EXT_fragment_density_map2 %s%s",
            f1.fragmentDensityMap, f1.fragmentDensityMapDynamic, f1.fragmentDensityMapNonSubsampledImages, has2 ? "offered" : "NOT offered",
            has2 ? (f2x.fragmentDensityMapDeferred ? ", Deferred 1" : ", Deferred 0") : "");
    ps2_log("vk: FDM facts: texel size %ux%u..%ux%u, fragmentDensityInvocations %u; subsampledLoads %u, subsampledCoarseReconstructionEarlyAccess %u, maxSubsampledArrayLayers %u, maxDescriptorSetSubsampledSamplers %u%s",
            p1.minFragmentDensityTexelSize.width, p1.minFragmentDensityTexelSize.height, p1.maxFragmentDensityTexelSize.width, p1.maxFragmentDensityTexelSize.height,
            p1.fragmentDensityInvocations, p2x.subsampledLoads, p2x.subsampledCoarseReconstructionEarlyAccess, p2x.maxSubsampledArrayLayers,
            p2x.maxDescriptorSetSubsampledSamplers, has2 ? "" : " (FDM2 not offered: the last four unknown, printed 0)");
    ps2_log("vk: FDM facts: chain 6 %s (PS2_XR_SC_SUBSAMPLED), the eye pass: 4x colour and depth CLEAR, the swapchain resolve DONT_CARE / STORE, render area = the dynamic-resolution rectangle (vk: PS2_RQ_TESTPAT lines name it)",
            ps2_xr_rq_fdm() ? (ps2_xr_rq_sc_sub() ? "subsampled (assumed or declared)" : "plain") : "without the runtime's map");
}
static int rq_eye_make(u32 w, u32 h) {
    rq_fdm_facts();
    if (rqe.ok) return rqe.w == w && rqe.h == h;
    if (rqe.tried) return 0;
    rqe.tried = 1;
    const u64 rqpw_t0 = vk_mono_ns();
    if (!rqv_gs || !rqv_mesh || !rqf_gs || !rqf_lr || !rqv_ds || !rqf_ds) { ps2_log("vk: PS2_RQ=eye: shaders missing"); return 0; }
    PFN_vkCreateRenderPass2 crp2 = (PFN_vkCreateRenderPass2)vkGetDeviceProcAddr(dev, "vkCreateRenderPass2");
    if (!crp2) { ps2_log("vk: PS2_RQ=eye: no vkCreateRenderPass2"); return 0; }
    VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM, zf = VK_FORMAT_D32_SFLOAT;
    /* attachments: 0 colour 4x, 1 depth 4x, 2 colour resolve (the swapchain's UNORM view), 3 depth resolve */
    rqe.fdm = rq_fdm_dev && (rqf_make(w, h) || ps2_xr_rq_fdm());     /* (PS2_RQ_FDM_OWN=1: our map, chain 6 plain; =2: our map, chain 6 subsampled; 0: the runtime's, chain 6 subsampled) */
    VkAttachmentDescription2 at[5]; memset(at, 0, sizeof at);
    for (int i = 0; i < 5; i++) at[i].sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
    at[4].format = VK_FORMAT_R8G8_UNORM; at[4].samples = VK_SAMPLE_COUNT_1_BIT; at[4].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[4].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[4].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[4].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[4].initialLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT; at[4].finalLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
    at[0].format = cf; at[0].samples = rq_samples(); at[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at[0].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    at[1].format = zf; at[1].samples = rq_samples(); at[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    at[2].format = cf; at[2].samples = VK_SAMPLE_COUNT_1_BIT; at[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at[2].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    at[3].format = zf; at[3].samples = VK_SAMPLE_COUNT_1_BIT; at[3].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[3].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at[3].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[3].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[3].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at[3].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    /* PS2_RQ_ZPRE: no depth resolve, no stored depth; the density map moves up to attachment 3 */
    int zp = rqz_cfg();
    if (zp && (!rnb.ok || !rqz_vs || !rqz_ck_fs || !rqz_ds_nz)) { ps2_log("vk: PS2_RQ_ZPRE: needs PS2_RN_BATCH and its shaders: off"); zp = 0; rqz.mode = 0; }
    if (zp) at[3] = at[4];
    VkAttachmentReference2 ca, ra, za, zr, ina;
    memset(&ca, 0, sizeof ca); ca.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2; ca.attachment = 0; ca.layout = VK_IMAGE_LAYOUT_GENERAL; ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ra = ca; ra.attachment = 2; ra.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    za = ca; za.attachment = 1; za.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; za.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    zr = za; zr.attachment = 3;
    ina = ca;
    VkSubpassDescriptionDepthStencilResolve dr; memset(&dr, 0, sizeof dr);
    dr.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE;
    dr.depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT; dr.stencilResolveMode = VK_RESOLVE_MODE_NONE; dr.pDepthStencilResolveAttachment = &zr;
    VkSubpassDescription2 sp; memset(&sp, 0, sizeof sp);
    sp.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2; sp.pNext = zp ? NULL : &dr; sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.viewMask = 3;
    sp.colorAttachmentCount = 1; sp.pColorAttachments = &ca; sp.pResolveAttachments = &ra; sp.pDepthStencilAttachment = &za;
    sp.inputAttachmentCount = 1; sp.pInputAttachments = &ina;
    VkSubpassDependency2 dep[3]; memset(dep, 0, sizeof dep);
    for (int i = 0; i < 3; i++) dep[i].sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
    dep[0].srcSubpass = 0; dep[0].dstSubpass = 0; dep[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep[0].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; dep[0].dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT; dep[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT | VK_DEPENDENCY_VIEW_LOCAL_BIT;
    dep[1].srcSubpass = VK_SUBPASS_EXTERNAL; dep[1].dstSubpass = 0; dep[1].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; dep[1].dstStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
    {   const char *ebd = getenv("PS2_RQ_BINDEP");           /* PS2_RQ_BINDEP=1: the eye pass's external dependency without the vertex stages, so that its binning need not wait for what came before */
        if (ebd && *ebd == '1') dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT; }
    dep[1].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; dep[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    dep[2].srcSubpass = 0; dep[2].dstSubpass = VK_SUBPASS_EXTERNAL; dep[2].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep[2].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep[2].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; dep[2].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    uint32_t cm = 3;
    VkRenderPassCreateInfo2 rc; memset(&rc, 0, sizeof rc);
    VkRenderPassFragmentDensityMapCreateInfoEXT fdmi; memset(&fdmi, 0, sizeof fdmi);
    fdmi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT;
    fdmi.fragmentDensityMapAttachment.attachment = zp ? 3 : 4; fdmi.fragmentDensityMapAttachment.layout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
    rc.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2; rc.pNext = rqe.fdm ? &fdmi : NULL; rc.attachmentCount = (rqe.fdm ? 5 : 4) - (zp ? 1 : 0); rc.pAttachments = at; rc.subpassCount = 1; rc.pSubpasses = &sp;
    rc.dependencyCount = 3; rc.pDependencies = dep; rc.correlatedViewMaskCount = 1; rc.pCorrelatedViewMasks = &cm;
    if (crp2(dev, &rc, NULL, &rqe.rp) != VK_SUCCESS) { ps2_log("vk: PS2_RQ=eye: the render pass could not be made"); return 0; }
    if (rq_mk_img(w, h, cf, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, rq_samples(), VK_IMAGE_ASPECT_COLOR_BIT, &rqe.mc, &rqe.mcm, &rqe.mcv, 1) != 0
        || rq_mk_img(w, h, zf, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, rq_samples(), VK_IMAGE_ASPECT_DEPTH_BIT, &rqe.md, &rqe.mdm, &rqe.mdv, 1) != 0
        || (!zp && rq_mk_img(w, h, zf, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, &rqe.rd, &rqe.rdm, &rqe.rdv, 0) != 0)) {
        ps2_log("vk: PS2_RQ=eye: the attachments could not be made"); return 0;
    }
    rqe.iset = alloc_input_set(rqe.mcv);
    /* the feed's pass: the feed target's colour and depth, multiview, whole image redrawn */
    VkAttachmentDescription2 da[2]; memset(da, 0, sizeof da);
    da[0] = at[2]; da[0].format = cf; if (!zp) { da[1] = at[3]; da[1].format = zf; }
    VkAttachmentReference2 dc, dz;
    memset(&dc, 0, sizeof dc); dc.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2; dc.attachment = 0; dc.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; dc.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    dz = dc; dz.attachment = 1; dz.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; dz.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    VkSubpassDescription2 ds; memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2; ds.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; ds.viewMask = 3;
    ds.colorAttachmentCount = 1; ds.pColorAttachments = &dc; ds.pDepthStencilAttachment = zp ? NULL : &dz;
    VkSubpassDependency2 dd[2]; memset(dd, 0, sizeof dd);
    for (int i = 0; i < 2; i++) dd[i].sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
    dd[0].srcSubpass = VK_SUBPASS_EXTERNAL; dd[0].dstSubpass = 0; dd[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; dd[0].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dd[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; dd[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dd[1].srcSubpass = 0; dd[1].dstSubpass = VK_SUBPASS_EXTERNAL; dd[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT; dd[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dd[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; dd[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo2 rd; memset(&rd, 0, sizeof rd);
    rd.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2; rd.attachmentCount = zp ? 1 : 2; rd.pAttachments = da; rd.subpassCount = 1; rd.pSubpasses = &ds;
    rd.dependencyCount = 2; rd.pDependencies = dd; rd.correlatedViewMaskCount = 1; rd.pCorrelatedViewMasks = &cm;
    if (crp2(dev, &rd, NULL, &rqe.dsrp) != VK_SUCCESS) { ps2_log("vk: PS2_RQ=eye: the feed's pass could not be made"); return 0; }
    /* its set layout, sets (one per swapchain image), pipeline */
    VkDescriptorSetLayoutBinding bi[2]; memset(bi, 0, sizeof bi);
    bi[0].binding = 0; bi[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; bi[0].descriptorCount = 1; bi[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bi[1] = bi[0]; bi[1].binding = 1;
    if (rqe.fdm && ps2_xr_rq_sc_sub()) {                      /* (a subsampled image: read through an immutable subsampled sampler; PS2_RQ_FDM_OWN=1: chain 6 is plain) */
        VkSamplerCreateInfo sc; memset(&sc, 0, sizeof sc);
        sc.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO; sc.flags = VK_SAMPLER_CREATE_SUBSAMPLED_BIT_EXT; sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
        sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(dev, &sc, NULL, &rqe.smp_sub) != VK_SUCCESS) return 0;
        bi[0].pImmutableSamplers = &rqe.smp_sub;
    }
    VkDescriptorSetLayoutCreateInfo li; memset(&li, 0, sizeof li);
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; li.bindingCount = 2; li.pBindings = bi;
    if (vkCreateDescriptorSetLayout(dev, &li, NULL, &rqe.dsdsl) != VK_SUCCESS) return 0;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * RQ_MAX_IMG };
    VkDescriptorPoolCreateInfo pi; memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; pi.maxSets = RQ_MAX_IMG; pi.poolSizeCount = 1; pi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(dev, &pi, NULL, &rqe.dspool) != VK_SUCCESS) return 0;
    VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32 };
    VkPipelineLayoutCreateInfo pl; memset(&pl, 0, sizeof pl);
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pl.setLayoutCount = 1; pl.pSetLayouts = &rqe.dsdsl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev, &pl, NULL, &rqe.dslo) != VK_SUCCESS) return 0;
    VkPipelineShaderStageCreateInfo st[2]; memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = rqv_ds; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = zp ? rqz_ds_nz : rqf_ds; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin; memset(&vin, 0, sizeof vin); vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia; memset(&ia, 0, sizeof ia); ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vs; memset(&vs, 0, sizeof vs); vs.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vs.viewportCount = 1; vs.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs; memset(&rs, 0, sizeof rs); rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms; memset(&ms, 0, sizeof ms); ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo dss; memset(&dss, 0, sizeof dss); dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO; dss.depthTestEnable = zp ? VK_FALSE : VK_TRUE; dss.depthWriteEnable = zp ? VK_FALSE : VK_TRUE; dss.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    VkPipelineColorBlendAttachmentState cba; memset(&cba, 0, sizeof cba); cba.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo cb; memset(&cb, 0, sizeof cb); cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cb.attachmentCount = 1; cb.pAttachments = &cba;
    static const VkDynamicState dy[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsi; memset(&dsi, 0, sizeof dsi); dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; dsi.dynamicStateCount = 2; dsi.pDynamicStates = dy;
    VkGraphicsPipelineCreateInfo gp; memset(&gp, 0, sizeof gp);
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia; gp.pViewportState = &vs;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &dss; gp.pColorBlendState = &cb; gp.pDynamicState = &dsi; gp.layout = rqe.dslo; gp.renderPass = rqe.dsrp;
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &rqe.dspipe) != VK_SUCCESS) { ps2_log("vk: PS2_RQ=eye: the feed's pipeline could not be made"); return 0; }
    ps2_log("rq: raster-order attachment access extensions: EXT %d ARM %d QCOM %d", vk_dev_has_ext("VK_EXT_rasterization_order_attachment_access"), vk_dev_has_ext("VK_ARM_rasterization_order_attachment_access"), vk_dev_has_ext("VK_QCOM_rasterization_order_attachment_access"));
    rqe.w = w; rqe.h = h; rqe.ok = 1;
    rqpw_made("the eye pass (render passes, transient attachments, the feed's pipeline)", rqpw_t0);
    ps2_log("vk: PS2_RQ=eye: one %ux%u pass per frame, 4x MSAA, multiview, %s%s; %.0f MB of transient attachments", w, h, zp ? "colour stored only" : "D32 resolved", !rqe.fdm ? "" : !rqf.on ? (ps2_xr_rq_sc_sub() ? ", the runtime's density map (the swapchain subsampled)" : ", the runtime's density map (the swapchain PLAIN: PS2_XR_SC_SUBSAMPLED)") : ps2_xr_rq_fdm() ? ", our own density map (the swapchain subsampled)" : ", our own density map (the swapchain plain)", (double)w * h * 2.0 * 4.0 * 8.0 / 1048576.0);
    return 1;
}
/* ---- PS2_RQ_DYNRES (plan 2, 4.7): dynamic resolution of the eye pass ----
   The swapchain stays at the eye's full size; the frame draws into a centred rectangle of it (so the runtime's density map, centred
   on the image, stays centred on the view) and the layer's imageRect names that rectangle. The rectangle is reached by scaling the
   per-view crop (x' = A x + B w) about the image's centre: every vertex and full-screen shader that uses rqe.xf follows without a
   change, the viewport stays the whole image, the render area and the scissors are clamped to the rectangle. PS2_RQ_DYNRES=1 turns
   it on; =0.xx fixes the scale (a test). The scale follows the runtime's app GPU time at the GPU's top clock (rqd_sample): every 24 frames, against
   PS2_RQ_DYNRES_MS (12.5): above it the scale drops by sqrt(budget / signal) (at most 3 % a window); two windows under 0.92 x the
   budget raise it by 1 %. Floor PS2_RQ_DYNRES_MIN (0.85); rectangle snapped to 16 pixels. */
static struct { int cfg; float fixed, min, ms, s; u32 vx, vy, vw, vh; float w[24]; u32 n; u32 under; u64 fr, floor_fr; float mhz; float q; int src, over_drop; float ceil; } rqd;
static int rqd_on(void) {
    if (!rqd.cfg) {
        const char *e = getenv("PS2_RQ_DYNRES"), *m = getenv("PS2_RQ_DYNRES_MIN"), *b = getenv("PS2_RQ_DYNRES_MS");
        rqd.cfg = 1; rqd.s = 1.0f; rqd.min = m ? (float)atof(m) : 0.85f; rqd.ms = b ? (float)atof(b) : 12.5f;
        if (e && *e) { float v = (float)atof(e); if (v >= 2.0f || v == 1.0f) rqd.cfg = 2; else if (v > 0.0f) { rqd.cfg = 3; rqd.fixed = v; rqd.s = v; } }
        if (rqd.min < 0.5f) rqd.min = 0.5f;
        if (rqd.min > 1.0f) rqd.min = 1.0f;
        if (rqd.cfg > 1) ps2_log("vk: PS2_RQ_DYNRES: %s, floor %.2f, budget %.1f ms", rqd.cfg == 3 ? "fixed scale" : "the eye rectangle follows the GPU time", (double)rqd.min, (double)rqd.ms);
    }
    return rqd.cfg > 1;
}
/* 1 = the scale is at the floor and the frame is still over the budget: the governor's turn (its first lever is this one) */
int ps2_rq_dynres_spent(void) { return rqd.cfg == 2 && rqd.s <= rqd.min + 0.001f && rqd.floor_fr > 300u; }   /* (5 s at the floor and still over: looks are the governor's last resort) */
static int rqd_busy(void) { return rqd.cfg == 2 && !ps2_rq_dynres_spent(); }
float ps2_rq_dynres_scale(void) { return rqd.cfg > 1 ? rqd.s : 1.0f; }
extern unsigned ps2_xr_perf_metrics(double v[7]);
/* The scale's signal (the plan's second review): the runtime's own app GPU time (XR_META_performance_metrics; our timestamps also hold the compositor's
   pass and over-read by 1-2 ms), at the top clock, so that a lower clock chosen by the system because the frame has room is not read as load; and the
   runtime's dropped frames above what the cadence repeats by design (72 Hz with 60 frames: 12 a second). Fallback where the runtime has no metrics: the p90
   of our timestamps. Every 24 frames. The target leaves the system its own margin: 12.5 ms at the top clock is a utilisation of 0.9 at 72 Hz. */
static void rqd_sample(double ms) {
    if (rqd.cfg != 2) return;
    rqd.w[rqd.n++] = (float)ms;
    if (rqd.n < 24u) return;
    rqd.n = 0;
    float s[24]; memcpy(s, rqd.w, sizeof s);
    for (int a = 1; a < 24; a++) { float v = s[a]; int b = a - 1; while (b >= 0 && s[b] > v) { s[b + 1] = s[b]; b--; } s[b + 1] = v; }
    float sig = s[21];
    {
        static float top = 0.0f;
        /* PS2_RQ_DYNRES_CEIL=1 (prompt 6, 2026-11-11): the clock the system will give, kgsl's max_gpuclk, caps the top clock the
           signal is read at. In the heat state (VrApi PLS=1, thermal_pwrlevel 3) the system holds 545 MHz whatever the load; read
           at 640 MHz the 12.5 ms target is 14.7 ms raw, over the 13.9 ms display period, and the scale settles on a load that
           drops frames. With the ceiling the scale gives up resolution down to its floor first. Only a ceiling under the top
           counts: at PLS 0 (max_gpuclk 690 MHz, the table's top) the signal is read as before. */
        static int ceil_on = -1; static float ceil_last = -1.0f;
        if (ceil_on < 0) { const char *e = getenv("PS2_RQ_DYNRES_CEIL"); ceil_on = e && *e == '1'; }
        FILE *f = fopen("/sys/class/kgsl/kgsl-3d0/gpuclk", "r");
        if (f) {
            long hz = 0;
            if (fscanf(f, "%ld", &hz) == 1 && hz > 0) {
                if ((float)hz > top) top = (float)hz;
                if (top < 640e6f) top = 640e6f;
                float tq = top;
                if (ceil_on) {
                    FILE *c = fopen("/sys/class/kgsl/kgsl-3d0/max_gpuclk", "r");
                    long ch = 0;
                    if (c) { if (fscanf(c, "%ld", &ch) != 1) ch = 0; fclose(c); }
                    float ce = ch > 0 && (float)ch < top ? (float)ch : 0.0f;
                    if (ce > 0.0f) tq = ce;
                    if (ce != ceil_last) {
                        if (ce > 0.0f) ps2_log("rq dynres: the GPU's ceiling %.0f MHz (max_gpuclk, under the top %.0f): the budget is read at that clock (PS2_RQ_DYNRES_CEIL)", (double)ce / 1e6, (double)top / 1e6);
                        else if (ceil_last > 0.0f) ps2_log("rq dynres: the GPU's ceiling back at the top %.0f MHz (max_gpuclk %.0f MHz; PS2_RQ_DYNRES_CEIL)", (double)top / 1e6, (double)ch / 1e6);
                        ceil_last = ce;
                    }
                    rqd.ceil = ce > 0.0f ? ce / 1e6f : 0.0f;
                }
                rqd.q = (float)hz / tq;
                rqd.mhz = (float)hz / 1e6f;
            }
            fclose(f);
        }
    }
    double v[7]; unsigned ok = ps2_xr_perf_metrics(v);
    rqd.src = 0;
    if ((ok & 2u) && v[1] > 0.5) { sig = (float)v[1]; rqd.src = 1; }
    if (rqd.q > 0.0f) sig *= rqd.q;
    float drops = 0.0f;
    if (ok & 0x20u) {
        static double prev = -1.0;
        if (prev >= 0.0 && v[5] >= prev) drops = (float)((v[5] - prev) / 0.4);
        prev = v[5];
    }
    {
        static float expect = -1.0f;
        if (expect < 0.0f) { const char *e = getenv("PS2_XR_REFRESH_HZ"); float hz = e && *e ? (float)atof(e) : 72.0f; expect = hz > 60.0f ? hz - 60.0f : 0.0f; }
        rqd.over_drop = drops > expect + 3.0f;
    }
    float ns = rqd.s;
    if (sig > rqd.ms) {
        ns = rqd.s * sqrtf(rqd.ms / sig);
        if (ns < rqd.s - 0.03f) ns = rqd.s - 0.03f;
        rqd.under = 0;
    } else if (rqd.over_drop && sig > 0.85f * rqd.ms) {
        ns = rqd.s - 0.01f;
        rqd.under = 0;
    } else if (sig < 0.92f * rqd.ms) {
        if (++rqd.under >= 2u) { ns = rqd.s + 0.01f; rqd.under = 0; }
    } else rqd.under = 0;
    if (ns < rqd.min) ns = rqd.min;
    if (ns > 1.0f) ns = 1.0f;
    if (sig > rqd.ms && ns <= rqd.min + 0.001f) {
        rqd.floor_fr += 24u;
        if (!(rqd.floor_fr % 600u)) ps2_log("rq dynres: at the floor %.2f and over the budget: %.1f ms at the top clock (now %.0f MHz)", (double)rqd.min, (double)sig, (double)rqd.mhz);
    } else rqd.floor_fr = 0u;
    rqd.s = ns;
    {   /* (the scale the frames really had: the ledger's and the gate's number) */
        static float sum, lo = 9.0f, hi, ssum; static u32 n;
        sum += ns; ssum += sig; n++; if (ns < lo) lo = ns; if (ns > hi) hi = ns;
        if (n >= 38u) { ps2_log("rq dynres: scale over the last 15 s: mean %.3f, min %.3f, max %.3f (%s %.1f ms at the %s clock, now %.0f MHz, dropped %.1f/s)", (double)(sum / (float)n), (double)lo, (double)hi, rqd.src ? "app GPU" : "p90", (double)(ssum / (float)n), rqd.ceil > 0.0f ? "ceiling's" : "top", (double)rqd.mhz, (double)drops); sum = 0.0f; ssum = 0.0f; n = 0u; lo = 9.0f; hi = 0.0f; }
    }
}
/* this frame's rectangle; the crop's A and B scaled about the centre */
static void rqd_frame(u32 w, u32 h) {
    u32 vw = w, vh = h;
    if (rqd_on()) {
        vw = (u32)((float)w * rqd.s / 16.0f + 0.5f) * 16u; vh = (u32)((float)h * rqd.s / 16.0f + 0.5f) * 16u;
        if (vw > w) vw = w;
        if (vh > h) vh = h;
    }
    rqd.vw = vw; rqd.vh = vh; rqd.vx = (w - vw) / 2u; rqd.vy = (h - vh) / 2u;
    float sx = (float)vw / (float)w, sy = (float)vh / (float)h;
    for (int k = 0; k < 4; k++) rqe.xf[k] *= sx;
    rqe.xf[4] *= sy; rqe.xf[5] *= sy;
    rqe.pxs *= sx;
    ps2_xr_rq_rect((int32_t)rqd.vx, (int32_t)rqd.vy, (int32_t)vw, (int32_t)vh);
}
/* swapchain image idx's framebuffer of the eye pass and the feed pass's set reading it (once per image view: at the image's
   first eye frame, or before the first eye frame with PS2_RQ_PREWARM); 0 = failed */
static int rq_eye_fb(u32 idx, VkImageView view) {
    if (idx >= RQ_MAX_IMG || !view) return 0;
    if (rqe.fv[idx] == view) return 1;
    const u64 rqpw_t0 = vk_mono_ns();
    if (rqe.fb[idx]) vkDestroyFramebuffer(dev, rqe.fb[idx], NULL);
    VkImageView av[5] = { rqe.mcv, rqe.mdv, view, rqe.rdv, !rqe.fdm ? VK_NULL_HANDLE : rq_pass_fdm(idx) };
    if (rqz_cfg()) av[3] = av[4];                /* (PS2_RQ_ZPRE: no depth resolve attachment; the density map is attachment 3) */
    VkFramebufferCreateInfo fi; memset(&fi, 0, sizeof fi);
    fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO; fi.renderPass = rqe.rp; fi.attachmentCount = (rqe.fdm ? 5 : 4) - (rqz_cfg() ? 1 : 0); fi.pAttachments = av; fi.width = rqe.w; fi.height = rqe.h; fi.layers = 1;
    if (vkCreateFramebuffer(dev, &fi, NULL, &rqe.fb[idx]) != VK_SUCCESS) { rqe.fb[idx] = VK_NULL_HANDLE; rqe.fv[idx] = VK_NULL_HANDLE; return 0; }
    rqe.fv[idx] = view;
    /* the feed pass's sources */
    if (!rqe.dsset[idx]) {
        VkDescriptorSetAllocateInfo ai; memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; ai.descriptorPool = rqe.dspool; ai.descriptorSetCount = 1; ai.pSetLayouts = &rqe.dsdsl;
        if (vkAllocateDescriptorSets(dev, &ai, &rqe.dsset[idx]) != VK_SUCCESS) { rqe.dsset[idx] = VK_NULL_HANDLE; return 0; }
    }
    VkDescriptorImageInfo ii[2]; memset(ii, 0, sizeof ii);
    ii[0].sampler = rqe.smp_sub ? rqe.smp_sub : sampler; ii[0].imageView = view; ii[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ii[1].sampler = sampler_point; ii[1].imageView = rqz_cfg() ? rq_feed.dview : rqe.rdv; ii[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;   /* (PS2_RQ_ZPRE: only its debug view, PS2_RQ_ZPRE_VIEW) */
    VkWriteDescriptorSet wr[2]; memset(wr, 0, sizeof wr);
    for (int i = 0; i < 2; i++) { wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[i].dstSet = rqe.dsset[idx]; wr[i].dstBinding = (uint32_t)i; wr[i].descriptorCount = 1; wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[i].pImageInfo = &ii[i]; }
    vkUpdateDescriptorSets(dev, 2, wr, 0, NULL);
    rqpw_made("an eye image's framebuffer and feed set", rqpw_t0);
    return 1;
}
/* the frame's image and crop; 0 = the frame stays at the target's size */
static int rq_eye_prepare(u32 seq) {
    uint32_t w, h, idx; VkImage img; VkImageView view; float c[2][4];
    rqtp_seq = seq;                                       /* (PS2_RQ_TESTPAT: the frame's phase) */
    if (!ps2_xr_rq_size(&w, &h) || !rq_eye_make(w, h) || !rq_feed.created) return 0;
    if (!ps2_xr_rq_acquire(seq, &idx, &img, &view, c)) return 0;
    if (idx >= RQ_MAX_IMG) return 0;
    if (rqf.on && rqf_fill()) rqf.due = 1;                 /* (PS2_RQ_FDM_OWN: the map from the eyes' fields, once; uploaded in rq_eye_begin) */
    if (rqf.on && rqf.due && rq_fdm_upload_first()) rq_oneshot(rqf_upload, "PS2_RQ_FDM_OWN's density map");   /* (R-48: before any pass is recorded with it) */
    if (rqf.on && !rqf.logged) return 0;                   /* (no map yet: the pass may not use its image) */
    if (!rq_eye_fb(idx, view)) return 0;
    rqe.idx = idx; rqe.img = img;
    /* the crops: [x0, x1] x [y0, y1] of the symmetric frame, normalised (ps2_xr_rq_acquire) */
    memcpy(rqe.crop, c, sizeof rqe.crop);
    for (int e = 0; e < 2; e++) {
        float x0 = c[e][0], x1 = c[e][1];
        rqe.xf[2 * e] = 1.0f / (x1 - x0);                  /* eye_ndc = A ndc + B */
        rqe.xf[2 * e + 1] = (1.0f - 2.0f * x0) / (x1 - x0) - 1.0f;
    }
    {   float y0 = c[0][2], y1 = c[0][3];
        rqe.xf[4] = 1.0f / (y1 - y0);
        rqe.xf[5] = (1.0f - 2.0f * y0) / (y1 - y0) - 1.0f;
    }
    rqe.xf[6] = (float)w; rqe.xf[7] = (float)h;
    rqe.pxs = (float)w / ((c[0][1] - c[0][0]) * (float)rt_w);
    rqd_frame(w, h);
    return 1;
}
/* the targets as select_target would have left them (the pass opens no target of ours) */
static void rq_close_others(VkCommandBuffer cmd) {
    VkImageMemoryBarrier2 batch[RT_SLOTS * 2u + 2u];
    u32 nb = 0;
    if (rendering_open) { vkCmdEndRendering(cmd); rendering_open = 0; }
    for (u32 i = 0; i < RT_SLOTS; i++) {
        vk_target *o = &rts[i];
        if (!o->created) continue;
        if (o->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && o->layout != VK_IMAGE_LAYOUT_UNDEFINED) {
            barrier_fill(&batch[nb++], o->color, VK_IMAGE_ASPECT_COLOR_BIT, o->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
            o->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        if (o->dlayout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL) {
            barrier_fill(&batch[nb++], o->depth, VK_IMAGE_ASPECT_DEPTH_BIT, o->dlayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
            o->dlayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
    }
    barrier_flush(cmd, batch, nb);
}
static void rq_eye_begin(VkCommandBuffer cmd) {
    rq_close_others(cmd);
    rqtm_build(cmd);                                        /* (PS2_RQ_TREE_AA bit 16: the tree impostors' mip chains, rq_treemip.inc) */
    rqf_upload(cmd);                                        /* (PS2_RQ_FDM_OWN: the map's first upload) */
    VkClearValue cv[4]; memset(cv, 0, sizeof cv);          /* (colour 0, depth 0.0: reversed depth, as begin_rendering clears) */
    VkRenderPassBeginInfo bi; memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; bi.renderPass = rqe.rp; bi.framebuffer = rqe.fb[rqe.idx];
    bi.renderArea.offset.x = (int32_t)rqd.vx; bi.renderArea.offset.y = (int32_t)rqd.vy;
    bi.renderArea.extent.width = rqd.vw; bi.renderArea.extent.height = rqd.vh; bi.clearValueCount = 4; bi.pClearValues = cv;
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    rendering_open = 1; fg_rp_open = 1; fg_rp_mv = 1; fg_epoch++; lr_mapped = 1; rq_open = 1; rqe.began = 1;
    rts[0].lr_dirty = 1; fg_cw[0] = 1; rts[0].drawn = 1;
    vrs_open_kind = 0;
}
/* the map of a game scissor rectangle (PS2 px) to the eye image: the union of the two views' rectangles */
static void rq_scissor(VkRect2D *sc, u32 dsc) {
    float W = (float)rqe.w, H = (float)rqe.h;
    float x0 = (float)sc->offset.x / (float)dsc, x1 = x0 + (float)sc->extent.width / (float)dsc;
    float y0 = (float)sc->offset.y / (float)dsc, y1 = y0 + (float)sc->extent.height / (float)dsc;
    float rw = (float)rt_w, rh = (float)rt_h;
    float ex0 = 1e9f, ex1 = -1e9f;
    for (int e = 0; e < 2; e++) {
        float a = (x0 / rw - rqe.crop[e][0]) / (rqe.crop[e][1] - rqe.crop[e][0]) * W;
        float b = (x1 / rw - rqe.crop[e][0]) / (rqe.crop[e][1] - rqe.crop[e][0]) * W;
        if (a < ex0) ex0 = a;
        if (b > ex1) ex1 = b;
    }
    float ey0 = (y0 / rh - rqe.crop[0][2]) / (rqe.crop[0][3] - rqe.crop[0][2]) * H;
    float ey1 = (y1 / rh - rqe.crop[0][2]) / (rqe.crop[0][3] - rqe.crop[0][2]) * H;
    s32 ix0 = (s32)floorf(ex0), ix1 = (s32)ceilf(ex1), iy0 = (s32)floorf(ey0), iy1 = (s32)ceilf(ey1);
    {   /* (PS2_RQ_DYNRES: the same rectangle in the scaled image, inside the render area) */
        float qx = (float)rqd.vw / W, qy = (float)rqd.vh / H;
        ix0 = (s32)floorf((float)ix0 * qx) + (s32)rqd.vx; ix1 = (s32)ceilf((float)ix1 * qx) + (s32)rqd.vx;
        iy0 = (s32)floorf((float)iy0 * qy) + (s32)rqd.vy; iy1 = (s32)ceilf((float)iy1 * qy) + (s32)rqd.vy;
    }
    if (ix0 < (s32)rqd.vx) ix0 = (s32)rqd.vx;
    if (iy0 < (s32)rqd.vy) iy0 = (s32)rqd.vy;
    if (ix1 > (s32)(rqd.vx + rqd.vw)) ix1 = (s32)(rqd.vx + rqd.vw);
    if (iy1 > (s32)(rqd.vy + rqd.vh)) iy1 = (s32)(rqd.vy + rqd.vh);
    if (ix1 <= ix0) ix1 = ix0 + 1;
    if (iy1 <= iy0) iy1 = iy0 + 1;
    sc->offset.x = ix0; sc->offset.y = iy0; sc->extent.width = (u32)(ix1 - ix0); sc->extent.height = (u32)(iy1 - iy0);
}
/* after the pass: the feed for the next frame, the swapchain image back to COLOR_ATTACHMENT_OPTIMAL */
#include "rq_zpre.inc"
#include "rq_rec.inc"
static void rq_eye_after(VkCommandBuffer cmd, vk_frame *fr) {
    vk_target *f = &rq_feed;
    u32 w = tew(f) * tsc(f), h = teh(f) * tsc(f);
    int dec = rqfd_frame(cmd);                              /* (PS2_RQ_FEED_DECODE: the eye image decoded first; the feed reads that) */
    VkFramebuffer xfb = dec ? VK_NULL_HANDLE : rqx_feed_fb(f, w, h);   /* (PS2_RQ_FEED_FDM: the feed pass's twin with a density map) */
    if (xfb) { if (rqx.up_due && rq_fdm_upload_first()) rq_oneshot(rqx_upload, "the twin passes' full-density map"); rqx_upload(cmd); }
    VkFramebuffer fb = xfb ? xfb : rqz_cfg() ? rqz_feedfb(f) : fg_fb_get_any(rqe.dsrp, f->cview, f->dview, tew(f) * tsc(f), teh(f) * tsc(f));
    if (fb) {
        VkClearValue cv[2]; memset(cv, 0, sizeof cv);
        VkRenderPassBeginInfo bi; memset(&bi, 0, sizeof bi);
        bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; bi.renderPass = xfb ? rqx.rp : rqe.dsrp; bi.framebuffer = fb; bi.renderArea.extent.width = w; bi.renderArea.extent.height = h; bi.clearValueCount = 2; bi.pClearValues = cv;
        vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport vp; memset(&vp, 0, sizeof vp); vp.width = (float)w; vp.height = (float)h; vp.maxDepth = 1.0f;
        VkRect2D sc; memset(&sc, 0, sizeof sc); sc.extent.width = w; sc.extent.height = h;
        vkCmdSetViewport(cmd, 0, 1, &vp); vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, xfb ? rqx.pipe : rqe.dspipe);
        VkDescriptorSet src = rq_eye_src_set();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqe.dslo, 0, 1, &src, 0, NULL);
        float p[8] = { rqe.crop[0][0] * (float)w, rqe.crop[0][1] * (float)w, rqe.crop[1][0] * (float)w, rqe.crop[1][1] * (float)w,
                       rqe.crop[0][2] * (float)h, rqe.crop[0][3] * (float)h, (float)rqd.vw / (float)rqe.w * (rqh_fade_a0 ? -1.0f : 1.0f), (float)rqd.vh / (float)rqe.h * (rqz_view() ? -1.0f : 1.0f) };   /* (sc.x negative: the skipped fade quad's alpha 0, rq_hud.inc) */
        vkCmdPushConstants(cmd, rqe.dslo, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, p);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        f->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (rqz_cfg()) rqz.mark_ok = rql.cp_quest;                 /* (the fill of the next pre-pass reads the mark the feed's alpha keeps: a real fade quad's alpha 0 leaves none, the fill then discards everything) */
        else f->dlayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        rq_feed_valid = 1;
        rqr_grab_eye(cmd, fr);             /* (ACZ_QUEST_RECORD: the left eye to NV12 while the image is still SHADER_READ_ONLY) */
    }
    if (!dec) rqx_decode(cmd);             /* (PS2_RQ_SC_DECODE: a dump frame's eye images through a pass with the density map, for the _sc files;
                                              with PS2_RQ_FEED_DECODE this frame's decode is already in rqx.dimg) */
    rqfd.cur = VK_NULL_HANDLE;
    VkImageMemoryBarrier2 b;
    barrier_fill(&b, rqe.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
    barrier_flush(cmd, &b, 1);
    rq_open = 0;
    rqe.drawn = 1;
}
/* in place of the eye copies: 1 = this frame's eyes are the pass's own images */
static int rq_eye_present(VkCommandBuffer cmd, u32 seq, int dump, u32 x0, u32 y0, u32 w, u32 h) {
    if (!rqe.drawn) return 0;
    rqe.drawn = 0;
    ps2_xr_rq_present(seq);
    if (dump) for (int eye = 0; eye < 2; eye++) {
        eyedump_record(cmd, rq_feed.color, rq_feed.layout, x0, y0, w, h, eye);   /* (the 1x feed: eye_*.ppm) */
        scdump_record(cmd, eye, seq);                                            /* (the eye image itself: _sc) */
    }
    rqx.dfresh = 0;
    return 1;
}

/* ---- R2a (PS2_RQ_TERRAIN=1, NOTES_RQ): the terrain's detail passes 1-3 folded into its first pass ----
   mesh.vert / mesh.frag -DRQT carry the passes' uv and fades and subtract the passes' textures in the first pass's shader: one
   draw per patch instead of up to four (no DATE reads, no extra fill). The draws of passes 1-3 are dropped from the frame; their
   textures (the same for every patch of a frame) are bound once per frame in set 4. */
static VkImageView rqt_views[FRAMES_IN_FLIGHT][3];
static u8 rqt_written[FRAMES_IN_FLIGHT];
static int rqt_want(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_TERRAIN"); v = e && (*e == '1' || *e == '2' || *e == '3'); rqt_lean_on = e && *e == '2'; if (v) ps2_log("vk: PS2_RQ_TERRAIN=1: the terrain's detail passes folded into its first pass"); }
    return v && !perf_ab_flipped("rqterrain");
}
static int rqt_make(void) {
    if (rqt.ok) return 1;
    if (rqt.tried) return 0;
    rqt.tried = 1;
    const u64 rqpw_t0 = vk_mono_ns();
    for (int q = 0; q < 4; q++) if (!rqt_v[q]) { ps2_log("vk: PS2_RQ_TERRAIN: vertex shader %d missing", q); return 0; }
    VkDescriptorSetLayoutBinding b[3]; memset(b, 0, sizeof b);
    for (u32 q = 0; q < 3u; q++) { b[q].binding = q; b[q].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b[q].descriptorCount = 1; b[q].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT; }   /* (rqt_l3 also per vertex: PS2_RQ_L3V) */
    VkDescriptorSetLayoutCreateInfo lc; memset(&lc, 0, sizeof lc);
    lc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; lc.bindingCount = 3; lc.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev, &lc, NULL, &rqt.dsl) != VK_SUCCESS) return 0;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * FRAMES_IN_FLIGHT };
    VkDescriptorPoolCreateInfo pc; memset(&pc, 0, sizeof pc);
    pc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; pc.maxSets = FRAMES_IN_FLIGHT; pc.poolSizeCount = 1; pc.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(dev, &pc, NULL, &rqt.pool) != VK_SUCCESS) return 0;
    VkDescriptorSetLayout ls[FRAMES_IN_FLIGHT];
    for (u32 q = 0; q < FRAMES_IN_FLIGHT; q++) ls[q] = rqt.dsl;
    VkDescriptorSetAllocateInfo ai; memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; ai.descriptorPool = rqt.pool; ai.descriptorSetCount = FRAMES_IN_FLIGHT; ai.pSetLayouts = ls;
    if (vkAllocateDescriptorSets(dev, &ai, rqt.set) != VK_SUCCESS) return 0;
    /* sets 0..2 as the mesh pipelines', set 3 only as a filler (unused by the -DRQT shaders), set 4 ours */
    VkDescriptorSetLayout sl[5] = { deslayout, deslayout, mesh_slayout, rnb.tlayout ? rnb.tlayout : deslayout, rqt.dsl };   /* (set 3: the batched draws' texture table) */
    VkPushConstantRange pr; memset(&pr, 0, sizeof pr);
    pr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT; pr.size = sizeof(gs_push);
    VkPipelineLayoutCreateInfo pl; memset(&pl, 0, sizeof pl);
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pl.setLayoutCount = 5; pl.pSetLayouts = sl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev, &pl, NULL, &rqt.layout) != VK_SUCCESS) return 0;
    {   VkSamplerCreateInfo sc; memset(&sc, 0, sizeof sc);        /* (the detail textures repeat: uv x 16) */
        sc.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO; sc.magFilter = sc.minFilter = VK_FILTER_LINEAR; sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT; sc.maxLod = 0.25f;
        if (vkCreateSampler(dev, &sc, NULL, &rqt.smp) != VK_SUCCESS) return 0;
    }
    rqt.ok = 1;
    rqpw_made("the folded terrain's layouts (PS2_RQ_TERRAIN)", rqpw_t0);
    return 1;
}
/* this frame's textures of passes 1, 2, 3, read from the list; 0 = no terrain in it */
static int rqt_frame_setup(vk_frame *fr, const u32 *order) {
    (void)order;
    if (!rep_mesh || !rqt_make()) return 0;
    VkImageView v[3] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
    int any0 = 0;
    for (u32 q = 0; q < rep_ndraws; q++) {
        const vk_draw *d = &rep_draws[q];
        if (d->native != PS2_VK_NATIVE_MESH || d->mblock >= rep_mesh->nblocks || rep_mesh->blocks[d->mblock].prog != 1u) continue;
        u32 pass = rep_mesh->blocks[d->mblock].pass;
        if (!pass) { any0 = 1; continue; }
        if (pass > 3u || v[pass - 1u]) continue;
        if (d->tex_rt && d->tex_rt <= RT_SLOTS) v[pass - 1u] = ((rq_snap_mask >> (d->tex_rt - 1u)) & 1u) && rq_snapd[q] ? RQ_SNAP_OF(d->tex_rt - 1u, rq_snapd[q])->cview : rts[d->tex_rt - 1u].cview;
        if (pass == 3u && d->tex_rt && d->tex_rt <= RT_SLOTS) {        /* (the layer's texels in its target's image: as mesh.frag's tc / img_size) */
            rqt.s3[0] = d->tex_w > 0.0f ? d->tex_w / (float)(rtz_norm(d) ? tew(&rts[d->tex_rt - 1u]) : rt_w) : 1.0f;
            rqt.s3[1] = d->tex_h > 0.0f ? d->tex_h / (float)(rtz_norm(d) ? teh(&rts[d->tex_rt - 1u]) : rt_h) : 1.0f;
        }
        else if (d->tex != 0xFFFF && d->tex < ntextures) v[pass - 1u] = textures[d->tex].view;
    }
    if (!any0) return 0;
    VkDescriptorImageInfo ii[3]; memset(ii, 0, sizeof ii);
    VkWriteDescriptorSet w[3]; memset(w, 0, sizeof w);
    u32 slot = (u32)(fr - frames);
    if (slot >= FRAMES_IN_FLIGHT) return 0;
    for (u32 k = 0; k < 3u; k++) {
        ii[k].sampler = rqt.smp; ii[k].imageView = v[k] ? v[k] : white_view; ii[k].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[k].dstSet = rqt.set[slot]; w[k].dstBinding = k; w[k].descriptorCount = 1;
        w[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[k].pImageInfo = &ii[k];
    }
    if (!rqt_written[slot] || memcmp(rqt_views[slot], v, sizeof v)) {          /* (only when they change: the A/B re-draws re-enter here within one command buffer) */
        vkUpdateDescriptorSets(dev, 3, w, 0, NULL);
        memcpy(rqt_views[slot], v, sizeof v); rqt_written[slot] = 1;
    }
    return 1;
}

/* PS2_RQ_LEAN (default off): the GS draws that use gs.frag's ordinary path only take rq_gs.frag in the eye-sized pass */
/* PS2_RQ_X3=1: the game's full-screen glow overlay through rq_gs_rt.frag (the topology byte's bit 4 without bit 5) */
static int rq_x3_cfg(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_X3"); v = e && *e == '1'; if (v) ps2_log("vk: PS2_RQ_X3=1: the full-screen glow overlay through its own shader"); }
    return v;
}
static int rq_lean_cfg(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_LEAN"); v = e && *e == '1'; }
    return v;
}
/* ---- R-15: our clouds and the 3D cockpit in the eye-sized pass, as layers (PS2_RQ_LAYERS, default on with PS2_RQ=eye) ----
   Their files stay as they are (ps2_clouds*.c, cloud_*.comp, cockpit*.frag belong to the other session):
   - the clouds run their normal cl_point (compute + composite) before the pass, into a private 1x canvas instead of slot
     0, against the PREVIOUS frame's depth (rq_feed: one frame late). The composite is dst = L + dst T with RGB written
     only, so two runs tell L and T apart: a canvas cleared to 0 gives L, the same composite (composite_only) over a canvas
     cleared to 1 gives L + T. Inside the pass one full-screen draw (rq_layer.frag -DCL) does L + dst T at the eye's size.
   - the 3D cockpit's own multisampled pass runs before the pass (the PS2_PERF_COCKPIT_FOLD path); inside it one
     full-screen draw (rq_layer.frag) puts the resolved image over the frame, as cpit_composite does. Its depth is not
     written into the eye pass (nothing later needs it: the HUD does not test against it in the frames checked).
   Limits (BUGS_RQ R-15): the cloud-vs-scene edge is the 1x canvas's and one frame late; the effects' stamp goes into
   the feed's depth, not the eye's; the eye adaptation (expo_pass) and the soft effects are off in those frames. */
static int rql_cfg(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_LAYERS"); v = !(e && *e == '0'); }
    return v;
}
/* the canvases, before the list's first draw */
static int rql_cl_make(void) {
    vk_target *s = &rts[0];
    if (!s->created) return 0;
    for (int k = 0; k < 2; k++) {
        vk_target *t = &rql.can[k];
        if (t->created && (t->ew != s->ew || t->eh != s->eh || t->sc != s->sc || t->mono != s->mono || tew(t) != tew(s) || teh(t) != teh(s))) {
            vkDeviceWaitIdle(dev);
            destroy_target_images(t);
            rql.can_set_ok = 0;
        }
        if (!t->created) {
            t->ew = s->ew; t->eh = s->eh; t->sc = s->sc; t->mono = s->mono;
            if (create_target(t) != 0) return 0;
            rql.can_set_ok = 0;
        }
    }
    return 1;
}
static int rql_pipes(void) {
    if (rql.rp_for == rqe.rp && rql.pc && rql.pp) return 1;
    if (!rqf_lc || !rqf_lp || !rqv_ds) return 0;
    const u64 rqpw_t0 = vk_mono_ns();
    if (!rql.dsl) {
        VkDescriptorSetLayoutBinding bi[2]; memset(bi, 0, sizeof bi);
        for (int k = 0; k < 2; k++) { bi[k].binding = (uint32_t)k; bi[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; bi[k].descriptorCount = 1; bi[k].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; }
        VkDescriptorSetLayoutCreateInfo li; memset(&li, 0, sizeof li); li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; li.bindingCount = 2; li.pBindings = bi;
        if (vkCreateDescriptorSetLayout(dev, &li, NULL, &rql.dsl) != VK_SUCCESS) return 0;
        VkDescriptorPoolSize ps; ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ps.descriptorCount = 2u * (1u + FRAMES_IN_FLIGHT);
        VkDescriptorPoolCreateInfo pi; memset(&pi, 0, sizeof pi); pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; pi.maxSets = 1u + FRAMES_IN_FLIGHT; pi.poolSizeCount = 1; pi.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(dev, &pi, NULL, &rql.pool) != VK_SUCCESS) return 0;
        VkDescriptorSetLayout L[1u + FRAMES_IN_FLIGHT];
        for (u32 k = 0; k < 1u + FRAMES_IN_FLIGHT; k++) L[k] = rql.dsl;
        VkDescriptorSet S[1u + FRAMES_IN_FLIGHT];
        VkDescriptorSetAllocateInfo ai; memset(&ai, 0, sizeof ai); ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; ai.descriptorPool = rql.pool; ai.descriptorSetCount = 1u + FRAMES_IN_FLIGHT; ai.pSetLayouts = L;
        if (vkAllocateDescriptorSets(dev, &ai, S) != VK_SUCCESS) return 0;
        rql.can_set = S[0];
        for (u32 k = 0; k < FRAMES_IN_FLIGHT; k++) rql.cp_set[k] = S[1u + k];
        VkPushConstantRange pr; memset(&pr, 0, sizeof pr); pr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; pr.size = 32;
        VkPipelineLayoutCreateInfo pl; memset(&pl, 0, sizeof pl); pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pl.setLayoutCount = 1; pl.pSetLayouts = &rql.dsl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pr;
        if (vkCreatePipelineLayout(dev, &pl, NULL, &rql.lo) != VK_SUCCESS) return 0;
    }
    VkPipelineShaderStageCreateInfo st[2]; memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = rqv_ds; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin; memset(&vin, 0, sizeof vin); vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia; memset(&ia, 0, sizeof ia); ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vs; memset(&vs, 0, sizeof vs); vs.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vs.viewportCount = 1; vs.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs; memset(&rs, 0, sizeof rs); rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms; memset(&ms, 0, sizeof ms); ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = rq_samples();
    VkPipelineDepthStencilStateCreateInfo dss; memset(&dss, 0, sizeof dss); dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
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
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &dss; gp.pColorBlendState = &cb; gp.pDynamicState = &dsi; gp.layout = rql.lo; gp.renderPass = rqe.rp;
    st[1].module = rqf_lc;
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &rql.pc) != VK_SUCCESS) return 0;
    st[1].module = rqf_lp;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &rql.pp) != VK_SUCCESS) return 0;
    rql.rp_for = rqe.rp;
    rqpw_made("the layers' pipelines (rql_pipes)", rqpw_t0);
    return 1;
}
static void rql_write(VkDescriptorSet s, VkImageView a, VkImageView b) {
    VkDescriptorImageInfo ii[2]; memset(ii, 0, sizeof ii);
    ii[0].sampler = sampler; ii[0].imageView = a; ii[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ii[1] = ii[0]; ii[1].imageView = b;
    VkWriteDescriptorSet wr[2]; memset(wr, 0, sizeof wr);
    for (int k = 0; k < 2; k++) { wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[k].dstSet = s; wr[k].dstBinding = (uint32_t)k; wr[k].descriptorCount = 1; wr[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[k].pImageInfo = &ii[k]; }
    vkUpdateDescriptorSets(dev, 2, wr, 0, NULL);
}
/* ---- PS2_RQ_TESTPAT [test] (outside note 5, step A, 2026-11-10): the ground truth for every reader of the eye image.
   The eye pass's last draw covers its render area with rq_testpat.frag: an analytic picture of gl_FragCoord (R the x ramp,
   reversed in view 1; G the y ramp; B a checker of N px whose levels carry the frame's sequence number mod 4). Each reader
   (the raw _sc copy, PS2_RQ_SC_DECODE, the 1x feed with or without PS2_RQ_FEED_DECODE / PS2_RQ_FEED_FDM, the recorder, a
   system screenshot) is then compared with the formula per bin (tools: the coordinator's testpat.py). =1: 16 px cells;
   =N (2..256): N px. Off: nothing is drawn. */
static struct { int cfg; VkPipelineLayout lo; VkPipeline pipe; VkRenderPass rp_for; int said; } rqtp;
static int rqtp_cfg(void) {
    if (!rqtp.cfg) {
        const char *e = getenv("PS2_RQ_TESTPAT"); int v = e && *e ? atoi(e) : 0;
        rqtp.cfg = v <= 0 ? -1 : v == 1 ? 1000 : v > 256 ? 256 : v;   /* (1000: no checker, B = the phase only) */
        if (rqtp.cfg > 0) ps2_log("vk: PS2_RQ_TESTPAT=%s: the eye pass ends with the test picture (%s; the frame's phase = its sequence number mod 4)", e, rqtp.cfg == 1000 ? "B = the phase only" : "a checker in B");
    }
    return rqtp.cfg > 0;
}
static int rqtp_pipe(void) {
    if (rqtp.pipe && rqtp.rp_for == rqe.rp) return 1;
    if (!rqtp_fs || !rqv_ds || !rqe.rp) return 0;
    if (!rqtp.lo) {
        VkPushConstantRange pr; memset(&pr, 0, sizeof pr); pr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; pr.size = 16;
        VkPipelineLayoutCreateInfo pl; memset(&pl, 0, sizeof pl); pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pr;
        if (vkCreatePipelineLayout(dev, &pl, NULL, &rqtp.lo) != VK_SUCCESS) { rqtp.lo = VK_NULL_HANDLE; return 0; }
    }
    VkPipelineShaderStageCreateInfo st[2]; memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = rqv_ds; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = rqtp_fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin; memset(&vin, 0, sizeof vin); vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia; memset(&ia, 0, sizeof ia); ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vs; memset(&vs, 0, sizeof vs); vs.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vs.viewportCount = 1; vs.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs; memset(&rs, 0, sizeof rs); rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms; memset(&ms, 0, sizeof ms); ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = rq_samples();
    VkPipelineDepthStencilStateCreateInfo dss; memset(&dss, 0, sizeof dss); dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    VkPipelineColorBlendAttachmentState cba; memset(&cba, 0, sizeof cba); cba.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo cb; memset(&cb, 0, sizeof cb); cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cb.attachmentCount = 1; cb.pAttachments = &cba;
    static const VkDynamicState dy[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsi; memset(&dsi, 0, sizeof dsi); dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; dsi.dynamicStateCount = 2; dsi.pDynamicStates = dy;
    VkGraphicsPipelineCreateInfo gp; memset(&gp, 0, sizeof gp);
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia; gp.pViewportState = &vs;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &dss; gp.pColorBlendState = &cb; gp.pDynamicState = &dsi; gp.layout = rqtp.lo; gp.renderPass = rqe.rp;
    if (rqtp.pipe) vkDestroyPipeline(dev, rqtp.pipe, NULL);
    rqtp.pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &rqtp.pipe) != VK_SUCCESS) { rqtp.pipe = VK_NULL_HANDLE; ps2_log("vk: PS2_RQ_TESTPAT: the pipeline could not be made"); return 0; }
    rqtp.rp_for = rqe.rp;
    return 1;
}
/* PS2_RQ_ZPRE_AMASK (agent clouds3 2026-11-10, R-54; default 0, the Quest default set in ps2_android.c): with PS2_RQ_ZPRE and the HUD batch's
   skipped fade quad (rqh_fade_a0), rq_ds.frag -DNOZ keeps every feed alpha within 16/255 +- 5.5/255 as the Quest cockpit's coverage mark, and the
   next pre-pass writes cockpit depth there (rq_zpre_ck.frag): the near clouds stop. The game's sun overlays after the cockpit write alpha too (M4 in
   cloud facing the sun: the full-screen ring draw, GS (Cd - Cs) FIX, tex 421): their soft ring edges fell into the band, the near clouds were cut in
   rings and the far cloud / sky behind showed as thick dark-blue rings with 1x / near-texel stair-steps (C3). This last draw of the eye pass writes
   alpha 0 wherever the depth lies below the cockpit's range (depth bounds [0, 0.9956), colour mask A only, no blend): the PS2 fade quad's alpha
   write, minus the cockpit, so only the cockpit's own mark survives. The colour is untouched; the feed's alpha is 0 there already (rq_ds). */
/* PS2_RQ_ZPRE_AMASK=2 (R-54 on the device, 2026-11-11): every eye-pass frame, not only those whose HUD batch skipped the fade quad. On the
   headset the rings stayed with =1 (M4 field 2900, the feed's depth view: the rings in the cockpit band; gone with the ring draw skipped or
   without the pre-pass): a frame without the skipped fade quad (no HUD batch: an in-between frame, PS2_RQ_HUD's refusals) wrote the ring
   draw's alpha into the feed unmasked, the next pre-pass took it as cockpit coverage. The write is right in every frame: alpha 0 only where
   the eye pass's own depth lies below the cockpit's range. The log counts the frames =1 skipped (rq amask:). */
static struct { int cfg; VkPipelineLayout lo; VkPipeline pipe; VkRenderPass rp_for; u64 n, nf, nrp; } rqam;
static int rqam_cfg(void) {
    if (!rqam.cfg) {
        const char *e = getenv("PS2_RQ_ZPRE_AMASK");
        rqam.cfg = e && *e == '2' ? 2 : e && *e == '1' ? 1 : -1;
        ps2_log("vk: PS2_RQ_ZPRE_AMASK=%d: %s", rqam.cfg > 0 ? rqam.cfg : 0, rqam.cfg == 2 ? "every eye-pass frame ends with alpha 0 outside the cockpit (no false cockpit marks from the sun's overlays)"
                : rqam.cfg == 1 ? "the eye pass ends with alpha 0 outside the cockpit where the HUD batch skipped the fade quad"
                : "the feed's mark band takes any alpha in it as the cockpit's coverage");
    }
    return rqam.cfg > 0;
}
/* its pipeline (at its first draw, or before the first eye frame: PS2_RQ_PREWARM); 0 = none */
static int rqam_make(void) {
    if (rqam.pipe && rqam.rp_for == rqe.rp) return 1;
    if (!rqz_am_fs || !rqv_ds || !rqe.rp) return 0;
    {   const u64 rqpw_t0 = vk_mono_ns();
        if (!rqam.lo) {
            VkPipelineLayoutCreateInfo pl; memset(&pl, 0, sizeof pl); pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            if (vkCreatePipelineLayout(dev, &pl, NULL, &rqam.lo) != VK_SUCCESS) { rqam.lo = VK_NULL_HANDLE; return 0; }
        }
        VkPipelineShaderStageCreateInfo st[2]; memset(st, 0, sizeof st);
        st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = rqv_ds; st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = rqz_am_fs; st[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vin; memset(&vin, 0, sizeof vin); vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia; memset(&ia, 0, sizeof ia); ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vs; memset(&vs, 0, sizeof vs); vs.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vs.viewportCount = 1; vs.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs; memset(&rs, 0, sizeof rs); rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms; memset(&ms, 0, sizeof ms); ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = rq_samples();
        VkPipelineDepthStencilStateCreateInfo dss; memset(&dss, 0, sizeof dss); dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        dss.depthBoundsTestEnable = VK_TRUE; dss.minDepthBounds = 0.0f; dss.maxDepthBounds = 0.99555f;   /* (the Quest cockpit: (0.9956, 1]) */
        VkPipelineColorBlendAttachmentState cba; memset(&cba, 0, sizeof cba); cba.colorWriteMask = VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb; memset(&cb, 0, sizeof cb); cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cb.attachmentCount = 1; cb.pAttachments = &cba;
        static const VkDynamicState dy[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dsi; memset(&dsi, 0, sizeof dsi); dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; dsi.dynamicStateCount = 2; dsi.pDynamicStates = dy;
        VkGraphicsPipelineCreateInfo gp; memset(&gp, 0, sizeof gp);
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia; gp.pViewportState = &vs;
        gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &dss; gp.pColorBlendState = &cb; gp.pDynamicState = &dsi; gp.layout = rqam.lo; gp.renderPass = rqe.rp;
        if (rqam.pipe) vkDestroyPipeline(dev, rqam.pipe, NULL);
        rqam.pipe = VK_NULL_HANDLE;
        if (vkCreateGraphicsPipelines(dev, pipe_cache, 1, &gp, NULL, &rqam.pipe) != VK_SUCCESS) { rqam.pipe = VK_NULL_HANDLE; ps2_log("vk: PS2_RQ_ZPRE_AMASK: the pipeline could not be made"); return 0; }
        rqam.rp_for = rqe.rp;
        rqpw_made("the alpha mask's pipeline (PS2_RQ_ZPRE_AMASK)", rqpw_t0);
    }
    return 1;
}
static void rqz_amask(VkCommandBuffer cmd) {
    if (!rqam_cfg() || !rqz_cfg() || !rqz_am_fs || !rqv_ds || !rqe.rp) return;
    if (!rqh_fade_a0) {
        rqam.nf++; if (xr_rp_on) rqam.nrp++;
        if (rqam.nf == 1u || !(rqam.nf % 600u))
            ps2_log("rq amask: %llu eye frames without the HUD batch's skipped fade quad (%llu of them in-between frames), %llu masked so far%s (field %u)",
                    (unsigned long long)rqam.nf, (unsigned long long)rqam.nrp, (unsigned long long)rqam.n, rqam.cfg == 2 ? "; masked too (=2)" : "; NOT masked (=1)", ps2_gs_frame_count);
        if (rqam.cfg < 2) return;
    }
    if (!rqam_make()) return;
    VkViewport vp; memset(&vp, 0, sizeof vp); vp.width = (float)rqe.w; vp.height = (float)rqe.h; vp.maxDepth = 1.0f;
    VkRect2D sc; memset(&sc, 0, sizeof sc); sc.offset.x = (int32_t)rqd.vx; sc.offset.y = (int32_t)rqd.vy; sc.extent.width = rqd.vw ? rqd.vw : rqe.w; sc.extent.height = rqd.vh ? rqd.vh : rqe.h;
    vkCmdSetViewport(cmd, 0, 1, &vp); vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqam.pipe);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    rqam.n++;
}
/* ps2_video.c, the eye pass still open, after its last draw */
static void rqtp_draw(VkCommandBuffer cmd) {
    rqz_amask(cmd);                                       /* (PS2_RQ_ZPRE_AMASK: the eye pass's real last draw, before the test picture) */
    if (!rqtp_cfg() || !rqtp_pipe()) return;
    VkViewport vp; memset(&vp, 0, sizeof vp); vp.width = (float)rqe.w; vp.height = (float)rqe.h; vp.maxDepth = 1.0f;
    VkRect2D sc; memset(&sc, 0, sizeof sc); sc.offset.x = (int32_t)rqd.vx; sc.offset.y = (int32_t)rqd.vy; sc.extent.width = rqd.vw; sc.extent.height = rqd.vh;
    vkCmdSetViewport(cmd, 0, 1, &vp); vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqtp.pipe);
    float p[4] = { rqtp.cfg == 1000 ? 0.0f : (float)rqtp.cfg, (float)(rqtp_seq & 3u), (float)rqe.w, (float)rqe.h };
    vkCmdPushConstants(cmd, rqtp.lo, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, p);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    if (xr_dump_now || rqtp.said < 2) {
        rqtp.said++;
        ps2_log("vk: PS2_RQ_TESTPAT: game frame %u, phase %u, render area x %u..%u y %u..%u of %ux%u (layer 1's R ramp reversed)",
                rqtp_seq, rqtp_seq & 3u, rqd.vx, rqd.vx + rqd.vw, rqd.vy, rqd.vy + rqd.vh, rqe.w, rqe.h);
    }
}
/* one cl_point against a canvas and the feed's depth */
static int rql_point(VkCommandBuffer cmd, vk_frame *fr, int which, vk_target *t, int co) {
    vk_target *zt = &rq_feed;
    ps2_cloud_pass p; memset(&p, 0, sizeof p);
    p.cmd = cmd; p.frame_index = (int)(fr - frames); p.cmd_id = cl_cmd_serial;
    p.color = t->color; p.color_view = t->cview; p.color_layout = t->layout;
    p.depth = zt->depth; p.depth_layout = &zt->dlayout; p.layers = rt_layers;
    p.eye = -1;
    p.view = &cl_view;
    if (co) {
        p.composite_only = 1;
        p.clip[0] = 0; p.clip[1] = 0; p.clip[2] = (s32)(tew(t) * tsc(t)); p.clip[3] = (s32)(teh(t) * tsc(t));
        p.near_h_on = 1;
        static const float id[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        memcpy(p.near_h, id, sizeof id);
    }
    return which ? ps2_clouds_near_point(&p) : ps2_clouds_far_point(&p);
}
static void rql_clear(VkCommandBuffer cmd, vk_target *t, float v) {
    VkImageMemoryBarrier2 b;
    barrier_fill(&b, t->color, VK_IMAGE_ASPECT_COLOR_BIT, t->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    barrier_flush(cmd, &b, 1);
    VkClearColorValue cv; memset(&cv, 0, sizeof cv); cv.float32[0] = cv.float32[1] = cv.float32[2] = v; cv.float32[3] = 0.0f;
    VkImageSubresourceRange r; memset(&r, 0, sizeof r); r.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; r.levelCount = 1; r.layerCount = rt_layers;
    vkCmdClearColorImage(cmd, t->color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &r);
    barrier_fill(&b, t->color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, RT_ATT_LAYOUT,
                 VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    barrier_flush(cmd, &b, 1);
    t->layout = RT_ATT_LAYOUT;
}
/* before the eye pass begins (no rendering open after it). Returns 1 if anything was recorded */
static int rql_before(VkCommandBuffer cmd, vk_frame *fr) {
    if (!rql.cl_want && !rql.cp_want) return 0;
    rq_close_others(cmd);
    if (rql_pipes()) {
        if (rql.cl_want && rq_feed.created && rq_feed.dlayout != VK_IMAGE_LAYOUT_UNDEFINED && rqcl_want() && rqcl_make()) {
            rql.cl_ok = rqcl_prepare(cmd, fr);                  /* (PS2_RQ_CLOUDS: the compute only; rq_clouds_composite in the pass) */
            if (rql.cl_ok && rqfx_cfg()) rqfx_dispatch(cmd, fr);   /* (PS2_RQ_CLOUDS_FXFADE: the effects' fade, rq_fxfade.inc) */
            if (rq_feed.dlayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                VkImageMemoryBarrier2 bz;
                barrier_fill(&bz, rq_feed.depth, VK_IMAGE_ASPECT_DEPTH_BIT, rq_feed.dlayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
                barrier_flush(cmd, &bz, 1); rq_feed.dlayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
        } else if (rql.cl_want && rq_feed.created && rq_feed.dlayout != VK_IMAGE_LAYOUT_UNDEFINED) {
            vk_target *A = &rql.can[0], *B = &rql.can[1];
            rql_clear(cmd, A, 0.0f); rql_clear(cmd, B, 1.0f);
            int did = rql_point(cmd, fr, 0, A, 0);
            did |= rql_point(cmd, fr, 1, A, 0);
            if (did) { rql_point(cmd, fr, 0, B, 1); rql_point(cmd, fr, 1, B, 1); }
            VkImageMemoryBarrier2 b[3]; u32 n = 0;
            for (int k = 0; k < 2; k++) {
                vk_target *t = &rql.can[k];
                barrier_fill(&b[n++], t->color, VK_IMAGE_ASPECT_COLOR_BIT, t->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
                t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
            if (rq_feed.dlayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                barrier_fill(&b[n++], rq_feed.depth, VK_IMAGE_ASPECT_DEPTH_BIT, rq_feed.dlayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
                rq_feed.dlayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
            barrier_flush(cmd, b, n);
            if (!rql.can_set_ok) { vkDeviceWaitIdle(dev); rql_write(rql.can_set, A->cview, B->cview); rql.can_set_ok = 1; }
            rql.cl_ok = did;
            { static u32 nl_; if (!(nl_++ % 300u)) ps2_log("rq layers: clouds did %d feed dlayout %d can %u x %u near %d far %d", did, (int)rq_feed.dlayout, tew(A), teh(A), ps2_clouds_cfg()->near_on, ps2_clouds_cfg()->far_on); }
        }
        if (rql.cp_want) {
            rql.cp_quest = rqc_before_pass(cmd, fr);                /* (ACZ_VR_COCKPIT_MATERIAL=quest: rq_cockpit.inc) */
            if (rql.cp_quest) rql.cp_ok = 1;
            else {
            int ok = 0;
            if (cpf_pre) { ok = cpf_pre == 1; cpf_pre = 0; }
            else if (cpit_ready()) { cpf_early_now = 1; ok = cpit_msaa_pass(cmd, fr); cpf_early_now = 0; }
            if (ok && cpit.ms_rv) {
                rql_write(rql.cp_set[fr - frames], cpit.ms_rv, cpit.ms_rv);
                rql.cp_ok = 1;
            }
            }
        }
    }
    rendering_open = 0;
    return 1;
}
static void rql_full(VkCommandBuffer cmd, VkPipeline pipe, VkDescriptorSet set) {
    VkViewport vp; memset(&vp, 0, sizeof vp); vp.width = (float)rqe.w; vp.height = (float)rqe.h; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc; memset(&sc, 0, sizeof sc); sc.offset.x = (int32_t)rqd.vx; sc.offset.y = (int32_t)rqd.vy; sc.extent.width = rqd.vw; sc.extent.height = rqd.vh;
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rql.lo, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cmd, rql.lo, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, rqe.xf);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}
static void rql_cl_draw(VkCommandBuffer cmd, vk_frame *fr) {
    {   /* PS2_RQ_SKIPC=1 [test, wrong image, with PS2_RQ_ABSKIP]: the cloud composite left out in every second block (its compute still runs);
           =2 only its far tiles, =3 only its near cells (clouds6 2026-11-11, prompt 6 step 5: the composite's parts by absence in one flight) */
        static int skc = -1;
        if (skc < 0) { const char *e = getenv("PS2_RQ_SKIPC"); skc = e ? atoi(e) : 0; }
        rqcl_skip = 0;
        if (skc == 1 && rqab_active()) return;
        if (skc >= 2 && rqab_active()) rqcl_skip = skc - 1;          /* (1 = the far tiles, 2 = the near cells; rq_clouds_composite) */
    }
    if (rqcl_want()) rq_clouds_composite(cmd, fr);              /* (PS2_RQ_CLOUDS: plan 4.2's one composite; the coordinator merges rq_clouds_composite as the group-5 draw) */
    else rql_full(cmd, rql.pc, rql.can_set);
    rql.cl_drawn = 1; rql.n_cl++;
    if (!(rql.n_cl % 300u)) ps2_log("rq layers: %llu cloud draws, %llu cockpit draws", (unsigned long long)rql.n_cl, (unsigned long long)rql.n_cp);
}
static void rql_cp_draw(VkCommandBuffer cmd, vk_frame *fr) {
    rql_full(cmd, rql.pp, rql.cp_set[fr - frames]);
    rql.n_cp++;
}
/* what is still due before the pass closes */
static void rql_tail(VkCommandBuffer cmd, vk_frame *fr, int cl_at, int cpit_after, int *cpit_done) {
    if (rql.cl_ok && !rql.cl_drawn && cl_at >= 0) rql_cl_draw(cmd, fr);
    rqb_tail(cmd, fr);                                   /* (PS2_RQ_BURST: a burst not drawn yet in this list) */
    if (cpit_after >= 0 && !*cpit_done && rql.cp_want) {
        if (rql.cp_ok) { if (rql.cp_quest) { rq_cockpit_solid(cmd, fr); rq_cockpit_after_world(cmd); } else rql_cp_draw(cmd, fr); }
        *cpit_done = 1;
    }
}
