/* rq_fxfade.inc -- PS2_RQ_CLOUDS_FXFADE (default 0; R-55 / B-4 of a private script, agent clouds4
   2026-11-11): the game's effects in the Quest eye pass fade by our cloud in front of them, per vertex (rq_cloud_fxfade.comp). Included by
   ps2_video.c after rq_hud.inc, PS2_RQ_SUPPORT only.
   The problem: the game's smoke, explosions, fire and missile / aircraft trails are GS draws without a depth write, drawn after our cloud
   composite (ACZ_VR_CLOUD_FX's point, before the first effect) and in no depth buffer: no cloud ever covered them (M3 45 s of the user's
   flights: a trail drawn bright over a cloud; M4 177 s: an enemy's trail seen through the cloud the player sits in). The PC fades them per
   pixel by the cloud's transmittance in front of their own depth (ACZ_VR_CLOUD_FX_SOFT: a veil image + gs.frag). Here the same fade, at
   the effects' VERTICES, once a frame, in place in the vertex data the eye pass draws them from: one compute dispatch before the pass
   (after the clouds' compute), over the in-place GS effect draws (fr->vbuf) and the HUD batch's effect rows (rqh.vb, rows marked in the
   row's spare word). Nothing in the pass changes: no pipeline, no binding, no fragment read (foveation-safe), no attachment.
   The table of ranges, the batch's vertices and rows are written by the host while the pass is recorded, after the dispatch was recorded
   and before the submit (host-coherent memory: visible to the dispatch at the submit). An effect drawn BEFORE the composite (under it) is
   not faded: the composite covers it with the whole column. Counters: `rq fxfade:` every 600 frames. */
