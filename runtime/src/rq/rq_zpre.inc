/* rq_zpre.inc -- PS2_RQ_ZPRE (agent depth, plan 2 section 4.6; included by rq_video.inc, PS2_RQ_SUPPORT only). The eye pass stores colour only; this frame's
   depth comes from a pre-pass before it. The pre-pass:
   - target: rq_feed.depth itself (D32, the game's symmetric frame at the feed's size, 2 layers, multiview, reversed depth cleared to 0, final layout
     SHADER_READ_ONLY): every consumer of the scene depth (the clouds' near march and composite, the scene light's bands, the game's sun probe) already reads this
     image at this mapping, and it does not depend on the eye crop or on dynamic resolution. (The feed's former depth write, a full-screen copy from the eye pass's
     depth resolve, is gone: the feed pass writes colour only.)
   - content: the opaque native meshes of the frame's list, one batched indexed draw (the records and indices of PS2_RN_BATCH, written here before the eye pass; no
     fragment shader), and the COCKPIT'S COVERAGE: the eye pass's solid cockpit writes a mark into alpha (RQZ_MARK = 16/255: below every DATE and blend threshold the frame's
     alpha is used for) and the feed pass keeps it where the fade quad was skipped (PS2_RQ_HUD=batch), so the feed's alpha holds the previous frame's coverage. One
     full-screen draw writes depth 0.9975 there (rq_zpre_ck.frag). That keeps the near clouds from marching behind the panel (they stop at the mask as they did at the
     resolved cockpit depth; rq_clouds_near.frag does not take the band as an occluder, so the near composite has no halo along the frame; the far composite reads no scene depth at all) and gives the scene light's rejection (rqc_sl_copy: z >= 0.99) what it read before. A merged cockpit draw was rejected: 263 k vertices x
     2 views per frame would be most of the 0.3 ms budget (the shadow map's 200 k vertices cost 0.7-1.9 ms in a traced frame).
   - excluded (not depth-accurate without their shaders): alpha-tested draws whose test can fail, their fail halves, DATE, overlays (no depth write), the game's cockpit,
     the sky (depth 0 is the clear value), the GS-path draws (tree cards, effects): the consumers see them no more (the clouds' composite still depth-tests against the
     eye pass's own depth).
   - PS2_RQ_ZPRE_VIEW=1 [debug]: the feed shows the pre-pass's depth (eye_*.ppm dumps).
   The sun probe's reads of slot 0's depth (hoisted, before this pass in the command buffer) read the previous frame's pre-pass in SHADER_READ_ONLY layout; any
   later reads of the same frame read this frame's. */
