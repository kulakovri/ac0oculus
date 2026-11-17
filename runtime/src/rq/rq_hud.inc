/* rq_hud.inc -- the HUD batcher of the eye-sized pass (PS2_RQ_HUD=batch, default off; plan 2, section 4.4; included by ps2_video.c,
   PS2_RQ_SUPPORT only). BUGS_RQ R-21: the HUD's cost rides on its draws in the bins it covers, not on its pixels or shaders.
   - rq_hud_flush(): the GS draws of a run go through one host-built vertex stream (lines expanded to quads, in rq_hud.vert: the
     rectangle of strict wide lines, width rqe.pxs eye pixels) and at most a few draws: one pipeline per (depth-bounds key, depth
     compare), EVERY blend of the ADD equation through one dual-source blend state (rq_hud.frag: the factors are per draw),
     per-draw state in a storage-buffer table read by the vertex shader, textures from a table filled once per list. No per-draw
     descriptor, push constant, scissor or pipeline change.
   - Two runs: mode 0, the HUD, from the first class-2 draw to the end of slot 0's run (class 2, and the class 1 target boxes
     between them); mode 1, the world's effects: each run of GS draws without a depth write that nothing interrupts (default on:
     PS2_RQ_HUD_FX=0 leaves them out).
   - Order: draws keep the list's order inside a group; a draw joins an earlier group only if it overlaps nothing later that does
     not commute with it (two additive draws without alpha writes commute). A draw it cannot take stays in the list, is drawn at
     its own place by the old path and is a barrier: later draws it overlaps stay out too (`left in place` in the log).
   - The radar's DATE masks need no attachment read and no barrier: the mask writers are known rectangles of known alpha, so the
     destination-alpha test is decided on the CPU (rqh_date) or, where a reader straddles several, per fragment against those
     rectangles (rq_hud.frag: DATE by rectangles, a table in the storage buffer).
   - Depth-bounds keys of the combiner: as the draw loop's (rq_hud_flush: the occl test), one pipeline each.
   - Replacement images (textures/hud, ps2_texrep) go the same path (rep_scale per draw).
   - Class 3's fade quad with alpha 0: skipped (rqh_fade_skip), exact; its write of alpha 0 is done on the feed.
   A/B: PS2_RQ_HUD=alt (every second list; two consecutive dumps compare the two paths), PS2_RQ_HUD=ab with PS2_RQ_ABSKIP=N (`rq-ab:`).
   Hooks: rq_load_shaders, the frame setup (rq_hud_begin), the eye pass's begin / end (rqh_count), rq_hud_at in the draw loop. */
#define RQH_TEXS 256u                 /* the texture table (rq_hud.frag tab[]) */
#define RQH_MAX_V (1u << 15)          /* vertices a frame in flight */
#define RQH_MAX_D 4096u               /* table rows a frame in flight */
#define RQH_KEYS 32u                  /* pipelines: depth-bounds key (2 bits) | depth compare (2 bits) | plain (bit 4) */
#define RQH_MAX_G 64u                 /* groups of a run: the arrays; the limit is 16 (RQH_NG), 64 with PS2_RQ_DATE_FREE (M4's radar: 16 left draws reading) */
#define RQH_NG (rq_date_free() ? RQH_MAX_G : 16u)
#define RQH_BARS 512u                 /* foreign draws remembered as barriers */
#define RQH_WR 256u                   /* alpha writers (rectangles, triangles) remembered */
#define RQH_RECTS 8192u               /* rectangles of the analytic DATE test a frame in flight (512 before PS2_RQ_DATE_FREE: its eye-space entries are two a writer) */

typedef struct { u32 flags, wrap, mode, tex; u32 cu[4]; float ts[2], is[2]; float fog[4]; u32 dfirst, dcount; float rep, pad; float k[4], m[4], cl[4]; float sc[4]; } rqh_row;   /* (sc: PS2_RQ_DATE_FREE, the draw's scissor in eye pixels; x1 <= x0 none) */
typedef struct { float x, y, z, r, g, b, a, s, t, q, fog; u32 ruv; float dx; u32 id; float ox, oy, odx, side; } rqh_vtx;
static u64 rqh_host_fid; static u32 rqh_host_fi;   /* (PS2_RQ_MAP_HOST=2's check: the frame and the slot the host arrays were last written for) */
static rqh_vtx *rqh_hv_dbg; static rqh_row *rqh_hr_dbg;   /* PS2_RQ_MAP_HOST: the batch's vertices and rows in host memory (the frame's, at their places) */

typedef struct { u32 j; u16 g; u8 add, wa; u32 key; float b[4]; u32 dfirst, dcount; float m3; float zc, ztop; u8 zbelow; float sc[4]; } rqh_cand;
static struct {
    int tried, ok;
    VkDescriptorSetLayout tdsl, bdsl;
    VkPipelineLayout lo;
    VkDescriptorPool pool;
    VkDescriptorSet tset[FRAMES_IN_FLIGHT], bset[FRAMES_IN_FLIGHT];
    VkBuffer vb[FRAMES_IN_FLIGHT], sb[FRAMES_IN_FLIGHT];
    VkDeviceMemory vm[FRAMES_IN_FLIGHT], sm[FRAMES_IN_FLIGHT];
    void *vp[FRAMES_IN_FLIGHT], *sp[FRAMES_IN_FLIGHT];
    VkPipeline pipe[RQH_KEYS];
    VkRenderPass pipe_rp;
    u32 *tk;                          /* per draw index: the list's stamp when the batch took it */
    u32 stamp, flushed, nv, nr, frozen;
    int dome;
    u32 hud_ii, fx_until;             /* positions in the list's order: the first class-2 draw; the end of the effects' run being drawn */
    int fx_on;                        /* PS2_RQ_HUD_FX (default on) */
    int cl_drop;                      /* (the loop drops the game's cloud sprites this frame) */
    u32 nrect;
    int list_on, armed;               /* this list goes through the batch (PS2_RQ_HUD=alt / ab: not every list); armed: decided at the pass's first draw */
    u64 alt_n;
    struct { u64 draw, pipe, desc, push, scis, mesh, groups, cl, cp, frames; } cnt;
    u32 ntex;
    struct { VkDescriptorSet set; u32 stamp; u32 slot; } tmap[1024];
    VkCopyDescriptorSet cp[RQH_TEXS];
    u32 ncp;
    u64 s_plain, s_fxlists, s_lists, s_draws, s_groups, s_verts, s_left[10], s_date[3], s_fade, s_cand;
    u32 pend, pend_fi, pend_g0, pend_ng; u32 pend_first[RQH_MAX_G], pend_n[RQH_MAX_G], pend_key[RQH_MAX_G];   /* (PS2_RQ_HUD_CLSPLIT: a run's groups after the clouds' point, drawn there by rqh_flush_rest) */
    struct { float rqx[4]; float rqy[2]; float rqsz[2]; float inv[2]; float halfw; u32 flags; } pend_pc;
    int cl_pt;                        /* (the loop's cloud point while our clouds are still to come this pass, else -1) */
    u64 s_split;
} rqh;

static int rqh_cfg(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_HUD"); v = !e ? 0 : !strcmp(e, "batch") ? 1 : !strcmp(e, "alt") ? 2 : !strcmp(e, "ab") ? 3 : 0; if (v) ps2_log("vk: PS2_RQ_HUD=%s: the HUD through one batch%s", e, v == 2 ? " on every second list (test)" : v == 3 ? " in the blocks of PS2_RQ_ABSKIP (test; rq-ab: with = each draw on its own, left out = the batch)" : ""); }
    return v;
}
/* PS2_RQ_HUD_CLSPLIT (default on): a batched run that our clouds' composite point falls inside is drawn in two parts, the draws
   from that point on after the clouds, as the list orders them (the game's cloud sprites came there); 0: the whole run at its
   start, under the clouds (before 2026-11-10: M4 in cloud, the radar window's part over the world lost to the near cloud) */
