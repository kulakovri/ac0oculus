/* rq_burst.inc -- Pixy's MPBM burst as a volume in the Quest's eye pass (PS2_RQ_BURST; BUGS_RQ R-120, R-121; included by ps2_video.c,
   PS2_RQ_SUPPORT only). The user's M18 flight of 2026-11-12 (video 67-140 s): "it turns, it is not volumetric; copy how this explosion is
   made in the PC version, study the notes; an important part of this mission"; the review: a flat white blob with a stepped outline and a
   purple fringe that turns with the head, and App GPU 15-20 ms (46-59 fps) for the 7 s it is on screen.
   The PC answer (ACZ_VR_FX_BURST, fx_burst 2026-11-02, bug #266: bst_* in ps2_video.c, burst.vert / burst.frag) is its own pass "in the
   trails' slot", so PS2_FRAME_GRAPH's fg_defaults turn it off (ACZ_VR_FX_BURST=0) and the Quest drew the game's own burst: up to ~56 GS
   sprite columns of an 8 x 8 texel additive glow in two layers (each column a card of its own, turned upright by ACZ_VR_FX_BILLBOARD_BIG:
   the staircase), plus a copy into the game's glow target that its glow chain lays back as a blocky lavender halo. Here:
   - the event and the game's size and strength are the PC's (zero_vr.c burst_install -> bst_events; bst_prepass measures the game's
     columns once a game frame); bst_prepass also takes the game's glow out -- in the eye pass by collapsing its sprites to a point (no
     fragments: their overdraw is gone, not just their colour) in both the frame's vertex copy and the list's vertices (the HUD batch's
     effect rows read those), the frame's columns and the glow-target copy alike (rqb_collapse);
   - the volume is drawn INSIDE the eye pass, where the game drew its first column (the list's order: after the world, among the effects),
     with rq_burst.vert / rq_burst.frag: the PC's medium and colours, each eye's own ray, at the Quest's price (a noise texture instead of
     hashes; rqb.steps steps, fewer when it covers half the view or more; per pixel, no coarse shading: the density map does that).
   Switches: PS2_RQ_BURST (strength; the Quest 1, else off), PS2_RQ_BURST_STEPS (8), PS2_RQ_BURST_PARTS (7: 1 the frame's columns collapsed,
   2 the glow-target copies collapsed, 4 the volume drawn), PS2_RQ_BURST_AB=N [test]: blocks of N eye-pass lists rotate through the masks
   of PS2_RQ_BURST_ARMS (default "0,7": the game's burst as before against all of it); `rq burst ab:` gives the GPU ms of the frames with a
   live burst per arm (the cost of each part by absence, in one flight). `rq burst:` lines. */