static int rqz_view(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PS2_RQ_ZPRE_VIEW"); v = e && *e == '1'; }
    return v;
}
/* the feed's colour-only framebuffer (rq_eye_after) */
static VkFramebuffer rqz_feedfb(const vk_target *f) {
    if (rqz.ffb && rqz.ffbv == f->cview) return rqz.ffb;
    if (rqz.ffb) { vkDestroyFramebuffer(dev, rqz.ffb, NULL); rqz.ffb = VK_NULL_HANDLE; }
    VkFramebufferCreateInfo ci; memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO; ci.renderPass = rqe.dsrp; ci.attachmentCount = 1; ci.pAttachments = &f->cview;
    ci.width = tew(f) * tsc(f); ci.height = teh(f) * tsc(f); ci.layers = 1;
    if (vkCreateFramebuffer(dev, &ci, NULL, &rqz.ffb) != VK_SUCCESS) { rqz.ffb = VK_NULL_HANDLE; rqz.ffbv = VK_NULL_HANDLE; return VK_NULL_HANDLE; }
    rqz.ffbv = f->cview;
    return rqz.ffb;
}
static int rqz_make(void) {
    if (rqz.ok) return 1;
    if (rqz.tried) return 0;
    rqz.tried = 1;
    const u64 rqpw_t0 = vk_mono_ns();
    PFN_vkCreateRenderPass2 crp2 = (PFN_vkCreateRenderPass2)vkGetDeviceProcAddr(dev, "vkCreateRenderPass2");
    if (!crp2 || !rqz_vs || !rqz_ck_fs || !rqv_ds || !rnb.ok || !rnb.layout) { ps2_log("vk: PS2_RQ_ZPRE: no pass or shaders: off"); return 0; }
    /* the pass: one depth attachment, multiview, cleared to 0 (reversed depth), stored, read by the frame's consumers */
    VkAttachmentDescription2 at; memset(&at, 0, sizeof at);
    at.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2; at.format = VK_FORMAT_D32_SFLOAT; at.samples = VK_SAMPLE_COUNT_1_BIT;
    at.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference2 za; memset(&za, 0, sizeof za);
    za.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2; za.attachment = 0; za.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; za.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    VkSubpassDescription2 sp; memset(&sp, 0, sizeof sp);
    sp.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2; sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.viewMask = 3; sp.pDepthStencilAttachment = &za;
    VkSubpassDependency2 d[2]; memset(d, 0, sizeof d);
    for (int i = 0; i < 2; i++) d[i].sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
    d[0].srcSubpass = VK_SUBPASS_EXTERNAL; d[0].dstSubpass = 0; d[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    d[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    d[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; d[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    d[1].srcSubpass = 0; d[1].dstSubpass = VK_SUBPASS_EXTERNAL; d[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    d[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    d[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; d[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    uint32_t cm = 3;
    VkRenderPassCreateInfo2 rc; memset(&rc, 0, sizeof rc);
    rc.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2; rc.attachmentCount = 1; rc.pAttachments = &at; rc.subpassCount = 1; rc.pSubpasses = &sp;
    rc.dependencyCount = 2; rc.pDependencies = d; rc.correlatedViewMaskCount = 1; rc.pCorrelatedViewMasks = &cm;
    if (crp2(dev, &rc, NULL, &rqz.rp) != VK_SUCCESS) { ps2_log("vk: PS2_RQ_ZPRE: the pass could not be made"); return 0; }
    VkPipelineShaderStageCreateInfo st[2]; memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = rqz_vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = rqz_ck_fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin; memset(&vin, 0, sizeof vin); vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia; memset(&ia, 0, sizeof ia); ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vs; memset(&vs, 0, sizeof vs); vs.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vs.viewportCount = 1; vs.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs; memset(&rs, 0, sizeof rs); rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms; memset(&ms, 0, sizeof ms); ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo dss; memset(&dss, 0, sizeof dss); dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO; dss.depthTestEnable = VK_TRUE; dss.depthWriteEnable = VK_TRUE; dss.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; dss.maxDepthBounds = 1.0f;
    VkPipelineColorBlendStateCreateInfo cb; memset(&cb, 0, sizeof cb); cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    static const VkDynamicState dy[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsi; memset(&dsi, 0, sizeof dsi); dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; dsi.dynamicStateCount = 2; dsi.pDynamicStates = dy;
    VkGraphicsPipelineCreateInfo gp; memset(&gp, 0, sizeof gp);
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; gp.stageCount = 1; gp.pStages = st; gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia; gp.pViewportState = &vs;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &dss; gp.pColorBlendState = &cb; gp.pDynamicState = &dsi; gp.layout = rnb.layout; gp.renderPass = rqz.rp;
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &rqz.pipe) != VK_SUCCESS) { ps2_log("vk: PS2_RQ_ZPRE: the mesh pipeline could not be made"); return 0; }
    /* the coverage fill: the feed's alpha, full-screen, discard, depth 0.9975 */
    st[0].module = rqv_ds;
    gp.stageCount = 2;
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &rqz.ckp) != VK_SUCCESS) { ps2_log("vk: PS2_RQ_ZPRE: the coverage pipeline could not be made"); return 0; }
    rqz.ok = 1;
    rqpw_made("the depth pre-pass's pass and pipelines (PS2_RQ_ZPRE)", rqpw_t0);
    return 1;
}
/* a draw of the pre-pass: an opaque native mesh of the world that writes depth and covers the whole frame */
static int rqz_take(const vk_draw *d) {
    if (d->rt != 0u || d->native != PS2_VK_NATIVE_MESH || !rep_mesh || d->mblock >= rep_mesh->nblocks) return 0;
    if (d->cockpit || d->date || d->tex_self || d->mfd || d->hudblk || d->topology != PS2_VK_TRIANGLES) return 0;
    if (!(d->mcull & 2u) || !d->count || d->first + d->count > rep_mesh->nidx) return 0;
    if (!((d->depth >> 2) & 1u) || (d->depth & 3u) < 2u) return 0;     /* (writes depth, test GEQUAL / GREATER) */
    const ps2_vk_mesh_block *k = &rep_mesh->blocks[d->mblock];
    if (k->prog > 2u || (k->prog == 1u && k->pass != 0u)) return 0;    /* (programs 3, 4, 5: billboards, screen-space; overlays do not write depth anyway) */
    u32 f = d->frag_flags;
    if ((f >> 20) & 1u) return 0;                                      /* (the alpha test's fail half) */
    if (((f >> 3) & 1u) && ((f >> 4) & 7u) != 1u && !(d->tex != 0xFFFFu && !d->tex_rt && rnb_tex_opaque(d->tex))) return 0;   /* (an alpha test that can fail: punch-through texels are not depth) */
    return 1;
}
/* before the eye pass (the draw loop's hook): 1 = recorded */
static int rqz_pass(VkCommandBuffer cmd, vk_frame *fr, const u32 *order) {
    if (!rqz_cfg() || !rqe.ok || !rq_feed.created || !rnb.ok || !rep_mesh || !rqz_make()) return 0;
    int fi = (int)(fr - frames);
    if (fi < 0 || fi >= (int)FRAMES_IN_FLIGHT) return 0;
    vk_target *t = &rq_feed;
    u32 w = tew(t) * tsc(t), h = teh(t) * tsc(t);
    if (rqz.fbv != t->dview) {
        if (rqz.fb) vkDestroyFramebuffer(dev, rqz.fb, NULL);
        VkFramebufferCreateInfo ci; memset(&ci, 0, sizeof ci);
        ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO; ci.renderPass = rqz.rp; ci.attachmentCount = 1; ci.pAttachments = &t->dview; ci.width = w; ci.height = h; ci.layers = 1;
        if (vkCreateFramebuffer(dev, &ci, NULL, &rqz.fb) != VK_SUCCESS) { rqz.fb = VK_NULL_HANDLE; rqz.fbv = VK_NULL_HANDLE; return 0; }
        rqz.fbv = t->dview;
    }
    /* the records and indices, onto the frame's batch buffers (the eye pass's own batched draws go on after them) */
    gs_push head; memset(&head, 0, sizeof head);
    head.inv_size[0] = 2.0f / (float)rt_w; head.inv_size[1] = 2.0f / (float)rt_h;
    head.mesh_pad = (mv_shift ? 4u : 0u) | (xr_rp_on ? 256u : 0u) | behind_z_bit() | (rqlp.on ? RQ_LATE_BIT : 0u);   /* (PS2_XR_LATE_POSE: rq_zpre.vert) */
    u32 first = rnb.nidx[fi], nd = 0, sk = 0;
    u32 lim = rep_mesh->nrecs < MESH_MAX_VERTS ? rep_mesh->nrecs : MESH_MAX_VERTS;
    for (u32 q = 0; q < rep_ndraws; q++) {
        const vk_draw *d = &rep_draws[order ? order[q] : q];
        if (!rqz_take(d)) continue;
        if (d->scissor[0] > 0 || d->scissor[2] > 0 || d->scissor[1] + 1 < (s32)rt_w || d->scissor[3] + 1 < (s32)rt_h) { sk++; continue; }   /* (a partial scissor: one batch has one rectangle) */
        if (rnb.nrec[fi] >= RNB_MAX_REC || rnb.nidx[fi] + d->count > RNB_MAX_IDX) { sk++; continue; }
        u32 *dst = (u32 *)rnb.iptr[fi] + rnb.nidx[fi], hi = rnb.nrec[fi] << RNB_VTX_BITS, ok = 1;
        const u32 *src = rep_mesh->idx + d->first;
        for (u32 k = 0; k < d->count; k++) {
            u32 ix = src[k] + d->mvbase;
            if (ix >= lim) { ok = 0; break; }
            dst[k] = hi | ix;
        }
        if (!ok) { sk++; continue; }
        head.mesh_block = d->mblock;
        memcpy((u8 *)rnb.rptr[fi] + (size_t)rnb.nrec[fi] * RNB_REC, &head, RNB_HEAD);
        memset((u8 *)rnb.rptr[fi] + (size_t)rnb.nrec[fi] * RNB_REC + RNB_HEAD, 0, RNB_REC - RNB_HEAD);
        rnb.nrec[fi]++; rnb.nidx[fi] += d->count;
        nd++;
    }
    u32 nidx = rnb.nidx[fi] - first;
    rq_close_others(cmd);
    VkClearValue cv; memset(&cv, 0, sizeof cv);                /* (depth 0.0: reversed depth, as the eye pass's clear) */
    VkRenderPassBeginInfo bi; memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; bi.renderPass = rqz.rp; bi.framebuffer = rqz.fb; bi.renderArea.extent.width = w; bi.renderArea.extent.height = h; bi.clearValueCount = 1; bi.pClearValues = &cv;
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp; memset(&vp, 0, sizeof vp); vp.width = (float)w; vp.height = (float)h; vp.maxDepth = 1.0f;
    VkRect2D sc; memset(&sc, 0, sizeof sc); sc.extent.width = w; sc.extent.height = h;
    vkCmdSetViewport(cmd, 0, 1, &vp); vkCmdSetScissor(cmd, 0, 1, &sc);
    if (nidx) {
        memset(&head, 0, sizeof head);                         /* (the push constants: only rp / rp2 are read) */
        if (xr_rp_on) { memcpy(head.rp, xr_rp, sizeof head.rp); head.rp2[0] = xr_rp_alpha; head.rp2[2] = xr_unify_on() ? 1.0f : 0.0f; }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqz.pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rnb.layout, 2, 1, &fr->mset, 0, NULL);
        vkCmdPushConstants(cmd, rnb.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof head, &head);
        vkCmdBindIndexBuffer(cmd, rnb.ibuf[fi], 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, nidx, 1, first, 0, 0);
    }
    /* PS2_RQ_ZPRE_MARK=0 [test] (R-45b, outside note 5, 2026-11-10): no coverage fill from the feed's alpha mark. The mark is the
       PREVIOUS frame's cockpit coverage: with the head turning, the near clouds stop where the canopy frame was a frame ago (a dark
       ghost of the frame in a cloudy sky; coarse blocks in low-density bins). Without it the near clouds march behind the panel
       too and the composite's own depth test against this frame's eye depth hides them there. */
    static int mk = -1;
    if (mk < 0) { const char *e = getenv("PS2_RQ_ZPRE_MARK"); mk = !(e && *e == '0'); if (!mk) ps2_log("vk: PS2_RQ_ZPRE_MARK=0: no cockpit coverage fill in the depth pre-pass (R-45b test)"); }
    if (mk && rqz.mark_ok && rq_feed.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && rql.cp_want) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqz.ckp);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rnb.layout, 0, 1, &rq_feed.set, 0, NULL);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        rqz.n_ck++;
    }
    vkCmdEndRenderPass(cmd);
    t->dlayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    rqz.n_pass++; rqz.n_d += nd; rqz.n_i += nidx; rqz.n_sk += sk;
    if (!(rqz.n_pass % 1200u)) {
        ps2_log("rq zpre: %llu passes; a frame: %.0f mesh draws, %.0f indices, %.1f left out; coverage fill in %.0f %% of the frames", (unsigned long long)rqz.n_pass, rqz.n_d / 1200.0, rqz.n_i / 1200.0, rqz.n_sk / 1200.0, (double)rqz.n_ck * 100.0 / 1200.0);
        rqz.n_d = rqz.n_i = rqz.n_sk = 0.0; rqz.n_ck = 0;
    }
    return 1;
}
