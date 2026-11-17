/* rq_late.inc -- PS2_XR_LATE_POSE=1 (default off; the end-of-run review's item 2b, agent latency; included by ps2_video.c after rq_video.inc, PS2_RQ_SUPPORT only).
   A game frame is built with the head pose of the renderer's previous frame (ps2_xr_game_frame hands the game xr_latest), so the eye image is one to two fields old on
   the head's translation (the compositor corrects rotation only). Right before the eye pass of a head-tracked stereo frame the views are located again for this frame's
   predicted display time (ps2_xr_late_pose), and the difference between the head the game built the frame with and the new one goes into the frame's views:
   - the world (GS draws, gs.vert's reprojection with CINE_BIT: the push constants rp / rp2.w, the machinery of the recording's cinematic camera; native meshes,
     mesh.vert -DRQ and rq_zpre.vert: the reserved block RQ_LATE_BLOCK with the same four rows under mesh_pad bit RQ_LATE_BIT, because the merged water's push constants
     hold the water's numbers in rp);
   - the HUD and the world's effects in the batch (rq_hud.inc: the host-built vertices move on the CPU, and the DATE rectangles with them);
   - the Quest cockpit (cpit_pose reads rqlp.q / rqlp.p instead of the tag's head);
   - our clouds (cl_view_fill: cl_view_make's in-between branch, xr_rp = the same four rows);
   - the projection layer's pose (ps2_xr_rq_present: the late located eyes).
   The aircraft is fixed between the two poses (one game frame), so the head's difference in the cockpit frame is the whole motion: Rd = H_late^T H_tag, t = -H_late^T (p_late - p_tag) k,
   the formulas of cine_track with Rc = Rb H_late (k = ACZ_VR_UNITS_PER_M x ACZ_VR_STEREO_SCALE). Frames in which the head does not turn the game's camera (the chase view with stick head
   look) keep the tag's pose. */
