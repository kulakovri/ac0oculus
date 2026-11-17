/* rq_rec.inc -- ACZ_QUEST_RECORD's GPU half (included by rq_video.inc; PS2_RQ_SUPPORT only; the encoder, the muxer and the
   microphone are runtime/src/android/ps2_qrec_android.c; NOTES_RQ.md "in-app recorder").
   The recorder's only GPU work is the NV12 conversion: one pass without attachments (rq_rec.frag, (W / 4) x (H / 2) fragments)
   samples a rectangle of a source image and writes the frame's NV12 bytes straight into one of the recorder's host-visible
   buffers. No blit, no copy, no intermediate image. Sources:
   - the eye pass (rq_eye_after, after the feed pass, while the swapchain image is still SHADER_READ_ONLY): the LEFT eye's
     layer through the feed pass's own descriptor set (its immutable subsampled sampler under the density map), a W x H
     rectangle centred on the view's forward direction (the centre of the game's symmetric frame), 1:1 eye pixels at
     ACZ_QUEST_RECORD_ZOOM=1 and at full dynamic resolution (PS2_RQ_DYNRES's rectangle mapped back);
   - the render target (the old stereo path's frames, the head-tracked single image, the screen frames: menus, movies,
     briefings): its display region (head-tracked: the same rectangle of the symmetric frame the eye path recorded).
   Pacing: at most one capture per game frame (a new list publication time), thinned to ACZ_QUEST_RECORD_FPS (30) by that
   time; a capture without a free host buffer is dropped and counted by the recorder (the renderer never waits). The buffer
   goes to the encoder after the frame's fence (rqr_frame_done). The device feature the shader needs
   (fragmentStoresAndAtomics) is enabled only with the recorder's video on: ACZ_QUEST_RECORD=0 or ACZ_QUEST_RECORD_VIDEO=0
   leave the GPU and the device exactly as before. */