static int rqh_clsplit_cfg(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_HUD_CLSPLIT"); v = !(e && *e == '0'); if (!v) ps2_log("vk: PS2_RQ_HUD_CLSPLIT=0: a batched run is drawn whole at its start, also across the clouds' point"); }
    return v;
}
static u32 rqh_split_at(u32 from_ii, u32 to_ii) {
    if (!rqh_clsplit_cfg() || rqh.cl_pt < 0) return ~0u;
    u32 c = (u32)rqh.cl_pt;
    return c > from_ii && c < to_ii ? c : ~0u;
}
static int rqh_init(void) {
    if (rqh.ok) return 1;
    if (rqh.tried) return 0;
    rqh.tried = 1;
    const u64 rqpw_t0 = vk_mono_ns();
    if (!rqh_vs || !rqh_fs || !rqh_pvs || !rqh_pfs) { ps2_log("rq-hud: shaders missing: off"); return 0; }
    rqh.tk = (u32 *)calloc(VK_MAX_DRAWS, sizeof(u32));
    if (!rqh.tk) return 0;
    VkDescriptorSetLayoutBinding tb; memset(&tb, 0, sizeof tb);
    tb.binding = 0; tb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; tb.descriptorCount = RQH_TEXS; tb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorBindingFlags tf = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo tfi; memset(&tfi, 0, sizeof tfi);
    tfi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO; tfi.bindingCount = 1; tfi.pBindingFlags = &tf;
    VkDescriptorSetLayoutCreateInfo lc; memset(&lc, 0, sizeof lc);
    lc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; lc.pNext = &tfi; lc.bindingCount = 1; lc.pBindings = &tb;
    if (vkCreateDescriptorSetLayout(dev, &lc, NULL, &rqh.tdsl) != VK_SUCCESS) return 0;
    VkDescriptorSetLayoutBinding sbn[2]; memset(sbn, 0, sizeof sbn);
    sbn[0].binding = 0; sbn[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; sbn[0].descriptorCount = 1; sbn[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    sbn[1] = sbn[0]; sbn[1].binding = 1; sbn[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    lc.pNext = NULL; lc.bindingCount = 2; lc.pBindings = sbn;
    if (vkCreateDescriptorSetLayout(dev, &lc, NULL, &rqh.bdsl) != VK_SUCCESS) return 0;
    VkDescriptorPoolSize ps[2];
    ps[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ps[0].descriptorCount = RQH_TEXS * FRAMES_IN_FLIGHT;
    ps[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; ps[1].descriptorCount = 2u * FRAMES_IN_FLIGHT;
    VkDescriptorPoolCreateInfo pc; memset(&pc, 0, sizeof pc);
    pc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; pc.maxSets = 2u * FRAMES_IN_FLIGHT; pc.poolSizeCount = 2; pc.pPoolSizes = ps;
    if (vkCreateDescriptorPool(dev, &pc, NULL, &rqh.pool) != VK_SUCCESS) return 0;
    VkDescriptorSetLayout ls[FRAMES_IN_FLIGHT * 2u];
    for (u32 k = 0; k < FRAMES_IN_FLIGHT; k++) { ls[k] = rqh.tdsl; ls[k + FRAMES_IN_FLIGHT] = rqh.bdsl; }
    VkDescriptorSet sets[FRAMES_IN_FLIGHT * 2u];
    VkDescriptorSetAllocateInfo ai; memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; ai.descriptorPool = rqh.pool; ai.descriptorSetCount = 2u * FRAMES_IN_FLIGHT; ai.pSetLayouts = ls;
    if (vkAllocateDescriptorSets(dev, &ai, sets) != VK_SUCCESS) return 0;
    for (u32 k = 0; k < FRAMES_IN_FLIGHT; k++) { rqh.tset[k] = sets[k]; rqh.bset[k] = sets[k + FRAMES_IN_FLIGHT]; }
    VkDescriptorSetLayout sl[2] = { rqh.tdsl, rqh.bdsl };
    VkPushConstantRange pr; memset(&pr, 0, sizeof pr); pr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT; pr.size = 48;
    VkPipelineLayoutCreateInfo pl; memset(&pl, 0, sizeof pl);
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pl.setLayoutCount = 2; pl.pSetLayouts = sl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev, &pl, NULL, &rqh.lo) != VK_SUCCESS) return 0;
    for (u32 k = 0; k < FRAMES_IN_FLIGHT; k++) {
        if (make_buffer_pref((VkDeviceSize)RQH_MAX_V * sizeof(rqh_vtx), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | (rqfx_cfg() ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : 0u), 1, &rqh.vb[k], &rqh.vm[k], &rqh.vp[k]) != 0) return 0;
        VkDeviceSize rsz = (VkDeviceSize)RQH_MAX_D * sizeof(rqh_row), wsz = (VkDeviceSize)RQH_RECTS * 32u;
        if (make_buffer_pref(rsz + wsz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1, &rqh.sb[k], &rqh.sm[k], &rqh.sp[k]) != 0) return 0;
        VkDescriptorBufferInfo bi[2]; bi[0].buffer = bi[1].buffer = rqh.sb[k]; bi[0].offset = 0; bi[0].range = rsz; bi[1].offset = rsz; bi[1].range = wsz;
        VkWriteDescriptorSet w[2]; memset(w, 0, sizeof w);
        for (int q = 0; q < 2; q++) { w[q].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[q].dstSet = rqh.bset[k]; w[q].dstBinding = (uint32_t)q; w[q].descriptorCount = 1; w[q].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[q].pBufferInfo = &bi[q]; }
        vkUpdateDescriptorSets(dev, 2, w, 0, NULL);
    }
    rqh.stamp = 1;
    rqh.ok = 1;
    rqpw_made("the HUD batch's sets and buffers (rqh_init)", rqpw_t0);
    ps2_log("rq-hud: ready (%u KB of vertices, %u rows a frame in flight)", (unsigned)(RQH_MAX_V * sizeof(rqh_vtx) / 1024u), RQH_MAX_D);
    return 1;
}
/* key: bits 0-1 the depth-bounds bits (dkey >> 14), bits 2-3 the depth compare (dkey & 3) */
static VkPipeline rqh_pipe(u32 key) {
    if (rqh.pipe_rp != rqe.rp) { memset(rqh.pipe, 0, sizeof rqh.pipe); rqh.pipe_rp = rqe.rp; }
    if (rqh.pipe[key]) return rqh.pipe[key];
    const u64 rqpw_t0 = vk_mono_ns();
    VkPipelineShaderStageCreateInfo st[2]; memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = (key & 16u) ? rqh_pvs : rqh_vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = (key & 16u) ? rqh_pfs : rqh_fs; st[1].pName = "main";
    VkVertexInputBindingDescription bd; memset(&bd, 0, sizeof bd); bd.stride = sizeof(rqh_vtx);
    VkVertexInputAttributeDescription ad[8]; memset(ad, 0, sizeof ad);
    ad[0].location = 0; ad[0].format = VK_FORMAT_R32G32B32_SFLOAT; ad[0].offset = 0;
    ad[1].location = 1; ad[1].format = VK_FORMAT_R32G32B32A32_SFLOAT; ad[1].offset = 12;
    ad[2].location = 2; ad[2].format = VK_FORMAT_R32G32B32_SFLOAT; ad[2].offset = 28;
    ad[3].location = 3; ad[3].format = VK_FORMAT_R32_SFLOAT; ad[3].offset = 40;
    ad[4].location = 4; ad[4].format = VK_FORMAT_R32_UINT; ad[4].offset = 44;
    ad[5].location = 5; ad[5].format = VK_FORMAT_R32_SFLOAT; ad[5].offset = 48;
    ad[6].location = 6; ad[6].format = VK_FORMAT_R32_UINT; ad[6].offset = 52;
    ad[7].location = 7; ad[7].format = VK_FORMAT_R32G32B32A32_SFLOAT; ad[7].offset = 56;
    VkPipelineVertexInputStateCreateInfo vin; memset(&vin, 0, sizeof vin); vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &bd; vin.vertexAttributeDescriptionCount = 8; vin.pVertexAttributeDescriptions = ad;
    VkPipelineInputAssemblyStateCreateInfo ia; memset(&ia, 0, sizeof ia); ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vs; memset(&vs, 0, sizeof vs); vs.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vs.viewportCount = 1; vs.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs; memset(&rs, 0, sizeof rs); rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms; memset(&ms, 0, sizeof ms); ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = rq_samples();
    VkPipelineDepthStencilStateCreateInfo ds; memset(&ds, 0, sizeof ds); ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_FALSE; ds.depthCompareOp = compare_of_ztst((key >> 2) & 3u); ds.maxDepthBounds = 1.0f;
    {   float top = hg_boot() ? HG_MARK + HG_BAND : 0.99f;      /* (as pipe_build) */
        u32 db = key & 3u;
        if (db == 3u) { ds.depthBoundsTestEnable = VK_TRUE; ds.minDepthBounds = HG_MARK - HG_BAND; ds.maxDepthBounds = HG_MARK + HG_BAND; }
        else if (db == 1u) { ds.depthBoundsTestEnable = VK_TRUE; ds.minDepthBounds = 0.0f; ds.maxDepthBounds = top; }
        else if (db == 2u) { ds.depthBoundsTestEnable = VK_TRUE; ds.minDepthBounds = top; ds.maxDepthBounds = 1.0f; }
    }
    VkPipelineColorBlendAttachmentState cba; memset(&cba, 0, sizeof cba);
    cba.blendEnable = VK_TRUE; cba.colorWriteMask = 15; cba.colorBlendOp = VK_BLEND_OP_ADD; cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; cba.dstColorBlendFactor = VK_BLEND_FACTOR_SRC1_COLOR;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_SRC1_ALPHA;
    VkPipelineColorBlendStateCreateInfo cb; memset(&cb, 0, sizeof cb); cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cb.attachmentCount = 1; cb.pAttachments = &cba;
    static const VkDynamicState dy[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsi; memset(&dsi, 0, sizeof dsi); dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; dsi.dynamicStateCount = 2; dsi.pDynamicStates = dy;
    VkGraphicsPipelineCreateInfo gp; memset(&gp, 0, sizeof gp);
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia; gp.pViewportState = &vs;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds; gp.pColorBlendState = &cb; gp.pDynamicState = &dsi; gp.layout = rqh.lo; gp.renderPass = rqe.rp;
    if (vkCreateGraphicsPipelines(dev, pipe_cache, 1, &gp, NULL, &rqh.pipe[key]) != VK_SUCCESS) { rqh.pipe[key] = VK_NULL_HANDLE; ps2_log("rq-hud: pipeline %u failed", key); }
    else rqpw_made("a HUD batch pipeline (rqh_pipe)", rqpw_t0);
    return rqh.pipe[key];
}
/* the draw's GS blend as (source factor, destination factor) of one ADD equation, in rq_hud.frag's codes; 0 = cannot */
static int rqh_factor(VkBlendFactor f, u32 *c) {
    if (f == VK_BLEND_FACTOR_ZERO) *c = 0;
    else if (f == VK_BLEND_FACTOR_ONE) *c = 1;
    else if (f == VK_BLEND_FACTOR_SRC1_ALPHA) *c = 2;
    else if (f == VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA) *c = 3;
    else if (f == VK_BLEND_FACTOR_CONSTANT_ALPHA) *c = 4;
    else if (f == VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA) *c = 5;
    else return 0;
    return 1;
}
static int rqh_blend(u32 blend, u32 *sf, u32 *df) {
    if (!(blend & 0x100u)) { *sf = 1; *df = 0; return 1; }
    VkBlendFactor s, d; VkBlendOp o;
    translate_blend(blend & 3u, (blend >> 2) & 3u, (blend >> 4) & 3u, (blend >> 6) & 3u, &s, &d, &o);
    if (o != VK_BLEND_OP_ADD) return 0;
    return rqh_factor(s, sf) && rqh_factor(d, df);
}
/* a class-2 draw the batch can take (the state part; the geometry's tests are in the flush) */
static int rqh_ok(const vk_draw *e, u32 *why) {
    if (e->rt != 0u || e->native || e->tex_self || e->tex_depth || e->cockpit) { *why = 1; return 0; }
    if (e->xcls != 2u && e->xcls != 1u) { *why = 1; return 0; }  /* (class 1 after the HUD began: the target boxes, the labels) */
    if (e->xcls == 1u && (e->tex == 0xFFFFu && !e->tex_rt && (e->depth & 7u) == 3u && e->blend == 0x164u)) { *why = 5; return 0; }   /* (the haze ring's layers: radial fog) */
    if (e->topology != PS2_VK_TRIANGLES && e->topology != PS2_VK_LINES) { *why = 2; return 0; }
    if ((e->frag_flags >> 24) & 1u) { *why = 3; return 0; }     /* (shuffle) */
    if (e->tex_rt) {
        if (e->tex_rt > RT_SLOTS || e->tex_rt == 1u || tsc(&rts[e->tex_rt - 1u]) > 1 || rts[e->tex_rt - 1u].layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            || (rq_snap_mask && ((rq_snap_mask >> (e->tex_rt - 1u)) & 1u))) { *why = 3; return 0; }
    }
    u32 dk = e->depth;
    if (!(dk & 8u) || (dk & 0xE0u) || (dk & (1u << 2)) || (dk & ((1u << 9) | (1u << 10) | (1u << 12)))) { *why = 4; return 0; }
    u32 s, d;
    if (!rqh_blend(e->blend, &s, &d)) { *why = 5; return 0; }
    return 1;
}
/* the plain path (rq_hud_p.frag): one texture read or none, no fog, no discard, TFX modulate, coordinates inside the texture, no
   region modes, no sprite rounding, no analytic DATE. m: wa, tcc, FBA, the reference of the alpha-test on the alpha write (-1 none) */
static int rqh_plain_cfg(void) { static int v = -1; if (v < 0) { const char *e = getenv("PS2_RQ_HUD_PLAIN"); v = !(e && *e == '0'); } return v; }
/* PS2_RQ_HUD_ONE=1 (default off): one state for the run: depth compare and the depth-bounds keys [0,1], [0,top] folded into the vertices' z of one GEQUAL
   pipeline; analytic DATE, region repeat and the image grid in the plain shader. =alt: every second list (the picture check) */
static int rqh_one_cfg(void) { static int v = -1; if (v < 0) { const char *e = getenv("PS2_RQ_HUD_ONE"); v = !e ? 0 : *e == '1' ? 1 : !strcmp(e, "alt") ? 2 : 0; if (v) ps2_log("vk: PS2_RQ_HUD_ONE=%s", e); } return v; }
#define RQH_ONE() (rqh_one_cfg() == 1 || (rqh_one_cfg() == 2 && (rqh.alt_n & 1u)))
/* PS2_RQ_HUD_LAST=1 (default off): the clouds' and cockpit's leftovers and the glass before the HUD, so the HUD is the eye pass's last run. =alt: every second list */
static int rqh_last_cfg(void) { static int v = -1; if (v < 0) { const char *e = getenv("PS2_RQ_HUD_LAST"); v = !e ? 0 : *e == '1' ? 1 : !strcmp(e, "alt") ? 2 : 0; if (v) ps2_log("vk: PS2_RQ_HUD_LAST=%s", e); } return v; }
static u64 rqh_np[12];             /* (why a draw is not plain: the log) */
#define RQH_NP(k) do { rqh_np[k]++; if (xr_dump_now && getenv("PS2_RQ_LIST") && e->xcls == 2u) ps2_log("rq-hud-np: why %d blend 0x%X frag 0x%08X tex %d texrt %u wrap %X uv %u..%u %u..%u ts %.0fx%.0f img %ux%u n %u", (k), e->blend, e->frag_flags, (int)(s16)e->tex, e->tex_rt, e->wrap, e->minu, e->maxu, e->minv, e->maxv, (double)e->tex_w, (double)e->tex_h, e->tex != 0xFFFFu && e->tex < ntextures ? textures[e->tex].w : 0u, e->tex != 0xFFFFu && e->tex < ntextures ? textures[e->tex].h : 0u, e->count); return 0; } while (0)
static int rqh_plain(const vk_draw *e, u32 dcount, float *m3) {
    u32 f = e->frag_flags;
    *m3 = -1.0f;
    if (!rqh_plain_cfg()) return 0;
    int one = RQH_ONE();
    if (dcount && !one) RQH_NP(0);
    if (f & (1u << 16)) RQH_NP(1);
    if (e->tex_rt) RQH_NP(3);
    if ((f >> 3) & 1u) {
        u32 atst = (f >> 4) & 7u, afonly = (f >> 20) & 1u, afkeep = (f >> 18) & 1u, merge = (f >> 30) & 1u;
        if (atst == 1u) { if (afonly) RQH_NP(4); }
        else if (atst == 0u) { if (!afonly && afkeep) RQH_NP(4); }
        else {
            if (afonly || afkeep) RQH_NP(4);               /* (a real discard) */
            if (merge) {                                   /* (the test decides only the alpha write) */
                float aref = (float)((f >> 8) & 0xFFu) / 128.0f;
                if (atst == 5u) *m3 = aref;
                else if (atst == 6u) *m3 = aref + 1.0f / 1024.0f;
                else RQH_NP(4);
            }
        }
    }
    if (e->tex == 0xFFFFu) return 1;                       /* (untextured) */
    if (e->tex >= ntextures || !textures[e->tex].set) RQH_NP(5);
    if (f & 3u) RQH_NP(6);
    u32 mu = e->wrap & 3u, mv = (e->wrap >> 2) & 3u;
    int point = (f >> 19) & 1u, fst = (f >> 2) & 1u;
    if ((mu == 3u || mv == 3u) && !(one && point)) RQH_NP(7);    /* (region repeat: PS2_RQ_HUD_ONE takes the point-sampled ones) */
    const vk_texture *t = &textures[e->tex];
    u32 s = t->scale > 1u ? t->scale : 1u;
    float tw = e->tex_w > 0.0f ? e->tex_w : 1.0f, th = e->tex_h > 0.0f ? e->tex_h : 1.0f;
    if (((float)t->w != tw * (float)s || (float)t->h != th * (float)s) && !(one && point && s == 1u)) RQH_NP(8);    /* (the image is not exactly the texture's grid; ONE: a point read of the texture's own grid is the same fetch) */
    if (!point && t->levels > 1u) RQH_NP(9);
    float ofs = !point && ((f >> 25) & 1u) ? -0.5f : 0.0f;
    /* clamp modes (1 to the texture, 2 to the region) are the shader's clamp; repeat (0) needs coordinates inside the texture; a bilinear
       replacement image clamps per its own texels: inside too */
    int need_x = !point && s > 1u, need_y = !point && s > 1u;   /* (point: the shader repeats or clamps; bilinear repeat is the sampler's) */
    if (mu == 3u || mv == 3u) return 1;                    /* (point, region repeat, one: the wrap comes first in the shader, per original texel) */
    float x0 = mu == 2u ? (float)e->minu : 0.0f, x1 = mu == 2u ? (float)e->maxu + 1.0f : tw;
    float y0 = mv == 2u ? (float)e->minv : 0.0f, y1 = mv == 2u ? (float)e->maxv + 1.0f : th;
    for (u32 q = 0; q < e->count; q++) {
        const ps2_vk_vertex *v = &rep_verts[e->first + q];
        float qq = v->q > 1e-9f ? v->q : 1e-9f;
        float x = (fst ? v->s : v->s / qq * tw) + ofs, y = (fst ? v->t : v->t / qq * th) + ofs;
        if (point) { if ((need_x && (x < x0 || x + 1.0f / 32.0f >= x1)) || (need_y && (y < y0 || y + 1.0f / 32.0f >= y1))) RQH_NP(11); }
        else if ((need_x && (x < x0 || x > x1 - 1.0f)) || (need_y && (y < y0 || y > y1 - 1.0f))) RQH_NP(11);
    }
    return 1;
}
static float rqh_code_k0(u32 c, float fix) { return c == 1u || c == 3u ? 1.0f : c == 4u ? fix : c == 5u ? 1.0f - fix : 0.0f; }
static float rqh_code_k1(u32 c) { return c == 2u ? 1.0f : c == 3u ? -1.0f : 0.0f; }
/* the texture's slot in this frame's table */
static u32 rqh_tex_slot(u32 fi, VkDescriptorSet set) {
    u32 h = (u32)(((u64)(uintptr_t)set * 0x9E3779B97F4A7C15ull) >> 40) & 1023u;
    for (;; h = (h + 1u) & 1023u) {
        if (rqh.tmap[h].stamp != rqh.stamp) break;
        if (rqh.tmap[h].set == set) return rqh.tmap[h].slot;
    }
    if (rqh.frozen || rqh.ntex >= RQH_TEXS) return 0xFFFFFFFFu;
    u32 n = rqh.ntex++;
    rqh.tmap[h].set = set; rqh.tmap[h].stamp = rqh.stamp; rqh.tmap[h].slot = n;
    VkCopyDescriptorSet *c = &rqh.cp[rqh.ncp++];
    memset(c, 0, sizeof *c);
    c->sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET; c->srcSet = set; c->dstSet = rqh.tset[fi]; c->dstArrayElement = n; c->descriptorCount = 1;
    return n;
}
/* the draw's vertex bounds in game pixels, widened by its eye shift and a margin for wide lines */
static int rqh_box(const vk_draw *e, float b[4]) {
    if (e->native == PS2_VK_NATIVE_MESH) {
        if (e->nbox[2] >= e->nbox[0] && e->nbox[3] >= e->nbox[1]) { memcpy(b, e->nbox, 4 * sizeof(float)); return 1; }
        return 0;
    }
    if (e->native || !e->count || e->first + e->count > rep_nverts) return 0;
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (u32 q = 0; q < e->count; q++) {
        const ps2_vk_vertex *v = &rep_verts[e->first + q];
        float dx = fabsf(v->xr_dx);
        if (v->x - dx < x0) x0 = v->x - dx;
        if (v->x + dx > x1) x1 = v->x + dx;
        if (v->y < y0) y0 = v->y;
        if (v->y > y1) y1 = v->y;
    }
    b[0] = x0 - 2.0f; b[1] = y0 - 2.0f; b[2] = x1 + 2.0f; b[3] = y1 + 2.0f;
    return 1;
}
static int rqh_over(const float *a, const float *b) { return a[0] < b[2] && b[0] < a[2] && a[1] < b[3] && b[1] < a[3]; }

/* Writers of the alpha channel in this run, in the list's order: their rectangles and whether the alpha they leave passes
   a DATE of datm 1 (>= 1.0). a1: 1 the rectangle holds alpha >= 1.0, 0 < 1.0, -1 not known (textured, per-vertex alpha) */
typedef struct { float b[4]; float v[6]; int a1, rect; float dx; float vdx[3]; int cond; u32 src; } rqh_writer;   /* rect: 1 the box itself, 2 the triangle v, 0 not known;
   PS2_RQ_DATE_FREE: 3 the triangle v with each vertex's own eye shift vdx; cond 1 / 2: written only where the alpha below is < 1 / >= 1 (a DATE mask) */

/* vertices [q0, q0 + n) of draw e: do they tile their axis-aligned bounds exactly? */
static int rqh_is_rect(const vk_draw *e, u32 q0, u32 n, float b[4]) {
    if (e->topology != PS2_VK_TRIANGLES || n < 6u || (n % 3u) || e->first + q0 + n > rep_nverts) return 0;
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    double area = 0.0;
    for (u32 q = q0; q + 2u < q0 + n; q += 3u) {
        const ps2_vk_vertex *A = &rep_verts[e->first + q], *B = A + 1, *C = A + 2;
        const ps2_vk_vertex *t[3] = { A, B, C };
        for (int k = 0; k < 3; k++) {
            if (t[k]->x < x0) x0 = t[k]->x;
            if (t[k]->x > x1) x1 = t[k]->x;
            if (t[k]->y < y0) y0 = t[k]->y;
            if (t[k]->y > y1) y1 = t[k]->y;
        }
        double ax = B->x - A->x, ay = B->y - A->y, bx = C->x - A->x, by = C->y - A->y;
        area += fabs(ax * by - ay * bx) * 0.5;
    }
    double box = (double)(x1 - x0) * (double)(y1 - y0);
    if (box <= 0.0 || fabs(area - box) > 0.01 + box * 0.001) return 0;
    for (u32 q = q0; q < q0 + n; q++) {
        const ps2_vk_vertex *v = &rep_verts[e->first + q];
        if ((fabsf(v->x - x0) > 0.01f && fabsf(v->x - x1) > 0.01f) || (fabsf(v->y - y0) > 0.01f && fabsf(v->y - y1) > 0.01f)) return 0;
    }
    b[0] = x0; b[1] = y0; b[2] = x1; b[3] = y1;
    return 1;
}
/* PS2_RQ_DATE_FREE: the class of the alpha a textured writer leaves over its whole coverage: 1 all >= 1.0, 0 all < 1.0, -1 mixed
   or not known. The texels its texture coordinates can reach (region clamp: the box; region repeat: (u & mask) | fix; else the
   whole image) scanned in the decoded image, times the vertex alpha va (TFX modulate) or alone (decal, highlight 2); FBA sets the
   MSB; an alpha test that can fail leaves holes (not known). A small memo per texture, generation and state. */
static int rqh_tex_class(const vk_draw *e, float va) {
    u32 f = e->frag_flags, tfx = f & 3u;
    if ((f >> 21) & 1u) return 1;                                   /* (FBA) */
    if (((f >> 3) & 1u) && ((f >> 4) & 7u) != 1u) return -1;        /* (an alpha test that can fail) */
    if (!((f >> 17) & 1u)) return va >= 1.0f ? 1 : 0;               /* (no texture alpha: the vertex's) */
    if (tfx == 2u) return -1;                                       /* (highlight: At + Av, not scanned) */
    if (e->tex_rt || e->tex == 0xFFFFu || e->tex >= ntextures) return -1;
    vk_texture *t = &textures[e->tex];
    if (!t->rgba || !t->w || !t->h) return -1;
    static struct { u32 tex, gen, wrap, uv[4]; float va; int c; } memo[16];
    static u32 mnext;
    u32 uv[4] = { e->minu, e->maxu, e->minv, e->maxv };
    for (u32 k = 0; k < 16u; k++)
        if (memo[k].tex == (u32)e->tex + 1u && memo[k].gen == t->generation && memo[k].wrap == e->wrap && !memcmp(memo[k].uv, uv, sizeof uv) && memo[k].va == va) return memo[k].c;
    float amin = 2.0f, amax = -1.0f;
    u32 W = t->w, H = t->h, S = t->scale > 1u ? t->scale : 1u;     /* (a replacement image: S x S pixels a game texel) */
    u32 mx = e->wrap & 3u, my = (e->wrap >> 2) & 3u;
    for (u32 y = 0; y < H; y++) {
        u32 gy = y / S;
        if (my == 2u && (gy < e->minv || gy > e->maxv)) continue;
        if (my == 3u && ((gy & e->minv) | e->maxv) != gy) continue;   /* (region repeat: only the texels the mask and fix reach) */
        for (u32 x = 0; x < W; x++) {
            u32 gx = x / S;
            if (mx == 2u && (gx < e->minu || gx > e->maxu)) continue;
            if (mx == 3u && ((gx & e->minu) | e->maxu) != gx) continue;
            float a = (float)t->rgba[4u * ((size_t)y * W + x) + 3u] / 255.0f;
            if (a < amin) amin = a;
            if (a > amax) amax = a;
        }
    }
    int c = -1;
    if (amax >= 0.0f) {
        float lo = tfx == 0u ? amin * va : amin, hi = tfx == 0u ? amax * va : amax;
        c = lo >= 1.0f - 1e-4f ? 1 : hi < 1.0f - 1e-4f ? 0 : -1;
    }
    u32 k = mnext++ & 15u;
    memo[k].tex = (u32)e->tex + 1u; memo[k].gen = t->generation; memo[k].wrap = e->wrap; memcpy(memo[k].uv, uv, sizeof uv); memo[k].va = va; memo[k].c = c;
    rqdf.wscan++;
    return c;
}
/* an alpha writer's effect: each quad (6 vertices) that tiles its bounds a rectangle, else its triangles; uniform alpha known only
   when untextured and equal over the piece (the radar's masks: the clear, the field-of-view wedge, the labels' clears) */
static void rqh_writer_tri(rqh_writer *wr, u32 *nw, const vk_draw *e, u32 q) {
    if (*nw >= RQH_WR) return;
    rqh_writer *w = &wr[(*nw)++];
    const ps2_vk_vertex *v = &rep_verts[e->first + q];
    float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
    for (int k = 0; k < 3; k++) {
        w->v[2 * k] = v[k].x; w->v[2 * k + 1] = v[k].y;
        if (v[k].x < x0) x0 = v[k].x;
        if (v[k].x > x1) x1 = v[k].x;
        if (v[k].y < y0) y0 = v[k].y;
        if (v[k].y > y1) y1 = v[k].y;
    }
    w->b[0] = x0; w->b[1] = y0; w->b[2] = x1; w->b[3] = y1;
    w->rect = 2; w->dx = v[0].xr_dx; w->a1 = -1; w->cond = 0; w->src = (u32)(e - rep_draws);
    for (int k = 0; k < 3; k++) w->vdx[k] = v[k].xr_dx;
    if (v[1].xr_dx != w->dx || v[2].xr_dx != w->dx) w->rect = rq_date_free() ? 3 : 0;   /* (PS2_RQ_DATE_FREE: eye space per view) */
    if (v[1].a == v[0].a && v[2].a == v[0].a && e->tex == 0xFFFFu && !e->tex_rt) { float A = v[0].a / 128.0f; if ((e->frag_flags >> 21) & 1u) A = A < 1.0f ? 1.0f : A; w->a1 = A >= 1.0f ? 1 : 0; }
    else if (rq_date_free() && v[1].a == v[0].a && v[2].a == v[0].a) w->a1 = rqh_tex_class(e, v[0].a / 128.0f);
    if (rq_date_free() && e->occl) w->a1 = -1;          /* (a depth-bounds key: it writes only over / not over the cockpit model: not known here) */
}
static void rqh_writer_add(rqh_writer *wr, u32 *nw, const vk_draw *e, float eb[4]) {
    if (e->topology != PS2_VK_TRIANGLES || (e->count % 3u) || !e->count) {     /* (lines, points: not known) */
        if (*nw < RQH_WR) { rqh_writer *w = &wr[(*nw)++]; w->rect = 0; memcpy(w->b, eb, sizeof w->b); w->a1 = -1; w->dx = 0.0f; w->cond = 0; }
        return;
    }
    u32 q = 0;
    while (q < e->count) {
        float b[4];
        if (q + 6u <= e->count && rqh_is_rect(e, q, 6u, b)) {
            const ps2_vk_vertex *v = &rep_verts[e->first + q];
            int same = 1;
            for (u32 k = 1; k < 6u; k++) if (v[k].a != v[0].a || v[k].xr_dx != v[0].xr_dx) same = 0;
            if (!same && rq_date_free()) { rqh_writer_tri(wr, nw, e, q); rqh_writer_tri(wr, nw, e, q + 3u); q += 6u; continue; }   /* (PS2_RQ_DATE_FREE) */
            if (*nw < RQH_WR) {
                rqh_writer *w = &wr[(*nw)++];
                memcpy(w->b, b, sizeof w->b); w->rect = same ? 1 : 0; w->dx = v[0].xr_dx; w->a1 = -1; w->cond = 0; w->src = (u32)(e - rep_draws);
                if (same && e->tex == 0xFFFFu && !e->tex_rt) { float A = v[0].a / 128.0f; if ((e->frag_flags >> 21) & 1u) A = A < 1.0f ? 1.0f : A; w->a1 = A >= 1.0f ? 1 : 0; }
                else if (same && rq_date_free()) w->a1 = rqh_tex_class(e, v[0].a / 128.0f);
                if (rq_date_free() && e->occl) w->a1 = -1;   /* (a depth-bounds key: not known here) */
            }
            q += 6u;
        } else { rqh_writer_tri(wr, nw, e, q); q += 3u; }
    }
}
/* the one eye shift of all its vertices, or NaN */
static float rqh_dx(const vk_draw *e) {
    float dx = rep_verts[e->first].xr_dx;
    for (u32 q = 1; q < e->count; q++) if (rep_verts[e->first + q].xr_dx != dx) return NAN;
    return dx;
}

/* the DATE of draw e against the writers so far: 1 passes everywhere, 0 fails everywhere, 2 depends on the rectangles idx[0..*n)
   (newest first: the fragment's alpha is that of the first one containing it), -1 not known (drawn in place) */
static int rqh_date(const vk_draw *e, const rqh_writer *wr, u32 nw, u32 *idx, u32 *n) {
    int datm = (int)((e->frag_flags >> 23) & 1u);
    float b[4] = { 1e9f, 1e9f, -1e9f, -1e9f };
    for (u32 q = 0; q < e->count; q++) {
        const ps2_vk_vertex *v = &rep_verts[e->first + q];
        if (v->x < b[0]) b[0] = v->x;
        if (v->y < b[1]) b[1] = v->y;
        if (v->x > b[2]) b[2] = v->x;
        if (v->y > b[3]) b[3] = v->y;
    }
    float dx = rqh_dx(e);
    if (isnan(dx)) return -1;
    *n = 0;
    for (int k = (int)nw - 1; k >= 0; k--) {
        const rqh_writer *w = &wr[k];
        if (!(b[0] < w->b[2] && w->b[0] < b[2] && b[1] < w->b[3] && w->b[1] < b[3])) continue;
        if (!w->rect || w->a1 < 0 || !(fabsf(w->dx - dx) < 1e-3f) || *n >= RQH_WR) return -1;
        idx[(*n)++] = (u32)k;
        if (w->rect == 1 && b[0] >= w->b[0] - 0.01f && b[1] >= w->b[1] - 0.01f && b[2] <= w->b[2] + 0.01f && b[3] <= w->b[3] + 0.01f) {
            if (*n == 1u) return (w->a1 != 0) == (datm != 0);
            return 2;                                   /* (the rectangles decide, in the fragment shader) */
        }
    }
    return -1;
}

/* PS2_RQ_DATE_FREE: rqh_date for any eye shift. Writers are tested in eye space per view in the fragment (rect 1 / 2 moved by
   their own shift, 3 by each vertex's), so the reader's shift need not be uniform; conditional writers (DATE masks) are collected
   and resolved in the fragment. The scan (newest first) stops at an unconditional writer that covers the reader: a rectangle
   with the reader's box inside (0.5 px, widened by the shifts' difference), or two triangles of one box whose areas tile it
   (one plane: the shift is affine over it; the reader's shifts within theirs). 1 / 0 when every collected writer leaves the same
   class unconditionally, 2 the fragment decides, -1 not known (the read stays: counted as unresolved) */
/* PS2_RQ_DATE_FREE: the draw's scissor as the eye pass sets it (the draw loop's rectangle, rqlp_scissor, rq_scissor), in eye pixels */
static void rqh_sc_eye(const vk_draw *e, float o[4]) {
    u32 dsc = tsc(&rts[0]);
    s32 x0 = e->scissor[0] < 0 ? 0 : e->scissor[0], y0 = e->scissor[2] < 0 ? 0 : e->scissor[2];
    s32 x1 = e->scissor[1] + 1, y1 = e->scissor[3] + 1;
    if (x0 > (s32)rt_w - 1) x0 = (s32)rt_w - 1;
    if (y0 > (s32)rt_h - 1) y0 = (s32)rt_h - 1;
    if (x1 > (s32)rt_w) x1 = (s32)rt_w;
    if (y1 > (s32)rt_h) y1 = (s32)rt_h;
    if (x1 <= x0) x1 = x0 + 1;
    if (y1 <= y0) y1 = y0 + 1;
    VkRect2D sc;
    sc.offset.x = x0 * (s32)dsc; sc.offset.y = y0 * (s32)dsc; sc.extent.width = (u32)(x1 - x0) * dsc; sc.extent.height = (u32)(y1 - y0) * dsc;
    if (rqlp.on) rqlp_scissor(&sc, dsc);
    rq_scissor(&sc, dsc);
    o[0] = (float)sc.offset.x; o[1] = (float)sc.offset.y; o[2] = (float)(sc.offset.x + (s32)sc.extent.width); o[3] = (float)(sc.offset.y + (s32)sc.extent.height);
}
#define RQH_BASE 0xFFFFFFFFu            /* (PS2_RQ_DATE_FREE: the cockpit's mark below the HUD, an index of no writer) */
static float rqh_wmaxdx(const rqh_writer *w) {
    if (w->rect == 3) return fmaxf(fabsf(w->vdx[0]), fmaxf(fabsf(w->vdx[1]), fabsf(w->vdx[2])));
    return fabsf(w->dx);
}
/* (a writer's vertex m in eye space of the view with shift sign s) */
static void rqh_wvert(const rqh_writer *w, int m, float s, float *x, float *y) {
    float dxm = w->rect == 3 ? w->vdx[m] : w->dx;
    *x = w->v[2 * m] + s * dxm; *y = w->v[2 * m + 1];
}
/* point in triangle with a tolerance t (pixels, by the signed distances to the edges) */
static int rqh_in_tri(float px, float py, const float P[6], float t) {
    float area = (P[2] - P[0]) * (P[5] - P[1]) - (P[3] - P[1]) * (P[4] - P[0]);
    if (fabsf(area) < 1e-6f) return 0;
    float sg = area > 0.0f ? 1.0f : -1.0f;
    for (int k = 0; k < 3; k++) {
        float ax = P[2 * k], ay = P[2 * k + 1], bx = P[(2 * k + 2) % 6], by = P[(2 * k + 3) % 6];
        float ex = bx - ax, ey = by - ay, l = sqrtf(ex * ex + ey * ey);
        if (l < 1e-6f) return 0;
        if (sg * (ex * (py - ay) - ey * (px - ax)) / l < -t) return 0;
    }
    return 1;
}
static int rqh_date_free(const vk_draw *e, const rqh_writer *wr, u32 nw, u32 *idx, u32 *n) {
    int datm = (int)((e->frag_flags >> 23) & 1u);
    if (e->occl) return -1;                                   /* (a depth-bounds key: its fragments and the base below are the cockpit's or the world's) */
    float b[4] = { 1e9f, 1e9f, -1e9f, -1e9f }, rdx = 0.0f, rb[2][4];
    int vis[2] = { 1, 1 };
    if (!e->count || e->first + e->count > rep_nverts) return -1;
    for (int view = 0; view < 2; view++) { rb[view][0] = rb[view][1] = 1e9f; rb[view][2] = rb[view][3] = -1e9f; }
    for (u32 q = 0; q < e->count; q++) {
        const ps2_vk_vertex *v = &rep_verts[e->first + q];
        if (v->x < b[0]) b[0] = v->x;
        if (v->y < b[1]) b[1] = v->y;
        if (v->x > b[2]) b[2] = v->x;
        if (v->y > b[3]) b[3] = v->y;
        if (fabsf(v->xr_dx) > rdx) rdx = fabsf(v->xr_dx);
        for (int view = 0; view < 2; view++) {                 /* (the reader's box in eye space per view) */
            float x = v->x + (mv_shift ? (view == 0 ? -1.0f : 1.0f) : 0.0f) * v->xr_dx;
            if (x < rb[view][0]) rb[view][0] = x;
            if (x > rb[view][2]) rb[view][2] = x;
            if (v->y < rb[view][1]) rb[view][1] = v->y;
            if (v->y > rb[view][3]) rb[view][3] = v->y;
        }
    }
    for (int view = 0; view < 2; view++) {                     /* (clipped by its scissor: the eye pass maps it without the shift) */
        if (rb[view][0] < (float)e->scissor[0]) rb[view][0] = (float)e->scissor[0];
        if (rb[view][2] > (float)e->scissor[1] + 1.0f) rb[view][2] = (float)e->scissor[1] + 1.0f;
        if (rb[view][1] < (float)e->scissor[2]) rb[view][1] = (float)e->scissor[2];
        if (rb[view][3] > (float)e->scissor[3] + 1.0f) rb[view][3] = (float)e->scissor[3] + 1.0f;
        vis[view] = rb[view][0] < rb[view][2] && rb[view][1] < rb[view][3];
    }
    if (!vis[0] && !vis[1]) return 0;                          /* (nothing inside its scissor: it draws nothing) */
    const float t = 0.5f;
    *n = 0;
    int cover = 0;
    for (int k = (int)nw - 1; k >= 0 && !cover; k--) {
        const rqh_writer *w = &wr[k];
        float pad = rdx + rqh_wmaxdx(w) + 1.0f;
        if (!(b[0] - pad < w->b[2] && w->b[0] < b[2] + pad && b[1] - 1.0f < w->b[3] && w->b[1] < b[3] + 1.0f)) continue;
        if (!w->rect || w->a1 < 0 || *n + 2u > RQH_WR) return -1;
        idx[(*n)++] = (u32)k;
        if (w->cond) continue;
        if (w->rect == 1) {                                     /* (a rectangle: the reader's eye box inside it in both views) */
            int in = 1;
            for (int view = 0; view < 2 && in; view++) {
                if (!vis[view]) continue;
                float s = mv_shift ? (view == 0 ? -1.0f : 1.0f) : 0.0f;
                if (!(rb[view][0] >= w->b[0] + s * w->dx - t && rb[view][2] <= w->b[2] + s * w->dx + t && rb[view][1] >= w->b[1] - t && rb[view][3] <= w->b[3] + t)) in = 0;
            }
            cover = in;
        } else if (k > 0) {                                     /* (two triangles sharing an edge: a convex quad; the reader's eye box corners inside it) */
            const rqh_writer *u = &wr[k - 1];
            if (u->rect < 2 || u->rect > 3 || u->cond || u->a1 < 0) continue;
            int shared = 0;
            for (int m = 0; m < 3; m++) for (int m2 = 0; m2 < 3; m2++)
                if (fabsf(w->v[2 * m] - u->v[2 * m2]) < 0.05f && fabsf(w->v[2 * m + 1] - u->v[2 * m2 + 1]) < 0.05f) shared++;
            if (shared < 2) continue;
            int in = 1;
            for (int view = 0; view < 2 && in; view++) {
                if (!vis[view]) continue;
                float s = mv_shift ? (view == 0 ? -1.0f : 1.0f) : 0.0f, Pw[6], Pu[6];
                for (int m = 0; m < 3; m++) { rqh_wvert(w, m, s, &Pw[2 * m], &Pw[2 * m + 1]); rqh_wvert(u, m, s, &Pu[2 * m], &Pu[2 * m + 1]); }
                for (int c = 0; c < 4 && in; c++) {
                    float px = (c & 1) ? rb[view][2] : rb[view][0], py = (c & 2) ? rb[view][3] : rb[view][1];
                    if (!rqh_in_tri(px, py, Pw, t) && !rqh_in_tri(px, py, Pu, t)) in = 0;
                }
            }
            if (in) { idx[(*n)++] = (u32)(k - 1); cover = 1; }
        }
    }
    if (!cover) {
        /* (not covered by known writers: over the Quest cockpit's solid model the alpha below the HUD is its mark, RQZ_MARK < 1, so a
           datm-0 reader passes where no writer is; uncovered fragments pass in rq_hud.frag. Counted: the read path agrees only there) */
        static int nobase = -1;                         /* (PS2_RQ_MAP_HOST, P-5: read once, not by getenv per reader) */
        if (nobase < 0) nobase = getenv("PS2_RQ_DFREE_NOBASE") != NULL;
        if (e->xcls == 2u && rql.cp_quest && rql.cp_drawn && *n + 1u <= RQH_WR && !(rqmh_now ? nobase : getenv("PS2_RQ_DFREE_NOBASE") != NULL)) { rqdf.hbase++; idx[(*n)++] = RQH_BASE; }   /* (PS2_RQ_DFREE_NOBASE [test]: no base, the reader reads) */   /* (the mark as the oldest writer: a < 1 everywhere) */
        else return -1;
    }
    int same = 1;
#define RQH_A1(k) (idx[k] == RQH_BASE ? 0 : wr[idx[k]].a1)
    for (u32 k = 0; k < *n; k++) if ((idx[k] != RQH_BASE && wr[idx[k]].cond) || RQH_A1(k) != RQH_A1(0)) same = 0;
    if (same) return (RQH_A1(0) != 0) == (datm != 0) ? 1 : 0;
#undef RQH_A1
    return 2;
}
/* PS2_RQ_DATE_FREE: the collected writers as eye-space entries, two per writer (views 0 and 1): kinds 2 / 3 a triangle, 4 / 5 a
   rectangle; code = a1 + 2 cond */
static void rqh_free_entries(float (*WR)[4], u32 first, const rqh_writer *wr, const u32 *idx, u32 n) {
    for (u32 k = 0; k < n; k++) {
        if (idx[k] == RQH_BASE) {                       /* (the base: a rectangle over everything, alpha < 1) */
            for (int view = 0; view < 2; view++) {
                float *E0 = WR[2u * (first + 2u * k + (u32)view)], *E1 = WR[2u * (first + 2u * k + (u32)view) + 1u];
                E0[0] = -1e6f; E0[1] = -1e6f; E0[2] = 1e6f; E0[3] = 1e6f; E1[0] = 0.0f; E1[1] = 0.0f; E1[2] = 0.0f; E1[3] = (float)(4 + view);
            }
            continue;
        }
        const rqh_writer *w = &wr[idx[k]];
        for (int view = 0; view < 2; view++) {
            float s = mv_shift ? (view == 0 ? -1.0f : 1.0f) : 0.0f;
            float *E0 = WR[2u * (first + 2u * k + (u32)view)], *E1 = WR[2u * (first + 2u * k + (u32)view) + 1u];
            float code = (float)(w->a1 + 2 * w->cond);
            if (w->rect == 1) {
                float x0 = w->b[0], y0 = w->b[1], x1 = w->b[2], y1 = w->b[3];
                if (rqlp.on) { rqlp_vert(&x0, &y0, -1.0f, w->dx); rqlp_vert(&x1, &y1, -1.0f, w->dx); }
                E0[0] = x0 + s * w->dx; E0[1] = y0; E0[2] = x1 + s * w->dx; E0[3] = y1;
                E1[0] = 0.0f; E1[1] = 0.0f; E1[2] = code; E1[3] = (float)(4 + view);
            } else {
                float P[6];
                for (int m = 0; m < 3; m++) {
                    float dxm = w->rect == 3 ? w->vdx[m] : w->dx, x = w->v[2 * m], y = w->v[2 * m + 1];
                    if (rqlp.on) rqlp_vert(&x, &y, -1.0f, dxm);
                    P[2 * m] = x + s * dxm; P[2 * m + 1] = y;
                }
                E0[0] = P[0]; E0[1] = P[1]; E0[2] = P[2]; E0[3] = P[3];
                E1[0] = P[4]; E1[1] = P[5]; E1[2] = code; E1[3] = (float)(2 + view);
            }
        }
    }
}

static void rqh_emit(rqh_vtx *o, const ps2_vk_vertex *v, u32 id, const ps2_vk_vertex *other, float side) {
    o->x = v->x; o->y = v->y; o->z = v->z; o->r = v->r; o->g = v->g; o->b = v->b; o->a = v->a; o->s = v->s; o->t = v->t; o->q = v->q;
    o->fog = v->fog; o->ruv = v->round_uv; o->dx = v->xr_dx; o->id = id;
    if (other) { o->ox = other->x; o->oy = other->y; o->odx = other->xr_dx; o->side = side; }
    else { o->ox = o->oy = o->odx = 0.0f; o->side = 0.0f; }
    if (rqlp.on) {                                      /* (PS2_XR_LATE_POSE: as gs.vert's CINE_BIT path; the eye shift stays) */
        rqlp_vert(&o->x, &o->y, v->xr_iw, v->xr_dx);
        if (other) rqlp_vert(&o->ox, &o->oy, other->xr_iw, other->xr_dx);
    }
}

static int rqh_box_in_scissor(const vk_draw *e) {
    s32 x0 = e->scissor[0] < 0 ? 0 : e->scissor[0], y0 = e->scissor[2] < 0 ? 0 : e->scissor[2];
    s32 x1 = e->scissor[1] + 1, y1 = e->scissor[3] + 1;
    if (x1 > (s32)rt_w) x1 = (s32)rt_w;
    if (y1 > (s32)rt_h) y1 = (s32)rt_h;
    if (x0 == 0 && y0 == 0 && x1 == (s32)rt_w && y1 == (s32)rt_h) return 1;
    for (u32 q = 0; q < e->count; q++) {
        const ps2_vk_vertex *v = &rep_verts[e->first + q];
        float dx = fabsf(v->xr_dx);
        if (v->x - dx < (float)x0 || v->x + dx > (float)x1 || v->y < (float)y0 || v->y > (float)y1) return 0;
    }
    return 1;
}

/* the flush: at the position of the run's first class-2 draw. Scans from there to the end of slot 0's run */
/* mode 0: the HUD (classes 2 and the class 1 between them), from the first class-2 draw to the end of the run; mode 1: the world's effects (class 1), [from_ii, to_ii) */
static int rq_hud_flush(VkCommandBuffer cmd, vk_frame *fr, const u32 *order, u32 from_ii, u32 to_ii, int mode, int cpit_done, int cpit_after, u32 split_ii) {
    static rqh_cand *C;
    static float (*bars)[4];
    static u8 *badd;
    if (!C) { C = (rqh_cand *)malloc(sizeof(rqh_cand) * 8192u); bars = (float (*)[4])malloc(sizeof(float) * 4u * RQH_BARS); badd = (u8 *)malloc(RQH_BARS); if (!C || !bars || !badd) return 0; }
    u32 fi = (u32)(fr - frames);
    if (fi >= FRAMES_IN_FLIGHT) return 0;
    rqh_writer wr[RQH_WR];
    u32 nc = 0, nb = 0, nw = 0;
    u32 g_key[RQH_MAX_G]; u32 ng = 0;
    int ov = cpit_done && cpit_after >= 0 && depth_bounds_ok;
    int wr_full = 0;
    u32 last_reader = 0;
    u32 split_g = 0; int split_set = 0;                         /* (PS2_RQ_HUD_CLSPLIT: the first group after the clouds' point) */
    static u32 dl_idx[RQH_WR]; u32 dl_n = 0, dl_first = 0; int dl_ok = 0;   /* (PS2_RQ_DATE_FREE: the last reader's entries, shared by the next one with the same writers) */
    for (u32 q = from_ii; q < rep_ndraws; q++) {
        const vk_draw *e = &rep_draws[order ? order[q] : q];
        if (e->rt != 0u || q >= to_ii) break;
        if (e->date) last_reader = q;
    }
    for (u32 q = from_ii; q < rep_ndraws; q++) {
        u32 j = order ? order[q] : q;
        const vk_draw *e = &rep_draws[j];
        if (e->rt != 0u || q >= to_ii) break;                   /* (the end of slot 0's run, of the effects' range) */
        if (q >= split_ii && !split_set) { split_g = ng; split_set = 1; }
        if (cpit_after >= 0 && e->cockpit) continue;            /* (the game's cockpit under the model) */
        if (rqh.cl_drop && cl_sprite(e)) continue;              /* (the game's cloud sprites: our clouds replace them, the loop drops them) */
        u32 why = 0;
        int take = rqh_ok(e, &why);
        if (take && mode == 1 && e->xcls != 1u) { take = 0; why = 1; }
        if (take && e->hudblk && e->hudblk < MFD_NBLK && mfd_rp.hide[e->hudblk]) { rqh.tk[j] = rqh.stamp; continue; }  /* (shown by the cockpit) */
        if (take && (e->first + e->count > rep_nverts || !e->count)) { take = 0; why = 6; }
        u32 dbk = 0;
        if (take && e->occl) {                                  /* (the depth-bounds keys, as the draw loop) */
            if (e->occl == 2u) { if (!ov) { rqh.tk[j] = rqh.stamp; continue; } dbk = 2u; }
            else if (e->occl == 3u && ov && hg_drawn) dbk = 3u;
            else if (ov) dbk = 1u;
        }
        int fails = 0, date_o = -1;
        u32 d_idx[RQH_WR], d_n = 0, d_first = 0;
        if (take && e->date && mode == 1) { take = 0; why = 7; }  /* (the world's DATE draws, the haze ring: not this batch's) */
        if (take && e->date) {
            int o = wr_full ? -1 : rq_date_free() ? rqh_date_free(e, wr, nw, d_idx, &d_n) : rqh_date(e, wr, nw, d_idx, &d_n);
            date_o = o;
            if (rq_date_free() && xr_dump_now && getenv("PS2_RQ_LIST")) {   /* ([test] PS2_RQ_LIST: the free decision and its writers) */
                char tt[900]; int oo = 0;
                for (u32 k = 0; k < d_n && o >= 0 && oo < 840; k++) {
                    if (d_idx[k] == RQH_BASE) { oo += snprintf(tt + oo, sizeof tt - (size_t)oo, " [base]"); continue; }
                    const rqh_writer *w = &wr[d_idx[k]]; const vk_draw *s = &rep_draws[w->src];
                    oo += snprintf(tt + oo, sizeof tt - (size_t)oo, " [d%u a%d r%d c%d %.0f,%.0f,%.0f,%.0f depth %X occl %u tex %d frag %08X]", w->src, w->a1, w->rect, w->cond,
                                   (double)w->b[0], (double)w->b[1], (double)w->b[2], (double)w->b[3], s->depth, s->occl, s->tex == 0xFFFFu ? -1 : (int)s->tex, s->frag_flags);
                }
                ps2_log("rq-hud-free: field %u reader d%u datm %u depth %X o %d writers %u:%s", ps2_gs_frame_count, j, (e->frag_flags >> 23) & 1u, e->depth, o, d_n, tt);
            }
            float b_[4]; rqh_box(e, b_);
            if (o < 0) {
                take = 0; why = 7; rqh.s_date[2]++;
                static u32 ndl;
                if ((xr_dump_now || ndl < 6u) && getenv("PS2_RQ_LIST")) {
                    ndl++;
                    char tt[700]; int oo = 0;
                    for (u32 k = 0; k < nw && oo < 640; k++) oo += snprintf(tt + oo, sizeof tt - (size_t)oo, " [%.1f,%.1f,%.1f,%.1f a%d r%d c%d]", (double)wr[k].b[0], (double)wr[k].b[1], (double)wr[k].b[2], (double)wr[k].b[3], wr[k].a1, wr[k].rect, wr[k].cond);
                    ps2_log("rq-hud-date: field %u draw %u unknown (full %d, dx %.2f, verts %u, box %.1f,%.1f,%.1f,%.1f) writers:%s", ps2_gs_frame_count, j, wr_full, (double)rqh_dx(e), e->count, (double)b_[0], (double)b_[1], (double)b_[2], (double)b_[3], tt);
                }
            }
            else if (o == 0) { fails = 1; rqh.s_date[1]++; }
            else if (o == 2) {                                  /* (the fragment decides between the rectangles) */
                rqh.s_date[0]++;
                if (rq_date_free()) {                   /* (PS2_RQ_DATE_FREE: eye-space entries, two per writer) */
                    if (dl_ok && d_n == dl_n && !memcmp(d_idx, dl_idx, d_n * sizeof(u32))) { d_first = dl_first; d_n *= 2u; rqdf.eshare++; }   /* (the same writers: the same entries) */
                    else if (rqh.nrect + 2u * d_n > RQH_RECTS) { take = 0; why = 6; }
                    else {
                        d_first = rqh.nrect;
                        rqh_free_entries((float (*)[4])((u8 *)rqh.sp[fi] + (VkDeviceSize)RQH_MAX_D * sizeof(rqh_row)), d_first, wr, d_idx, d_n);
                        memcpy(dl_idx, d_idx, d_n * sizeof(u32)); dl_n = d_n; dl_first = d_first; dl_ok = 1;
                        d_n *= 2u; rqh.nrect += d_n;
                    }
                } else
                if (rqh.nrect + d_n > RQH_RECTS) { take = 0; why = 6; }
                else {
                    d_first = rqh.nrect;
                    float (*WR)[4] = (float (*)[4])((u8 *)rqh.sp[fi] + (VkDeviceSize)RQH_MAX_D * sizeof(rqh_row));
                    for (u32 k = 0; k < d_n; k++) {
                        const rqh_writer *w = &wr[d_idx[k]];
                        float *E0 = WR[2u * (d_first + k)], *E1 = WR[2u * (d_first + k) + 1u];
                        if (w->rect == 1) { memcpy(E0, w->b, 4 * sizeof(float)); E1[0] = 0.0f; E1[1] = 0.0f; E1[3] = 0.0f; }
                        else { memcpy(E0, w->v, 4 * sizeof(float)); E1[0] = w->v[4]; E1[1] = w->v[5]; E1[3] = 1.0f; }
                        if (rqlp.on) {                  /* (PS2_XR_LATE_POSE: the writers moved with their vertices; the fragment's game pixel is the moved one) */
                            rqlp_vert(&E0[0], &E0[1], -1.0f, w->dx); rqlp_vert(&E0[2], &E0[3], -1.0f, w->dx);
                            if (w->rect != 1) rqlp_vert(&E1[0], &E1[1], -1.0f, w->dx);
                        }
                        E1[2] = w->a1 ? 1.0f : 0.0f;
                    }
                    rqh.nrect += d_n;
                }
            } else { d_n = 0; rqh.s_date[0]++; }
        }
        int sc_need = 0;                                        /* (PS2_RQ_DATE_FREE: a draw its scissor cuts goes in, the scissor in the fragment) */
        if (take && !rqh_box_in_scissor(e)) { if (rq_date_free()) sc_need = 1; else { take = 0; why = 8; } }
        u32 sf = 0, df = 0;
        int wa = (e->depth >> 4) & 1u && !((e->depth >> 8) & 1u);
        float b[4];
        int hasb = rqh_box(e, b);
        if (!hasb) { b[0] = b[1] = -1e9f; b[2] = b[3] = 1e9f; }
        (void)rqh_blend(e->blend, &sf, &df);
        int add = (e->blend & 0x100u) && df == 1u && !wa && (sf == 1u || sf == 2u || sf == 4u);
        if (take && !fails) {
            /* a draw behind a foreign draw it overlaps and does not commute with stays out */
            for (u32 k = 0; k < nb; k++) if (rqh_over(b, bars[k]) && !(add && badd[k])) { take = 0; why = 9; break; }
        }
        if (!take) {                                            /* a foreign draw: a barrier for the rest */
            if (e->xcls == 2u && why) rqh.s_left[why < 10 ? why : 9]++;
            if (xr_dump_now && getenv("PS2_RQ_LIST")) ps2_log("rq-hud-left: draw %u xcls %u why %u blend 0x%X depth 0x%X date %u box %.0f,%.0f,%.0f,%.0f", j, (unsigned)e->xcls, why, e->blend, e->depth, e->date, (double)b[0], (double)b[1], (double)b[2], (double)b[3]);
            if (rq_date_free() && e->date) rqdf_why[j] = (u8)(why ? why : 99u);
            if (rq_date_free() && e->date && e->xcls == 2u) {   /* (PS2_RQ_DATE_FREE: the in-place draw's DATE, decided here when it can be) */
                u32 ti[RQH_WR], tn = 0;
                int o = date_o >= 0 && date_o != 2 ? date_o : wr_full ? -1 : rqh_date_free(e, wr, nw, ti, &tn);
                if (o == 0 || o == 1) { rqdf_dec[j] = o == 0 ? 2u : 1u; date_o = o; } else date_o = -1;
            }
            if (((e->depth >> 4) & 1u) && !((e->depth >> 8) & 1u) && !((e->depth >> 2) & 1u) && q < last_reader) {  /* (drawn elsewhere: its alpha is not known here) */
                if (rq_date_free() && (!e->date || date_o == 1) && nw + e->count / 3u + 1u <= RQH_WR) {   /* (PS2_RQ_DATE_FREE: drawn whole: its alpha as a taken one's) */
                    u32 nw0 = nw;
                    rqh_writer_add(wr, &nw, e, b);
                    if (((e->frag_flags >> 3) & 1u) && ((e->frag_flags >> 4) & 7u) != 1u) for (u32 k = nw0; k < nw; k++) wr[k].a1 = -1;   /* (an alpha test: holes) */
                } else if (rq_date_free() && e->date && date_o == 0) { }   /* (skipped: writes nothing) */
                else if (nw < RQH_WR) { rqh_writer *w = &wr[nw++]; w->rect = 0; memcpy(w->b, b, sizeof w->b); memset(w->v, 0, sizeof w->v); w->a1 = -1; w->dx = 0.0f; w->cond = 0; }
                else wr_full = 1;
            }
            if (nb < RQH_BARS) {
                memcpy(bars[nb], b, sizeof(float) * 4u);
                u32 s2 = 0, d2 = 0; int rb = rqh_blend(e->blend, &s2, &d2);
                badd[nb] = (u8)(rb && (e->blend & 0x100u) && d2 == 1u && (s2 == 1u || s2 == 2u || s2 == 4u) && !(((e->depth >> 4) & 1u) && !((e->depth >> 8) & 1u)) && !((e->depth >> 2) & 1u) ? 1 : 0);
                nb++;
            } else break;                                       /* (too many: the rest stays in the list) */
            continue;
        }
        rqh.tk[j] = rqh.stamp;                                  /* (taken: the loop leaves it out) */
        if (e->date) rqdf_boxlog(e, j, fails ? "batch-fail" : d_n ? "batch-rects" : "batch-pass");   /* (PS2_RQ_LIST: a dumped frame's DATE boxes) */
        if (wa && q < last_reader) {                            /* (the alpha it leaves, quad by quad; only what a later DATE can see) */
            if (nw + e->count / 3u + 1u > RQH_WR) wr_full = 1;
            else if (!(rq_date_free() && e->date && fails)) {   /* (PS2_RQ_DATE_FREE: a mask whose DATE fails everywhere writes nothing) */
                u32 nw0 = nw;
                rqh_writer_add(wr, &nw, e, b);
                if (rq_date_free() && e->date && date_o == 2) for (u32 k = nw0; k < nw; k++) wr[k].cond = 1 + (int)((e->frag_flags >> 23) & 1u);
            }
        }
        if (fails) { rqh.tk[j] = rqh.stamp; continue; }          /* (DATE fails everywhere: nothing to draw) */
        /* the group */
        u32 ztst = e->depth & 3u, key = dbk | (ztst << 2);
        float m3 = -1.0f;
        int plain = !sc_need && rqh_plain(e, d_n, &m3);
        if (plain) key |= 16u;
        float zc = -1.0f, ztop = 2.0f; u8 zbelow = 0;
        if (RQH_ONE() && ztst && dbk < 2u) {                     /* (one GEQUAL pipeline: the compare and the bounds key in the vertices' z; D32F, so nextafter is exact) */
            float top = hg_boot() ? HG_MARK + HG_BAND : 0.99f;
            int ok = 1;
            if (ztst == 1u) zc = dbk ? top : 1.0f;
            else {
                zbelow = ztst == 3u;
                if (dbk) {
                    int lo = 0, hi = 0;
                    for (u32 q = 0; q < e->count; q++) { float z_ = rep_verts[e->first + q].z; if (z_ > top) hi = 1; else lo = 1; }
                    if (lo && hi) ok = 0; else ztop = top;   /* (a triangle crossing the bound keeps its own pipeline) */
                }
            }
            if (ok) key = (plain ? 16u : 0u) | (2u << 2);
            else { zc = -1.0f; ztop = 2.0f; zbelow = 0; }
        }
        u32 jmin = 0;
        for (u32 k = nc; k-- > 0; ) {
            if (C[k].g < jmin) continue;
            if (rqh_over(b, C[k].b) && !(add && C[k].add)) { jmin = C[k].g; }
        }
        if (split_set && jmin < split_g) jmin = split_g;        /* (PS2_RQ_HUD_CLSPLIT: no group across the clouds' point) */
        u32 g = ng;
        for (u32 k = jmin; k < ng; k++) if (g_key[k] == key) { g = k; break; }
        if (g == ng) {
            if (ng >= RQH_NG) { rqh.s_left[6]++; rqh.tk[j] = 0; if (rq_date_free()) rqdf_why[j] = 101u; if (nb < RQH_BARS) { memcpy(bars[nb], b, sizeof b); badd[nb++] = 0; } continue; }
            g_key[ng++] = key;
        }
        C[nc].j = j; C[nc].g = (u16)g; C[nc].add = (u8)add; C[nc].wa = (u8)wa; C[nc].key = key; memcpy(C[nc].b, b, sizeof b); C[nc].dfirst = d_first; C[nc].dcount = d_n; C[nc].m3 = m3; C[nc].zc = zc; C[nc].ztop = ztop; C[nc].zbelow = zbelow;
        if (sc_need) rqh_sc_eye(e, C[nc].sc); else memset(C[nc].sc, 0, sizeof C[nc].sc);
        if (++nc >= 8192u) break;
    }
    if (!nc) return 0;
    /* the vertices and rows, group by group */
    rqh_vtx *V = (rqh_vtx *)rqh.vp[fi];
    rqh_row *R = (rqh_row *)rqh.sp[fi];
    rqh_vtx *const Vm = V; rqh_row *const Rm = R;
    if (rqmh_now) {           /* (PS2_RQ_MAP_HOST: built at the same places in host arrays, then one copy each; the depth clamp below read V[q].z
                                 back from the uncached vertex buffer, and the rows' fields went out one store at a time) */
        if (!rqh_hv_dbg) rqh_hv_dbg = (rqh_vtx *)malloc((size_t)RQH_MAX_V * sizeof *rqh_hv_dbg);
        if (!rqh_hr_dbg) rqh_hr_dbg = (rqh_row *)malloc((size_t)RQH_MAX_D * sizeof *rqh_hr_dbg);
        if (rqh_hv_dbg && rqh_hr_dbg) { V = rqh_hv_dbg; R = rqh_hr_dbg; rqh_host_fid = rqmh_fid; rqh_host_fi = fi; }
    }
    u32 nv = rqh.nv, nr = rqh.nr;
    const u32 nv0 = nv, nr0 = nr;
    struct { u32 first, n; u32 key; } gr[RQH_MAX_G];
    memset(gr, 0, sizeof gr);
    for (u32 g = 0; g < ng; g++) {
        gr[g].first = nv; gr[g].key = g_key[g];
        for (u32 c = 0; c < nc; c++) {
            if (C[c].g != g) continue;
            const vk_draw *e = &rep_draws[C[c].j];
            u32 nvert = e->topology == PS2_VK_LINES ? (e->count / 2u) * 6u : (e->count / 3u) * 3u;
            if (nr >= RQH_MAX_D || nv + nvert > RQH_MAX_V) { rqh.s_left[6]++; rqh.tk[C[c].j] = 0; if (rq_date_free()) rqdf_why[C[c].j] = 102u; continue; }
            /* the row */
            rqh_row *w = &R[nr];
            VkDescriptorSet set = white_set;
            int layered = 0;
            if (e->tex_rt && e->tex_rt <= RT_SLOTS) { set = rts[e->tex_rt - 1u].set; layered = vk_mv && !rts[e->tex_rt - 1u].mono; }
            else if (e->tex != 0xFFFFu && e->tex < ntextures && textures[e->tex].set) set = textures[e->tex].set;
            u32 slot = rqh_tex_slot(fi, set);
            if (slot == 0xFFFFFFFFu) { rqh.s_left[6]++; rqh.tk[C[c].j] = 0; if (rq_date_free()) rqdf_why[C[c].j] = 103u; continue; }
            u32 sf = 0, df = 0;
            (void)rqh_blend(e->blend, &sf, &df);
            u32 wa = ((e->depth >> 4) & 1u) && !((e->depth >> 8) & 1u);
            u32 textured = (e->tex != 0xFFFFu || e->tex_rt) ? 1u : 0u;
            w->flags = e->frag_flags & ~(1u << 22);    /* (DATE decided here or by the rectangles) */
            w->dfirst = C[c].dcount ? C[c].dfirst : 0u; w->dcount = C[c].dcount;
            w->rep = (!e->tex_rt && e->tex != 0xFFFFu && e->tex < ntextures && textures[e->tex].scale > 1u) ? (float)textures[e->tex].scale : 1.0f;
            w->pad = rqfx_row(e, rqh.cl_pt < 0 || (split_set && g >= split_g));   /* (PS2_RQ_CLOUDS_FXFADE: the effect's fade mode, drawn after our clouds; rq_fxfade.inc) */
            w->wrap = e->wrap;
            w->mode = sf | (df << 3) | (wa << 6) | ((u32)layered << 7) | (textured << 8);
            w->tex = slot;
            {   float fx = (float)e->fix / 128.0f;
                w->k[0] = rqh_code_k0(sf, fx); w->k[1] = rqh_code_k1(sf); w->k[2] = rqh_code_k0(df, fx); w->k[3] = rqh_code_k1(df);
                {   float tw_ = e->tex_w > 0.0f ? e->tex_w : 1.0f, th_ = e->tex_h > 0.0f ? e->tex_h : 1.0f;
                    u32 mu_ = e->wrap & 3u, mv_ = (e->wrap >> 2) & 3u;
                    w->cl[0] = mu_ == 1u ? 0.0f : mu_ >= 2u ? (float)e->minu : -1.0f; w->cl[2] = mu_ == 1u ? tw_ - 1.0f : mu_ == 2u ? (float)(e->maxu > e->minu ? e->maxu : e->minu) : mu_ == 3u ? (float)e->maxu : tw_;     /* (repeat: x < 0, the size in z; region repeat: UMSK, UFIX) */
                    w->cl[1] = mv_ == 1u ? 0.0f : mv_ >= 2u ? (float)e->minv : -1.0f; w->cl[3] = mv_ == 1u ? th_ - 1.0f : mv_ == 2u ? (float)(e->maxv > e->minv ? e->maxv : e->minv) : mv_ == 3u ? (float)e->maxv : th_;
                }
                memcpy(w->sc, C[c].sc, sizeof w->sc);
                w->m[0] = (float)wa; w->m[1] = ((e->frag_flags >> 17) & 1u) ? 1.0f : 0.0f; w->m[2] = ((e->frag_flags >> 21) & 1u) ? 1.0f : 0.0f; w->m[3] = C[c].m3;
            }
            w->cu[0] = e->minu; w->cu[1] = e->maxu; w->cu[2] = e->minv; w->cu[3] = e->maxv;
            w->ts[0] = e->tex_w > 0.0f ? e->tex_w : 1.0f; w->ts[1] = e->tex_h > 0.0f ? e->tex_h : 1.0f;
            if (e->tex_rt) { int nrm = e->tex_rt <= RT_SLOTS && rtz_norm(e); w->is[0] = (float)(nrm ? tew(&rts[e->tex_rt - 1u]) : rt_w); w->is[1] = (float)(nrm ? teh(&rts[e->tex_rt - 1u]) : rt_h); }
            else if (e->tex != 0xFFFFu && e->tex < ntextures) { w->is[0] = (float)textures[e->tex].w; w->is[1] = (float)textures[e->tex].h; }
            else w->is[0] = w->is[1] = 1.0f;
            w->fog[0] = (float)((e->fogcol >> 0) & 0xFF) / 255.0f; w->fog[1] = (float)((e->fogcol >> 8) & 0xFF) / 255.0f; w->fog[2] = (float)((e->fogcol >> 16) & 0xFF) / 255.0f;
            w->fog[3] = (float)e->fix / 128.0f;        /* (FIX: the blend's constant) */
            /* the vertices */
            const ps2_vk_vertex *v = &rep_verts[e->first];
            u32 nv0 = nv;
            if (e->topology == PS2_VK_LINES) {
                for (u32 l = 0; l + 1u < e->count; l += 2u) {
                    rqh_emit(&V[nv++], &v[l], nr, &v[l + 1], 1.0f);    /* a+ */
                    rqh_emit(&V[nv++], &v[l], nr, &v[l + 1], -1.0f);   /* a- */
                    rqh_emit(&V[nv++], &v[l + 1], nr, &v[l], 1.0f);    /* b- (the other end's normal is opposite) */
                    rqh_emit(&V[nv++], &v[l], nr, &v[l + 1], 1.0f);    /* a+ */
                    rqh_emit(&V[nv++], &v[l + 1], nr, &v[l], 1.0f);    /* b- */
                    rqh_emit(&V[nv++], &v[l + 1], nr, &v[l], -1.0f);   /* b+ */
                }
            } else {
                for (u32 t = 0; t + 2u < e->count; t += 3u) {
                    rqh_emit(&V[nv++], &v[t], nr, NULL, 0.0f);
                    rqh_emit(&V[nv++], &v[t + 1], nr, NULL, 0.0f);
                    rqh_emit(&V[nv++], &v[t + 2], nr, NULL, 0.0f);
                }
            }
            if (C[c].zc >= 0.0f || C[c].ztop < 1.5f || C[c].zbelow) {
                for (u32 q = nv0; q < nv; q++) {
                    float z_ = C[c].zc >= 0.0f ? C[c].zc : C[c].zbelow ? nextafterf(V[q].z, 0.0f) : V[q].z;
                    V[q].z = z_ < C[c].ztop ? z_ : C[c].ztop;
                }
            }
            nr++;
            rqh.s_draws++; if (C[c].key & 16u) rqh.s_plain++;
        }
        gr[g].n = nv - gr[g].first;
    }
    if (V != Vm) { memcpy(Vm + nv0, V + nv0, (size_t)(nv - nv0) * sizeof *V); memcpy(Rm + nr0, R + nr0, (size_t)(nr - nr0) * sizeof *R); }
    rqh.nv = nv; rqh.nr = nr;
    rqfx_batch_n(nv);                                           /* (PS2_RQ_CLOUDS_FXFADE: the batch's vertices for the fade's dispatch) */
    /* the draws */
    VkViewport vp; memset(&vp, 0, sizeof vp); vp.width = (float)rqe.w; vp.height = (float)rqe.h; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc; memset(&sc, 0, sizeof sc); sc.offset.x = (int32_t)rqd.vx; sc.offset.y = (int32_t)rqd.vy; sc.extent.width = rqd.vw ? rqd.vw : rqe.w; sc.extent.height = rqd.vh ? rqd.vh : rqe.h;   /* (PS2_RQ_DYNRES: the frame's rectangle) */
    vkCmdSetScissor(cmd, 0, 1, &sc);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &rqh.vb[fi], &off);
    VkDescriptorSet sets[2] = { rqh.tset[fi], rqh.bset[fi] };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqh.lo, 0, 2, sets, 0, NULL);
    struct { float rqx[4]; float rqy[2]; float rqsz[2]; float inv[2]; float halfw; u32 flags; } pc;
    memcpy(pc.rqx, rqe.xf, sizeof pc.rqx); pc.rqy[0] = rqe.xf[4]; pc.rqy[1] = rqe.xf[5]; pc.rqsz[0] = (float)rqe.w; pc.rqsz[1] = (float)rqe.h;
    pc.inv[0] = 2.0f / (float)rt_w; pc.inv[1] = 2.0f / (float)rt_h;
    float lw = rqe.pxs < max_line_width ? rqe.pxs : max_line_width;
    pc.halfw = 0.5f * lw; pc.flags = (mv_shift ? 1u : 0u) | (rq_date_free() ? 2u : 0u);   /* (bit 1, PS2_RQ_DATE_FREE: positions from the vertices) */
    vkCmdPushConstants(cmd, rqh.lo, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    VkPipeline bound = VK_NULL_HANDLE;
    u32 drawn = 0, ng_now = split_set && split_g < ng ? split_g : ng;
    for (u32 g = 0; g < ng_now; g++) {
        if (!gr[g].n) continue;
        VkPipeline p = rqh_pipe(gr[g].key);
        if (!p) continue;
        if (p != bound) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p); bound = p; }
        vkCmdDraw(cmd, gr[g].n, 1, gr[g].first, 0);
        drawn++;
    }
    rqh.s_groups += drawn; rqh.s_verts += nv - gr[0].first; rqh.s_cand += nc; if (mode == 0) rqh.s_lists++; else rqh.s_fxlists++;
    if (ng_now < ng) {                                          /* (PS2_RQ_HUD_CLSPLIT: the rest when the loop has drawn the clouds) */
        rqh.pend = 1; rqh.pend_fi = fi; rqh.pend_g0 = ng_now; rqh.pend_ng = ng; rqh.s_split++;
        for (u32 g = ng_now; g < ng; g++) { rqh.pend_first[g] = gr[g].first; rqh.pend_n[g] = gr[g].n; rqh.pend_key[g] = gr[g].key; }
        memcpy(&rqh.pend_pc, &pc, sizeof rqh.pend_pc);
    }
    if (xr_dump_now && getenv("PS2_RQ_LIST")) {
        char t[256]; int o = 0;
        for (u32 g = 0; g < ng && o < 200; g++) {
            u8 seen[RQH_TEXS]; memset(seen, 0, sizeof seen); u32 nu = 0;
            for (u32 c = 0; c < nc; c++) { if (C[c].g != g) continue; const vk_draw *ee = &rep_draws[C[c].j]; if (ee->tex == 0xFFFFu && !ee->tex_rt) continue; VkDescriptorSet s_ = ee->tex_rt && ee->tex_rt <= RT_SLOTS ? rts[ee->tex_rt - 1u].set : ee->tex < ntextures ? textures[ee->tex].set : VK_NULL_HANDLE; u32 sl = s_ ? rqh_tex_slot(fi, s_) : 0xFFFFFFFFu; if (sl < RQH_TEXS && !seen[sl]) { seen[sl] = 1; nu++; } }
            o += snprintf(t + o, sizeof t - (size_t)o, " [key %u: %u v %u tex]", gr[g].key, gr[g].n, nu);
        }
        ps2_log("rq-hud-flush: mode %d field %u list of %u draws from %u: taken %u, %u vertices, %u groups:%s; barriers %u, writers %u, rects %u, textures %u; clouds' point %d, groups after it %u", mode, ps2_gs_frame_count, rep_ndraws, from_ii, nc, nv - gr[0].first, ng, t, nb, nw, rqh.nrect, rqh.ntex, split_ii == ~0u ? -1 : (int)split_ii, ng - ng_now);
    }
    if (mode == 0) rqh.flushed = 1;
    if (mode == 0 && !(rqh.s_lists % 300u)) {
        double n = (double)rqh.s_lists;
        ps2_log("rq-hud: not plain (a list): analytic DATE %.1f, fog %.1f, marker %.1f, target texture %.1f, alpha test %.1f, no set %.1f, TFX %.1f, region wrap %.1f, image grid %.1f, mips %.1f, sprite rounding %.1f, out of range %.1f",
                (double)rqh_np[0] / n, (double)rqh_np[1] / n, (double)rqh_np[2] / n, (double)rqh_np[3] / n, (double)rqh_np[4] / n, (double)rqh_np[5] / n, (double)rqh_np[6] / n, (double)rqh_np[7] / n, (double)rqh_np[8] / n, (double)rqh_np[9] / n, (double)rqh_np[10] / n, (double)rqh_np[11] / n);
        ps2_log("rq-hud: %llu lists: %.1f draws taken (%.1f plain), %.1f draws issued, %.1f vertices a list; left in place: state %.2f, topology %.2f, shader %.2f, depth key %.2f, blend %.2f, room %.2f, DATE unknown %.2f, scissor %.2f, overlap %.2f; DATE pass %.2f fail %.2f unknown %.2f; fades skipped %.2f; runs split at the clouds' point %.3f",
                (unsigned long long)rqh.s_lists, (double)rqh.s_draws / n, (double)rqh.s_plain / n, (double)rqh.s_groups / n, (double)rqh.s_verts / n,
                (double)rqh.s_left[1] / n, (double)rqh.s_left[2] / n, (double)rqh.s_left[3] / n, (double)rqh.s_left[4] / n, (double)rqh.s_left[5] / n, (double)rqh.s_left[6] / n, (double)rqh.s_left[7] / n, (double)rqh.s_left[8] / n, (double)rqh.s_left[9] / n,
                (double)rqh.s_date[0] / n, (double)rqh.s_date[1] / n, (double)rqh.s_date[2] / n, (double)rqh.s_fade / n, (double)rqh.s_split / n);
    }
    return 1;
}
/* PS2_RQ_HUD_CLSPLIT: the batched run's groups after the clouds' point, drawn right after the clouds (or at the pass's tail).
   The loop's bound state is stale after it */
static int rqh_flush_rest(VkCommandBuffer cmd) {
    if (!rqh.pend) return 0;
    rqh.pend = 0;
    u32 fi = rqh.pend_fi;
    VkViewport vp; memset(&vp, 0, sizeof vp); vp.width = (float)rqe.w; vp.height = (float)rqe.h; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc; memset(&sc, 0, sizeof sc); sc.offset.x = (int32_t)rqd.vx; sc.offset.y = (int32_t)rqd.vy; sc.extent.width = rqd.vw ? rqd.vw : rqe.w; sc.extent.height = rqd.vh ? rqd.vh : rqe.h;
    vkCmdSetScissor(cmd, 0, 1, &sc);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &rqh.vb[fi], &off);
    VkDescriptorSet sets[2] = { rqh.tset[fi], rqh.bset[fi] };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqh.lo, 0, 2, sets, 0, NULL);
    vkCmdPushConstants(cmd, rqh.lo, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof rqh.pend_pc, &rqh.pend_pc);
    VkPipeline bound = VK_NULL_HANDLE;
    u32 drawn = 0;
    for (u32 g = rqh.pend_g0; g < rqh.pend_ng; g++) {
        if (!rqh.pend_n[g]) continue;
        VkPipeline p = rqh_pipe(rqh.pend_key[g]);
        if (!p) continue;
        if (p != bound) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p); bound = p; }
        vkCmdDraw(cmd, rqh.pend_n[g], 1, rqh.pend_first[g], 0);
        drawn++;
    }
    rqh.s_groups += drawn;
    return 1;
}
/* PS2_RQ_HUD_LAST: this draw starts the HUD: the loop draws what is left of the clouds and the cockpit and the glass first */
static int rqh_last_due(u32 ii, const vk_draw *d) {
    int v = rqh_last_cfg();
    if (!v || !rqh.list_on || rqh.flushed || ii != rqh.hud_ii || d->rt != 0u || d->xcls != 2u) return 0;
    return v == 1 || (rqh.alt_n & 1u);
}
/* the draw loop's one test: 0 the draw goes the normal path, 1 taken already, 2 taken by the flush made here (the
   loop's bound state is stale). Called for draws of the open eye pass */