extern int ps2_xr_late_pose(u32 seq, float q[4], float p[3]);
extern void ps2_xr_late_clear(void);
extern int ps2_xr_camera_base_of(u32 seq, float eye[3], float rot[9]);
static void cq_mat(const float q[4], float H[9]);            /* (defined with the cinematic camera) */
static struct { u64 n_on, n_off; double s_deg, s_mm; float m_deg, m_mm; u32 win; int dbg; } rqls = { 0, 0, 0.0, 0.0, 0.0f, 0.0f, 0u, -1 };
static float rqlp_kupm(void) {
    static float k = -1.0f; static unsigned mg;
    if (ps2_menv_take(&mg, mg_rt) | (k < 0.0f)) {
        const char *e = getenv("ACZ_VR_UNITS_PER_M");
        k = (e ? (float)atof(e) : 6.0f) * mg_sscale();
        if (!(k > 0.0f)) k = 6.0f;
    }
    return k;
}
/* before the frame's render_list (draw_and_present): the pose and the view constants; rqlp.on = 0 when this frame keeps the tag's pose */
static void rqlp_begin(vk_frame *fr, u32 seq) {
    rqlp.on = 0;
    ps2_xr_late_clear();
    if (rq_on() != 3 || xr_rp_on || !vk_mv || !fr->mcptr || !seq) return;
    float e1[3], R1[9], f[2], c[2], qt[4], pt[3], ipd, be[3], Rb[9], a, cc, mh;
    if (!ps2_xr_camera_of(seq, e1, R1, f, c) || !ps2_xr_head_of(seq, qt, pt, &ipd) || !ps2_xr_camera_base_of(seq, be, Rb) || !(f[0] > 1.0f) || !(f[1] > 1.0f)) return;
    if (!ps2_xr_shift_of(seq, &a, &cc, &mh)) mh = 0.0f;
    float Hq[9], RbT[9], Hd[9], dm = 0.0f;
    cq_mat(qt, Hq); m3_tr(Rb, RbT); m3_mul(RbT, R1, Hd);
    for (int i = 0; i < 9; i++) dm = fmaxf(dm, fabsf(Hd[i] - Hq[i]));
    if (!(dm < 3e-3f)) { rqls.n_off++; return; }             /* (the head does not turn this frame's camera) */
    float q[4], p[3];
    if (!ps2_xr_late_pose(seq, q, p)) { rqls.n_off++; return; }
    float H[9], HT[9], Rd[9];
    cq_mat(q, H); m3_tr(H, HT); m3_mul(HT, Hq, Rd);
    float k = rqlp_kupm(), dp[3] = { (p[0] - pt[0]) * k, (p[1] - pt[1]) * k, (p[2] - pt[2]) * k }, t[3];
    for (int i = 0; i < 3; i++) t[i] = -(HT[i * 3] * dp[0] + HT[i * 3 + 1] * dp[1] + HT[i * 3 + 2] * dp[2]);
    for (int i = 0; i < 3; i++) { rqlp.rp[i * 4] = Rd[i * 3]; rqlp.rp[i * 4 + 1] = Rd[i * 3 + 1]; rqlp.rp[i * 4 + 2] = Rd[i * 3 + 2]; rqlp.rp[12 + i] = t[i]; }
    rqlp.rp[3] = c[0] - present_frame_off[0]; rqlp.rp[7] = c[1] - present_frame_off[1];
    rqlp.rp[11] = f[0]; rqlp.rp[15] = f[1];
    rqlp.inv_mh = mh > 1e-6f ? 1.0f / mh : 0.0f;
    memcpy(rqlp.q, q, sizeof rqlp.q); memcpy(rqlp.p, p, sizeof rqlp.p);
    memcpy(((ps2_vk_mesh_block *)fr->mcptr + RQ_LATE_BLOCK)->m, rqlp.rp, sizeof rqlp.rp);
    rqlp.on = 1;
    {   /* the statistics (one line every 600 frames) and PS2_XR_LATE_POSE_DEBUG=1: every frame's delta */
        float tr = Rd[0] + Rd[4] + Rd[8];
        float deg = acosf(fminf(1.0f, fmaxf(-1.0f, 0.5f * (tr - 1.0f)))) * 57.29578f;
        float mm = sqrtf((p[0] - pt[0]) * (p[0] - pt[0]) + (p[1] - pt[1]) * (p[1] - pt[1]) + (p[2] - pt[2]) * (p[2] - pt[2])) * 1000.0f;
        rqls.n_on++; rqls.s_deg += deg; rqls.s_mm += mm; if (deg > rqls.m_deg) rqls.m_deg = deg; if (mm > rqls.m_mm) rqls.m_mm = mm;
        if (rqls.dbg < 0) { const char *e = getenv("PS2_XR_LATE_POSE_DEBUG"); rqls.dbg = e && *e == '1'; }
        if (rqls.dbg) ps2_log("rq-late: seq %u: the head moved %.3f deg, %.2f mm since the game frame's pose; q %.5f %.5f %.5f %.5f p %.4f %.4f %.4f (tag q %.5f %.5f %.5f %.5f p %.4f %.4f %.4f)", seq, (double)deg, (double)mm, (double)q[0], (double)q[1], (double)q[2], (double)q[3], (double)p[0], (double)p[1], (double)p[2], (double)qt[0], (double)qt[1], (double)qt[2], (double)qt[3], (double)pt[0], (double)pt[1], (double)pt[2]);
        if (++rqls.win >= 600u) {
            ps2_log("rq-late: %llu frames with the late pose, %llu kept the tag's: the head moved since the game frame's pose by avg %.2f deg, %.1f mm (max %.2f deg, %.1f mm)", (unsigned long long)rqls.n_on, (unsigned long long)rqls.n_off, rqls.s_deg / (double)rqls.win, rqls.s_mm / (double)rqls.win, (double)rqls.m_deg, (double)rqls.m_mm);
            rqls.win = 0; rqls.s_deg = rqls.s_mm = 0.0; rqls.m_deg = rqls.m_mm = 0.0f;
        }
    }
}
/* a point (game frame pixels) with inverse view depth iw, as the late view sees it (gs.vert's reproject) */
static inline void rqlp_xy(float *x, float *y, float iw) {
    float d[3] = { (*x - rqlp.rp[3]) / rqlp.rp[11], -(*y - rqlp.rp[7]) / rqlp.rp[15], -1.0f };
    float q[3];
    for (int i = 0; i < 3; i++) q[i] = rqlp.rp[i * 4] * d[0] + rqlp.rp[i * 4 + 1] * d[1] + rqlp.rp[i * 4 + 2] * d[2] + rqlp.rp[12 + i] * iw;
    if (!(-q[2] > 1e-4f)) return;
    *x = rqlp.rp[3] + rqlp.rp[11] * q[0] / -q[2];
    *y = rqlp.rp[7] - rqlp.rp[15] * q[1] / -q[2];
}
/* a GS vertex of the batch (position, its xr_iw and eye shift): gs.vert's choice of the depth with CINE_BIT */
static inline void rqlp_vert(float *x, float *y, float iw, float dx) {
    if (iw < 0.0f) iw = fmaxf(-dx * rqlp.inv_mh, 0.0f);
    rqlp_xy(x, y, iw);
}
/* a partial scissor of a moved draw moves with it (the rectangle's centre, at infinity: the rotation's part; a whole-frame scissor stays) */
static inline void rqlp_scissor(VkRect2D *sc, u32 dsc) {
    if (sc->offset.x <= 0 && sc->offset.y <= 0 && sc->offset.x + (s32)sc->extent.width >= (s32)(rt_w * dsc) && sc->offset.y + (s32)sc->extent.height >= (s32)(rt_h * dsc)) return;
    float cx = ((float)sc->offset.x + 0.5f * (float)sc->extent.width) / (float)dsc, cy = ((float)sc->offset.y + 0.5f * (float)sc->extent.height) / (float)dsc, x = cx, y = cy;
    rqlp_xy(&x, &y, 0.0f);
    sc->offset.x += (s32)lroundf((x - cx) * (float)dsc);
    sc->offset.y += (s32)lroundf((y - cy) * (float)dsc);
}
