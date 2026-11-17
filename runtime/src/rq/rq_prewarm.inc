/* rq_prewarm.inc -- PS2_RQ_PREWARM (R-64, 2026-11-11; included by ps2_video.c after the other rq parts, PS2_RQ_SUPPORT only).
   The hand-over from a static screen (an intro, a menu: the true old path, PS2_RQ_KINDS) to the first head-tracked frame made the
   eye pass's whole world in that one frame: the device's VrApi showed FPS 37-45 / 72 with 22-42 stale frames in the hand-over
   second (t8, t8a, t9c), 12 / 72 with 72 stale with glass3's readable canopy (t9e). What the first eye frame made (simulator logs,
   NOTES_RQ R-64):
   - the eye pass (rq_eye_make: its render pass and the feed's, the 4x colour and depth (transient, lazily allocated memory), the
     feed's pipeline; with PS2_RQ_FDM our density map), swapchain 6 (ps2_xr_rq_acquire: xrCreateSwapchain, 3 images, views), a
     framebuffer and a feed set per swapchain image (the first three eye frames), our density map's fill and its upload with a
     queue wait (PS2_RQ_FDM_OWN);
   - the folded terrain's layouts (rqt_make), the HUD batch's sets and buffers (rqh_init) and its pipelines (rqh_pipe), the clouds'
     composite set and pipelines (rqcl_make / rqcl_pipe), the layers' pipelines (rql_pipes), the depth pre-pass (rqz_make), the
     alpha mask's pipeline (PS2_RQ_ZPRE_AMASK), the cockpit material's ~20 pipelines with the readable glass's (rqc_pipes ->
     rqgx_pipes: base, near-sun, dust, cracks, the cracks' buffers) and the screens' (rqco_pipes), the eye adaptation's pipeline
     (at its first exposure step), the scene light's read-back buffers, lean2's blur chain (at the first wet frame), the
     recorder's eye pipeline (ACZ_QUEST_RECORD);
   - every GS / mesh pipeline state of the eye pass (key bit 46 / 47): the startup warm-up builds the remembered states before any
     frame, when the eye pass does not exist yet, so pipe_build refused all of them and the first eye frames built each on demand.
   PS2_RQ_PREWARM=1 makes these before the first eye frame, in two steps, while static frames run on their old path unchanged:
   A. on the renderer thread, once the XR session runs in stereo with the eye size known, slot 0 and the feed exist and the
      startup pipeline warm-up has finished (the session's start: the logos, the title; seconds after an APK update): the eye pass, swapchain 6 (ps2_xr_rq_prepare: made, not acquired, named by no layer
      until the first eye frame: the compositor's picture does not change), the framebuffers and feed sets of all its images, the
      density map from the eyes' predicted fields (uploaded with its queue wait now), the terrain's layouts, the clouds' set, the
      HUD batch's buffers, the scene light's buffers -- objects, not compiles (a few ms; lean2's blur chain stays at its first wet
      frame: its size follows the flight's feed, which a mission start may still change);
   B. on one worker thread (the compiles): the pipelines above, then the startup warm-up's refused eye-pass states (kept from the
      warm-up, built with pipe_build as its workers do; the renderer inserts them as they finish). Each item starts only once what
      it reads is final (the cockpit's: the layouts, the lean wet canopy's and the wear texture's bakes of the first model with
      glass; the keys: the warm-up done). The first eye frame stops the worker between items and waits for the one it is on
      (rq_prewarm_join: logged), so nothing races; whatever is left is made lazily as before. Nothing is ever made twice: every
      maker keeps its object, and a pause or a menu (eye -> static -> eye) finds them all (the `rq eye start` line of the next
      eye frame counts what it made: 0).
   Memory held from the session's start instead of from the first flight: swapchain 6 (3 images of 2 x W x H RGBA8: 71 MB at
   1680 x 1760, the compositor's), the HUD batch's buffers, the cracks' buffers (3 x 0.76 MB), the scene light's buffers; the 4x
   attachments are transient and lazily allocated (no memory until a pass uses them on the device; 180 MB device-local on the
   simulator, whose GPU has no lazily allocated type). 0 (the default in code): as before. The Quest default 1
   (android/ps2_android.c defaults()).
   Always on with PS2_RQ=eye (either setting): `rq made:` names every lazily made object with its time and where it was made
   ("lazily" = in a frame), and `rq eye start` gives each start of eye frames (the first, and each after >= 1 s without one): the
   first eye frame's renderer time and what it made, and the next 60 frames' worst time, frames over 1.5 refresh periods and what
   they made -- the simulator's evidence; on the device the VrApi line of the hand-over second is the judge. */