#define RQFX_MAXR 4096u
static struct {
    int tried, ok;
    VkDescriptorSetLayout dsl;
    VkDescriptorPool pool;
    VkDescriptorSet set[FRAMES_IN_FLIGHT];
    VkPipelineLayout lo;
    VkPipeline pipe;
    VkBuffer tab[FRAMES_IN_FLIGHT]; VkDeviceMemory tmem[FRAMES_IN_FLIGHT]; u32 *tp[FRAMES_IN_FLIGHT];
    int armed; u32 fi, nr;
    u64 s_frames, s_ranges, s_gsv, s_rows, s_before, s_over, s_hudv;
} rqfx;
static int rqfx_make(void) {
    if (rqfx.ok) return 1;
    if (rqfx.tried) return 0;
    if (rqh_cfg() && !rqh.ok && !rqh.tried) return 0;               /* (the batch's buffers are bound too: made at its first list) */
    rqfx.tried = 1;
    if (!rqfx_cs) { ps2_log("rq fxfade: rq_cloud_fxfade.comp.spv missing: off"); return 0; }
    if (!rqcl.tiles || !ps2_clouds_common_set_layout()) { ps2_log("rq fxfade: needs the eye pass's clouds: off"); return 0; }
    VkDescriptorSetLayoutBinding b[5]; memset(b, 0, sizeof b);
    for (u32 i = 0; i < 5; i++) { b[i].binding = i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; }
    VkDescriptorSetLayoutCreateInfo lc; memset(&lc, 0, sizeof lc);
    lc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; lc.bindingCount = 5; lc.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev, &lc, NULL, &rqfx.dsl) != VK_SUCCESS) return 0;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5u * FRAMES_IN_FLIGHT };
    VkDescriptorPoolCreateInfo pi; memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; pi.maxSets = FRAMES_IN_FLIGHT; pi.poolSizeCount = 1; pi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(dev, &pi, NULL, &rqfx.pool) != VK_SUCCESS) return 0;
    VkDescriptorSetLayout L[FRAMES_IN_FLIGHT];
    for (u32 k = 0; k < FRAMES_IN_FLIGHT; k++) L[k] = rqfx.dsl;
    VkDescriptorSetAllocateInfo ai; memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; ai.descriptorPool = rqfx.pool; ai.descriptorSetCount = FRAMES_IN_FLIGHT; ai.pSetLayouts = L;
    if (vkAllocateDescriptorSets(dev, &ai, rqfx.set) != VK_SUCCESS) return 0;
    for (u32 k = 0; k < FRAMES_IN_FLIGHT; k++) {
        if (make_buffer_pref(16u + 16u * (VkDeviceSize)RQFX_MAXR, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, &rqfx.tab[k], &rqfx.tmem[k], (void **)&rqfx.tp[k]) != 0) return 0;
        memset(rqfx.tp[k], 0, 16u);
        if (!frames[k].vbuf) return 0;
        VkDescriptorBufferInfo bi[5];                               /* (without the batch: the table stands in for its buffers, its count is 0) */
        bi[0].buffer = rqcl.tiles; bi[1].buffer = frames[k].vbuf; bi[2].buffer = rqh.ok ? rqh.vb[k] : rqfx.tab[k]; bi[3].buffer = rqh.ok ? rqh.sb[k] : rqfx.tab[k]; bi[4].buffer = rqfx.tab[k];
        VkWriteDescriptorSet w[5]; memset(w, 0, sizeof w);
        for (u32 i = 0; i < 5; i++) {
            bi[i].offset = 0; bi[i].range = i == 1 ? (VkDeviceSize)VK_MAX_VERTS * sizeof(ps2_vk_vertex) : VK_WHOLE_SIZE;   /* (the eye pass's half: vb_off 0) */
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = rqfx.set[k]; w[i].dstBinding = i; w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[i].pBufferInfo = &bi[i];
        }
        vkUpdateDescriptorSets(dev, 5, w, 0, NULL);
    }
    VkDescriptorSetLayout sl[2] = { ps2_clouds_common_set_layout(), rqfx.dsl };
    VkPushConstantRange pr; memset(&pr, 0, sizeof pr); pr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; pr.size = 80;
    VkPipelineLayoutCreateInfo pl; memset(&pl, 0, sizeof pl);
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pl.setLayoutCount = 2; pl.pSetLayouts = sl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev, &pl, NULL, &rqfx.lo) != VK_SUCCESS) return 0;
    VkComputePipelineCreateInfo cp; memset(&cp, 0, sizeof cp);
    cp.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO; cp.layout = rqfx.lo;
    cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cp.stage.module = rqfx_cs; cp.stage.pName = "main";
    if (vkCreateComputePipelines(dev, pipe_cache, 1, &cp, NULL, &rqfx.pipe) != VK_SUCCESS) { rqfx.pipe = VK_NULL_HANDLE; return 0; }
    rqfx.ok = 1;
    float lo = 0.0f, hi = 0.0f; (void)ps2_clouds_fx_soft_range(&lo, &hi);
    ps2_log("rq fxfade: PS2_RQ_CLOUDS_FXFADE=%d: the effects fade per vertex by our cloud in front of them (the PC's range %.2f .. %.2f)%s",
            rqfx_cfg(), (double)lo, (double)hi, rqfx_cfg() == 2 ? " -- [test] tinted: red faded, green clear, blue no cloud in front" : "");
    return 1;
}
/* rql_before, after rqcl_prepare: the dispatch (its table filled while the pass is recorded) */
static void rqfx_dispatch(VkCommandBuffer cmd, vk_frame *fr) {
    rqfx.armed = 0;
    if (!rqfx_cfg() || !rqcl.cset || !rqfx_make()) return;
    {   /* PS2_RQ_CLOUDS_FXFADE_AB=1 [test] with PS2_RQ_ABSKIP=N: no dispatch (nothing faded) in the "left out" blocks, the cost by absence
           in one flight (`rq-ab:`; this list's block: rqab_begin counts it after this point) */
        static int ab = -1;
        if (ab < 0) { const char *e = getenv("PS2_RQ_CLOUDS_FXFADE_AB"); ab = e && *e == '1'; if (ab) ps2_log("rq fxfade: [test] PS2_RQ_CLOUDS_FXFADE_AB: left out in every second block of PS2_RQ_ABSKIP=%d", rqab_get()); }
        if (ab && rqab_get() > 0 && !(((rqab_list + 1u) / (unsigned)rqab_get()) & 1u)) return;
    }
    u32 fi = (u32)(fr - frames);
    if (fi >= FRAMES_IN_FLIGHT) return;
    if (rqsun_cfg()) {                                  /* (PS2_RQ_CLOUDS_SUN_T: this slot's last dispatch wrote the sun's T into hdr.w, z = 1 when valid) */
        const volatile u32 *h = rqfx.tp[fi];
        u32 ok = h[2], tb = h[3]; float tr; memcpy(&tr, &tb, 4);
        if (ok == 1u && tr >= 0.0f && tr <= 1.0f) { rqsun.t += (tr - rqsun.t) * 0.3f; rqsun.n++; rqsun.sum += (double)rqsun.t;
            static u32 last; if (tr < 0.9f && ps2_gs_frame_count - last >= 30u) { last = ps2_gs_frame_count;   /* (where the sun stands in view behind our cloud) */
                ps2_log("rq clouds: the sun in view behind our cloud at field %u: T %.3f (eased %.3f)", ps2_gs_frame_count, (double)tr, (double)rqsun.t); } }
        else if (ok) rqsun.nbad++;
        if (!((rqsun.n + rqsun.nbad) % 600u) && rqsun.n + rqsun.nbad) {
            ps2_log("rq clouds: the sun's T (PS2_RQ_CLOUDS_SUN_T): now %.3f, the mean %.3f of %llu frames, %llu frames without a fresh tile there", (double)rqsun.t,
                    rqsun.n ? rqsun.sum / (double)rqsun.n : 1.0, (unsigned long long)rqsun.n, (unsigned long long)rqsun.nbad);
            rqsun.sum = 0.0; rqsun.n = rqsun.nbad = 0;
        }
    }
    memset(rqfx.tp[fi], 0, 16u);
    {   VkMemoryBarrier mb; memset(&mb, 0, sizeof mb);   /* (the clouds' compute just wrote the near buffer: its barrier names the graphics stages only) */
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER; mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL); }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rqfx.pipe);
    VkDescriptorSet s[2] = { rqcl.cset, rqfx.set[fi] };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rqfx.lo, 0, 2, s, 0, NULL);
    struct { float p0[4], p1[4], p2[4]; u32 p3[4], p4[4]; } pc;
    memset(&pc, 0, sizeof pc);
    float lo = 0.25f, hi = 0.75f; (void)ps2_clouds_fx_soft_range(&lo, &hi);
    pc.p0[0] = lo; pc.p0[1] = hi; pc.p0[2] = (float)rqfx_cfg(); pc.p0[3] = rqcl.near_on ? 1.0f : 0.0f;
    pc.p1[0] = rqcl.ndiv > 0.0f ? rqcl.ndiv : 1.0f; pc.p1[1] = rqcl.nsz[0]; pc.p1[2] = rqcl.nsz[1]; pc.p1[3] = ps2_clouds_rq_format2() ? 1.0f : 0.0f;
    ps2_clouds_far_rq_cam(pc.p2); pc.p2[3] = (float)ps2_clouds_rq_map_n();
    pc.p3[0] = (u32)(sizeof(ps2_vk_vertex) / 4u); pc.p3[1] = (u32)(sizeof(rqh_vtx) / 4u); pc.p3[2] = (u32)(sizeof(rqh_row) / 4u);
    pc.p3[3] = (u32)(offsetof(rqh_row, pad) / 4u) | ((u32)(offsetof(rqh_vtx, id) / 4u) << 8);
    pc.p4[0] = (u32)(vb_off / 4u); pc.p4[1] = 1u; pc.p4[2] = rqsun_cfg() ? 1u : 0u;   /* (z: PS2_RQ_CLOUDS_SUN_T, the sun's T into hdr.w) */
    vkCmdPushConstants(cmd, rqfx.lo, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cmd, 32, 1, 1);
    {   VkMemoryBarrier mb; memset(&mb, 0, sizeof mb);
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER; mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &mb, 0, NULL, 0, NULL); }
    if (rqsun_cfg()) {   /* (PS2_RQ_CLOUDS_SUN_T: the header's sun word read by the host after this slot's fence) */
        VkMemoryBarrier mb; memset(&mb, 0, sizeof mb);
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER; mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL); }
    rqfx.armed = 1; rqfx.fi = fi; rqfx.nr = 0;
    rqfx.s_frames++;
    if (!(rqfx.s_frames % 600u)) {
        double n = 600.0;
        ps2_log("rq fxfade: %llu eye frames: a frame %.2f in-place effect draws faded (%.1f vertices), %.2f batch effect rows faded (%.1f batch vertices scanned), "
                "%.2f effect draws under the composite (not faded), %.2f ranges over the table", (unsigned long long)rqfx.s_frames, (double)rqfx.s_ranges / n,
                (double)rqfx.s_gsv / n, (double)rqfx.s_rows / n, (double)rqfx.s_hudv / n, (double)rqfx.s_before / n, (double)rqfx.s_over / n);
        rqfx.s_ranges = rqfx.s_gsv = rqfx.s_rows = rqfx.s_before = rqfx.s_over = rqfx.s_hudv = 0;
    }
}
/* an effect drawn under the composite: the first few and every 600th to the log (what holds it there) */
static void rqfx_said_before(const vk_draw *d, const char *how) {
    static u32 n;
    if (++n > 6u && (n % 600u)) return;
    float z0 = 1e30f, z1 = -1e30f;
    for (u32 j = d->first; j < d->first + d->count && j < rep_nverts; j++) { if (rep_verts[j].z < z0) z0 = rep_verts[j].z; if (rep_verts[j].z > z1) z1 = rep_verts[j].z; }
    ps2_log("rq fxfade: under the composite (%s, #%u, field %u): blend %03X tex %d n %u topo %u depth 0x%X z %.6g..%.6g rgba0 %.0f,%.0f,%.0f,%.0f%s", how, n, ps2_gs_frame_count,
            d->blend & 0x1FFu, d->tex == 0xFFFFu ? -1 : (int)d->tex, d->count, d->topology, d->depth, (double)z0, (double)z1,
            d->count ? (double)rep_verts[d->first].r : 0.0, d->count ? (double)rep_verts[d->first].g : 0.0, d->count ? (double)rep_verts[d->first].b : 0.0,
            d->count ? (double)rep_verts[d->first].a : 0.0, cl_fx(d) ? " (an effect sprite: cl_fx)" : " (not cl_fx: untextured or at the far plane)");
}
/* a draw at the far plane only (every vertex at GS z 0: a sky band or glow at infinity) has no distance to fade by: left as it is */
static int rqfx_far_plane(const vk_draw *d) {
    if (d->native || !d->count || d->first + d->count > rep_nverts) return 0;
    for (u32 j = d->first; j < d->first + d->count; j++) if (rep_verts[j].z > 0.0f) return 0;
    return 1;
}
/* the draw loop: a GS draw of the open eye pass drawn in place (the batch did not take it) */
static void rqfx_gs(const vk_draw *d) {
    if (!rqfx.armed || d->native || vb_off) return;
    int m = fxv_draw(d);
    if (!m || rqfx_far_plane(d)) return;
    if (rql.cl_ok && !rql.cl_drawn) { rqfx.s_before++; rqfx_said_before(d, "in place"); return; }   /* (under the composite: it covers the effect with the whole column) */
    if (rqfx.nr >= RQFX_MAXR || d->first + d->count > rep_nverts) { rqfx.s_over++; return; }
    u32 *t = rqfx.tp[rqfx.fi];
    u32 *r = t + 4u + 4u * rqfx.nr;
    r[0] = d->first; r[1] = d->count; r[2] = (u32)m; r[3] = 0;
    t[0] = ++rqfx.nr;
    rqfx.s_ranges++; rqfx.s_gsv += d->count;
}
/* PS2_RQ_CLOUDS_SUN_T: the game's flare, glow and ghosts (class 5 after the cockpit, in place: the HUD batch takes none) x the sun's eased T,
   on top of the in-cloud fade (ps2_video.c render_list, the hook on the line that closes it: written from rep_verts as that block does, so
   a re-recorded frame does not compound) */