int ps2_xr_session_visible(void);             /* (ps2_xr.c) the XR session is running and VISIBLE or FOCUSED */
#ifdef __ANDROID__
void ps2_qrec_xr_visible(int vis);            /* every frame: the XR session's visibility (a new file waits for it, R-47) */
int ps2_qrec_accepting(void);                 /* a session is open and takes video frames */
int ps2_qrec_size(u32 *w, u32 *h);            /* the encoder's frame size, once its first configuration succeeded */
int ps2_qrec_fps(void);
float ps2_qrec_zoom(void);
int ps2_qrec_nbuf(void);
void ps2_qrec_buffers(int n, u8 *const *ptrs, u32 w, u32 h);
int ps2_qrec_acquire(void);                   /* a free host buffer, -1 = none (a drop, counted) */
void ps2_qrec_cancel(int slot);
void ps2_qrec_ready(int slot, u64 t_ns);
void ps2_qrec_note(int kind);                 /* (statistics: 0 eye image, 1 render target) */
void ps2_qrec_video_failed(const char *why);
void ps2_qrec_stop(const char *why);
#else                                         /* (the PC test build of PS2_RQ_SUPPORT: no recorder) */
static inline void ps2_qrec_xr_visible(int vis) { (void)vis; }
static inline int ps2_qrec_accepting(void) { return 0; }
static inline int ps2_qrec_size(u32 *w, u32 *h) { (void)w; (void)h; return 0; }
static inline int ps2_qrec_fps(void) { return 30; }
static inline float ps2_qrec_zoom(void) { return 1.0f; }
static inline int ps2_qrec_nbuf(void) { return 0; }
static inline void ps2_qrec_buffers(int n, u8 *const *ptrs, u32 w, u32 h) { (void)n; (void)ptrs; (void)w; (void)h; }
static inline int ps2_qrec_acquire(void) { return -1; }
static inline void ps2_qrec_cancel(int slot) { (void)slot; }
static inline void ps2_qrec_ready(int slot, u64 t_ns) { (void)slot; (void)t_ns; }
static inline void ps2_qrec_note(int kind) { (void)kind; }
static inline void ps2_qrec_video_failed(const char *why) { (void)why; }
static inline void ps2_qrec_stop(const char *why) { (void)why; }
#endif
#define RQR_MAXB 16
static struct {
    int init, ok, failed, nb, coherent, eye_tried;
    u32 w, h;
    VkRenderPass rp; VkFramebuffer fb; VkShaderModule fs;
    VkDescriptorSetLayout outl, srcl; VkDescriptorPool pool;
    VkDescriptorSet out[RQR_MAXB], src[RQR_MAXB];
    VkPipelineLayout pl_eye, pl_rt; VkPipeline pipe_eye, pipe_rt;
    VkBuffer buf[RQR_MAXB]; VkDeviceMemory mem[RQR_MAXB]; u8 *map[RQR_MAXB];
    u64 last_t, next_t;
    float gx[2], gy[2]; int have_g;           /* the recorded rectangle of the game's symmetric frame (from the eye path) */
    int said_mv;
} rqr;
static void rqr_fail(const char *why) {
    rqr.failed = 1;
    ps2_log("rec (ACZ_QUEST_RECORD): the GPU capture could not be set up (%s): no video in the recording", why);
    ps2_qrec_video_failed(why);
}
static VkPipeline rqr_pipe(VkPipelineLayout pl) {
    VkPipelineShaderStageCreateInfo st[2]; memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = rqv_ds; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = rqr.fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin; memset(&vin, 0, sizeof vin); vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia; memset(&ia, 0, sizeof ia); ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vs; memset(&vs, 0, sizeof vs); vs.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vs.viewportCount = 1; vs.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs; memset(&rs, 0, sizeof rs); rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms; memset(&ms, 0, sizeof ms); ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendStateCreateInfo cb; memset(&cb, 0, sizeof cb); cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    static const VkDynamicState dy[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsi; memset(&dsi, 0, sizeof dsi); dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; dsi.dynamicStateCount = 2; dsi.pDynamicStates = dy;
    VkGraphicsPipelineCreateInfo gp; memset(&gp, 0, sizeof gp);
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vs; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pColorBlendState = &cb; gp.pDynamicState = &dsi;
    gp.layout = pl; gp.renderPass = rqr.rp; gp.subpass = 0;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev, pipe_cache, 1, &gp, NULL, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
    return p;
}
static VkPipelineLayout rqr_layout(VkDescriptorSetLayout set0) {
    VkDescriptorSetLayout sl[2] = { set0, rqr.outl };
    VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32 };
    VkPipelineLayoutCreateInfo pl; memset(&pl, 0, sizeof pl);
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pl.setLayoutCount = 2; pl.pSetLayouts = sl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pr;
    VkPipelineLayout l = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(dev, &pl, NULL, &l) != VK_SUCCESS) return VK_NULL_HANDLE;
    return l;
}
/* the buffers, the pass and the render target's pipeline: once the encoder has its size (1 = ready) */
static int rqr_init(void) {
    if (rqr.init) return rqr.ok;
    u32 w = 0, h = 0;
    if (!rqr_fsa || !ps2_qrec_size(&w, &h)) return 0;     /* (not yet: the recorder's thread configures the encoder first) */
    rqr.init = 1;
    if (!rqv_ds) { rqr_fail("rq_ds.vert not loaded"); return 0; }
    if ((w & 3u) || (h & 1u) || w < 64u || h < 64u) { rqr_fail("frame size"); return 0; }
    rqr.w = w; rqr.h = h;
    char sb[512], spv[600];
    snprintf(spv, sizeof spv, "%s/rq_rec.frag.spv", shader_dir(sb, sizeof sb));
    rqr.fs = load_spv(spv);
    if (!rqr.fs) { rqr_fail("rq_rec.frag.spv missing"); return 0; }
    rqr.nb = ps2_qrec_nbuf();
    if (rqr.nb > RQR_MAXB) rqr.nb = RQR_MAXB;
    if (rqr.nb < 2) { rqr_fail("no buffers"); return 0; }
    VkDescriptorSetLayoutBinding b; memset(&b, 0, sizeof b);
    b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo lc; memset(&lc, 0, sizeof lc);
    lc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; lc.bindingCount = 1; lc.pBindings = &b;
    if (vkCreateDescriptorSetLayout(dev, &lc, NULL, &rqr.outl) != VK_SUCCESS) { rqr_fail("set layout"); return 0; }
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    if (vkCreateDescriptorSetLayout(dev, &lc, NULL, &rqr.srcl) != VK_SUCCESS) { rqr_fail("set layout"); return 0; }
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, (u32)rqr.nb }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (u32)rqr.nb } };
    VkDescriptorPoolCreateInfo pc; memset(&pc, 0, sizeof pc);
    pc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; pc.maxSets = 2u * (u32)rqr.nb; pc.poolSizeCount = 2; pc.pPoolSizes = ps;
    if (vkCreateDescriptorPool(dev, &pc, NULL, &rqr.pool) != VK_SUCCESS) { rqr_fail("descriptor pool"); return 0; }
    /* host buffers: cached coherent memory if there is such (the recorder's thread reads every byte), else cached, else any */
    VkDeviceSize bytes = (VkDeviceSize)w * h * 3u / 2u;
    const VkMemoryPropertyFlags want[3] = {
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT };
    int kind = -1;
    for (int i = 0; i < rqr.nb; i++) {
        VkBufferCreateInfo bi; memset(&bi, 0, sizeof bi);
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; bi.size = bytes; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(dev, &bi, NULL, &rqr.buf[i]) != VK_SUCCESS) { rqr_fail("buffer"); return 0; }
        VkMemoryRequirements br; vkGetBufferMemoryRequirements(dev, rqr.buf[i], &br);
        VkMemoryAllocateInfo ai; memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = br.size; ai.memoryTypeIndex = 0xFFFFFFFFu;
        for (int k = kind < 0 ? 0 : kind; k < 3 && ai.memoryTypeIndex == 0xFFFFFFFFu; k++) {
            ai.memoryTypeIndex = find_mem(br.memoryTypeBits, want[k]);
            if (ai.memoryTypeIndex != 0xFFFFFFFFu) kind = k;
        }
        if (ai.memoryTypeIndex == 0xFFFFFFFFu || vkAllocateMemory(dev, &ai, NULL, &rqr.mem[i]) != VK_SUCCESS) { rqr_fail("host memory"); return 0; }
        vkBindBufferMemory(dev, rqr.buf[i], rqr.mem[i], 0);
        void *m = NULL;
        if (vkMapMemory(dev, rqr.mem[i], 0, VK_WHOLE_SIZE, 0, &m) != VK_SUCCESS) { rqr_fail("map"); return 0; }
        rqr.map[i] = (u8 *)m;
        VkDescriptorSetAllocateInfo da; memset(&da, 0, sizeof da);
        da.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; da.descriptorPool = rqr.pool; da.descriptorSetCount = 1; da.pSetLayouts = &rqr.outl;
        if (vkAllocateDescriptorSets(dev, &da, &rqr.out[i]) != VK_SUCCESS) { rqr_fail("sets"); return 0; }
        da.pSetLayouts = &rqr.srcl;
        if (vkAllocateDescriptorSets(dev, &da, &rqr.src[i]) != VK_SUCCESS) { rqr_fail("sets"); return 0; }
        VkDescriptorBufferInfo dbi; memset(&dbi, 0, sizeof dbi); dbi.buffer = rqr.buf[i]; dbi.range = VK_WHOLE_SIZE;
        VkWriteDescriptorSet wr; memset(&wr, 0, sizeof wr);
        wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr.dstSet = rqr.out[i]; wr.dstBinding = 0; wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr.pBufferInfo = &dbi;
        vkUpdateDescriptorSets(dev, 1, &wr, 0, NULL);
    }
    rqr.coherent = kind != 1;
    /* the pass: no attachments, (W / 4) x (H / 2) */
    VkSubpassDescription sp; memset(&sp, 0, sizeof sp);
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    VkRenderPassCreateInfo rc; memset(&rc, 0, sizeof rc);
    rc.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO; rc.subpassCount = 1; rc.pSubpasses = &sp;
    if (vkCreateRenderPass(dev, &rc, NULL, &rqr.rp) != VK_SUCCESS) { rqr_fail("render pass"); return 0; }
    VkFramebufferCreateInfo fi; memset(&fi, 0, sizeof fi);
    fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO; fi.renderPass = rqr.rp; fi.width = w / 4u; fi.height = h / 2u; fi.layers = 1;
    if (vkCreateFramebuffer(dev, &fi, NULL, &rqr.fb) != VK_SUCCESS) { rqr_fail("framebuffer"); return 0; }
    rqr.pl_rt = rqr_layout(rqr.srcl);
    rqr.pipe_rt = rqr.pl_rt ? rqr_pipe(rqr.pl_rt) : VK_NULL_HANDLE;
    if (!rqr.pipe_rt) { rqr_fail("pipeline"); return 0; }
    ps2_qrec_buffers(rqr.nb, rqr.map, w, h);
    rqr.ok = 1;
    ps2_log("rec (ACZ_QUEST_RECORD): GPU capture %ux%u NV12 by rq_rec.frag (%ux%u fragments, no attachments), %d host buffers of %.1f MB (%s)",
            w, h, w / 4u, h / 2u, rqr.nb, (double)bytes / 1048576.0, kind == 0 ? "cached coherent" : kind == 1 ? "cached" : "uncached");
    return 1;
}
/* the eye pass's pipeline: the feed pass's set layout (its immutable subsampled sampler) as set 0 */
static int rqr_eye_pipe(void) {
    if (rqr.pipe_eye) return 1;
    if (rqr.eye_tried || !rqe.ok || !rqe.dsdsl) return 0;
    rqr.eye_tried = 1;
    const u64 rqpw_t0 = vk_mono_ns();
    rqr.pl_eye = rqr_layout(rqe.dsdsl);
    rqr.pipe_eye = rqr.pl_eye ? rqr_pipe(rqr.pl_eye) : VK_NULL_HANDLE;
    if (!rqr.pipe_eye) { ps2_log("rec (ACZ_QUEST_RECORD): the eye image's pipeline could not be made: eye frames not recorded"); return 0; }
    rqpw_made("the recorder's eye pipeline (ACZ_QUEST_RECORD)", rqpw_t0);
    return 1;
}
/* this frame records a picture: the recorder takes frames, a game frame not recorded yet, the rate's turn */
static int rqr_due(vk_frame *fr) {
    if (rqr.failed || fr->rqr_slot1) return 0;
    if (!ps2_xr_session_visible()) return 0;      /* (R-47: no capture while the session stops, idles or starts again) */
    if (!ps2_qrec_accepting()) return 0;
    if (!rqr_fsa) { rqr_fail("the Vulkan device has no fragmentStoresAndAtomics"); return 0; }
    u64 t = present_rx_t;
    if (!t || t <= rqr.last_t) return 0;
    int fps = ps2_qrec_fps();
    if (fps > 0 && fps < 59) {
        u64 P = 1000000000ull / (u64)fps;
        if (rqr.next_t && t + P / 4u < rqr.next_t) return 0;
        rqr.next_t = !rqr.next_t || t > rqr.next_t + P ? t + P : rqr.next_t + P;
    }
    rqr.last_t = t;
    return rqr_init();
}
static void rqr_draw(VkCommandBuffer cmd, vk_frame *fr, int s, VkPipeline pipe, VkPipelineLayout pl, VkDescriptorSet src,
                     const float m[4], const float sc[2]) {
    VkRenderPassBeginInfo bi; memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; bi.renderPass = rqr.rp; bi.framebuffer = rqr.fb;
    bi.renderArea.extent.width = rqr.w / 4u; bi.renderArea.extent.height = rqr.h / 2u;
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp; memset(&vp, 0, sizeof vp); vp.width = (float)(rqr.w / 4u); vp.height = (float)(rqr.h / 2u); vp.maxDepth = 1.0f;
    VkRect2D sc2; memset(&sc2, 0, sizeof sc2); sc2.extent = bi.renderArea.extent;
    vkCmdSetViewport(cmd, 0, 1, &vp); vkCmdSetScissor(cmd, 0, 1, &sc2);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkDescriptorSet sets[2] = { src, rqr.out[s] };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 2, sets, 0, NULL);
    struct { float m[4]; float sc[2]; u32 w, h; } p;
    memcpy(p.m, m, sizeof p.m); p.sc[0] = sc[0]; p.sc[1] = sc[1]; p.w = rqr.w; p.h = rqr.h;
    vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, &p);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    /* the bytes to the host after the fence; ALL_COMMANDS: a later layout change of the source waits for these reads */
    VkBufferMemoryBarrier bb; memset(&bb, 0, sizeof bb);
    bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER; bb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bb.buffer = rqr.buf[s]; bb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         0, NULL, 1, &bb, 0, NULL);
    fr->rqr_slot1 = s + 1; fr->rqr_t = rqr.last_t;
}
/* rq_eye_after, right after the feed pass and only when it ran: the left eye of the eye pass (its swapchain image SHADER_READ_ONLY,
   through the very set the feed pass just read, rqe.dsset[rqe.idx]: this frame's acquired image) */