extern int ps2_xr_rq_prepare(VkImageView *views, uint32_t cap);
extern void ps2_xr_rq_fov_tan(int eye, float t[4]);
extern long long ps2_xr_display_period(void);
extern int ps2_xr_session_running(void);
enum { RQPW_CPIT = 1, RQPW_HUD = 2, RQPW_CL = 4, RQPW_LAYERS = 8, RQPW_ZPRE = 16, RQPW_AMASK = 32, RQPW_EXPO = 64, RQPW_REC = 128, RQPW_KEYS = 256 };
static struct {
    int cfg;                                  /* PS2_RQ_PREWARM: -1 unread */
    int a_state;                              /* step A: 0 not yet, 1 made, -1 not possible (logged) */
    int in_a;                                 /* (step A is running: rqpw_made's place) */
    int flown;                                /* the first eye frame came: nothing more is scheduled */
    volatile int busy, cancel;                /* the worker runs / is asked to stop between items */
    u32 job;                                  /* this run's items */
    volatile u32 done;                        /* the items made so far (all runs; the worker's bits) */
    u32 tried;                                /* the items handed to a worker already (each once) */
    u64 *keys; u32 nkeys, kfrom, kto, ktaken; VkPipeline *kout; volatile u32 kbuilt;   /* the warm-up's eye-pass states */
    u32 kins;                                 /* of them inserted into pipes[] */
    u32 fr_n, fr_od; double fr_ms, fr_od_ms;  /* this frame (renderer thread): made lazily, states built on demand */
    u32 od_logged;
    int eye_now, win; u32 starts, n, over, worst_at, field0, f_n, f_od, w_n, w_od;
    double first_ms, first_iv, worst, sum, ivmax, f_ms, f_od_ms, w_ms, w_od_ms;
    u64 prev_t0, last_eye;
    int bg;                                   /* PS2_RQ_PREWARM_BG: -1 unread */
    volatile int in_keys, icancel;            /* (the worker builds the states now; the items asked to stop, the states go on) */
    volatile int kbusy;                       /* (PS2_RQ_PREWARM_BG: the states' own workers running) */
    volatile u32 knext, kfin;                 /* (the next state a worker takes; states finished) */
    u8 *kdone;                                /* per state: 1 built (or refused) by a worker, 2 taken into pipes[] */
    int early, nthr, test_ms, hold_ms;        /* PS2_RQ_PREWARM_EARLY, _THREADS, _TEST_MS [test], _HOLD_MS (-1 unread) */
    u64 hold_t0, kt0; u32 hold_n; int hold_done;
} rqpw = { -1, .bg = -1, .early = -1, .nthr = -1, .test_ms = -1, .hold_ms = -1 };
static __thread const char *rqpw_ctx;        /* (the worker's thread: where rqpw_made was called) */
static int rqpw_cfg(void) {
    if (rqpw.cfg < 0) {
        const char *e = getenv("PS2_RQ_PREWARM");
        rqpw.cfg = e && *e == '1';
        ps2_log("vk: PS2_RQ_PREWARM=%d: %s", rqpw.cfg, rqpw.cfg ? "the eye pass's objects and pipelines made before the first eye frame (the renderer at the session's start, then one worker thread)"
                                                             : "the eye pass's objects and pipelines made lazily by the first eye frames");
    }
    return rqpw.cfg;
}
/* a maker's note (each object once): what, how long, where */
static void rqpw_made(const char *what, u64 t0) {
    double ms = (double)(vk_mono_ns() - t0) / 1e6;
    const char *where = rqpw_ctx ? rqpw_ctx : rqpw.in_a ? "PS2_RQ_PREWARM, the renderer before the first eye frame" : "lazily, in a frame";
    if (!rqpw_ctx && !rqpw.in_a) { rqpw.fr_n++; rqpw.fr_ms += ms; }
    ps2_log("rq made: %s in %.2f ms (%s; field %u)", what, ms, where, ps2_gs_frame_count);
}
/* get_pipeline_ex (renderer thread): a state built on demand */
static void rqpw_ondemand(u64 key, u64 ns) {
    if (((key >> 40) & 0xC0u) && rqpw_test_ms()) { SDL_Delay((u32)rqpw_test_ms()); ns += (u64)rqpw_test_ms() * 1000000ull; }   /* (PS2_RQ_PREWARM_TEST_MS [test]) */
    rqpw.fr_od++; rqpw.fr_od_ms += (double)ns / 1e6;
    if (((key >> 40) & 0xC0u) && rqpw.od_logged < 48u) {
        rqpw.od_logged++;
        ps2_log("rq made: eye-pass pipeline state %016llx built on demand in %.1f ms (field %u)", (unsigned long long)key, (double)ns / 1e6, ps2_gs_frame_count);
    }
}
/* the startup warm-up (renderer thread): an eye-pass state it could not build (the pass did not exist yet) is kept here, built
   by the worker once it does (1); 0 = not one (or the switch off: dropped as before) */
static int rqpw_defer(u64 k) {
    if (!((k >> 40) & 0xC0u) || !rqpw_cfg()) return 0;
    if (!rqpw.keys) {
        rqpw.keys = (u64 *)malloc(PIPE_MAX * sizeof *rqpw.keys);
        rqpw.kout = (VkPipeline *)calloc(PIPE_MAX, sizeof *rqpw.kout);
        rqpw.kdone = (u8 *)calloc(PIPE_MAX, 1);
        if (!rqpw.keys || !rqpw.kout || !rqpw.kdone) {
            free(rqpw.keys); free(rqpw.kout); free(rqpw.kdone); rqpw.keys = NULL; rqpw.kout = NULL; rqpw.kdone = NULL; return 0;
        }
    }
    if (rqpw.nkeys >= PIPE_MAX) return 0;
    rqpw.keys[rqpw.nkeys++] = k;
    return 1;
}
static u32 rqpw_nkeys(void) { return rqpw.nkeys; }
/* pipe_cache_keys: the kept states that were not built (no eye pass this session) stay in the key list */
static void rqpw_keys_add(u64 *keys, u32 *count) {
    for (u32 i = 0; i < rqpw.nkeys && *count < PIPE_MAX; i++) {
        u32 j;
        for (j = 0; j < *count && keys[j] != rqpw.keys[i]; j++) ;
        if (j == *count) keys[(*count)++] = rqpw.keys[i];
    }
}
/* PS2_RQ_PIPE_KEYS=<path> (rq4_water 2026-11-11, PROMPT_2026-11-11_flights2_fixes G.2, BUGS_RQ R-91; unset = none, as before; the
   Quest default shaders/rq_pipelines.keys): a key list in the cache's .keys format that ships with the build -- tools/quest/
   rq_pipelines.keys, packed by tools/quest/build_game.sh into the APK's assets/shaders/, which ps2_android.c unpacks into the files
   dir. Its states join the startup warm-up's list after this device's own, without duplicates, so a fresh install (no .keys, no
   cache) builds them before use, and a state this device never met (the user's M4: 8043c22d0044001e, a far water patch with pass
   3's layer and the haze, 243.7 ms on demand at field 3470) is built before the flight needs it. The list's eye-pass states are kept
   by rqpw_defer and built by this file's worker once the eye pass exists (PS2_RQ_PREWARM; PS2_RQ_PREWARM_BG lets that run on past
   the first eye frame). tools/quest/rq_keys.py makes the list: the device's and the simulator's key files of the missions (with the
   device's key bits: PS2_VRS_FOV=0, PS2_PERF_DYN_BLEND=0) and every class of the merged terrain / water shader. */