/* PS2_RQ_GLOW_CLEAN_FADE (rq16_hunt 2026-11-16, BUGS_RQ R-200; default 1, read once; =0 the subtraction at the game's strength as
   before): PS2_RQ_GLOW_CLEAN's subtraction (ps2_video.c rqsg_inject: last list's class-5 sun overlays drawn again with ((0 - Cs) C + Cd)
   into the glow's 2:1 copy of the feed, marked sunsub) took the game's colours, while the overlays the feed holds had been faded by the
   in-cloud density (sk, the block before this hook) and the sun's T (ts, PS2_RQ_CLOUDS_SUN_T): inside cloud facing the sun the copy
   lost more sun than the feed held, and the glow laid that hole over the picture as a dark brownish star (the hunt's M4 on t13g, fields
   4650-5700, both eyes, T 0.26-0.36). The sunsub draws now take the same factor sk x ts, written from rep_verts as the fades do */
static int rqsg_fade_cfg(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_GLOW_CLEAN_FADE"); v = !(e && *e == '0');
                 ps2_log("rq glow-clean: PS2_RQ_GLOW_CLEAN_FADE=%d: the glow copy's sun subtraction %s", v,
                         v ? "x the in-cloud fade and the sun's T its overlays got (R-200)" : "at the game's strength"); }
    return v;
}
static void rqsun_fade(vk_frame *fr, int cpit_after_, int hk_) {
    if (cpit_after_ < 0 || !rep_nverts || hk_) return;
    float ts = rqsun_k();                               /* (1 without PS2_RQ_CLOUDS_SUN_T) */
    if (ts < 0.0f) ts = 0.0f;
    if (ts > 1.0f) ts = 1.0f;
    float sk = 1.0f;                                    /* (the in-cloud fade the block before applied, the same terms) */
    {   static int sun_on = -1;
        if (sun_on < 0) { const char *e = getenv("ACZ_IMM_CLOUD_SUN"); sun_on = !(e && *e == '0'); }
        float vs = ps2_imm_veil(); vs = vs > 1.0f ? 1.0f : vs;
        float s0 = 1.0f - 0.9f * cf_dens * vs;
        if (cf_dens > 0.01f && sun_on && vs > 0.0f && s0 < 0.995f) sk = s0; }
    ps2_vk_vertex *dst = (ps2_vk_vertex *)((u8 *)fr->vptr + vb_off);
    if (sk * ts < 0.995f && rqsg_fade_cfg()) {          /* (PS2_RQ_GLOW_CLEAN_FADE: the subtraction as faded as what it removes) */
        static u64 nf; static u32 lastf;
        const float k2 = sk * ts;
        int any = 0;
        for (u32 k = 0; k < rep_ndraws; k++) {
            const vk_draw *dd = &rep_draws[k];
            if (!dd->sunsub || dd->native || dd->first + dd->count > rep_nverts) continue;
            for (u32 j = dd->first; j < dd->first + dd->count; j++) {
                dst[j].r = rep_verts[j].r * k2; dst[j].g = rep_verts[j].g * k2; dst[j].b = rep_verts[j].b * k2;
            }
            any = 1;
        }
        if (any && (!nf++ || ps2_gs_frame_count - lastf >= 600u)) { lastf = ps2_gs_frame_count;
            ps2_log("rq glow-clean: the sun subtraction faded x %.3f (in cloud %.3f, the sun's T %.3f) at field %u (PS2_RQ_GLOW_CLEAN_FADE; %llu lists)",
                    (double)k2, (double)sk, (double)ts, ps2_gs_frame_count, (unsigned long long)nf); }
    }
    if (!rqsun_cfg() || !(ts < 0.995f)) return;
    for (u32 k = (u32)cpit_after_ + 1u; k < rep_ndraws; k++) {
        const vk_draw *dd = &rep_draws[k];
        if (dd->xcls != 5u || dd->rt != 0u || dd->native || dd->first + dd->count > rep_nverts) continue;
        for (u32 j = dd->first; j < dd->first + dd->count; j++) {
            dst[j].r = rep_verts[j].r * sk * ts; dst[j].g = rep_verts[j].g * sk * ts; dst[j].b = rep_verts[j].b * sk * ts;
        }
    }
}
/* the HUD batch's flush: the fade mode of a row (0: none) and the batch's vertex count for the dispatch */
static float rqfx_row(const vk_draw *e, int after) {
    if (!rqfx.armed) return 0.0f;
    int m = fxv_draw(e);
    if (!m || rqfx_far_plane(e)) return 0.0f;
    if (!after) { rqfx.s_before++; rqfx_said_before(e, "batch"); return 0.0f; }
    rqfx.s_rows++;
    return (float)m;
}
static void rqfx_batch_n(u32 nv) {
    if (!rqfx.armed) return;
    u32 *t = rqfx.tp[rqfx.fi];
    if (nv > t[1]) { rqfx.s_hudv += nv - t[1]; t[1] = nv; }
}