static void rqh_arm(void) {                              /* (the list's arm, at the pass's first draw: PS2_RQ_ABSKIP's block counter moves when the pass begins) */
    rqh.armed = 0;
    rqh.list_on = rqh_cfg() == 1 ? 1 : rqh_cfg() == 2 ? (int)(rqh.alt_n & 1u) : rqab_active();
}
static int rq_hud_at(VkCommandBuffer cmd, vk_frame *fr, const u32 *order, u32 ii, u32 i, const vk_draw *d, int cpit_done, int cpit_after) {
    if (rqh.armed) rqh_arm();
    if (!rqh.list_on || !rqh.ok) return 0;
    if (rqh.tk[i] == rqh.stamp) return 1;
    if (rqh.fx_on && ii < rqh.hud_ii && ii >= rqh.fx_until && d->rt == 0u && !d->native && d->xcls == 1u && !((d->depth >> 2) & 1u)) {
        /* the world's effects: a run of GS draws without a depth write that nothing else interrupts, batched where it stands */
        u32 q = ii;
        for (; q < rep_ndraws && q < rqh.hud_ii; q++) {
            const vk_draw *e = &rep_draws[order ? order[q] : q];
            if (e->rt != 0u || e->native || e->xcls != 1u || ((e->depth >> 2) & 1u)) break;
        }
        rqh.fx_until = q;
        if (q - ii < 2u) return 0;                       /* (a single draw: nothing to gain) */
        if (!rq_hud_flush(cmd, fr, order, ii, q, 1, cpit_done, cpit_after, rqh_split_at(ii, q))) return 0;
        return rqh.tk[i] == rqh.stamp ? 2 : 3;
    }
    if (d->xcls != 2u || d->rt != 0u || rqh.flushed) return 0;
    u32 why;
    if (!rqh_ok(d, &why)) return 0;
    if (!rq_hud_flush(cmd, fr, order, ii, ~0u, 0, cpit_done, cpit_after, rqh_split_at(ii, ~0u))) { rqh.flushed = 1; return 0; }
    if (rqh.tk[i] == rqh.stamp) return 2;
    return 3;      /* (flushed, this one stays: the bound state is stale all the same) */
}
/* PS2_RQ_LIST: the slot-0 list's shape in a dumped frame: runs of N native, Z GS depth-writer, F GS class 1 (no z write), S sun 5, H HUD class 2, 3 class 3, o the rest */
static void rqh_shape(const u32 *order) {
    char t[1800]; int o = 0; char last = 0; u32 run = 0;
    for (u32 q = 0; q <= rep_ndraws && o < (int)sizeof t - 24; q++) {
        char c = 0;
        if (q < rep_ndraws) {
            const vk_draw *e = &rep_draws[order ? order[q] : q];
            if (e->rt != 0u) continue;
            c = e->native ? 'N' : ((e->depth >> 2) & 1u) && e->xcls != 2u ? 'Z' : e->xcls == 1u ? 'F' : e->xcls == 5u ? 'S' : e->xcls == 2u ? 'H' : e->xcls == 3u ? '3' : 'o';
        }
        if (c != last) { if (run) o += snprintf(t + o, sizeof t - (size_t)o, " %c%u", last, run); last = c; run = 0; }
        run++;
    }
    ps2_log("rq-hud-shape: %s", t);
}
/* PS2_RQ_SKY=1 (default off): rq_gs_sky.frag for the opaque flat draws; PS2_RQ_SKY=alt: every second list */
static int rqy_cfg(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_SKY"); v = !e ? 0 : *e == '1' ? 1 : !strcmp(e, "alt") ? 2 : 0; if (v) ps2_log("vk: PS2_RQ_SKY=%s: the sky through its own shader", e); }
    return v == 1 || (v == 2 && (rqh.alt_n & 1u));
}
/* PS2_RQ_SKYQUAD=1 (default off, needs PS2_RQ_ORDER): the background quad under the dome left out when this list has the dome (their samples are equal in the census); =alt every second list */
static int rqh_quad_skip(const vk_draw *d) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_SKYQUAD"); v = !e ? 0 : *e == '1' ? 1 : !strcmp(e, "alt") ? 2 : 0; if (v) ps2_log("vk: PS2_RQ_SKYQUAD=%s", e); }
    if (!v || d->count != 6u || d->native || d->tex != 0xFFFFu || d->tex_rt || !rqh.dome) return 0;
    return v == 1 || (v == 2 && (rqh.alt_n & 1u));
}
/* PS2_RQ_LIST: the off-screen runs of a dumped list, in the order drawn: target, size, draws, what they read, whether a draw copies a whole target */
static void rqh_passlog(const u32 *order) {
    u32 q = 0, nrun = 0, nd = 0;
    double px = 0.0;
    while (q < rep_ndraws) {
        const vk_draw *e = &rep_draws[order ? order[q] : q];
        u32 r = 0, a = q, ncopy = 0, nblend = 0, nz = 0, trd = 0; u64 rd = 0;
        while (q < rep_ndraws) {
            const vk_draw *f = &rep_draws[order ? order[q] : q];
            if (f->rt != e->rt || f->zrt != e->zrt) break;
            r += f->count;
            if (f->blend & 0x100u) nblend++;
            if ((f->depth >> 2) & 1u) nz++;
            if (f->tex_rt && f->tex_rt <= RT_SLOTS) { trd++; rd |= 1ull << (f->tex_rt - 1u); }
            if (!f->native && !(f->blend & 0x100u) && f->tex_rt && f->count == 6u) ncopy++;
            q++;
        }
        if (e->rt == 0u) { ps2_log("rq-pass: [%u] slot 0: %u draws", a, q - a); continue; }
        if (e->rt >= RT_SLOTS) continue;
        vk_target *t = &rts[e->rt];
        u32 w = tew(t) * tsc(t), h = teh(t) * tsc(t);
        ps2_log("rq-pass: [%u] rt%u %ux%u%s z rt%u: %u draws, %u verts, blended %u, z writes %u, copies %u, reads rts 0x%llX (%u draws); first: blend 0x%X frag 0x%08X tex %d texrt %u%s sc %d..%d,%d..%d", a, e->rt, w, h, t->mono ? " mono" : "", e->zrt, q - a, r, nblend, nz, ncopy, (unsigned long long)rd, trd, e->blend, e->frag_flags, (int)(s16)e->tex, e->tex_rt, e->tex_depth ? " depth" : "", e->scissor[0], e->scissor[1], e->scissor[2], e->scissor[3]);
        for (u32 z = a; z < q && z < a + 3u; z++) {           /* (the first draws' rectangles: what they write and what they read) */
            const vk_draw *g = &rep_draws[order ? order[z] : z];
            if (g->native || !g->count || g->first + g->count > rep_nverts) continue;
            float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, s0 = 1e9f, s1 = -1e9f, t0 = 1e9f, t1 = -1e9f;
            int fst = (g->frag_flags >> 2) & 1u;
            for (u32 v = 0; v < g->count; v++) { const ps2_vk_vertex *V = &rep_verts[g->first + v]; float qq = V->q > 1e-9f ? V->q : 1e-9f; float S = fst ? V->s : V->s / qq * g->tex_w, T = fst ? V->t : V->t / qq * g->tex_h;
                if (V->x < x0) x0 = V->x; if (V->x > x1) x1 = V->x; if (V->y < y0) y0 = V->y; if (V->y > y1) y1 = V->y; if (S < s0) s0 = S; if (S > s1) s1 = S; if (T < t0) t0 = T; if (T > t1) t1 = T; }
            ps2_log("rq-pass:     draw %u n %u rect %.1f..%.1f x %.1f..%.1f uv %.2f..%.2f x %.2f..%.2f rgba %.0f,%.0f,%.0f,%.0f depth 0x%X blend 0x%X frag 0x%08X fix %u tex_size %.0f,%.0f wrap %X", z, g->count, (double)x0, (double)x1, (double)y0, (double)y1, (double)s0, (double)s1, (double)t0, (double)t1, (double)rep_verts[g->first].r, (double)rep_verts[g->first].g, (double)rep_verts[g->first].b, (double)rep_verts[g->first].a, g->depth, g->blend, g->frag_flags, g->fix, (double)g->tex_w, (double)g->tex_h, g->wrap);
        }
        nrun++; nd += q - a; px += (double)w * (double)h;
    }
    ps2_log("rq-pass: %u off-screen runs, %u draws, %.3f Mpx of targets", nrun, nd, px / 1e6);
}
/* a new list */
static void rq_hud_begin(vk_frame *fr, const u32 *order) {
    rqh.alt_n++;
    rqh.list_on = 0; rqh.armed = 0;
    rqh_fade_a0 = 0;
    if (!rqh_cfg() || xr_rp_on || !rqh_init()) return;   /* (the in-between frames' reprojection is not in the batch) */
    rqh.armed = 1;
    if (++rqh.stamp == 0u) { memset(rqh.tk, 0, VK_MAX_DRAWS * sizeof(u32)); rqh.stamp = 1; }
    rqh.flushed = 0; rqh.fx_until = 0; rqh.nv = 0; rqh.nr = 0; rqh.nrect = 0; rqh.ncp = 0; rqh.ntex = 0; rqh.frozen = 0;
    rqh.hud_ii = ~0u; rqh.dome = 0; rqh.pend = 0; rqh.cl_pt = -1;
    for (u32 q = 0; q < rep_ndraws; q++) {                 /* (the dome in this list: PS2_RQ_SKYQUAD) */
        const vk_draw *e = &rep_draws[order ? order[q] : q];
        if (e->rt == 0u && !e->native && e->xcls == 1u && e->tex == 0xFFFFu && !e->tex_rt && e->count >= 200u && ((e->depth >> 2) & 1u)) { rqh.dome = 1; break; }
    }
    u32 fi = (u32)(fr - frames);
    if (fi >= FRAMES_IN_FLIGHT) { rqh.armed = 0; return; }
    for (u32 q = 0; q < rep_ndraws; q++) {                /* (the first class-2 draw: the effects' runs stop there, the HUD's flush takes over) */
        const vk_draw *e = &rep_draws[order ? order[q] : q];
        if (e->rt == 0u && e->xcls == 2u && !e->native) { rqh.hud_ii = q; break; }
    }
    {   static int fx = -1;
        if (fx < 0) { const char *e = getenv("PS2_RQ_HUD_FX"); fx = !(e && *e == '0'); }
        rqh.fx_on = fx;
    }
    /* the texture table, once, before anything of this frame binds it */
    (void)rqh_tex_slot(fi, white_set);
    for (u32 q = 0; q < rep_ndraws; q++) {
        const vk_draw *e = &rep_draws[order ? order[q] : q];
        if (e->rt != 0u || e->native || (e->xcls != 1u && e->xcls != 2u)) continue;
        VkDescriptorSet set = white_set;
        if (e->tex_rt && e->tex_rt <= RT_SLOTS) set = rts[e->tex_rt - 1u].set;
        else if (e->tex != 0xFFFFu && e->tex < ntextures && textures[e->tex].set) set = textures[e->tex].set;
        if (set) (void)rqh_tex_slot(fi, set);
    }
    if (rqh.ncp) vkUpdateDescriptorSets(dev, 0, NULL, rqh.ncp, rqh.cp);
    rqh.ncp = 0; rqh.frozen = 1;
}
/* Class 3's fade quad with alpha 0 changes nothing in the colour (exact); its write of the alpha channel (0 over the whole
   frame) is done by rq_ds on the feed instead (rqh_fade_a0). */