static void rqpw_shipped(void) {
    const char *e = getenv("PS2_RQ_PIPE_KEYS");
    if (!e || !*e) return;
    size_t n = 0;
    void *data = pipe_read_file(e, &n);
    u32 count = 0;
    if (!data || n < 16u || memcmp(data, PIPE_KEYS_MAGIC, 8) || (memcpy(&count, (const u8 *)data + 8, 4), count > PIPE_MAX) || 16u + (size_t)count * 8u > n) {
        ps2_log("vk: PS2_RQ_PIPE_KEYS=%s: no key list there (%zu bytes)", e, n);
        free(data);
        return;
    }
    u64 *w = (u64 *)realloc(pipe_warm, (size_t)(pipe_warm_n + count + 1u) * 8u);
    if (!w) { free(data); return; }
    pipe_warm = w;
    u32 added = 0, eye = 0, had = pipe_warm_n;
    for (u32 i = 0; i < count && pipe_warm_n < PIPE_MAX; i++) {
        u64 k; memcpy(&k, (const u8 *)data + 16 + (size_t)i * 8u, 8);
        u32 j;
        for (j = 0; j < pipe_warm_n && pipe_warm[j] != k; j++) ;
        if (j < pipe_warm_n) continue;
        pipe_warm[pipe_warm_n++] = k; added++;
        if ((k >> 40) & 0xC0u) eye++;
    }
    free(data);
    ps2_log("vk: PS2_RQ_PIPE_KEYS=%s: %u shipped pipeline states, %u of them new to this device's %u (%u for the eye pass): %u to build ahead of use", e, count, added, had, eye, pipe_warm_n);
}
/* PS2_RQ_PREWARM_EARLY=1 (rq4_water 2026-11-11, the coordinator's device runs of t10a, BUGS_RQ R-91; 0 = as before, the Quest
   default 1): the eye-pass states of the key lists (the device's own and PS2_RQ_PIPE_KEYS') leave the startup warm-up's list at
   load and go straight to this file's list, so step A no longer waits for the warm-up to end -- its workers read the eye pass's
   objects only for those states (pipe_build: topology bits 0x40 / 0x80), and none are left to them. The device's fresh M4 start
   (t10a): the warm-up of 493 states took 7.4 s, the mission's first tracked list came at 4.6 s, step A was NOT reached, and
   the first eye frame made the eye pass, the cockpit's pipelines (1.2 s) and 10 states on demand (1.1 s): 2.8 s. With it step
   A runs in the session's first frames (the logos, the boot) and the states' workers start at once. */
