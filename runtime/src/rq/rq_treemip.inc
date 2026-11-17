/* rq_treemip.inc -- PS2_RQ_TREE_AA bit 16: the tree impostors' mip chains (rq14_small 2026-11-16, BUGS_RQ R-242; included at the end of
   rq_slots.inc, PS2_RQ_SUPPORT only).
   The device priced TREE_AA=11 (NOTES_RQ 2026-11-16 12:45): the look right, but where the cards fill the view (the M2 tape's map B) +0.2..0.6 ms
   p50 and twice the seconds under 60, the densest window +4 ms; `9` (the box read alone) dearer still. The cost is the box read: up to 4 x 4
   bilinear taps a fragment over cards 16-24 eye px wide, many cards a frame. Bit 16 moves the filtering out of the fragments:
   - where the eye pass begins (rq_eye_begin, after rq_close_others: every slot and snapshot final and SHADER_READ_ONLY), the list's slot-0 tree
     cards (rqo_tree_card) name the impostors they will read -- the live slot or the snapshot rq_snapd gives the draw, the region its CLAMP reads
     (texels 0..tex_w-1 x 0..tex_h-1 of the slot: the game's texture origin is the slot's), powers of two 8..512, a slot at its own scale 1;
   - one compute dispatch (rq_treemip.comp, a workgroup per drawn layer) copies each region premultiplied (rgb x a, a) into level 0 of an image of
     its own and averages it 2 x 2 down to 1 x 1 (8 levels for 128 x 128); per frame in flight RQTM_N images, made at first need;
   - a card that reads one of them binds its chain instead of the slot (a sampler with trilinear filtering, CLAMP_TO_EDGE: nothing of the slot
     round the region bleeds in), pushes the region's size as img_size and flags.w bit 2: rq_gs.frag -DTREE_AA takes ONE textureGrad with the
     texel coordinate's derivatives and divides the colour by its alpha -- the alpha-weighted colour and the mean alpha the box read gives, from
     one tap. The near cards keep the coverage mask (bit 2; with bit 8 in rq_gs_tree_sd.frag: no second blend source, the Adreno rule of
     R-241); a card whose impostor has no chain this list falls back to bit 1's box read when set, else to the game's read (counted).
   Modes: 26 = 16 + 8 + 2 (the chain, the Adreno-safe modules, coverage on the near path) -- the candidate; 24 = the chain with the game's
   discard; 27 = 26 with the box read as the fallback. Without bit 8 the near cards take rq_gs_tree.frag (mask + second source: invisible on
   the device).
   PS2_RQ_TREE_MIP_SHARP (default 0.7: the derivatives' scale -- 1 = the footprint's level, a shade softer than the box read; 0.7 = about half a
   level sharper, the box read's sharpness). Tests: PS2_RQ_TREE_MIP_ANISO (the
   sampler's anisotropy), PS2_RQ_TREE_AA_AB (rq_order.inc: another mode in PS2_RQ_ABSKIP's left-out blocks), PS2_RQ_TREE_REPEAT (every card
   drawn N times: its fragments' price magnified for the A/B; wrong image). Counters and the build's GPU time: `rq treemip:` every 1200 lists. */