static void rqr_grab_eye(VkCommandBuffer cmd, vk_frame *fr) {
    if (!rqr_due(fr) || !rqr_eye_pipe()) return;
    float W = (float)rqe.w, H = (float)rqe.h;
    const float *c = rqe.crop[0];
    if (W < 16.0f || H < 16.0f || c[1] - c[0] < 1e-3f || c[3] - c[2] < 1e-3f) return;
    float z = ps2_qrec_zoom();
    float hu = (float)rqr.w / z / (2.0f * W), hv = (float)rqr.h / z / (2.0f * H), k = 1.0f;
    if (hu > 0.5f) k = 0.5f / hu;
    if (hv * k > 0.5f) k = 0.5f / hv;
    hu *= k; hv *= k;
    float cu = (0.5f - c[0]) / (c[1] - c[0]), cv = (0.5f - c[2]) / (c[3] - c[2]);   /* (the forward direction in the left eye image) */
    if (cu < hu) cu = hu;
    if (cu > 1.0f - hu) cu = 1.0f - hu;
    if (cv < hv) cv = hv;
    if (cv > 1.0f - hv) cv = 1.0f - hv;
    float m[4] = { cu - hu, 2.0f * hu, cv - hv, 2.0f * hv };
    float sc[2] = { (float)rqd.vw / W, (float)rqd.vh / H };
    rqr.gx[0] = c[0] + m[0] * (c[1] - c[0]); rqr.gx[1] = rqr.gx[0] + m[1] * (c[1] - c[0]);
    rqr.gy[0] = c[2] + m[2] * (c[3] - c[2]); rqr.gy[1] = rqr.gy[0] + m[3] * (c[3] - c[2]);
    rqr.have_g = 1;
    int s = ps2_qrec_acquire();
    if (s < 0 || s >= rqr.nb) { if (s >= 0) ps2_qrec_cancel(s); return; }
    rqr_draw(cmd, fr, s, rqr.pipe_eye, rqr.pl_eye, rq_eye_src_set(), m, sc);   /* (PS2_RQ_FEED_DECODE: the decoded plain image, R-45) */
    ps2_qrec_note(0);
}
/* a render target's display region x0, y0, w, h (target pixels), layer 0; tracked: a head-tracked image (the eye path's rectangle) */
static void rqr_grab_rt(VkCommandBuffer cmd, vk_frame *fr, vk_target *t, u32 x0, u32 y0, u32 w, u32 h, int tracked) {
    if (!t->created || !t->cview || !w || !h || t->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) return;
    if (!vk_mv) {                               /* (the shader reads a 2D array view: the targets have one with multiview only) */
        if (!rqr.said_mv++) ps2_log("rec (ACZ_QUEST_RECORD): render-target frames need PS2_XR_MULTIVIEW=1: not recorded");
        return;
    }
    if (!rqr_due(fr)) return;
    float TW = (float)(tew(t) * tsc(t)), TH = (float)(teh(t) * tsc(t));
    float rx = (float)x0, ry = (float)y0, rw = (float)w, rh = (float)h;
    if (tracked && rqr.have_g) {
        rx = (float)x0 + rqr.gx[0] * (float)w; rw = (rqr.gx[1] - rqr.gx[0]) * (float)w;
        ry = (float)y0 + rqr.gy[0] * (float)h; rh = (rqr.gy[1] - rqr.gy[0]) * (float)h;
    }
    float m[4] = { rx / TW, rw / TW, ry / TH, rh / TH }, sc[2] = { 1.0f, 1.0f };
    int s = ps2_qrec_acquire();
    if (s < 0 || s >= rqr.nb) { if (s >= 0) ps2_qrec_cancel(s); return; }
    VkDescriptorImageInfo ii; memset(&ii, 0, sizeof ii);
    ii.sampler = sampler; ii.imageView = t->cview; ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet wr; memset(&wr, 0, sizeof wr);           /* (slot s is free: no frame in flight reads its set) */
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr.dstSet = rqr.src[s]; wr.dstBinding = 0; wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(dev, 1, &wr, 0, NULL);
    rqr_draw(cmd, fr, s, rqr.pipe_rt, rqr.pl_rt, rqr.src[s], m, sc);
    ps2_qrec_note(1);
}
/* the frame's fence signalled (or the device is idle at the end): its picture to the encoder */
static void rqr_frame_done(vk_frame *fr, int ok) {
    ps2_qrec_xr_visible(ps2_xr_session_visible());   /* (every frame, at its fence: the recorder opens a new file only in a shown session) */
    if (!fr->rqr_slot1) return;
    int s = fr->rqr_slot1 - 1;
    fr->rqr_slot1 = 0;
    if (!ok) { ps2_qrec_cancel(s); return; }
    if (!rqr.coherent) {
        VkMappedMemoryRange mr; memset(&mr, 0, sizeof mr);
        mr.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE; mr.memory = rqr.mem[s]; mr.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(dev, 1, &mr);
    }
    ps2_qrec_ready(s, fr->rqr_t);
}
/* the renderer's end (the device idle, the last pictures handed over): the recorder finishes the file */
static void rqr_end(void) {
    ps2_qrec_stop("the renderer ended");
}