static int rqpw_early_cfg(void) {
    if (rqpw.early < 0) { const char *e = getenv("PS2_RQ_PREWARM_EARLY"); rqpw.early = e && *e == '1'; }
    return rqpw.early;
}
static void rqpw_split(void) {
    if (!pipe_warm || !pipe_warm_n || rq_on() != 3 || !rqpw_cfg() || !rqpw_early_cfg()) return;
    u32 n = 0, moved = 0;
    for (u32 i = 0; i < pipe_warm_n; i++) {
        u64 k = pipe_warm[i];
        if (rqpw_defer(k)) moved++;
        else pipe_warm[n++] = k;
    }
    pipe_warm_n = n;
    if (!n) { free(pipe_warm); pipe_warm = NULL; }
    ps2_log("vk: PS2_RQ_PREWARM_EARLY=1: %u eye-pass pipeline states taken from the startup warm-up's list at load (%u left to it): "
            "built by PS2_RQ_PREWARM's workers from the session's first frames", moved, n);
}
static int rqpw_test_ms(void) {           /* PS2_RQ_PREWARM_TEST_MS [test]: each prewarm / warm-up / eye-pass on-demand build takes N ms more */
    if (rqpw.test_ms < 0) {
        const char *e = getenv("PS2_RQ_PREWARM_TEST_MS"); rqpw.test_ms = e && *e ? atoi(e) : 0;
        if (rqpw.test_ms < 0) rqpw.test_ms = 0;
        if (rqpw.test_ms) ps2_log("vk: PS2_RQ_PREWARM_TEST_MS=%d [test]: every warm-up and prewarm state build and every eye-pass state built on demand waits %d ms more (the Adreno's compile time on the simulator)", rqpw.test_ms, rqpw.test_ms);
    }
    return rqpw.test_ms;
}
/* one remembered state, as pipe_warm_worker builds it */
static VkPipeline rqpw_build(u64 k) {
    if (rqpw_test_ms()) SDL_Delay((u32)rqpw_test_ms());
#if PS2_FRAME_GRAPH_SUPPORT
    if (((k >> 52) & 1u && !vrs_att_ok) || ((k >> 62) & 1u && !fg_rp) || ((k >> 63) & 1u && !rnb.ok)) return VK_NULL_HANDLE;
#else
    if (((k >> 52) & 1u && !vrs_att_ok) || ((k >> 63) & 1u && !rnb.ok)) return VK_NULL_HANDLE;
#endif
    return pipe_build((u32)((k >> 40) & 0xFFu), (u32)((k >> 16) & 0xFFFFFFu), (u32)(k & 0xFFFFu),
                      (u32)((k >> 48) & 3u) | ((k >> 51) & 1u ? 4u : 0u) | ((k >> 52) & 1u ? 8u : 0u)
                      | ((k >> 53) & 1u ? 16u : 0u) | ((k >> 54) & 1u ? 32u : 0u)
                      | ((k >> 55) & 1u ? 64u : 0u) | ((k >> 56) & 1u ? 128u : 0u)
                      | ((k >> 57) & 1u ? 256u : 0u) | ((k >> 58) & 1u ? 512u : 0u)
                      | ((k >> 59) & 1u ? 1024u : 0u)
                      | ((k >> 60) & 1u ? 2048u : 0u)
                      | ((k >> 61) & 1u ? 4096u : 0u)
#if PS2_FRAME_GRAPH_SUPPORT
                      | ((k >> 62) & 1u ? 8192u : 0u)
#endif
                      | ((k >> 32) & 1u ? 32768u : 0u) | (u32)(((k >> 33) & 63u) << 16)
                      | ((k >> 63) & 1u ? 16384u : 0u),
                      (u32)((k >> 50) & 1u), 1);
}
/* renderer thread: the worker's finished states into pipes[] */
static void rqpw_take(void) {
    u32 hi = __atomic_load_n(&rqpw.knext, __ATOMIC_ACQUIRE);
    if (hi > rqpw.kto) hi = rqpw.kto;
    for (u32 i = rqpw.ktaken; i < hi; i++) {        /* (several workers: each state as it finishes, in any order) */
        if (__atomic_load_n(&rqpw.kdone[i], __ATOMIC_ACQUIRE) != 1u) continue;
        rqpw.kdone[i] = 2u;
        if (rqpw.kout[i]) { (void)pipe_insert(rqpw.keys[i], rqpw.kout[i]); rqpw.kins++; }
    }
    while (rqpw.ktaken < hi && rqpw.kdone[rqpw.ktaken] == 2u) rqpw.ktaken++;
}
static void *rqpw_worker(void *arg) {
    (void)arg;
    extern void ps2_thread_helper(const char *name, int kind);
    ps2_thread_helper("rq-prewarm", 2);           /* (census name only, as pipe-warm: driver locks the renderer may wait for) */
    rqpw_ctx = "PS2_RQ_PREWARM's worker before the first eye frame";
    const u32 job = rqpw.job;
    u32 made = 0, nk = 0;
    const u64 t0 = vk_mono_ns();
    if ((job & RQPW_CPIT) && !rqpw.cancel && !rqpw.icancel) { (void)rqc_pipes(); (void)rqco_pipes(); made |= RQPW_CPIT; }
    if ((job & RQPW_HUD) && !rqpw.cancel && !rqpw.icancel) {
        u32 k = 0;
        for (; k < RQH_KEYS && !rqpw.cancel && !rqpw.icancel; k++) (void)rqh_pipe(k);
        if (k == RQH_KEYS) made |= RQPW_HUD;
    }
    if ((job & RQPW_CL) && !rqpw.cancel && !rqpw.icancel) { (void)rqcl_pipe(); made |= RQPW_CL; }
    if ((job & RQPW_LAYERS) && !rqpw.cancel && !rqpw.icancel) { (void)rql_pipes(); made |= RQPW_LAYERS; }
    if ((job & RQPW_ZPRE) && !rqpw.cancel && !rqpw.icancel) { (void)rqz_make(); made |= RQPW_ZPRE; }
    if ((job & RQPW_AMASK) && !rqpw.cancel && !rqpw.icancel) { (void)rqam_make(); made |= RQPW_AMASK; }
    if ((job & RQPW_EXPO) && !rqpw.cancel && !rqpw.icancel) { (void)rqc_expo_make(); made |= RQPW_EXPO; }
    if ((job & RQPW_REC) && !rqpw.cancel && !rqpw.icancel) { (void)rqr_eye_pipe(); made |= RQPW_REC; }
    if (job & RQPW_KEYS) {
        __atomic_store_n(&rqpw.in_keys, 1, __ATOMIC_RELEASE);     /* (PS2_RQ_PREWARM_BG: the first eye frame lets this part run on) */
        u64 k0 = vk_mono_ns();
        u32 nb = 0;
        while (!rqpw.cancel) {
            u32 i = __atomic_fetch_add(&rqpw.knext, 1u, __ATOMIC_ACQ_REL);
            if (i >= rqpw.kto) break;
            rqpw.kout[i] = rqpw_build(rqpw.keys[i]);
            nk += rqpw.kout[i] != VK_NULL_HANDLE; nb++;
            __atomic_store_n(&rqpw.kdone[i], 1u, __ATOMIC_RELEASE);
            __atomic_add_fetch(&rqpw.kfin, 1u, __ATOMIC_ACQ_REL);
        }
        if (nk || rqpw.kto > rqpw.kfrom)
            ps2_log("rq prewarm: %u of the %u eye-pass pipeline states the startup warm-up could not build made in %.0f ms%s", nk, rqpw.kto - rqpw.kfrom,
                    (double)(vk_mono_ns() - k0) / 1e6, rqpw.cancel ? " (stopped by the first eye frame: the rest on demand)" : "");
        if (nb >= rqpw.kto - rqpw.kfrom) made |= RQPW_KEYS;
        __atomic_store_n(&rqpw.in_keys, 0, __ATOMIC_RELEASE);
    }
    ps2_log("rq prewarm: the worker's run (items 0x%X) done in %.0f ms%s", job, (double)(vk_mono_ns() - t0) / 1e6, rqpw.cancel ? ", stopped by the first eye frame" : "");
    __atomic_or_fetch(&rqpw.done, made, __ATOMIC_ACQ_REL);
    __atomic_store_n(&rqpw.busy, 0, __ATOMIC_RELEASE);
    return NULL;
}
/* step A (renderer thread, no frame being recorded): the objects; 1 = done or not needed */
static int rqpw_step_a(void) {
    uint32_t w, h;
    /* (after the startup warm-up: its workers read rqe in pipe_build without a barrier, so the pass is made only once they are
       done; the eye-pass states they refused are then all in rqpw.keys) */
    if ((pipe_warm && !rqpw_early_cfg()) || !ps2_xr_session_running() || !ps2_xr_rq_size(&w, &h) || !rts[0].created || !rq_feed.created) return 0;
    const u64 t0 = vk_mono_ns();
    rqpw.in_a = 1;
    int ok = rq_eye_make(w, h);
    VkImageView views[RQ_MAX_IMG]; memset(views, 0, sizeof views);
    int nimg = ok ? ps2_xr_rq_prepare(views, RQ_MAX_IMG) : 0;
    if (ok && nimg == 0) { rqpw.in_a = 0; return 0; }      /* (the session not running yet: the next frame) */
    u32 nfb = 0;
    int fdm = 0;
    if (ok && nimg > 0) {
        if (rqf.on && rqf_fill()) {                           /* (the density map from the predicted fields; the first acquire fills it again only if they differ) */
            rqf.due = 1; fdm = 1;
            if (rq_fdm_upload_first()) rq_oneshot(rqf_upload, "PS2_RQ_FDM_OWN's density map (PS2_RQ_PREWARM)");
        }
        for (int i = 0; i < nimg; i++) nfb += (u32)rq_eye_fb((u32)i, views[i]);
        if (rqt_want()) (void)rqt_make();
        if (rqcl_want()) {
            VkBuffer t = VK_NULL_HANDLE, d = VK_NULL_HANDLE, l = VK_NULL_HANDLE;
            ps2_clouds_far_rq_buffers(&t, &d, &l);
            if (t && d) (void)rqcl_make();                     /* (else the far module is not up yet: its first list makes it, as before) */
        }
        if (rqh_cfg()) (void)rqh_init();
        if (rqc_cfg()) for (u32 fi = 0; fi < FRAMES_IN_FLIGHT; fi++) (void)rqc_sl_make((int)fi);
        (void)rqam_cfg();                                     /* (its switch read here, before a worker may make its pipeline) */
    }
    rqpw.in_a = 0;
    if (!ok || nimg < 0) {
        ps2_log("rq prewarm: the eye pass could not be made before the first eye frame (%s): made lazily as before", !ok ? "rq_eye_make" : "swapchain 6");
        return -1;
    }
    ps2_log("rq prewarm: step A at field %u in %.1f ms: the eye pass %ux%u, swapchain 6 (%d images, %u framebuffers)%s; the objects the first eye frame made",
            ps2_gs_frame_count, (double)(vk_mono_ns() - t0) / 1e6, w, h, nimg, nfb, fdm ? ", our density map from the predicted fields" : "");
    return 1;
}
/* the items whose inputs are final now and that no run has had */
static u32 rqpw_ready(void) {
    u32 m = 0;
    if (rqc_cfg() && !rqc.tried && rqc.layout && (!(rqc_wet_cfg() & 1) || rqw.tried) && (!rqgx_on() || rqgx.wtried)) m |= RQPW_CPIT;
    if (rqh_cfg() && rqh.ok) m |= RQPW_HUD;
    if (rqcl_want() && rqcl.ok) m |= RQPW_CL;
    if (rql_cfg()) m |= RQPW_LAYERS;
    if (rqz_cfg() && !rqz.tried && rnb.ok && rnb.layout) m |= RQPW_ZPRE;
    if (rqam_cfg() && rqz_cfg() && rqz_am_fs) m |= RQPW_AMASK;
    if (rqc_cfg() && expo.ok && !rqc_pexpo) m |= RQPW_EXPO;
    if (rqr.ok && !rqr.pipe_eye && !rqr.eye_tried) m |= RQPW_REC;
    return m & ~rqpw.tried;
}
/* PS2_RQ_PREWARM_BG: the eye-pass states on a worker of their own, beside the items' worker (a fresh install: the cockpit's pipelines
   took 1.2 s on the simulator, and the states waited behind them until the first eye frame came) */