typedef struct { float cx[4], cy[4], cz[4], proj[4], misc[4], eyes[4], ctr[4], st[4], heat[4], tint[4], fogc[4], rqx[4], rqy[4]; } rqb_push;   /* 208 bytes */
static struct {
    int cfg, tried, ok, steps, ab;
    float k;
    VkShaderModule vs, fs;
    VkDescriptorSetLayout dsl; VkDescriptorPool pool; VkDescriptorSet set;
    VkPipelineLayout layout; VkPipeline pipe; VkRenderPass rp_for;
    VkImage nimg; VkDeviceMemory nmem; VkImageView nview; VkSampler nsmp;
    VkBuffer nbuf; VkDeviceMemory nbmem; void *nbptr;
    /* this list: the draws that held the game's frame columns (the volume's place), drawn yet, its parts (PS2_RQ_BURST_PARTS / the arm) */
    u8 *col; u32 ncap; int any_col, drawn, parts, cfg_parts, live;
    u64 lists, draws, inside, ncol, ncopy, nlist;
    double area;
    /* PS2_RQ_BURST_AB: the arms' masks, the arm of each frame slot (-1 no live burst), the sums */
    int narm, arm_mask[8], slot_arm[FRAMES_IN_FLIGHT]; double arm_sum[8]; u32 arm_n[8], arm_tot;
} rqb;
static int rqb_read;
static int rqb_cfg(void) {
    if (!rqb_read) {
        rqb_read = 1;
        const char *e = getenv("PS2_RQ_BURST");
        rqb.k = e && *e ? (float)atof(e) : 0.0f;
        if (!(rqb.k > 0.0f)) rqb.k = 0.0f;
        if (rqb.k > 2.0f) rqb.k = 2.0f;
        e = getenv("PS2_RQ_BURST_STEPS"); rqb.steps = e && *e ? atoi(e) : 8;
        if (rqb.steps < 2) rqb.steps = 2;
        if (rqb.steps > 16) rqb.steps = 16;
        e = getenv("PS2_RQ_BURST_PARTS"); rqb.cfg_parts = e && *e ? atoi(e) & 7 : 7;
        e = getenv("PS2_RQ_BURST_AB"); rqb.ab = e && *e ? atoi(e) : 0;
        if (rqb.ab < 0) rqb.ab = 0;
        rqb.narm = 0;
        if (rqb.ab) {
            e = getenv("PS2_RQ_BURST_ARMS");
            const char *q = e && *e ? e : "0,7";
            while (*q && rqb.narm < 8) { rqb.arm_mask[rqb.narm++] = atoi(q) & 7; while (*q && *q != ',') q++; if (*q == ',') q++; }
            if (rqb.narm < 2) rqb.ab = 0;
        }
        for (int i = 0; i < (int)FRAMES_IN_FLIGHT; i++) rqb.slot_arm[i] = -1;
        rqb.cfg = rqb.k > 0.0f;
        if (rqb.cfg) ps2_log("vk: PS2_RQ_BURST=%.2f: the MPBM burst as a volume inside the eye pass (%d steps; parts %d: 1 the game's glow columns collapsed, 2 its glow-target copies collapsed, 4 the volume)%s",
                             (double)rqb.k, rqb.steps, rqb.cfg_parts, rqb.ab ? "; PS2_RQ_BURST_AB: blocks of lists rotate through PS2_RQ_BURST_ARMS" : "");
    }
    return rqb.cfg;
}
/* the eye pass draws the volume for this list */
static int rqb_now(void) { return rqb_cfg() && rq_eye_now && (rqb.parts & 4); }
/* (bst_prepass) a list starts: which arm, the noted sprites cleared */
#define RQB_NOTES 1024u
static struct { u32 j, di; u8 frame; } rqb_notes[RQB_NOTES];
static u32 rqb_nnotes;
static void rqb_list(void) {
    rqb.parts = rqb.ab ? rqb.arm_mask[(rqb.nlist / (u64)rqb.ab) % (u64)rqb.narm] : rqb.cfg_parts;
    rqb.nlist++;
    rqb.any_col = 0; rqb.drawn = 0; rqb.live = 0; rqb_nnotes = 0;
}
/* (bst_prepass) a sprite of the game's glow (j its first vertex, di its draw, frame: a column in slot 0, else a glow-target copy) */
static void rqb_note(u32 j, u32 di, int frame) {
    if (rqb_nnotes < RQB_NOTES) { rqb_notes[rqb_nnotes].j = j; rqb_notes[rqb_nnotes].di = di; rqb_notes[rqb_nnotes].frame = (u8)frame; rqb_nnotes++; }
}
static int rqb_make(void);
/* after the eye pass's decision (rq_eye_now known, before the HUD batch reads the list): the noted sprites collapsed onto their first
   vertex in both copies (no fragments), the draws that held frame columns remembered (the volume's place in the order) */