static int rqh_fade_skip(const vk_draw *d) {
    if (!rqh.list_on || d->rt != 0u || d->xcls != 3u || d->native || d->tex != 0xFFFFu || d->tex_rt || d->date || !(d->blend & 0x100u)) return 0;
    if (((d->blend >> 4) & 3u) != 0u || ((d->blend >> 6) & 3u) != 1u) return 0;   /* (C = As, D = Cd) */
    if (d->first + d->count > rep_nverts || !d->count) return 0;
    for (u32 q = 0; q < d->count; q++) if (rep_verts[d->first + q].a != 0.0f) return 0;
    if (((d->depth >> 4) & 1u) && !((d->depth >> 8) & 1u)) rqh_fade_a0 = 1;
    rqh.s_fade++;
    return 1;
}

/* the eye pass's draws and pipeline binds a list (R-21 / plan 4.4's budget): the old loop's counters between the pass's begin and end, plus the batch's and layers' */
static void rqh_count(int end) {
    static struct { u64 draw, pipe, desc, push, scis, mesh, subs, groups, cl, cp; } s0;
    if (!end) {
        s0.draw = rl_stat.draw; s0.pipe = rl_stat.pipe; s0.desc = rl_stat.desc; s0.push = rl_stat.push; s0.scis = rl_stat.scis;
        s0.mesh = stat_mesh_draws; s0.subs = rnb.c_subs; s0.groups = rqh.s_groups; s0.cl = rql.n_cl; s0.cp = rql.n_cp;
        return;
    }
    if (rl_stat.draw < s0.draw) return;                  /* (the periodic reset of rl_stat in between) */
    rqh.cnt.draw += rl_stat.draw - s0.draw; rqh.cnt.pipe += rl_stat.pipe - s0.pipe; rqh.cnt.desc += rl_stat.desc - s0.desc;
    rqh.cnt.push += rl_stat.push - s0.push; rqh.cnt.scis += rl_stat.scis - s0.scis;
    rqh.cnt.mesh += (stat_mesh_draws - s0.mesh) - (rnb.c_subs - s0.subs);
    rqh.cnt.groups += rqh.s_groups - s0.groups; rqh.cnt.cl += rql.n_cl - s0.cl; rqh.cnt.cp += rql.n_cp - s0.cp;
    if (!(++rqh.cnt.frames % 300u)) {
        double n = (double)rqh.cnt.frames;
        double dr = (double)(rqh.cnt.draw + rqh.cnt.mesh + rqh.cnt.groups + rqh.cnt.cl + rqh.cnt.cp) / n;
        ps2_log("rq-count: eye pass, per list: %.1f draws (GS %.1f, native %.1f, HUD batch %.1f, layers %.1f), %.1f pipeline binds, %.1f descriptor binds, %.1f pushes, %.1f scissors",
                dr, (double)rqh.cnt.draw / n, (double)rqh.cnt.mesh / n, (double)rqh.cnt.groups / n, (double)(rqh.cnt.cl + rqh.cnt.cp) / n,
                (double)rqh.cnt.pipe / n + (double)rqh.cnt.groups / n, (double)rqh.cnt.desc / n, (double)rqh.cnt.push / n, (double)rqh.cnt.scis / n);
    }
}