static volatile u32 rqpw_kmade;
static void *rqpw_kworker(void *arg) {
    const int tid = (int)(intptr_t)arg;
    extern void ps2_thread_helper(const char *name, int kind);
    ps2_thread_helper("rq-prewarm-k", 2);
    rqpw_ctx = "PS2_RQ_PREWARM's state worker";
    const u32 from = rqpw.kfrom, to = rqpw.kto;
    for (;;) {
        /* (PS2_RQ_PREWARM_THREADS: the extra workers stop at the first eye frame -- the game's threads need the cores in flight;
           worker 0 goes on alone) */
        if (tid > 0 && __atomic_load_n(&rqpw.flown, __ATOMIC_ACQUIRE)) break;
        u32 i = __atomic_fetch_add(&rqpw.knext, 1u, __ATOMIC_ACQ_REL);
        if (i >= to) break;
        rqpw.kout[i] = rqpw_build(rqpw.keys[i]);
        if (rqpw.kout[i]) __atomic_add_fetch(&rqpw_kmade, 1u, __ATOMIC_ACQ_REL);
        __atomic_store_n(&rqpw.kdone[i], 1u, __ATOMIC_RELEASE);
        __atomic_add_fetch(&rqpw.kfin, 1u, __ATOMIC_ACQ_REL);
    }
    if (__atomic_sub_fetch(&rqpw.kbusy, 1, __ATOMIC_ACQ_REL) == 0) {
        const u32 fin = __atomic_load_n(&rqpw.kfin, __ATOMIC_ACQUIRE);
        ps2_log("rq prewarm: %u of the %u eye-pass pipeline states made in %.0f ms (their own workers, PS2_RQ_PREWARM_BG; %u finished, field %u)",
                __atomic_load_n(&rqpw_kmade, __ATOMIC_ACQUIRE), to - from, (double)(vk_mono_ns() - rqpw.kt0) / 1e6, fin, ps2_gs_frame_count);
        __atomic_or_fetch(&rqpw.done, (u32)RQPW_KEYS, __ATOMIC_ACQ_REL);
    }
    return NULL;
}
/* PS2_RQ_PREWARM_THREADS=N (rq4_water 2026-11-11; 1 = as before, the Quest default 3): the states' workers before the first eye
   frame (the boot, the menus, a briefing: the EE and the GS thread are light then); from the first eye frame one goes on. Each
   Adreno compile of an eye-pass state takes 20-250 ms on one core (t10a: 176 states in 14.3-15.6 s on one worker). 1..6. */