static void rqb_apply(ps2_vk_vertex *dst, int fi) {
    if (fi >= 0 && fi < (int)FRAMES_IN_FLIGHT) rqb.slot_arm[fi] = -1;
    if (!rqb_cfg() || !rq_eye_now) return;
    (void)rqb_make();                                    /* (at the first eye-pass list: no hitch at the first burst) */
    {   int any = 0;
        for (int i = 0; i < BST_N; i++) any |= bst.b[i].on;
        rqb.live = any && rqb.ok && (rqb.parts & 4);
        if (rqb.ab && any && fi >= 0 && fi < (int)FRAMES_IN_FLIGHT) rqb.slot_arm[fi] = (int)(((rqb.nlist - 1u) / (u64)rqb.ab) % (u64)rqb.narm);
    }
    if (!rqb.ok || !rqb_nnotes || !(rqb.parts & 7)) return;
    if (rqb.ncap < rep_ndraws) {
        u32 n = rep_ndraws + 1024u;
        u8 *p = (u8 *)realloc(rqb.col, n);
        if (!p) return;
        rqb.col = p; rqb.ncap = n;
    }
    memset(rqb.col, 0, rep_ndraws);
    for (u32 q = 0; q < rqb_nnotes; q++) {
        u32 j = rqb_notes[q].j, di = rqb_notes[q].di;
        if (rqb_notes[q].frame && di < rqb.ncap) { rqb.col[di] = 1; rqb.any_col = 1; }   /* (the volume's place, collapsed or not) */
        if (j + 6u > rep_nverts || !(rqb.parts & (rqb_notes[q].frame ? 1 : 2))) continue;
        for (u32 k = j + 1u; k < j + 6u; k++) {
            dst[k].x = dst[j].x; dst[k].y = dst[j].y;
            rep_verts[k].x = rep_verts[j].x; rep_verts[k].y = rep_verts[j].y;
        }
        if (rqb_notes[q].frame) rqb.ncol++; else rqb.ncopy++;
    }
}
/* the frame's GPU time per arm (PS2_RQ_BURST_AB; frames with a live burst only) */
static void rqb_sample(int fi, double ms) {
    if (!rqb.ab || fi < 0 || fi >= (int)FRAMES_IN_FLIGHT) return;
    int a = rqb.slot_arm[fi];
    rqb.slot_arm[fi] = -1;
    if (a < 0 || a >= rqb.narm || !(ms > 0.0)) return;
    rqb.arm_sum[a] += ms; rqb.arm_n[a]++;
    if (++rqb.arm_tot % 120u) return;
    char b[400]; int o = 0;
    for (int i = 0; i < rqb.narm && o < 360; i++)
        o += snprintf(b + o, sizeof b - (size_t)o, "%sparts %d: %.3f ms (%u)", i ? ", " : "", rqb.arm_mask[i], rqb.arm_n[i] ? rqb.arm_sum[i] / rqb.arm_n[i] : 0.0, rqb.arm_n[i]);
    ps2_log("rq burst ab: frames with a live burst, GPU ms per arm (blocks of %d lists): %s", rqb.ab, b);
}
static void rqb_noise_upload(VkCommandBuffer cmd) {
    VkImageMemoryBarrier2 b;
    barrier_fill(&b, rqb.nimg, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    barrier_flush(cmd, &b, 1);
    VkBufferImageCopy rc; memset(&rc, 0, sizeof rc);
    rc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rc.imageSubresource.layerCount = 1;
    rc.imageExtent.width = 256u; rc.imageExtent.height = 256u; rc.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd, rqb.nbuf, rqb.nimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rc);
    barrier_fill(&b, rqb.nimg, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    barrier_flush(cmd, &b, 1);
}
/* the pipeline (the eye pass's render pass) and the noise texture: once; 0 = unavailable */
static int rqb_make(void) {
    if (rqb.ok && rqb.rp_for == rqe.rp) return 1;
    if (rqb.tried) return 0;
    rqb.tried = 1;
    if (!rqe.rp || !rqb.vs || !rqb.fs) { ps2_log("vk: PS2_RQ_BURST: no eye pass or shaders (rq_burst.*): the game's sprites"); return 0; }
    /* the noise: 256 x 256 RGBA8, r / b two independent lattices of values, g / a the same moved by (37, 17): one bilinear read gives
       two value noises' z slices (iq's packing; rq_burst.frag vnoise2) */
    {   VkImageCreateInfo ii; memset(&ii, 0, sizeof ii);
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent.width = 256u; ii.extent.height = 256u; ii.extent.depth = 1; ii.mipLevels = 1; ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(dev, &ii, NULL, &rqb.nimg) != VK_SUCCESS) { rqb.nimg = VK_NULL_HANDLE; return 0; }
        VkMemoryRequirements r; vkGetImageMemoryRequirements(dev, rqb.nimg, &r);
        VkMemoryAllocateInfo ai; memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = r.size; ai.memoryTypeIndex = find_mem(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (ai.memoryTypeIndex == 0xFFFFFFFFu || vkAllocateMemory(dev, &ai, NULL, &rqb.nmem) != VK_SUCCESS || vkBindImageMemory(dev, rqb.nimg, rqb.nmem, 0) != VK_SUCCESS) return 0;
        VkImageViewCreateInfo vi; memset(&vi, 0, sizeof vi);
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = rqb.nimg; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(dev, &vi, NULL, &rqb.nview) != VK_SUCCESS) return 0;
        VkSamplerCreateInfo si; memset(&si, 0, sizeof si);
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO; si.magFilter = VK_FILTER_LINEAR; si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT; si.maxLod = 0.0f;
        if (vkCreateSampler(dev, &si, NULL, &rqb.nsmp) != VK_SUCCESS) return 0;
        if (make_buffer(256u * 256u * 4u, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &rqb.nbuf, &rqb.nbmem, &rqb.nbptr) != 0 || !rqb.nbptr) return 0;
        u8 *px = (u8 *)rqb.nbptr;
        static u8 L[2][256 * 256];
        u32 h = 0x9E3779B9u;
        for (u32 k = 0; k < 2u; k++) for (u32 i = 0; i < 256u * 256u; i++) {
            h ^= h << 13; h ^= h >> 17; h ^= h << 5;             /* (xorshift32: deterministic, the same noise in every session) */
            L[k][i] = (u8)(h >> 24);
        }
        for (u32 y = 0; y < 256u; y++) for (u32 x = 0; x < 256u; x++) {
            u32 o = (y * 256u + x) * 4u, m = ((y + 17u) & 255u) * 256u + ((x + 37u) & 255u);
            px[o] = L[0][y * 256u + x]; px[o + 1u] = L[0][m]; px[o + 2u] = L[1][y * 256u + x]; px[o + 3u] = L[1][m];
        }
    }
    VkDescriptorSetLayoutBinding b; memset(&b, 0, sizeof b);
    b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo lc; memset(&lc, 0, sizeof lc);
    lc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; lc.bindingCount = 1; lc.pBindings = &b;
    if (vkCreateDescriptorSetLayout(dev, &lc, NULL, &rqb.dsl) != VK_SUCCESS) return 0;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo pi; memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; pi.maxSets = 1; pi.poolSizeCount = 1; pi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(dev, &pi, NULL, &rqb.pool) != VK_SUCCESS) return 0;
    VkDescriptorSetAllocateInfo ai; memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; ai.descriptorPool = rqb.pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &rqb.dsl;
    if (vkAllocateDescriptorSets(dev, &ai, &rqb.set) != VK_SUCCESS) return 0;
    VkDescriptorImageInfo di; memset(&di, 0, sizeof di);
    di.sampler = rqb.nsmp; di.imageView = rqb.nview; di.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w; memset(&w, 0, sizeof w);
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = rqb.set; w.dstBinding = 0; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &di;
    vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
    VkPushConstantRange pr; memset(&pr, 0, sizeof pr);
    pr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT; pr.size = sizeof(rqb_push);
    VkPipelineLayoutCreateInfo pl; memset(&pl, 0, sizeof pl);
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pl.setLayoutCount = 1; pl.pSetLayouts = &rqb.dsl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev, &pl, NULL, &rqb.layout) != VK_SUCCESS) return 0;
    VkPipelineShaderStageCreateInfo st[2]; memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = rqb.vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = rqb.fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin; memset(&vin, 0, sizeof vin); vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia; memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo vp; memset(&vp, 0, sizeof vp); vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs; memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms; memset(&ms, 0, sizeof ms); ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = rq_samples();
    VkPipelineDepthStencilStateCreateInfo ds; memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO; ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; ds.maxDepthBounds = 1.0f;          /* (reversed depth: the cockpit, nearer terrain hide it) */
    VkPipelineColorBlendAttachmentState cba; memset(&cba, 0, sizeof cba);
    cba.blendEnable = VK_TRUE; cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;   /* (premultiplied over; the alpha stays) */
    VkPipelineColorBlendStateCreateInfo cb; memset(&cb, 0, sizeof cb); cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cb.attachmentCount = 1; cb.pAttachments = &cba;
    static const VkDynamicState dy[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsi; memset(&dsi, 0, sizeof dsi); dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; dsi.dynamicStateCount = 2; dsi.pDynamicStates = dy;
    VkGraphicsPipelineCreateInfo gp; memset(&gp, 0, sizeof gp);
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds; gp.pColorBlendState = &cb;
    gp.pDynamicState = &dsi; gp.layout = rqb.layout; gp.renderPass = rqe.rp;
    if (vkCreateGraphicsPipelines(dev, pipe_cache, 1, &gp, NULL, &rqb.pipe) != VK_SUCCESS) { rqb.pipe = VK_NULL_HANDLE; return 0; }
    if (!rq_oneshot(rqb_noise_upload, "PS2_RQ_BURST's noise texture")) return 0;
    rqb.rp_for = rqe.rp; rqb.ok = 1;
    ps2_log("vk: PS2_RQ_BURST: the burst's pipeline and its 256 x 256 noise made");
    return 1;
}
/* inside the eye pass, at the list's first draw that held a frame column (or the fallback point): the live bursts, farthest first */
static int rqb_draw(VkCommandBuffer cmd, vk_frame *fr) {
    rqb.drawn = 1;
    if (!rqb_now() || !rqb.ok || !cpit_seq) return 0;
    int any = 0;
    for (int i = 0; i < BST_N; i++) any |= bst.b[i].on;
    if (!any) return 0;
    ps2_cloud_view v;
    if (!cl_view_make(&v, cpit_seq)) return 0;
    float upm = v.units_per_m > 0.5f ? v.units_per_m : 6.0f;
    double E[3] = { v.origin_m[0] * upm, v.origin_m[1] * upm, v.origin_m[2] * upm };
    ps2_cloud_env env;
    int envok = ps2_cloud_env_read(&env);
    int ord[BST_N], no = 0; float dist[BST_N];
    for (int i = 0; i < BST_N; i++) {
        if (!bst.b[i].on) continue;
        float w[3] = { (float)(bst.b[i].p[0] - E[0]), (float)(bst.b[i].p[1] - E[1]), (float)(bst.b[i].p[2] - E[2]) };
        dist[i] = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        int k = no++;
        while (k > 0 && dist[ord[k - 1]] < dist[i]) { ord[k] = ord[k - 1]; k--; }
        ord[k] = i;
    }
    float W = (float)(rt_w * rt_scale), H = (float)(rt_h * rt_scale);
    rqb_push pc; memset(&pc, 0, sizeof pc);
    for (int i = 0; i < 3; i++) { pc.cx[i] = v.cam_x[i]; pc.cy[i] = v.cam_y[i]; pc.cz[i] = v.cam_z[i]; }
    for (int i = 0; i < 4; i++) pc.proj[i] = v.proj[i];
    pc.misc[0] = 2.0f / W; pc.misc[1] = 2.0f / H;
    pc.misc[2] = v.depth_a != 0.0f && v.depth_zmax > 0.0f ? v.depth_mh / (v.depth_a * v.depth_zmax) : 0.0f;
    pc.misc[3] = v.depth_zmax > 0.0f ? -v.depth_c / v.depth_zmax : 0.0f;
    float h = v.half_ipd_m * upm;
    pc.eyes[0] = -h; pc.eyes[1] = h; pc.eyes[2] = (float)fmod(v.time_s, 4096.0); pc.eyes[3] = upm;
    pc.rqx[0] = rqe.xf[0]; pc.rqx[1] = rqe.xf[1]; pc.rqx[2] = rqe.xf[2]; pc.rqx[3] = rqe.xf[3];
    pc.rqy[0] = rqe.xf[4]; pc.rqy[1] = rqe.xf[5];
    /* the inside quad's distance: past the near plane (z / w = A / depth + B reaches 1 at A / (1 - B)), at least 20 m */
    {   float nearu = pc.misc[3] < 0.999f ? pc.misc[2] / (1.0f - pc.misc[3]) : 0.0f;
        pc.rqy[3] = fmaxf(fmaxf(2.0f * nearu, 20.0f * upm), 1.0f);
    }
    VkViewport vp; memset(&vp, 0, sizeof vp); vp.width = (float)rqe.w; vp.height = (float)rqe.h; vp.maxDepth = 1.0f;
    VkRect2D sc; memset(&sc, 0, sizeof sc); sc.offset.x = (int32_t)rqd.vx; sc.offset.y = (int32_t)rqd.vy; sc.extent.width = rqd.vw; sc.extent.height = rqd.vh;
    int bound = 0, drawn = 0;
    float alpha = 0.0f;
    for (int n = 0; n < no; n++) {
        bst_burst *b = &bst.b[ord[n]];
        float age = bst_age(b, cpit_seq, alpha);
        if (age < 0.0f || age > BST_LIFE) continue;
        int seen = b->seen_seq == cpit_seq && b->r_game > 0.0f;
        float R = seen ? b->r_game : bst_rcurve(age, upm) * b->r_curve_k;
        float ag = seen ? b->a_game : bst_acurve(age);
        if (!(R > 1.0f * upm)) R = 1.0f * upm;
        float cw[3] = { (float)(b->p[0] - E[0]), (float)(b->p[1] - E[1]), (float)(b->p[2] - E[2]) };
        float d = dist[ord[n]];
        float ag1 = fminf(fmaxf(ag, 0.0f), 1.0f);
        float heat = (0.42f + 0.58f * (age < 0.6f ? 1.0f : ag1)) * (1.0f - 0.22f * trl_smooth(0.4f, 3.2f, age));
        float fade = powf(ag1, 0.35f);
        float smoke = 0.12f + 0.55f * (1.0f - ag1) * trl_smooth(0.6f, 2.0f, age);
        for (int k = 0; k < 3; k++) pc.ctr[k] = cw[k];
        pc.ctr[3] = R;
        pc.st[0] = age / BST_LIFE; pc.st[1] = rqb.k * fade; pc.st[2] = b->seed;
        /* the quad lies at the sphere's front (d - R, rq_burst.vert) and must hold the cone of the sphere's outline there (+ 12 % for
           the lumps): Rq / sqrt(d^2 - Rq^2) is that cone's tangent. (The PC's bst_pass sizes it for the nearer plane d - Rq: too
           small when the eye nears the sphere -- the burst's outline cut straight, 0.4 of the size needed at d = 1.2 R.) */
        float Rq = R * 1.12f, dq = fmaxf(d - R, 1.0f);
        pc.st[3] = d > Rq * 1.02f ? Rq * dq / sqrtf(fmaxf(d * d - Rq * Rq, 1.0f)) : 0.0f;
        pc.heat[0] = heat; pc.heat[1] = sqrtf(ag1); pc.heat[2] = smoke; pc.heat[3] = d / R;
        for (int k = 0; k < 3; k++) pc.tint[k] = fminf(fmaxf(b->tint[k], 0.0f), 1.5f);
        pc.tint[3] = 0.35f;
        if (envok && (env.valid & CL_ENV_COLOURS) && env.fog_far_m > env.fog_near_m + 1.0f) {
            for (int k = 0; k < 3; k++) pc.fogc[k] = env.fog[k];
            pc.fogc[3] = 0.6f * trl_smooth(env.fog_near_m, env.fog_far_m, d / upm);
        }
        int full = !(pc.st[3] > 0.0f);
        /* its share of the game's frame: the steps (the PC's 4 x 4 rule: a burst over half the view is a glow of large shapes) */
        float area = 1.0f;
        if (!full) {
            float cv[3] = { cw[0] * v.cam_x[0] + cw[1] * v.cam_x[1] + cw[2] * v.cam_x[2],
                            cw[0] * v.cam_y[0] + cw[1] * v.cam_y[1] + cw[2] * v.cam_y[2],
                            cw[0] * v.cam_z[0] + cw[1] * v.cam_z[1] + cw[2] * v.cam_z[2] };
            float zf = -cv[2];
            if (zf > Rq) {
                float rr = pc.st[3] / fmaxf(dq / d * zf, 1.0f) * 1.3f + 2.0f * h / fmaxf(zf - Rq, 1.0f);
                float px = v.proj[2] + v.proj[0] * cv[0] / zf, py = v.proj[3] - v.proj[1] * cv[1] / zf;
                float x0 = fmaxf(px - v.proj[0] * rr, 0.0f), x1 = fminf(px + v.proj[0] * rr, W);
                float y0 = fmaxf(py - v.proj[1] * rr, 0.0f), y1 = fminf(py + v.proj[1] * rr, H);
                area = x1 > x0 && y1 > y0 ? (x1 - x0) * (y1 - y0) / (W * H) : 0.0f;
            }
        }
        if (!(area > 0.0f)) continue;
        int steps = rqb.steps;
        if (full) steps = steps * 1 / 2; else if (area > 0.5f) steps = steps * 5 / 8; else if (area > 0.25f) steps = steps * 3 / 4;
        pc.rqy[2] = (float)(steps < 2 ? 2 : steps);
        if (!bound) {
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqb.pipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqb.layout, 0, 1, &rqb.set, 0, NULL);
            bound = 1;
        }
        vkCmdPushConstants(cmd, rqb.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        if (full) { vkCmdDraw(cmd, 4, 1, 4, 0); rqb.inside++; }
        else vkCmdDraw(cmd, 4, 1, 0, 0);
        rqb.draws++; drawn++;
        rqb.area += (double)area;
    }
    (void)fr;
    if (drawn) {
        rqb.lists++;
        if (!(rqb.lists % 300u))
            ps2_log("rq burst: %llu lists with a volume, %llu draws (%llu with the eye inside), mean share of the frame %.2f; the game's sprites collapsed: %llu frame columns, %llu glow-target copies (%llu lists seen)",
                    (unsigned long long)rqb.lists, (unsigned long long)rqb.draws, (unsigned long long)rqb.inside, rqb.draws ? rqb.area / (double)rqb.draws : 0.0,
                    (unsigned long long)rqb.ncol, (unsigned long long)rqb.ncopy, (unsigned long long)rqb.nlist);
    }
    return bound;
}
/* the eye pass closes with a live burst not drawn (rq_video.inc rql_tail: a list without our clouds, the columns or the order's tail) */
static void rqb_tail(VkCommandBuffer cmd, vk_frame *fr) {
    if (rqb.live && !rqb.drawn) (void)rqb_draw(cmd, fr);
}
/* (rq_video.inc rq_load_shaders) */
static void rqb_load_shaders(const char *base) {
    char spv[512];
    snprintf(spv, sizeof spv, "%s/rq_burst.vert.spv", base); rqb.vs = load_spv(spv);
    snprintf(spv, sizeof spv, "%s/rq_burst.frag.spv", base); rqb.fs = load_spv(spv);
    if (!rqb.vs || !rqb.fs) ps2_log("vk: PS2_RQ_BURST: rq_burst.vert / .frag missing");
}
