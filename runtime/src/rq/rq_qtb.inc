/* rq_qtb.inc -- included by ps2_video.c (PS2_RQ_SUPPORT). PS2_XR_QUAD_TBLEND (rq19_sparkle 2026-11-16, BUGS_RQ R-260; the user's F-3
   "sparkles in the menu when I turn my head"): the cinema screen's image goes to the quad layer as this game frame's display region
   blended 50 / 50 (linear light) with the LAST GAME FRAME's raw region (a two-frame average, no feedback: a moving thing leaves one
   half-strength copy for one frame, a still picture is unchanged). Why: the game's animated 3D menus draw one-pixel lines without
   anti-aliasing (the briefing map's wireframe under its slowly orbiting camera, the sortie hangar's lights and edges); at 1x on a quad
   magnified ~3x they pop on and off from one frame to the next whether the head moves or not; the model (docs/rq/SPARKLE.md) gives the
   briefing map's popping (the temporal second difference) 13.9 -> 7.3 levels and the hangar's 4.5 -> 2.7 with this blend, while a 2x
   static scale leaves the briefing's at 13.6. Only screen frames (no head pose) from the render target; movies are skipped (their
   display region 640 wide: the game's FMV frames keep their own cuts). PS2_XR_QUAD_TBLEND=0 (default) | 1 (weight 0.5) | the last
   frame's weight 0.05..0.5. The pass: one triangle over a private 1-layer image of the region's size, then that image is what
   ps2_xr_record_copy_rt copies into the cinema chain; the raw region is copied to the history image after the blend read it. */