/* PS2_RQ_C1CENSUS=1 [test]: the samples each class-1 GS draw of a dumped frame really leaves (occlusion queries, per view; the effects' batch
   off with PS2_RQ_HUD_FX=0 to see them apart): which draws carry class 1's fragments. Logged at the next frame (rq-c1:). */
#define RQC_MAX 768u
static struct { int cfg, armed, pending; VkQueryPool pool; u32 n, cur; u32 ok; u32 id[RQC_MAX]; u32 cnt[RQC_MAX]; u32 blend[RQC_MAX], depth[RQC_MAX], frag[RQC_MAX]; s32 tex[RQC_MAX]; u16 kind[RQC_MAX]; float W, H; } rqc1;
static int rqc1_cfg(void) { if (!rqc1.cfg) { const char *e = getenv("PS2_RQ_C1CENSUS"); rqc1.cfg = e && *e == '1' ? 2 : 1; } return rqc1.cfg == 2; }
static void rqc1_reset(VkCommandBuffer cmd) {
    if (!rqc1_cfg() || !xr_dump_now || rqc1.pending) return;
    if (!rqc1.pool) {
        VkQueryPoolCreateInfo qi; memset(&qi, 0, sizeof qi); qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO; qi.queryType = VK_QUERY_TYPE_OCCLUSION; qi.queryCount = RQC_MAX * 2u;
        if (vkCreateQueryPool(dev, &qi, NULL, &rqc1.pool) != VK_SUCCESS) { rqc1.cfg = 1; return; }
    }
    vkCmdResetQueryPool(cmd, rqc1.pool, 0, RQC_MAX * 2u);
    rqc1.n = 0; rqc1.armed = 1;
}
static void rqc1_begin(VkCommandBuffer cmd, const vk_draw *d, u32 i) {
    if (!rqc1.armed || rqc1.n >= RQC_MAX) return;
    u32 k = rqc1.n;
    rqc1.id[k] = i; rqc1.cnt[k] = d->count; rqc1.blend[k] = d->blend; rqc1.depth[k] = d->depth; rqc1.frag[k] = d->frag_flags; rqc1.tex[k] = d->tex_rt ? -(s32)d->tex_rt : (s32)(s16)d->tex;
    rqc1.kind[k] = d->date ? 2u : (d->tex == 0xFFFFu && !d->tex_rt) ? 1u : 0u;
    vkCmdBeginQuery(cmd, rqc1.pool, 2u * k, 0);
}
static void rqc1_end(VkCommandBuffer cmd) { if (rqc1.armed && rqc1.n < RQC_MAX) { vkCmdEndQuery(cmd, rqc1.pool, 2u * rqc1.n); rqc1.n++; } }
static void rqc1_read(void) {
    if (!rqc1.pending) return;
    rqc1.pending = 0;
    static u64 res[RQC_MAX * 2u];
    if (!rqc1.n || vkGetQueryPoolResults(dev, rqc1.pool, 0, rqc1.n * 2u, sizeof res, res, sizeof(u64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) return;
    double tot = 0.0, by[3] = { 0.0, 0.0, 0.0 }, scr = 4.0 * 2.0 * (double)rqe.w * (double)rqe.h;
    for (u32 k = 0; k < rqc1.n; k++) { double v = (double)res[2u * k] + (double)res[2u * k + 1u]; tot += v; by[rqc1.kind[k]] += v; }
    ps2_log("rq-c1: %u class-1 GS draws: %.3f screens in all (lean %.3f, flat %.3f, DATE %.3f; both eyes, 4 samples a pixel)", rqc1.n, tot / scr, by[0] / scr, by[1] / scr, by[2] / scr);
    for (u32 k = 0; k < rqc1.n; k++) {
        double v = (double)res[2u * k] + (double)res[2u * k + 1u];
        if (v / scr >= 0.005) ps2_log("rq-c1: draw %u n %u blend 0x%X depth 0x%X frag 0x%08X tex %d %s: %.3f screens", rqc1.id[k], rqc1.cnt[k], rqc1.blend[k], rqc1.depth[k], rqc1.frag[k], (int)rqc1.tex[k], rqc1.kind[k] == 1u ? "flat" : rqc1.kind[k] == 2u ? "DATE" : "lean", v / scr);
    }
}
static void rqc1_done(void) { if (rqc1.armed) { rqc1.armed = 0; rqc1.pending = 1; } }