static int rqpw_threads_cfg(void) {
    if (rqpw.nthr < 0) {
        const char *e = getenv("PS2_RQ_PREWARM_THREADS");
        int n = e && *e ? atoi(e) : 1;
        rqpw.nthr = n < 1 ? 1 : n > 6 ? 6 : n;
    }
    return rqpw.nthr;
}
static int rqpw_bg_cfg(void);
/* PS2_RQ_PREWARM_BG (rq4_water 2026-11-11, PROMPT_2026-11-11_flights2_fixes G.2, BUGS_RQ R-91; 0 = as before, the Quest default 1):
   the eye-pass pipeline states the worker has not reached by the first eye frame are not left to be built on demand: the first eye
   frame stops only the worker's other items (and waits for the one it is on, as before), the states go on being built in the
   background in the list's order and the renderer takes each as it finishes (rqpw_take); a run of them still starts after the first
   eye frame when the startup warm-up ends late (a fresh install: every state compiled). A state the frame needs before its turn is
   built on demand as before (the worker's copy is then dropped by pipe_insert). The user's M4 flight met state 8043c22d0044001e at
   field 3470 (243.7 ms on demand): a list that reaches it before the flight needs it no longer stalls. */
static int rqpw_bg_cfg(void) {
    if (rqpw.bg < 0) {
        const char *e = getenv("PS2_RQ_PREWARM_BG"); rqpw.bg = e && *e == '1';
        if (rqpw.bg) ps2_log("vk: PS2_RQ_PREWARM_BG=1: the eye-pass states left at the first eye frame are built on in the background");
    }
    return rqpw.bg;
}
/* every frame before the frame is recorded (replay_frame_inner, renderer thread) */
static void rq_prewarm_step(void) {
    if (rq_on() != 3 || !rqpw_cfg()) return;
    if (rqpw.kout) rqpw_take();
    const int wdone = !pipe_warm || rqpw_early_cfg();   /* (PS2_RQ_PREWARM_EARLY: the list is complete at load, the warm-up holds no eye-pass state) */
    if (rqpw_bg_cfg() && (rqpw.a_state > 0 || rqpw.flown) && !__atomic_load_n(&rqpw.kbusy, __ATOMIC_ACQUIRE)
        && rqpw.nkeys > rqpw.kto && wdone && rqe.ok) {    /* (PS2_RQ_PREWARM_BG: the states start at once, beside the items) */
        if (rqpw.ktaken < rqpw.kto) rqpw_take();            /* (the previous run's last ones) */
        rqpw.kfrom = rqpw.kto; rqpw.kto = rqpw.nkeys; rqpw.knext = rqpw.kfrom; rqpw.kt0 = vk_mono_ns();
        const int n = rqpw.flown ? 1 : rqpw_threads_cfg();
        __atomic_store_n(&rqpw.kbusy, n, __ATOMIC_RELEASE);
        int started = 0;
        for (int t = 0; t < n; t++) {
            pthread_t th;
            if (pthread_create(&th, NULL, rqpw_kworker, (void *)(intptr_t)t) == 0) { pthread_detach(th); started++; }
            else __atomic_sub_fetch(&rqpw.kbusy, 1, __ATOMIC_ACQ_REL);
        }
        if (started) ps2_log("rq prewarm: the states' workers at field %u: %u eye-pass states on %d thread%s (PS2_RQ_PREWARM_BG, PS2_RQ_PREWARM_THREADS)",
                             ps2_gs_frame_count, rqpw.kto - rqpw.kfrom, started, started > 1 ? "s" : "");
        else rqpw.kto = rqpw.kfrom;
    }
    if ((rqpw.flown && !rqpw_bg_cfg()) || rqpw.a_state < 0 || __atomic_load_n(&rqpw.busy, __ATOMIC_ACQUIRE)) return;
    if (!rqpw.a_state && !rqpw.flown) { rqpw.a_state = rqpw_step_a(); if (rqpw.a_state <= 0) return; }
    u32 job = rqpw.flown ? 0u : rqpw_ready();       /* (after the first eye frame: the states only, PS2_RQ_PREWARM_BG) */
    int keys = !rqpw_bg_cfg() && rqpw.nkeys > rqpw.kto && wdone && rqe.ok;   /* (the startup warm-up done: every refused state is in the list; with PS2_RQ_PREWARM_BG their own worker) */
    if (keys) job |= RQPW_KEYS;
    if (!job) return;
    rqpw.job = job; rqpw.tried |= job & ~(u32)RQPW_KEYS;
    if (keys) { if (rqpw.ktaken < rqpw.kto) rqpw_take(); rqpw.kfrom = rqpw.kto; rqpw.kto = rqpw.nkeys; rqpw.knext = rqpw.kfrom; }
    rqpw.cancel = 0; rqpw.icancel = rqpw.flown;
    __atomic_store_n(&rqpw.busy, 1, __ATOMIC_RELEASE);
    pthread_t th;
    if (pthread_create(&th, NULL, rqpw_worker, NULL) == 0) {
        pthread_detach(th);
        ps2_log("rq prewarm: a worker run at field %u: items 0x%X%s", ps2_gs_frame_count, job, keys ? " with the warm-up's eye-pass states" : "");
    } else {
        __atomic_store_n(&rqpw.busy, 0, __ATOMIC_RELEASE);
        if (keys) rqpw.kto = rqpw.kfrom;
        rqpw.a_state = -1;
        ps2_log("rq prewarm: no worker thread: the pipelines made lazily as before");
    }
}
/* the eye-pass decision of a list (renderer thread): the worker stops between items, its states go in, nothing more is scheduled */
static void rq_prewarm_join(void) {
    if (!rqpw.flown) {
        rqpw.flown = 1;
        if (rqpw.cfg > 0) ps2_log("rq prewarm: the first eye-pass decision at field %u: step A %s, worker items made 0x%X of 0x%X handed out, %u of %u warm-up states inserted%s",
                                  ps2_gs_frame_count, rqpw.a_state > 0 ? "made" : rqpw.a_state < 0 ? "not possible" : "NOT reached",
                                  __atomic_load_n(&rqpw.done, __ATOMIC_ACQUIRE), rqpw.tried | (rqpw.kto ? (u32)RQPW_KEYS : 0u), rqpw.kins, rqpw.nkeys,
                                  __atomic_load_n(&rqpw.busy, __ATOMIC_ACQUIRE) ? "; the worker is still running" : "");
    }
    if (__atomic_load_n(&rqpw.busy, __ATOMIC_ACQUIRE) && rqpw_bg_cfg()) {   /* (PS2_RQ_PREWARM_BG: the items stop, the states go on) */
        if (!rqpw.icancel) {
            const u64 t0 = vk_mono_ns();
            rqpw.icancel = 1;
            while (__atomic_load_n(&rqpw.busy, __ATOMIC_ACQUIRE) && !__atomic_load_n(&rqpw.in_keys, __ATOMIC_ACQUIRE)) SDL_Delay(1);   /* (the items' worker has no states now: it ends after its current item) */
            ps2_log("rq prewarm: the eye frame waited %.1f ms for the worker's current item (field %u)%s", (double)(vk_mono_ns() - t0) / 1e6, ps2_gs_frame_count,
                    __atomic_load_n(&rqpw.busy, __ATOMIC_ACQUIRE) ? "; its eye-pass states go on in the background (PS2_RQ_PREWARM_BG)" : "");
        }
    } else if (__atomic_load_n(&rqpw.busy, __ATOMIC_ACQUIRE)) {
        const u64 t0 = vk_mono_ns();
        rqpw.cancel = 1;
        while (__atomic_load_n(&rqpw.busy, __ATOMIC_ACQUIRE)) SDL_Delay(1);
        ps2_log("rq prewarm: the eye frame waited %.1f ms for the worker's current item (field %u)", (double)(vk_mono_ns() - t0) / 1e6, ps2_gs_frame_count);
    }
    if (rqpw.kout) rqpw_take();
}
/* PS2_RQ_PREWARM_HOLD_MS=N (rq4_water 2026-11-11, the coordinator's device runs of t10a, BUGS_RQ R-91; 0 = as before, the Quest
   default 2000): the session's first list with a head pose is not drawn while the prewarm is not done -- step A, the worker's
   items (the cockpit's pipelines: 1.2 s on the Adreno) and every eye-pass state of the list -- for at most N ms: each such list
   is taken (the game goes on) and the headset's frame repeats its last layer (ps2_xr_frame_end: the static picture, the intro's
   or the loading screen's last frame on the cinema screen, world-locked), with no GPU work. The first eye frame then makes
   nothing (t10a's fresh M4 start: 2,774 ms for its first eye frame, 14 objects made lazily and 10 states on demand). After N
   ms it goes on with what is made (the rest lazily / on demand, as before). Only once a session (a fresh install or an update:
   later starts find every state in the cache, the prewarm done at the logos). `rq prewarm: hold` lines. */