static struct {
    int on; float w; int failed;
    VkPipeline pipe; VkPipelineLayout layout;
    VkImage dst, hist; VkDeviceMemory dmem, hmem; VkImageView dview, hview; VkDescriptorSet hset;
    u32 iw, ih, lw, lh; int valid; u64 last_frame, n, skipped_movie;
} qtb;
static void qtb_init(const char *shader_base) {
    const char *e = getenv("PS2_XR_QUAD_TBLEND");
    float w = e && *e ? (float)atof(e) : 0.0f;
    if (w >= 0.999f && w <= 1.001f) w = 0.5f;               /* (=1: the default weight) */
    if (!(w >= 0.05f && w <= 0.5f)) { if (e && *e && strcmp(e, "0")) ps2_log("vk: PS2_XR_QUAD_TBLEND=%s not understood (0, 1 or 0.05..0.5): off", e); return; }
    char spv[1100];
    snprintf(spv, sizeof spv, "%s/quad_tblend.vert.spv", shader_base);
    VkShaderModule vs = load_spv(spv);
    snprintf(spv, sizeof spv, vk_mv ? "%s/quad_tblend_mv.frag.spv" : "%s/quad_tblend.frag.spv", shader_base);
    VkShaderModule fs = load_spv(spv);
    VkPushConstantRange pr; memset(&pr, 0, sizeof pr);
    pr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; pr.size = 8 * sizeof(float);
    VkDescriptorSetLayout sl[2] = { deslayout, deslayout };
    VkPipelineLayoutCreateInfo lc; memset(&lc, 0, sizeof lc);
    lc.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    lc.setLayoutCount = 2; lc.pSetLayouts = sl; lc.pushConstantRangeCount = 1; lc.pPushConstantRanges = &pr;
    if (!vs || !fs || vkCreatePipelineLayout(dev, &lc, NULL, &qtb.layout) != VK_SUCCESS) {
        ps2_log("vk: PS2_XR_QUAD_TBLEND unavailable (shaders)");
        return;
    }
    VkPipelineShaderStageCreateInfo stg[2]; memset(stg, 0, sizeof stg);
    stg[0].sType = stg[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stg[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stg[0].module = vs; stg[0].pName = "main";
    stg[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stg[1].module = fs; stg[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin; memset(&vin, 0, sizeof vin);
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia; memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp; memset(&vp, 0, sizeof vp);
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs; memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms; memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds; memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO; ds.maxDepthBounds = 1.0f;
    VkPipelineColorBlendAttachmentState cba; memset(&cba, 0, sizeof cba);
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb; memset(&cb, 0, sizeof cb);
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsi; memset(&dsi, 0, sizeof dsi);
    dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; dsi.dynamicStateCount = 2; dsi.pDynamicStates = dyn;
    VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo rci; memset(&rci, 0, sizeof rci);
    rci.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO; rci.colorAttachmentCount = 1; rci.pColorAttachmentFormats = &cf;
    VkGraphicsPipelineCreateInfo gp; memset(&gp, 0, sizeof gp);
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; gp.pNext = &rci;
    gp.stageCount = 2; gp.pStages = stg; gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb; gp.pDynamicState = &dsi; gp.layout = qtb.layout;
    if (vkCreateGraphicsPipelines(dev, pipe_cache, 1, &gp, NULL, &qtb.pipe) != VK_SUCCESS) {
        qtb.pipe = VK_NULL_HANDLE;
        ps2_log("vk: PS2_XR_QUAD_TBLEND unavailable (pipeline)");
        return;
    }
    qtb.w = w; qtb.on = 1;
    ps2_log("vk: PS2_XR_QUAD_TBLEND: the cinema screen's image = this game frame's %.0f %% + the last one's %.0f %% (linear light; "
            "screen frames from the render target, movies skipped)", 100.0f * (1.0f - w), 100.0f * w);
}
/* (re)make the two private images when the region outgrows them (they only grow: a one-field 512 x 224 display at a CRT switch, a
   menu after a larger screen keep them; the region is their top-left corner); 0 = not available */
static int qtb_images(u32 w, u32 h) {
    if (qtb.dst && w <= qtb.iw && h <= qtb.ih) return 1;
    if (qtb.iw > w) w = qtb.iw;
    if (qtb.ih > h) h = qtb.ih;
    if (qtb.failed > 3) return 0;
    vkDeviceWaitIdle(dev);                                  /* (a size change: a menu <-> movie switch, rare) */
    if (qtb.dview) vkDestroyImageView(dev, qtb.dview, NULL);
    if (qtb.dst) vkDestroyImage(dev, qtb.dst, NULL);
    if (qtb.dmem) vkFreeMemory(dev, qtb.dmem, NULL);
    if (qtb.hview) vkDestroyImageView(dev, qtb.hview, NULL);
    if (qtb.hist) vkDestroyImage(dev, qtb.hist, NULL);
    if (qtb.hmem) vkFreeMemory(dev, qtb.hmem, NULL);
    qtb.dst = qtb.hist = VK_NULL_HANDLE; qtb.dmem = qtb.hmem = VK_NULL_HANDLE; qtb.dview = qtb.hview = VK_NULL_HANDLE;
    u32 ol = mk_layers; int oa = mk_array_view; VkSampleCountFlagBits os = mk_samples;
    mk_layers = 1; mk_array_view = 0; mk_samples = VK_SAMPLE_COUNT_1_BIT;
    int ok = make_image(w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, &qtb.dst, &qtb.dmem, &qtb.dview) == 0
          && make_image(w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, &qtb.hist, &qtb.hmem, &qtb.hview) == 0;
    mk_layers = ol; mk_array_view = oa; mk_samples = os;
    if (!ok) { qtb.failed++; qtb.iw = qtb.ih = 0; ps2_log("vk: PS2_XR_QUAD_TBLEND: images of %ux%u not made", w, h); return 0; }
    if (qtb.hset) rebind_set_s(qtb.hset, qtb.hview, sampler_point); else qtb.hset = alloc_set_s(qtb.hview, sampler_point);
    qtb.iw = w; qtb.ih = h; qtb.valid = 0;
    ps2_log("vk: PS2_XR_QUAD_TBLEND: blend images %ux%u", w, h);
    return qtb.hset != VK_NULL_HANDLE;
}
/* at the screen frame's copy (no rendering open): blend target t's region x0, y0, w x h (in t->layout = SHADER_READ_ONLY) with the
   last game frame's; *img / *lay = what to copy instead of t (the region at 0, 0). 0 = copy t as before */
static int qtb_apply(VkCommandBuffer cmd, vk_target *t, u32 x0, u32 y0, u32 w, u32 h, VkImage *img, VkImageLayout *lay) {
    if (!qtb.on || rendering_open || !t->set || t->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || !w || !h) return 0;
    if (w == 640u * rt_scale) { qtb.skipped_movie++; qtb.valid = 0; return 0; }      /* (the game's movies: 640 wide) */
    if (!qtb_images(w, h)) return 0;
    int fresh = !qtb.valid || stat_frames != qtb.last_frame + 1u || w != qtb.lw || h != qtb.lh;   /* (no last frame of this screen) */
    VkImageMemoryBarrier2 b[3];
    /* the target: written earlier in this command buffer, now read by the blend; the history: last written by a copy */
    barrier_fill(&b[0], t->color, VK_IMAGE_ASPECT_COLOR_BIT, t->layout, t->layout, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    barrier_fill(&b[1], qtb.dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    barrier_fill(&b[2], qtb.hist, VK_IMAGE_ASPECT_COLOR_BIT, fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    barrier_flush(cmd, b, 3);
    VkRenderingAttachmentInfo ca; memset(&ca, 0, sizeof ca);
    ca.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; ca.imageView = qtb.dview;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri; memset(&ri, 0, sizeof ri);
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO; ri.renderArea.extent.width = w; ri.renderArea.extent.height = h;
    ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vpt; memset(&vpt, 0, sizeof vpt);
    vpt.width = (float)w; vpt.height = (float)h; vpt.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vpt);
    VkRect2D sc; memset(&sc, 0, sizeof sc); sc.extent.width = w; sc.extent.height = h;
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, qtb.pipe);
    VkDescriptorSet sets[2] = { t->set, qtb.hset };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, qtb.layout, 0, 2, sets, 0, NULL);
    float tw = (float)(tew(t) * tsc(t)), th = (float)(teh(t) * tsc(t));   /* (the target image's extent: PS2_PERF_RT_FIT, its own scale) */
    float pc[8] = { (float)x0 / tw, (float)y0 / th, (float)w / tw, (float)h / th, fresh ? 0.0f : qtb.w, 0.0f,
                    (float)w / (float)qtb.iw, (float)h / (float)qtb.ih };          /* (par.zw: the history's corner in its image) */
    vkCmdPushConstants(cmd, qtb.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    /* the raw region becomes the history (after the blend read it); the blend image goes to the copy */
    barrier_fill(&b[0], t->color, VK_IMAGE_ASPECT_COLOR_BIT, t->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier_fill(&b[1], qtb.dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier_fill(&b[2], qtb.hist, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT);
    barrier_flush(cmd, b, 3);
    VkImageCopy c; memset(&c, 0, sizeof c);
    c.srcSubresource.aspectMask = c.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.srcSubresource.layerCount = c.dstSubresource.layerCount = 1;
    c.srcOffset.x = (int32_t)x0; c.srcOffset.y = (int32_t)y0;
    c.extent.width = w; c.extent.height = h; c.extent.depth = 1;
    vkCmdCopyImage(cmd, t->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, qtb.hist, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier_fill(&b[0], t->color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->layout,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    barrier_fill(&b[1], qtb.hist, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_READ_BIT);
    barrier_flush(cmd, b, 2);
    qtb.valid = 1; qtb.last_frame = stat_frames; qtb.lw = w; qtb.lh = h; qtb.n++;
    if (qtb.n == 1 || qtb.n % 3600u == 0u)
        ps2_log("vk: PS2_XR_QUAD_TBLEND: %llu screen frames blended (%ux%u at %u,%u), %llu movie frames left alone",
                (unsigned long long)qtb.n, w, h, x0, y0, (unsigned long long)qtb.skipped_movie);
    *img = qtb.dst; *lay = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return 1;
}