#define RQTM_N 6u                      /* impostors a list (M2: slots 5 and 13 and their snapshots) */
#define RQTM_LV 10u                    /* levels: up to 512 x 512 (rq_treemip.comp's lv[10]) */
struct rqtm_e {
    VkImage img; VkDeviceMemory mem; VkImageView view, lv[RQTM_LV];
    VkDescriptorSet cset, gset;        /* the build's set (source + levels), the eye pass's set (the chain, trilinear) */
    u32 w, h, n;
    const vk_target *src;              /* this list's impostor (NULL: unused) */
    u32 built;                         /* the list it was built for (rqtm.list) */
};
static struct {
    int tried, ok;
    VkDescriptorSetLayout dsl; VkDescriptorPool pool; VkPipelineLayout lo; VkPipeline pipe; VkSampler smp; VkQueryPool qp;
    struct rqtm_e e[FRAMES_IN_FLIGHT][RQTM_N];
    u32 fi, list;
    int qpend[FRAMES_IN_FLIGHT];
    float sharp;
    u64 s_lists, s_built, s_cards, s_skip, s_full, s_cmip, s_cfb, s_ms_n, s_px;
    double s_ms, s_ms_max;
} rqtm;
static vk_frame *rqtm_fr;              /* (set where the eye pass begins: this list's frame) */
static struct rqtm_e *rqtm_cur;        /* (the draw being recorded: its impostor's chain, rqtm_pick) */
static int rqtm_card;                  /* (the draw being recorded is a tree card: PS2_RQ_TREE_REPEAT) */
static u32 rqtm_rep(void) {             /* PS2_RQ_TREE_REPEAT=N [test, wrong image]: every tree card drawn N times (its price x N for the A/B) */
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_TREE_REPEAT"); v = e ? atoi(e) : 0; if (v < 0) v = 0; if (v > 64) v = 64; if (v > 1) ps2_log("vk: PS2_RQ_TREE_REPEAT=%d [test, wrong image]: every tree card drawn %d times", v, v); }
    return (u32)v;
}
/* the impostor draw i reads (the live slot or its snapshot, as the draw loop's rset) and the region; NULL = not one for a chain */
static const vk_target *rqtm_src(const vk_draw *d, u32 i, u32 *w, u32 *h) {
    if (d->tex_rt < 2u || d->tex_rt > RT_SLOTS || d->tex_depth || (d->wrap & 15u) != 5u) return NULL;   /* (an off-screen slot, CLAMP both ways) */
    u32 tw = (u32)d->tex_w, th = (u32)d->tex_h, k = d->tex_rt - 1u;
    if ((float)tw != d->tex_w || (float)th != d->tex_h || tw < 8u || th < 8u || tw > 512u || th > 512u || (tw & (tw - 1u)) || (th & (th - 1u))) return NULL;
    const vk_target *s = rq_snap_mask && ((rq_snap_mask >> k) & 1u) && rq_snapd && rq_snapd[i] ? RQ_SNAP_OF(k, rq_snapd[i]) : &rts[k];
    if (!s->created || tsc(s) != 1u || tew(s) < tw || teh(s) < th) return NULL;
    *w = tw; *h = th;
    return s;
}
static int rqtm_make(void) {
    if (rqtm.ok) return 1;
    if (rqtm.tried) return 0;
    rqtm.tried = 1;
    if (!rqtm_cs) { ps2_log("rq treemip: rq_treemip.comp.spv missing: off"); return 0; }
    VkDescriptorSetLayoutBinding b[2]; memset(b, 0, sizeof b);
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; b[1].descriptorCount = RQTM_LV; b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo lc; memset(&lc, 0, sizeof lc);
    lc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; lc.bindingCount = 2; lc.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev, &lc, NULL, &rqtm.dsl) != VK_SUCCESS) return 0;
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2u * FRAMES_IN_FLIGHT * RQTM_N }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, RQTM_LV * FRAMES_IN_FLIGHT * RQTM_N } };
    VkDescriptorPoolCreateInfo pi; memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; pi.maxSets = 2u * FRAMES_IN_FLIGHT * RQTM_N; pi.poolSizeCount = 2; pi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(dev, &pi, NULL, &rqtm.pool) != VK_SUCCESS) return 0;
    VkPushConstantRange pr; memset(&pr, 0, sizeof pr); pr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; pr.size = 16;
    VkPipelineLayoutCreateInfo pl; memset(&pl, 0, sizeof pl);
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pl.setLayoutCount = 1; pl.pSetLayouts = &rqtm.dsl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev, &pl, NULL, &rqtm.lo) != VK_SUCCESS) return 0;
    VkComputePipelineCreateInfo cp; memset(&cp, 0, sizeof cp);
    cp.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO; cp.layout = rqtm.lo;
    cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cp.stage.module = rqtm_cs; cp.stage.pName = "main";
    if (vkCreateComputePipelines(dev, pipe_cache, 1, &cp, NULL, &rqtm.pipe) != VK_SUCCESS) { rqtm.pipe = VK_NULL_HANDLE; return 0; }
    VkSamplerCreateInfo sc; memset(&sc, 0, sizeof sc);
    sc.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO; sc.magFilter = VK_FILTER_LINEAR; sc.minFilter = VK_FILTER_LINEAR; sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; sc.maxLod = VK_LOD_CLAMP_NONE;
    {   const char *e = getenv("PS2_RQ_TREE_MIP_ANISO"); float a = e ? (float)atof(e) : 0.0f;
        if (a > max_aniso) a = max_aniso;
        if (a >= 2.0f) { sc.anisotropyEnable = VK_TRUE; sc.maxAnisotropy = a; } }
    if (vkCreateSampler(dev, &sc, NULL, &rqtm.smp) != VK_SUCCESS) return 0;
    if (gpu_timestamp_bits) {
        VkQueryPoolCreateInfo qi; memset(&qi, 0, sizeof qi);
        qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = 2u * FRAMES_IN_FLIGHT;
        if (vkCreateQueryPool(dev, &qi, NULL, &rqtm.qp) != VK_SUCCESS) rqtm.qp = VK_NULL_HANDLE;
    }
    {   const char *e = getenv("PS2_RQ_TREE_MIP_SHARP"); rqtm.sharp = e ? (float)atof(e) : 0.7f; if (!(rqtm.sharp > 0.05f && rqtm.sharp < 4.0f)) rqtm.sharp = 0.7f; }   /* (0.7: 11's sharpness, R-233 0.47 % against 11's 0.59; 1.0: 0.29 %, a shade softer) */
    rqtm.ok = 1;
    ps2_log("rq treemip: PS2_RQ_TREE_AA bit 16: the tree cards read a premultiplied mip chain of their impostor (one trilinear tap); derivatives x %.2f, anisotropy %.0f%s",
            (double)rqtm.sharp, sc.anisotropyEnable ? (double)sc.maxAnisotropy : 1.0, rqtm.qp ? "; the build's GPU time in `rq treemip:`" : "");
    return 1;
}
/* an entry's image, its views and sets, for a w x h region (once; the entry keeps its size) */
static int rqtm_mkimg(struct rqtm_e *e, u32 w, u32 h) {
    u32 n = 1u;
    while (n < RQTM_LV && ((w >> (n - 1u)) > 1u || (h >> (n - 1u)) > 1u)) n++;
    VkImageCreateInfo ii; memset(&ii, 0, sizeof ii);
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent.width = w; ii.extent.height = h; ii.extent.depth = 1; ii.mipLevels = n; ii.arrayLayers = rt_layers; ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT; ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &ii, NULL, &e->img) != VK_SUCCESS) { e->img = VK_NULL_HANDLE; return 0; }
    VkMemoryRequirements req; vkGetImageMemoryRequirements(dev, e->img, &req);
    VkMemoryAllocateInfo ai; memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = req.size; ai.memoryTypeIndex = find_mem(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(dev, &ai, NULL, &e->mem) != VK_SUCCESS || vkBindImageMemory(dev, e->img, e->mem, 0) != VK_SUCCESS) return 0;
    VkImageViewCreateInfo vi; memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = e->img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount = n; vi.subresourceRange.layerCount = rt_layers;
    if (vkCreateImageView(dev, &vi, NULL, &e->view) != VK_SUCCESS) return 0;
    for (u32 l = 0; l < n; l++) {
        vi.subresourceRange.baseMipLevel = l; vi.subresourceRange.levelCount = 1;
        if (vkCreateImageView(dev, &vi, NULL, &e->lv[l]) != VK_SUCCESS) return 0;
    }
    VkDescriptorSetLayout L[2] = { deslayout, rqtm.dsl };
    VkDescriptorSet S[2];
    VkDescriptorSetAllocateInfo sa; memset(&sa, 0, sizeof sa);
    sa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; sa.descriptorPool = rqtm.pool; sa.descriptorSetCount = 2; sa.pSetLayouts = L;
    if (vkAllocateDescriptorSets(dev, &sa, S) != VK_SUCCESS) return 0;
    e->gset = S[0]; e->cset = S[1];
    VkDescriptorImageInfo gi; memset(&gi, 0, sizeof gi);
    gi.sampler = rqtm.smp; gi.imageView = e->view; gi.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo li[RQTM_LV]; memset(li, 0, sizeof li);
    for (u32 l = 0; l < RQTM_LV; l++) { li[l].imageView = e->lv[l < n ? l : n - 1u]; li[l].imageLayout = VK_IMAGE_LAYOUT_GENERAL; }   /* (unused levels name the last one) */
    VkWriteDescriptorSet wr[2]; memset(wr, 0, sizeof wr);
    wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[0].dstSet = e->gset; wr[0].dstBinding = 0; wr[0].descriptorCount = 1; wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[0].pImageInfo = &gi;
    wr[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[1].dstSet = e->cset; wr[1].dstBinding = 1; wr[1].descriptorCount = RQTM_LV; wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; wr[1].pImageInfo = li;
    vkUpdateDescriptorSets(dev, 2, wr, 0, NULL);
    e->w = w; e->h = h; e->n = n;
    ps2_log("rq treemip: a %ux%u chain (%u levels, %u layers) made", w, h, n, rt_layers);
    return 1;
}
/* rq_eye_begin, after rq_close_others: this list's chains, one dispatch */
static void rqtm_build(VkCommandBuffer cmd) {
    if (!(rqtree_eff() & 16) || !rqtm_fr || rqtm_fr < frames || rqtm_fr >= frames + FRAMES_IN_FLIGHT || !rqtm_make()) return;
    u32 fi = (u32)(rqtm_fr - frames);
    rqtm.fi = fi; rqtm.list++; rqtm.s_lists++;
    if (rqtm.qp && rqtm.qpend[fi]) {                     /* (this frame slot's last build: its GPU time, done by now) */
        u64 ts[2];
        if (vkGetQueryPoolResults(dev, rqtm.qp, 2u * fi, 2, sizeof ts, ts, sizeof ts[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] >= ts[0]) {
            double ms = (double)(ts[1] - ts[0]) * gpu_timestamp_period * 1e-6;
            rqtm.s_ms += ms; rqtm.s_ms_n++; if (ms > rqtm.s_ms_max) rqtm.s_ms_max = ms;
        }
        rqtm.qpend[fi] = 0;
    }
    struct { const vk_target *s; u32 w, h, nl; } src[RQTM_N];
    u32 ns = 0;
    for (u32 i = 0; i < rep_ndraws; i++) {
        const vk_draw *d = &rep_draws[i];
        if (d->rt != 0u || !rqo_tree_card(d, d->depth)) continue;
        rqtm.s_cards++;
        u32 w = 0, h = 0, j = 0;
        const vk_target *s = rqtm_src(d, i, &w, &h);
        if (!s || s->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) { rqtm.s_skip++; continue; }
        while (j < ns && !(src[j].s == s && src[j].w == w && src[j].h == h)) j++;
        if (j < ns) continue;
        if (ns == RQTM_N) { rqtm.s_full++; continue; }
        src[ns].s = s; src[ns].w = w; src[ns].h = h; src[ns].nl = rts[d->tex_rt - 1u].mono ? 1u : rt_layers; ns++;
    }
    struct rqtm_e *E = rqtm.e[fi];
    for (u32 k = 0; k < RQTM_N; k++) E[k].src = NULL;
    struct rqtm_e *use[RQTM_N]; u32 nl[RQTM_N], nu = 0;
    for (u32 j = 0; j < ns; j++) {
        struct rqtm_e *e = NULL;
        for (u32 k = 0; k < RQTM_N && !e; k++) if (E[k].img && !E[k].src && E[k].w == src[j].w && E[k].h == src[j].h) e = &E[k];
        for (u32 k = 0; k < RQTM_N && !e; k++) if (!E[k].img && rqtm_mkimg(&E[k], src[j].w, src[j].h)) e = &E[k];
        if (!e) { rqtm.s_full++; continue; }
        e->src = src[j].s; use[nu] = e; nl[nu] = src[j].nl; nu++;
    }
    if (rqtm.s_lists % 1200u == 0u) {
        ps2_log("rq treemip: per list %.2f impostors built (%llu px), %.1f cards with a chain, %.1f fell back (%.2f cards not a chain's: region / layout, %.2f over %u); the build %.3f ms mean, %.3f max (GPU, %llu builds)",
                (double)rqtm.s_built / 1200.0, (unsigned long long)rqtm.s_px, (double)rqtm.s_cmip / 1200.0, (double)rqtm.s_cfb / 1200.0, (double)rqtm.s_skip / 1200.0, (double)rqtm.s_full / 1200.0, RQTM_N,
                rqtm.s_ms_n ? rqtm.s_ms / (double)rqtm.s_ms_n : 0.0, rqtm.s_ms_max, (unsigned long long)rqtm.s_ms_n);
        rqtm.s_built = rqtm.s_cards = rqtm.s_skip = rqtm.s_full = rqtm.s_cmip = rqtm.s_cfb = rqtm.s_ms_n = rqtm.s_px = 0; rqtm.s_ms = rqtm.s_ms_max = 0.0;
    }
    if (!nu) return;
    {   /* the sources' writes (passes, copies) before the reads; the chains' last reads (an earlier frame's pass) before the writes */
        VkMemoryBarrier2 mb; memset(&mb, 0, sizeof mb);
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2; mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        VkImageMemoryBarrier2 ib[RQTM_N];
        for (u32 u = 0; u < nu; u++) {
            barrier_fill(&ib[u], use[u]->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            ib[u].subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
        }
        VkDependencyInfo di; memset(&di, 0, sizeof di);
        di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO; di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb; di.imageMemoryBarrierCount = nu; di.pImageMemoryBarriers = ib;
        vkCmdPipelineBarrier2(cmd, &di);
    }
    if (rqtm.qp) { vkCmdResetQueryPool(cmd, rqtm.qp, 2u * fi, 2); vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, rqtm.qp, 2u * fi); }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rqtm.pipe);
    for (u32 u = 0; u < nu; u++) {
        struct rqtm_e *e = use[u];
        VkDescriptorImageInfo si; memset(&si, 0, sizeof si);
        si.sampler = sampler_point; si.imageView = e->src->cview; si.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet wr; memset(&wr, 0, sizeof wr);
        wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr.dstSet = e->cset; wr.dstBinding = 0; wr.descriptorCount = 1; wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &si;
        vkUpdateDescriptorSets(dev, 1, &wr, 0, NULL);                  /* (this frame slot's set: its last use has completed) */
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rqtm.lo, 0, 1, &e->cset, 0, NULL);
        int32_t p[4] = { (int32_t)e->w, (int32_t)e->h, (int32_t)e->n, 0 };
        vkCmdPushConstants(cmd, rqtm.lo, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, p);
        vkCmdDispatch(cmd, 1, 1, nl[u]);
        e->built = rqtm.list; rqtm.s_built++; rqtm.s_px += (u64)e->w * e->h * nl[u];
    }
    if (rqtm.qp) { vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, rqtm.qp, 2u * fi + 1u); rqtm.qpend[fi] = 1; }
    {   VkImageMemoryBarrier2 ib[RQTM_N];
        for (u32 u = 0; u < nu; u++) {
            barrier_fill(&ib[u], use[u]->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            ib[u].subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
        }
        barrier_flush(cmd, ib, nu);
    }
}
/* the draw loop: tree card d (rqtree_m set) reads its impostor's chain when this list built one (flags.w bit 2 instead of the box read) */
static void rqtm_pick(const vk_draw *d, u32 i) {
    if (!(rqtree_eff() & 16)) return;
    u32 w = 0, h = 0;
    const vk_target *s = rqtm.ok ? rqtm_src(d, i, &w, &h) : NULL;
    if (s && rqtm.fi < FRAMES_IN_FLIGHT)
        for (u32 k = 0; k < RQTM_N; k++) {
            struct rqtm_e *e = &rqtm.e[rqtm.fi][k];
            if (e->src == s && e->w == w && e->h == h && e->built == rqtm.list) { rqtm_cur = e; rqtree_w = (rqtree_w & ~1u) | 4u; rqtm.s_cmip++; return; }
        }
    rqtm.s_cfb++;
}