static int rq_frame_tracked(u32 seq);
static int rqpw_hold_cfg(void) {
    if (rqpw.hold_ms < 0) {
        const char *e = getenv("PS2_RQ_PREWARM_HOLD_MS"); rqpw.hold_ms = e && *e ? atoi(e) : 0;
        if (rqpw.hold_ms < 0) rqpw.hold_ms = 0;
        if (rqpw.hold_ms > 10000) rqpw.hold_ms = 10000;
    }
    return rqpw.hold_ms;
}
/* what the first eye frame would still make: 0 none */
static u32 rqpw_missing(void) {
    u32 m = 0;
    if (rqpw.a_state <= 0) m |= 1u;                                            /* step A */
    if (__atomic_load_n(&rqpw.busy, __ATOMIC_ACQUIRE) || rqpw_ready()) m |= 2u;  /* the items */
    if (rqc_cfg() && !rqc.tried) m |= 4u;                                      /* the cockpit's pipelines (inputs not final yet) */
    if (rqpw.nkeys && (rqpw.kto < rqpw.nkeys || rqpw.ktaken < rqpw.kto)) m |= 8u;   /* the states */
    return m;
}
static int rq_prewarm_hold(void) {
    if (rq_on() != 3 || !rqpw_cfg() || rqpw.flown || rqpw.hold_done || !rqpw_hold_cfg() || rqpw.a_state < 0) return 0;
    if (!rq_frame_tracked(present_xr_scene_seq)) return 0;          /* (static lists: drawn as before) */
    const u64 now = vk_mono_ns();
    if (rqpw.kout) rqpw_take();
    const u32 m = rqpw_missing();
    if (!rqpw.hold_t0) {
        if (!m) { rqpw.hold_done = 1; ps2_log("rq prewarm: the first tracked list at field %u finds the prewarm done: no hold (PS2_RQ_PREWARM_HOLD_MS)", ps2_gs_frame_count); return 0; }
        rqpw.hold_t0 = now;
        ps2_log("rq prewarm: hold from the first tracked list at field %u: missing 0x%X (1 step A, 2 the worker's items, 4 the cockpit's pipelines, 8 states: %u of %u in), at most %d ms (PS2_RQ_PREWARM_HOLD_MS)",
                ps2_gs_frame_count, m, rqpw.kins, rqpw.nkeys, rqpw.hold_ms);
    }
    const double ms = (double)(now - rqpw.hold_t0) / 1e6;
    if (!m || ms >= (double)rqpw.hold_ms) {
        rqpw.hold_done = 1;
        ps2_log("rq prewarm: hold ended at field %u after %.0f ms, %u lists held: %s (states %u of %u in)", ps2_gs_frame_count, ms, rqpw.hold_n,
                !m ? "the prewarm done" : m & 2u ? "the cap; the worker's items not done" : m & 4u ? "the cap; the cockpit's pipelines not made" : m & 1u ? "the cap; step A not made" : "the cap; states left (built on as before)",
                rqpw.kins, rqpw.nkeys);
        return 0;
    }
    extern int ps2_xr_frame_begin(void); extern void ps2_xr_frame_end(void);
    const int show = ps2_xr_frame_begin();
    ps2_xr_frame_end();                                             /* (no new layer: the last one repeated; nothing when no frame is open) */
    if (!show) SDL_Delay(4);
    rqpw.hold_n++;
    return 1;
}
/* after each renderer frame (draw_and_present, beside stereo_start_note): the starts of eye frames, measured */
static void rq_eye_start_note(u64 t0) {
    if (rq_on() != 3) return;
    const u64 now = vk_mono_ns();
    const int eye = rqpw.eye_now;
    rqpw.eye_now = 0;
    const u32 fn = rqpw.fr_n, fod = rqpw.fr_od; const double fms = rqpw.fr_ms, fodms = rqpw.fr_od_ms;
    rqpw.fr_n = rqpw.fr_od = 0; rqpw.fr_ms = rqpw.fr_od_ms = 0.0;
    const double d = (double)(now - t0) / 1e6, iv = rqpw.prev_t0 ? (double)(t0 - rqpw.prev_t0) / 1e6 : 0.0;
    static double per = 0.0;
    if (per <= 0.0) { long long p = ps2_xr_display_period(); per = p > 1000000 ? (double)p / 1e6 : 1000.0 / 72.0; }
    if (eye && !rqpw.win && (!rqpw.last_eye || now - rqpw.last_eye > 1000000000ull)) {
        rqpw.win = 1; rqpw.starts++; rqpw.n = 0; rqpw.over = 0; rqpw.worst = 0.0; rqpw.worst_at = 0; rqpw.sum = 0.0; rqpw.ivmax = 0.0;
        rqpw.first_ms = d; rqpw.first_iv = iv; rqpw.field0 = ps2_gs_frame_count;
        rqpw.f_n = fn; rqpw.f_ms = fms; rqpw.f_od = fod; rqpw.f_od_ms = fodms;
        rqpw.w_n = rqpw.w_od = 0; rqpw.w_ms = rqpw.w_od_ms = 0.0;
    } else if (rqpw.win) {
        rqpw.n++; rqpw.sum += d;
        if (d > rqpw.worst) { rqpw.worst = d; rqpw.worst_at = rqpw.n; }
        if (iv > rqpw.ivmax) rqpw.ivmax = iv;
        if (iv > 1.5 * per) rqpw.over++;
        rqpw.w_n += fn; rqpw.w_ms += fms; rqpw.w_od += fod; rqpw.w_od_ms += fodms;
        if (rqpw.n >= 60u) {
            ps2_log("rq eye start %u (PS2_RQ_PREWARM=%d): field %u, the first eye frame %.1f ms (the interval before it %.1f ms), made in it %u objects (%.1f ms) and %u pipeline states on demand (%.1f ms); "
                    "the next 60 frames: worst %.1f ms (+%u), mean %.1f ms, %u intervals over 1.5 refresh periods (%.2f ms), the longest %.1f ms; made in them %u objects (%.1f ms), %u states on demand (%.1f ms)",
                    rqpw.starts, rqpw_cfg(), rqpw.field0, rqpw.first_ms, rqpw.first_iv, rqpw.f_n, rqpw.f_ms, rqpw.f_od, rqpw.f_od_ms,
                    rqpw.worst, rqpw.worst_at, rqpw.sum / (double)rqpw.n, rqpw.over, per, rqpw.ivmax, rqpw.w_n, rqpw.w_ms, rqpw.w_od, rqpw.w_od_ms);
            rqpw.win = 0;
        }
    }
    rqpw.prev_t0 = t0;
    if (eye) rqpw.last_eye = now;
}
