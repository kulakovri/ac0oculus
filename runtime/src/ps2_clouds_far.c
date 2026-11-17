// ps2_clouds_far.c - S2 far cloud renderer (CLOUDS_3D_RESEARCH_2026-10-20.md 8.1/8.2; contracts in ps2_clouds.h):
// the weather window (C3 weather: the splat of the game's cloud list with its density softened across the borders
// between clouds (PASS_WSOFT, bug #31), or the procedural field) and the update of the
// direction map cloud_dir / cloud_dir_depth (cloud_far.comp, cloud_far.glsl).
//
// The map is marched from the centre eye, so head rotation never needs an update; what goes stale is the parallax of
// the aircraft's translation. The scheduler (cloud_far.comp PASS_TILES, on the GPU, one thread per 8x8-texel tile)
// keeps per tile the eye position of its last march and the distance of its nearest cloud, and re-marches a tile
// when the stored content has moved by more than ACZ_VR_CLOUD_FAR_ERR eye pixels (near clouds abeam: every frame or
// two; far ones rarely), at least every ACZ_VR_CLOUD_FAR_SLICE frames (staggered, so colours and the weather keep
// up), and with looser limits outside both eyes' views ("visible first"). Tiles whose rays cannot reach the band are
// written once. The march is an indirect dispatch over the listed tiles. Bench numbers: tools/cloudbench/farbench.c.
//
// Frames in flight: the host-written buffers (parameters, the cloud list) have one copy per frame index; the tile
// metadata and the lists live on the GPU only and are ordered by barriers (one queue).
//
// 2026-10-22 (bugs #33 / #35 / #36 / #40 / #57, agent bx_cloudfield): an empty C1 list stays the list path (clear sky,
// fades continue, no procedural field, no new generation: ACZ_VR_CLOUD_LIST_KEEP), list changes ramp over
// ACZ_VR_CLOUD_LIST_RAMP seconds, the splat weighs the clouds by profile x opacity (ACZ_VR_CLOUD_SPLAT_MIX), and the
// governor's level 1 loosens every tile's limits instead of leaving most tiles stale (ACZ_VR_GOV_FAR_MODE).
#include "ps2_runtime.h"
#include "ps2_clouds.h"
#include "ps2_vkcen.h"          /* (PS2_VK_CENSUS: vkCmd* calls counted) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u64 ps2_kernel_vblank_count(void);
#define FAR_FIF 4u                /* frame indices handed to the hooks (ps2_clouds.c CL_FIF) */
#define FAR_MAXCL 512u            /* cloud list capacity (C1 CL_MAX_CLOUDS = 400) */

/* set 1 of the far pipelines (cloud_far.glsl ClFarParams, cloud_far.comp ClTiles / ClList / ClClouds) */
#if PS2_RQ_SUPPORT
typedef struct { float mz[4], spl[4], haze[4], stp[4], sch[4], fine[4], win[4], ext[4], dbg[4], rq[4], pred[4], rq2[4], rq3[4], bk[4], bk2[4], bk3[4]; } far_params;   /* (rq: PS2_RQ_CLOUDS_FARSWAP; pred: _FARLEAD; rq2: clouds6's FARONE; rq3: clouds7's FARSHOWN; bk: rq7_bake's PS2_RQ_CLOUDS_BAKE, at most 256 bytes: HOST_CLOUDS) */ static void bk_cfg(void); static int bk_resources(VkPhysicalDevice phys); static VkImageView bk_view(int k); static VkSampler bk_sampler(int lin); static void bk_init(const ps2_cloud_gpu *g); static void bk_splat_note(const void *clv, uint32_t n, const ps2_cloud_view *v); static int rq_one_cfg(void); static void rq_one_snap(void); static int rq_one_rebase(const ps2_cloud_view *v); static float rq_one_period(const ps2_cloud_env *e); static float rq_one_wrapf(double d); static float rq_one_thin(float t);   /* (rq8_onelayer: PS2_RQ_CLOUDS_ONE) */
#else
typedef struct { float mz[4], spl[4], haze[4], stp[4], sch[4], fine[4], win[4], ext[4], dbg[4]; } far_params; static inline void rq_one_snap(void) {} static inline int rq_one_rebase(const ps2_cloud_view *v) { (void)v; return 0; } static inline float rq_one_period(const ps2_cloud_env *e) { (void)e; return 0.0f; } static inline float rq_one_wrapf(double d) { return (float)d; } static inline float rq_one_thin(float t) { return t; }
#endif
typedef struct { float pos_r[4], info[4]; } far_cloud;
typedef struct { float cam[4]; uint32_t st[4]; } far_tile;
typedef struct { float cam[4], march[4], sched[4]; uint32_t fr[4]; } far_push;
/* a cloud of C1's list as the splat sees it: identified by its cloud-frame position and radius (stable across S1's
   re-centres), with its opacity eased toward the published value */
typedef struct { double x, z; float y, r, h, cur, tgt; uint32_t var, seed; int seen; float from, age, dur, vel; } far_track;
_Static_assert(sizeof(far_push) <= CL_PUSH_BYTES, "far push constants");
static int far_gov_on;                    /* the renderer's frame-time governor sheds (ps2_clouds_far_gov) */
/* PS2_GPU_TRACE (diagnostic, ps2_clouds_far_trace): per frame slot what the far module recorded -- bit 0 the weather
   splat, bit 1 its soft pass, bit 2 a new generation (every tile due), bit 3 the march, bits 4-5 the governor state the
   map followed (1 level 0, 2 level 1 relaxed, 3 level 1 capped); the tile counters are copied to the host slot as
   with ACZ_DEBUG_CLOUDS */
static uint8_t far_tr_flags[FAR_FIF];
static int far_trace_on(void) {
    static int t = -1;
    if (t < 0) { const char *e = getenv("PS2_GPU_TRACE"); t = e && *e; }
    return t;
}
static int far_gov_cap(void) {            /* ACZ_VR_GOV_FAR_CAP: tiles a frame while it does (256) */
    static int cap = -1;
    if (cap < 0) { const char *e = getenv("ACZ_VR_GOV_FAR_CAP"); cap = e ? atoi(e) : 256; if (cap < 0) cap = 0; }
    return cap;
}
/* ACZ_VR_GOV_FAR_MODE (bugs #35 / #36 / #57, 2026-10-22): what the governor's level 1 does to the far map.
   1 (default) = relax: the error limit x ACZ_VR_GOV_FAR_ERR (4) and the periodic refresh half as often for every tile,
   the tile cap only on the tiles outside both eyes' views -- every visible tile still meets a (looser) bound.
   0 = the cap of before: at most ACZ_VR_GOV_FAR_CAP tiles a frame of all lists. With 600-2000 tiles due a frame inside
   M1's overcast the cap left most of them stale for seconds, updated in list order: rings and rows of tiles marched
   at different eye positions = the "corridor" of nested frames, stripes and streaks the user saw (all at level >= 1). */
static int far_gov_mode(void) {
    static int m = -1;
    if (m < 0) { const char *e = getenv("ACZ_VR_GOV_FAR_MODE"); m = e && *e ? (atoi(e) != 0) : 1; }
    return m;
}
/* ACZ_VR_GOV_FAR_AB=N [measurement]: every N far frames the far map's governor state rotates level 0 / level 1 relaxed /
   level 1 capped (whatever the governor says), and the far update's own GPU time (timestamps around the tile classify,
   args and march) is logged per state, the first quarter of each phase dropped (a cap phase leaves a backlog) */
static int far_ab(void) {
    static int n = -1;
    if (n < 0) { const char *e = getenv("ACZ_VR_GOV_FAR_AB"); n = e && *e ? atoi(e) : 0; if (n < 0) n = 0; }
    return n;
}
/* ACZ_VR_GOV_FAR_AB_SEQ="0,1" [measurement]: the states the rotation visits (0 level 0, 1 relaxed, 2 capped; default
   0,1,2): "0,1" = the current governor's step up and down only (the transitions' own spikes in PS2_GPU_TRACE) */
static int far_ab_phase(uint32_t frames) {
    static int seq[8], ns = -1;
    if (ns < 0) {
        const char *e = getenv("ACZ_VR_GOV_FAR_AB_SEQ");
        ns = 0;
        for (const char *p = e; p && *p && ns < 8; p++) if (*p >= '0' && *p <= '2') seq[ns++] = *p - '0';
        if (ns == 0) { seq[0] = 0; seq[1] = 1; seq[2] = 2; ns = 3; }
    }
    int n = far_ab();
    return n > 0 ? seq[(frames / (uint32_t)n) % (uint32_t)ns] : -1;
}
/* the governor state the far map follows this frame: on = level >= 1, relax = ACZ_VR_GOV_FAR_MODE */
static void far_gov_state(uint32_t frames, int *on, int *relax) {
    int ph = far_ab_phase(frames);
    if (ph >= 0) { *on = ph != 0; *relax = ph == 1; return; }
    *on = far_gov_on; *relax = far_gov_mode();
}
static float far_gov_err(void) {
    static float k = -1.0f;
    if (k < 0.0f) { const char *e = getenv("ACZ_VR_GOV_FAR_ERR"); k = e && *e ? (float)atof(e) : 4.0f; if (k < 1.0f) k = 1.0f; if (k > 8.0f) k = 8.0f; }
    return k;
}

static struct {
    int ready, far_ready;
    VkDevice dev;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool pool;
    VkDescriptorSet set[FAR_FIF];
    VkPipelineLayout layout;         /* the first pass's layout: binds set 0/1 and push constants for all passes */
    VkPipelineLayout layouts[6]; int nlayouts;   /* every pass's own layout, kept until shutdown (no maintenance4) */
    VkPipeline p_weather, p_wsoft, p_tiles, p_args, p_march;
    VkBuffer host_buf; VkDeviceMemory host_mem; uint8_t *host;     /* per frame index: params | clouds | counters */
    VkBuffer wxc; VkDeviceMemory wxc_mem;   /* the splat's weather image copied for PASS_WSOFT (512^2 rgba8 texels) */
    float soft_m;                           /* ACZ_VR_CLOUD_SPLAT_SOFT: the density's softening radius (m; 0 = off) */
    int imp_alpha;                          /* ACZ_VR_CLOUD_IMP_ALPHA: the list's opacities x C1 imp_alpha (1 on) */
    VkBuffer tiles; VkDeviceMemory tiles_mem;
    VkBuffer list; VkDeviceMemory list_mem;
    uint32_t n, tpr, nt;
    /* scheduler state */
    double anchor[3]; int have_anchor, cleared; uint32_t rebase, gen;
    uint32_t frames;
    /* weather state */
    double splat_origin[3]; int have_splat; uint32_t splat_seq, splat_valid, splat_rebase;
    float splat_key[4];
    far_track trk[FAR_MAXCL]; uint32_t ntrk; int trk_ok; double trk_time; float fade_s, size_k;
    uint32_t fade_ctr; int fade_was;
    uint32_t wx_quiet; float quiet_k;
    float od_cu;                        /* ACZ_VR_CLOUD_OD_CU: the per-cloud optical depth for cumulus-like weather */
    float thin;                         /* ACZ_VR_CLOUD_FAR_THIN: thin-medium step factor (cumulus-like weather) */   /* far frames since the weather content last changed; FAR_SLICE factor then */
    int keep;                           /* ACZ_VR_CLOUD_LIST_KEEP: an empty C1 list is still the list path (see list_path) */
    float ramp_s;                       /* ACZ_VR_CLOUD_LIST_RAMP: seconds of a cloud's fade in / out (see track_target) */
    int ease;                           /* ACZ_VR_CLOUD_LIST_EASE: 1 the follower (track_follow), 0 the restarted ramp */
    float follow_s;                     /* ACZ_VR_CLOUD_LIST_FOLLOW: the follower's 95 % time for a change within the list */
    float near_rate, near_m;            /* ACZ_VR_CLOUD_AROUND_RATE / _M: the opt-in rate cap near the eye (0 = off) */
    float mix;                          /* ACZ_VR_CLOUD_SPLAT_MIX: opacity-weighted splat (cloud_far.comp PASS_WEATHER) */
    /* switches */
    float err_px, start_m, max_m, max_m_list, k_step, dt_min, margin_rad;
    int cap, debug, ovc_res, ovc_steps, list_res;
    uint32_t n_used;                 /* the map size the last frame marched (a change = a new generation) */
    uint32_t n_ubo; int ubo_asked;   /* the size ps2_clouds.c last wrote to CloudFrame sizes.x (via the getter) */
    /* ACZ_DEBUG_CLOUD_FAR_STALE (cl_pop 2026-10-31, bug #260): the visible tiles the scheduler leaves alone, checked
       against a march now (cloud_far.comp PASS_TILES); per frame slot the used tile count of its frame, sums */
    int stale; uint32_t st_nt[FAR_FIF], st_field[FAR_FIF]; uint8_t st_used[FAR_FIF]; uint64_t st_sum[6], st_frames, st_worst; uint32_t st_worst_field;
    /* ACZ_DEBUG_CLOUDS: the weather pass's own GPU time (2 timestamps per frame index around the splat + soft pass) */
    VkQueryPool wq; float wq_ns; uint8_t wq_used[FAR_FIF]; double wq_ms; uint32_t wq_n, wq_calls;
    /* ACZ_VR_GOV_FAR_AB: the far update's own GPU time per governor state (2 timestamps per frame index) */
    VkQueryPool mq; int8_t mq_phase[FAR_FIF]; uint32_t mq_frame[FAR_FIF]; double mq_ms[3]; uint32_t mq_n[3], mq_tiles[3];
#if PS2_RQ_SUPPORT
    /* PS2_RQ_CLOUDS (rq_clouds.inc): the far map of the Quest eye pass: the travel rule, the tile budget, the draw list */
    int rq;                             /* the mode: rq_cloud_far.comp's passes instead of cloud_far.comp's */
    VkPipeline p_tiles_rq, p_args_rq, p_march_rq;
    VkBuffer rq_draw; VkDeviceMemory rq_draw_mem;   /* uvec2(tile, proxy distance) per listed tile (rq_clouds.vert) */
    float rq_cam[3];                    /* the centre eye in the anchor frame at this frame's far update */
    float rq_travel, rq_band, rq_period;   /* ACZ_VR_CLOUD_RQ_TRAVEL, ACZ_VR_CLOUD_RQ_BAND (m), ACZ_VR_CLOUD_RQ_REFRESH (frames) */
    int rq_budget;                      /* ACZ_VR_CLOUD_RQ_TILES: tiles marched a frame */
    float rq_kstep, rq_fine;            /* the march from the near range outward (ACZ_VR_CLOUD_RQ_KSTEP, ACZ_VR_CLOUD_RQ_FINE) */
    /* PS2_RQ_CLOUDS_FARSWAP (B-1, R-52; rq_cloud_far.comp RqSweep / RqStage / PASS_COPY): sweeps from one origin, staged, copied in */
    int rq_swap; uint32_t rq_slots; int rq_all; float rq_lead, rq_pv[3], rq_vel[3]; int rq_pv_ok; VkQueryPool rq_tq; int rq_tn, rq_tab; int8_t rq_tarm[FAR_FIF]; double rq_tms[2]; uint32_t rq_tcnt[2];   /* (R-70: ps2_clouds_far_init) */
    uint32_t cold_n, cold_frames, cold_max, cold_all;   /* (ACZ_DEBUG_CLOUDS: a new generation's first marches, PS2_RQ_CLOUDS_COLD) */
    VkPipeline p_copy_rq;
    /* (clouds7 t11a2: PS2_RQ_CLOUDS_SPLAT_EVERY / _SPLAT_CULL, R-114 / R-115; ACZ_DEBUG_CLOUDS: the splats by cause) */
    VkPipeline p_weather_rq; uint32_t wx_every, wx_n, wx_last, wx_pend, wx_why[6], wx_quiet_keep; float wx_floor;
    VkBuffer rq_sweep, rq_stage; VkDeviceMemory rq_sweep_mem, rq_stage_mem; int list_hold; float trk_hold[FAR_MAXCL]; int coarse_k; uint32_t cg_gen, cg_k; float cg_org[3]; VkPipeline p_coarse_rq; int rq_stub; VkPipeline p_march_cnt, p_march_stub, p_march_lg; double rq_tmm[2], rq_smp[2][3]; int list_dbg; uint64_t ls_last; uint32_t ls_n[2], ls_view[2], ls_near[2], ls_ahead[2];   /* (clouds6: PS2_RQ_CLOUDS_FARSTUB, ACZ_DEBUG_CLOUD_LIST; on this line so that no PC line moves) */
    int sh_on, sh_init; VkImage sh_img[2]; VkDeviceMemory sh_mem[2]; VkImageView sh_view[2]; VkPipeline p_resolve_rq;   /* (clouds7: PS2_RQ_CLOUDS_FARSHOWN, the shown map: colour, depth data) */ uint32_t bk_splat_n, bk_content_n, bk_hard_n;   /* (rq7_bake, PS2_RQ_CLOUDS_BAKE: the splats, those that changed the content -- not the window's move alone --, the hard ones) */
#endif
    far_params par;
} F;

#define HOST_PARAMS 0u
#define HOST_CLOUDS 256u
#define HOST_COUNTS (HOST_CLOUDS + 16u + FAR_MAXCL * (uint32_t)sizeof(far_cloud))
#define HOST_STALE (HOST_COUNTS + 16u)   /* ACZ_DEBUG_CLOUD_FAR_STALE: 8 counters (cloud_far.comp list[3 NT ..]) */
#define HOST_SLOT ((HOST_COUNTS + 48u + 255u) & ~255u)

static float env_f(const char *n, float def, float lo, float hi) {
    const char *e = getenv(n);
    if (!e || !*e) return def;
    float v = (float)atof(e);
    return v < lo ? lo : v > hi ? hi : v;
}

/* The far march's own parameters (cloud_far.glsl ClFarParams documents each field). Colour, extinction and the "cloud
   above" probe are the shared core in cloud_common.glsl (cl_shade, cl_extinction, cl_above; S5 tunes them through
   CloudFrame user[0] / user[1]). */
static void default_params(far_params *p) {
    memset(p, 0, sizeof *p);
    p->mz[0] = 1.0f; p->mz[1] = 1.0f; p->mz[2] = 2.0f;           /* probe every 2nd fine step */
    p->mz[3] = 0.0f;                                             /* half rate below the hard limit: off (see ACZ_VR_CLOUD_FAR_HALF) */
    p->spl[0] = 0.675f; p->spl[1] = 1.0f; p->spl[2] = 0.45f; p->spl[3] = 0.9f;   /* round, as the game's sprites */
    p->haze[1] = 12000.0f; p->haze[2] = 20000.0f; p->haze[3] = 2500.0f;
    p->stp[0] = 3.0f; p->stp[1] = 800.0f; p->stp[2] = 0.02f; p->stp[3] = 0.35f;
    p->sch[0] = 64.0f; p->sch[1] = 16.0f; p->sch[2] = 0.0f; p->sch[3] = 2.0f;   /* outside the view: re-marched on entering the margin */
    p->fine[0] = 0.5f; p->fine[1] = 3.0f; p->fine[2] = 0.5f; p->fine[3] = 8.0f;   /* envelope billows: 8 puffs */
    p->win[3] = 512.0f / CL_WEATHER_M;
}

static VkPipeline make_pass(const ps2_cloud_gpu *g, const char *spv) {
    VkPipelineLayout lay = VK_NULL_HANDLE;
    (void)g;
    VkPipeline p = ps2_clouds_make_compute(spv, F.set_layout, &lay);
    if (lay && !F.layout) F.layout = lay;
    if (lay && F.nlayouts < 6) F.layouts[F.nlayouts++] = lay;   /* live as long as the pipeline (no maintenance4) */
    return p;
}

/* set 1, the buffers and the weather pipeline: shared by the weather writer and the far renderer */
static int common_init(const ps2_cloud_gpu *g) {
    if (F.ready && F.dev == g->dev) return 0;
    if (F.ready) memset(&F, 0, sizeof F);      /* a new device without our shutdown (near-only mode): start over */
    VkDevice dev = g->dev;
    F.dev = dev;
    /* bindings 0..3, and 6 = the weather copy of PASS_WSOFT (4 / 5 are the cloudbench's own: tools/cloudbench/far) */
#if PS2_RQ_SUPPORT
    /* binding 7 = the rq draw list (rq_cloud_far.comp); 8, 9 = PS2_RQ_CLOUDS_FARSWAP's sweep state and staging; 10, 11 = (clouds7)
       PS2_RQ_CLOUDS_FARSHOWN's shown map (storage images: colour, depth data) */
    VkDescriptorSetLayoutBinding b[19]; memset(b, 0, sizeof b);   /* (rq7_bake, PS2_RQ_CLOUDS_BAKE: 12-14 the baked volumes sampled -- density, light, the distance field --, 15-20 the same images and the distance field's passes as storage, rq_cloud_bake.comp; placeholders without the switch) */
    VkDescriptorType t[19] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE };
    for (uint32_t i = 0; i < 19; i++) {
        b[i].binding = i < 4 ? i : i == 4 ? 6u : i + 2u; b[i].descriptorType = t[i]; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo lc; memset(&lc, 0, sizeof lc);
    lc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; lc.bindingCount = 19; lc.pBindings = b;
#else
    VkDescriptorSetLayoutBinding b[5]; memset(b, 0, sizeof b);
    VkDescriptorType t[5] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
    for (uint32_t i = 0; i < 5; i++) {
        b[i].binding = i < 4 ? i : 6u; b[i].descriptorType = t[i]; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo lc; memset(&lc, 0, sizeof lc);
    lc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; lc.bindingCount = 5; lc.pBindings = b;
#endif
    if (vkCreateDescriptorSetLayout(dev, &lc, NULL, &F.set_layout) != VK_SUCCESS) return -1;
#if PS2_RQ_SUPPORT
    VkDescriptorPoolSize ps[4] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, FAR_FIF }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 7 * FAR_FIF },
                                   { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8 * FAR_FIF }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * FAR_FIF } };
#else
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, FAR_FIF }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * FAR_FIF } };
#endif
    VkDescriptorPoolCreateInfo pc; memset(&pc, 0, sizeof pc);
#if PS2_RQ_SUPPORT
    pc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; pc.maxSets = FAR_FIF; pc.poolSizeCount = 4; pc.pPoolSizes = ps;
#else
    pc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; pc.maxSets = FAR_FIF; pc.poolSizeCount = 2; pc.pPoolSizes = ps;
#endif
    if (vkCreateDescriptorPool(dev, &pc, NULL, &F.pool) != VK_SUCCESS) return -1;
    VkDescriptorSetLayout lays[FAR_FIF];
    for (uint32_t i = 0; i < FAR_FIF; i++) lays[i] = F.set_layout;
    VkDescriptorSetAllocateInfo ai; memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; ai.descriptorPool = F.pool; ai.descriptorSetCount = FAR_FIF; ai.pSetLayouts = lays;
    if (vkAllocateDescriptorSets(dev, &ai, F.set) != VK_SUCCESS) return -1;
    void *mapped = NULL;
    if (ps2_clouds_make_buffer((VkDeviceSize)HOST_SLOT * FAR_FIF, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1, &F.host_buf, &F.host_mem, &mapped) != 0) return -1;
    F.host = mapped;
    memset(F.host, 0, (size_t)HOST_SLOT * FAR_FIF);
    /* tiles and lists: sized for the configured map (the far renderer may be off: then 8 texels, unused) */
    F.n = g->dir_res ? g->dir_res : 8u;
    F.tpr = F.n / 8u; F.nt = F.tpr * F.tpr;
    if (ps2_clouds_make_buffer((VkDeviceSize)F.nt * sizeof(far_tile), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               0, &F.tiles, &F.tiles_mem, NULL) != 0) return -1;
#if PS2_RQ_SUPPORT
    /* (rq_cloud_far.comp's list: cnt[12] args[4] dargs[4], 8 buckets; also big enough for cloud_far.comp's 3) */
    if (ps2_clouds_make_buffer(80 + (VkDeviceSize)8 * F.nt * 4 + 32 + 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |   /* (+16: clouds7's resolve dispatch) */
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0, &F.list, &F.list_mem, NULL) != 0) return -1;
#else
    if (ps2_clouds_make_buffer(32 + (VkDeviceSize)3 * F.nt * 4 + 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0, &F.list, &F.list_mem, NULL) != 0) return -1;
#endif
#if PS2_RQ_SUPPORT
    if (ps2_clouds_make_buffer(16 + (VkDeviceSize)F.nt * 8 * 2 + 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, &F.rq_draw, &F.rq_draw_mem, NULL) != 0) return -1;   /* (x 2: clouds7's resolve candidates after the list) */
    /* (clouds7, PS2_RQ_CLOUDS_FARSHOWN, R-113; default 0, the Quest 1) the shown map: the map's size when on (8 x 8 when off: the bindings
       stay valid), RGBA16F colour + T and RGBA16F depth data, storage + sampled, GENERAL from the first far update */
    F.sh_on = ps2_clouds_rq_on() && env_f("PS2_RQ_CLOUDS_FARSHOWN", 0.0f, 0.0f, 1.0f) > 0.5f && !rq_one_cfg();   /* (PS2_RQ_CLOUDS_ONE: no shown map) */
    for (int k = 0; k < 2; k++) {
        uint32_t sz = F.sh_on ? F.n : 8u;
        VkImageCreateInfo ii; memset(&ii, 0, sizeof ii);
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        ii.extent.width = sz; ii.extent.height = sz; ii.extent.depth = 1; ii.mipLevels = 1; ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(dev, &ii, NULL, &F.sh_img[k]) != VK_SUCCESS) return -1;
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, F.sh_img[k], &mr);
        VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(g->phys, &mp);
        uint32_t mt = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < mp.memoryTypeCount && mt == 0xFFFFFFFFu; i++)
            if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) mt = i;
        VkMemoryAllocateInfo ma; memset(&ma, 0, sizeof ma);
        ma.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ma.allocationSize = mr.size; ma.memoryTypeIndex = mt;
        if (mt == 0xFFFFFFFFu || vkAllocateMemory(dev, &ma, NULL, &F.sh_mem[k]) != VK_SUCCESS || vkBindImageMemory(dev, F.sh_img[k], F.sh_mem[k], 0) != VK_SUCCESS) return -1;
        VkImageViewCreateInfo vi; memset(&vi, 0, sizeof vi);
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = F.sh_img[k]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = ii.format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(dev, &vi, NULL, &F.sh_view[k]) != VK_SUCCESS) return -1;
    }
    /* PS2_RQ_CLOUDS_FARSWAP (B-1, default 0): the sweep's state (80 bytes, the copy pass's dispatch arguments at 64) and a sweep number
       per tile; the staging: 4096 slots (PS2_RQ_CLOUDS_FARSWAP_SLOTS, R-70) x 65 x 16 bytes = 4.3 MB (a few bytes without the switch) */
    F.rq_swap = ps2_clouds_rq_on() && env_f("PS2_RQ_CLOUDS_FARSWAP", 0.0f, 0.0f, 1.0f) > 0.5f && !rq_one_cfg(); F.rq_slots = (uint32_t)env_f("PS2_RQ_CLOUDS_FARSWAP_SLOTS", 4096.0f, 1024.0f, 16384.0f);
    if (ps2_clouds_make_buffer(80 + (VkDeviceSize)F.nt * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0, &F.rq_sweep, &F.rq_sweep_mem, NULL) != 0) return -1;
    /* PS2_RQ_CLOUDS_FARCOARSE (clouds6 2026-11-11, A.4 P1c; default 0, the Quest 1 = the chunks, frames): the coarse level after the
       staging's slots: a header + (tiles a row + 1)^2 corners, 16 bytes each (266 KB at 1024^2) */
    F.coarse_k = F.rq_swap ? (int)env_f("PS2_RQ_CLOUDS_FARCOARSE", 0.0f, 0.0f, 16.0f) : 0;   bk_cfg(); if (bk_resources(g->phys) != 0) { ps2_log("clouds far rq bake: the volumes could not be made"); return -1; }   /* (rq7_bake, PS2_RQ_CLOUDS_BAKE: the volumes before set 1's writes) */
    if (ps2_clouds_make_buffer(F.rq_swap ? (VkDeviceSize)F.rq_slots * 65 * 16 + (F.coarse_k > 0 ? (1 + (VkDeviceSize)(F.tpr + 1) * (F.tpr + 1)) * 16 : 0) : 1040,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, &F.rq_stage, &F.rq_stage_mem, NULL) != 0) return -1;
#endif
    if (ps2_clouds_make_buffer((VkDeviceSize)CL_WEATHER_RES * CL_WEATHER_RES * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0, &F.wxc, &F.wxc_mem, NULL) != 0) return -1;
    for (uint32_t i = 0; i < FAR_FIF; i++) {
#if PS2_RQ_SUPPORT
        VkDescriptorBufferInfo bi[8];
#else
        VkDescriptorBufferInfo bi[5];
#endif
        bi[0].buffer = F.host_buf; bi[0].offset = (VkDeviceSize)HOST_SLOT * i + HOST_PARAMS; bi[0].range = sizeof(far_params);
        bi[1].buffer = F.tiles; bi[1].offset = 0; bi[1].range = VK_WHOLE_SIZE;
        bi[2].buffer = F.list; bi[2].offset = 0; bi[2].range = VK_WHOLE_SIZE;
        bi[3].buffer = F.host_buf; bi[3].offset = (VkDeviceSize)HOST_SLOT * i + HOST_CLOUDS;
        bi[3].range = 16 + FAR_MAXCL * sizeof(far_cloud);
        bi[4].buffer = F.wxc; bi[4].offset = 0; bi[4].range = VK_WHOLE_SIZE;
#if PS2_RQ_SUPPORT
        bi[5].buffer = F.rq_draw; bi[5].offset = 0; bi[5].range = VK_WHOLE_SIZE;
        bi[6].buffer = F.rq_sweep; bi[6].offset = 0; bi[6].range = VK_WHOLE_SIZE;
        bi[7].buffer = F.rq_stage; bi[7].offset = 0; bi[7].range = VK_WHOLE_SIZE;
        VkWriteDescriptorSet w[19]; memset(w, 0, sizeof w);
        VkDescriptorImageInfo shi[11]; memset(shi, 0, sizeof shi); static const int bkv[9] = { 0, 1, 5, 0, 1, 2, 3, 4, 5 };   /* (rq7_bake: the views of bindings 12-20) */
        for (uint32_t k = 0; k < 19; k++) {
            w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[k].dstSet = F.set[i]; w[k].dstBinding = b[k].binding;
            w[k].descriptorCount = 1; w[k].descriptorType = t[k];
            if (k < 8) w[k].pBufferInfo = &bi[k];
            else { shi[k - 8].imageView = k < 10 ? F.sh_view[k - 8] : bk_view(bkv[k - 10]); shi[k - 8].imageLayout = VK_IMAGE_LAYOUT_GENERAL; if (k >= 10 && k < 13) shi[k - 8].sampler = bk_sampler(k != 12); w[k].pImageInfo = &shi[k - 8]; }
        }
        vkUpdateDescriptorSets(dev, 19, w, 0, NULL);
#else
        VkWriteDescriptorSet w[5]; memset(w, 0, sizeof w);
        for (uint32_t k = 0; k < 5; k++) {
            w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[k].dstSet = F.set[i]; w[k].dstBinding = b[k].binding;
            w[k].descriptorCount = 1; w[k].descriptorType = t[k]; w[k].pBufferInfo = &bi[k];
        }
        vkUpdateDescriptorSets(dev, 5, w, 0, NULL);
#endif
    }
    F.p_weather = make_pass(g, "cloud_far_weather.comp.spv");
    if (!F.p_weather) return -1;
    F.p_wsoft = make_pass(g, "cloud_far_wsoft.comp.spv");     /* (missing: the splat stays as it is) */
    default_params(&F.par);
    F.err_px = env_f("ACZ_VR_CLOUD_FAR_ERR", 1.0f, 0.05f, 16.0f);
    F.cap = (int)env_f("ACZ_VR_CLOUD_FAR_CAP", 0.0f, 0.0f, 1.0e7f);
    F.start_m = env_f("ACZ_VR_CLOUD_FAR_START", -1.0f, -1.0f, 5000.0f);
    F.max_m = env_f("ACZ_VR_CLOUD_FAR_MAX", 20000.0f, 2000.0f, 60000.0f);
    F.max_m_list = env_f("ACZ_VR_CLOUD_FAR_MAX_LIST", 9000.0f, 2000.0f, 60000.0f);
    F.k_step = env_f("ACZ_VR_CLOUD_FAR_KSTEP", 0.03f, 0.005f, 0.2f);
    F.dt_min = 30.0f;
    F.margin_rad = env_f("ACZ_VR_CLOUD_FAR_MARGIN", 10.0f, 0.0f, 90.0f) * 3.14159265f / 180.0f;
    F.debug = (int)env_f("ACZ_DEBUG_CLOUDS", 0.0f, 0.0f, 1.0e6f);
    F.stale = (int)env_f("ACZ_DEBUG_CLOUD_FAR_STALE", 0.0f, 0.0f, 1.0e6f);
    F.par.dbg[0] = F.stale > 0 ? 1.0f : 0.0f;
    if (F.stale > 0)
        ps2_log("clouds far: ACZ_DEBUG_CLOUD_FAR_STALE: each visible tile the scheduler leaves alone is checked against a "
                "march of its centre texel (sums every %d frames; frames with >= 4 appearing / vanishing tiles logged)", F.stale);
    F.ovc_res = (int)env_f("ACZ_VR_CLOUD_FAR_RES_OVC", 1536.0f, 0.0f, 4096.0f) & ~7;
    F.ovc_steps = (int)env_f("ACZ_VR_CLOUD_FAR_STEPS_OVC", 48.0f, 0.0f, 255.0f);
    F.list_res = (int)env_f("ACZ_VR_CLOUD_FAR_RES_LIST", 1024.0f, 0.0f, 4096.0f) & ~7;
    F.par.sch[2] = F.margin_rad;
    F.par.sch[3] = env_f("ACZ_VR_CLOUD_FAR_LEVELS", 2.0f, 0.0f, 64.0f);
    F.par.fine[0] = env_f("ACZ_VR_CLOUD_FAR_FINE", 0.5f, 0.1f, 1.0f);
    F.par.fine[1] = env_f("ACZ_VR_CLOUD_OD", 3.0f, 0.0f, 100.0f);
    F.od_cu = env_f("ACZ_VR_CLOUD_OD_CU", 12.0f, 0.0f, 100.0f);
    F.thin = env_f("ACZ_VR_CLOUD_FAR_THIN", 0.5f, 0.0f, 4.0f);
    F.par.ext[0] = env_f("ACZ_VR_CLOUD_FILL_CU", 0.65f, 0.1f, 1.0f);
    F.fade_s = env_f("ACZ_VR_CLOUD_LIST_FADE", 1.0f, 0.0f, 30.0f);
    F.par.fine[2] = env_f("ACZ_VR_CLOUD_BILLOW", F.par.fine[2], 0.0f, 0.9f);
    F.par.spl[0] = env_f("ACZ_VR_CLOUD_BLOB_H", F.par.spl[0], 0.2f, 2.0f);
    /* a factor on C1's semi-axes for measurements (S1 publishes the game's drawn size since CL_CLOUD_SPRITE: keep 1) */
    F.size_k = env_f("ACZ_VR_CLOUD_SIZE", 1.0f, 0.1f, 20.0f);
    F.quiet_k = env_f("ACZ_VR_CLOUD_FAR_QUIET", 4.0f, 1.0f, 16.0f);
    F.par.mz[3] = env_f("ACZ_VR_CLOUD_FAR_HALF", F.par.mz[3], 0.0f, 1.0f);
    /* bug #31: the splat's density ramps over +-SPLAT_SOFT metres across the borders between clouds (PASS_WSOFT; at
       most 16 weather texels = 625 m; 0 = the hard borders of before) */
    F.soft_m = env_f("ACZ_VR_CLOUD_SPLAT_SOFT", 400.0f, 0.0f, 625.0f);
    F.par.ext[1] = F.soft_m * (float)CL_WEATHER_RES / CL_WEATHER_M;
    if (F.p_wsoft && F.par.ext[1] >= 0.5f)
        ps2_log("clouds: weather splat: density softened over %.0f m across the borders between clouds "
                "(ACZ_VR_CLOUD_SPLAT_SOFT=0 turns it off)", (double)F.soft_m);
    /* bug #194 (night_sky 2026-10-29, M11 "the night sky a muddy brown-orange"): the game's impostor build scales each
       cloud impostor's alpha by the stage's cloud alpha (C1 imp_alpha: M11 0.50, M7 0.66, M2/M6/M8 0.75, M12 0.78,
       others 1), so its clouds are that much more transparent; our list drew them at the sprite alpha alone: inside
       M11's smoke layer an opaque brown murk (55,43,37 vs the game's 38,35,34 looking up). 0 = as before */
    F.imp_alpha = (int)env_f("ACZ_VR_CLOUD_IMP_ALPHA", 1.0f, 0.0f, 1.0f);
    ps2_log("clouds: the game's impostor alpha (stage cloud alpha / 128) %s (ACZ_VR_CLOUD_IMP_ALPHA)",
            F.imp_alpha ? "scales the cloud list's opacity" : "ignored: the list at the sprite alpha (as before)");
    /* bugs #33 / #57 / #40 (2026-10-22): the list's continuity and the splat's per-texel choice (see list_path and
       cloud_far.comp PASS_WEATHER) */
    F.keep = (int)env_f("ACZ_VR_CLOUD_LIST_KEEP", 1.0f, 0.0f, 1.0f);
#if PS2_RQ_SUPPORT
    /* (PS2_RQ_CLOUDS: the panorama refreshes in about a second: the list's fades follow more slowly, so a tile's age shows
       less of a fade's change) */
    F.ramp_s = env_f("ACZ_VR_CLOUD_LIST_RAMP", ps2_clouds_rq_on() ? 4.0f : 2.0f, 0.0f, 30.0f);
#else
    F.ramp_s = env_f("ACZ_VR_CLOUD_LIST_RAMP", 2.0f, 0.0f, 30.0f);
#endif
    /* bug #159 (cl_storm, 2026-10-28): how a tracked cloud follows C1's opacity (see track_follow) */
    F.ease = (int)env_f("ACZ_VR_CLOUD_LIST_EASE", 1.0f, 0.0f, 1.0f);
#if PS2_RQ_SUPPORT
    F.follow_s = env_f("ACZ_VR_CLOUD_LIST_FOLLOW", ps2_clouds_rq_on() ? 3.0f : 1.0f, 0.1f, 10.0f);
#else
    F.follow_s = env_f("ACZ_VR_CLOUD_LIST_FOLLOW", 1.0f, 0.1f, 10.0f);
#endif
    F.near_rate = env_f("ACZ_VR_CLOUD_AROUND_RATE", 0.0f, 0.0f, 10.0f);
    F.near_m = env_f("ACZ_VR_CLOUD_AROUND_M", 1000.0f, 0.0f, 20000.0f);
    if (F.ramp_s > 0.0f) {
        if (F.ease)
            ps2_log("clouds: list opacities follow C1 smoothly (ACZ_VR_CLOUD_LIST_EASE): entries / exits over %.1f s "
                    "(ACZ_VR_CLOUD_LIST_RAMP), changes within the list 95 %% in %.1f s (ACZ_VR_CLOUD_LIST_FOLLOW)",
                    (double)F.ramp_s, (double)F.follow_s);
        else
            ps2_log("clouds: list opacities restart a %.1f s smoothstep on every C1 change (ACZ_VR_CLOUD_LIST_EASE=0)",
                    (double)F.ramp_s);
    }
    if (F.ramp_s > 0.0f && F.ease && F.near_rate > 0.0f)
        ps2_log("clouds: opacity of clouds within %.0f m of the eye changes at most %.2f per second "
                "(ACZ_VR_CLOUD_AROUND_RATE / _M)", (double)F.near_m, (double)F.near_rate);
    F.mix = env_f("ACZ_VR_CLOUD_SPLAT_MIX", 1.0f, 0.0f, 1.0f);
    F.par.ext[2] = F.mix > 0.5f ? 1.0f : 0.0f;
    ps2_log("clouds: cloud list kept across empty spells %s (ACZ_VR_CLOUD_LIST_KEEP), opacity-weighted splat %s "
            "(ACZ_VR_CLOUD_SPLAT_MIX), governor level 1 on the far map: %s (ACZ_VR_GOV_FAR_MODE)", F.keep ? "on" : "off",
            F.par.ext[2] > 0.5f ? "on" : "off", far_gov_mode() ? "looser limits, cap outside the view only" : "tile cap");
    if (F.debug > 0) {                  /* the weather pass's GPU time (measurement) */
        VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(g->phys, &pp);
        F.wq_ns = pp.limits.timestampPeriod;
        VkQueryPoolCreateInfo qc; memset(&qc, 0, sizeof qc);
        qc.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO; qc.queryType = VK_QUERY_TYPE_TIMESTAMP; qc.queryCount = 2u * FAR_FIF;
        if (vkCreateQueryPool(dev, &qc, NULL, &F.wq) != VK_SUCCESS) F.wq = VK_NULL_HANDLE;
    }
#if PS2_RQ_SUPPORT
    F.rq = ps2_clouds_rq_on();
    F.rq_travel = env_f("ACZ_VR_CLOUD_RQ_TRAVEL", 0.12f, 0.005f, 0.5f);
    F.rq_period = env_f("ACZ_VR_CLOUD_RQ_REFRESH", 64.0f, 8.0f, 1024.0f);
    F.rq_budget = (int)env_f("ACZ_VR_CLOUD_RQ_TILES", 64.0f, 0.0f, 65536.0f);
    F.rq_band = env_f("ACZ_VR_CLOUD_RQ_BAND", 200.0f, 0.0f, 2000.0f);
    F.rq_kstep = env_f("ACZ_VR_CLOUD_RQ_KSTEP", 0.045f, 0.005f, 0.2f);
    F.rq_fine = env_f("ACZ_VR_CLOUD_RQ_FINE", 1.0f, 0.1f, 1.0f);
    if (F.rq && F.debug > 0) F.par.dbg[1] = 1.0f;            /* (rq_cloud_far.comp: the drawn tiles' staleness in cnt[11]) */
    F.par.dbg[2] = F.rq && env_f("PS2_RQ_CLOUDS_FARNB", 0.0f, 0.0f, 1.0f) > 0.5f ? 1.0f : 0.0f;   /* (the empty tiles beside cloud drawn too) */
    /* (clouds6: the per-pixel resolve draws them: content moves across a tile's border; =2 every reachable tile in view: the parallax
       moves content many tiles in third person -- M10's sweep offsets up to 1.3 km, 12 tiles at 4 km -- and one ring cut it in steps) */
    if (F.rq) { float r = env_f("PS2_RQ_CLOUDS_FARRESOLVE", 0.0f, 0.0f, 2.0f); if (r > 0.5f) F.par.dbg[2] = r > 1.5f ? 2.0f : 1.0f; }
    F.par.rq[0] = F.rq && F.rq_swap ? 1.0f : 0.0f;
    F.par.rq[1] = env_f("PS2_RQ_CLOUDS_FARSWAP_MAX", 90.0f, 4.0f, 1024.0f);   /* (a sweep's longest run, frames: then it goes in as it is) */
    F.par.rq[2] = F.rq ? (float)(int)env_f("PS2_RQ_CLOUDS_COLD", 0.0f, 0.0f, 16384.0f) : 0.0f;   /* (a new generation's tiles in view a frame at most) */
    if (F.rq) ps2_log("clouds far rq: a new generation (the first frames, a rebase, a map size change) marches %s (PS2_RQ_CLOUDS_COLD)",
                      F.par.rq[2] > 0.5f ? "at most PS2_RQ_CLOUDS_COLD tiles in view a frame, those outside the view in the budget" : "every reachable tile in its first frame");
    /* (clouds7 2026-11-12, t11a2, R-114 / R-115) the static field's splat. Its list changes in small steps all the time (the opacities
       follow the coverage map under the drifting bodies in 1/64 steps every 10 m of drift; the follower eases each one over
       ACZ_VR_CLOUD_LIST_RAMP): on M10 that re-splatted ~120 frames in 600 and kept the far map at its full refresh rate. With
       PS2_RQ_CLOUDS_SPLAT_EVERY=K and a static list such a change (list_seq, a fade step, its end) waits until K frames after the last
       splat and then goes in with whatever else changed (a pending flag: the last state always lands); a new list path, a rebase, a band
       or valid change and the window's move by ACZ_VR_CLOUD_RQ_WX_MOVE still splat at once. Those small changes no longer restart the far
       map's full refresh rate (FAR_SLICE for 120 frames, else ACZ_VR_CLOUD_FAR_QUIET x slower): the static field's bodies change only
       beyond the far range or by a 1/64 step. PS2_RQ_CLOUDS_SPLAT_CULL=1: the splat's Quest variant (cloud_far.comp RQ_SPLAT_CULL,
       rq_cloud_far_weather.comp.spv): each 8 x 8 texel group loops over the clouds whose disc meets it, the second loop over the first's
       results; the same map (the same terms in the same order). */
    F.wx_every = F.rq ? (uint32_t)env_f("PS2_RQ_CLOUDS_SPLAT_EVERY", 0.0f, 0.0f, 600.0f) : 0u;
    if (F.rq && env_f("PS2_RQ_CLOUDS_SPLAT_CULL", 0.0f, 0.0f, 1.0f) > 0.5f) {
        F.p_weather_rq = make_pass(g, "rq_cloud_far_weather.comp.spv");
        if (!F.p_weather_rq) ps2_log("clouds far rq: rq_cloud_far_weather.comp.spv missing: the splat stays uncut (PS2_RQ_CLOUDS_SPLAT_CULL)");
    }
    /* (R-117) PS2_RQ_CLOUDS_SPLAT_FLOOR=o (with the culled splat): a body's coverage ramps from 0 at opacity o to full where it did before
       (its density 0.05) instead of from 0 at opacity 0, so the bodies the game's fade by the centre's distance took to about 0 leave the
       weather map's occupancy and neither march samples them; continuous in the opacity (no entry or exit) */
    F.wx_floor = F.p_weather_rq ? env_f("PS2_RQ_CLOUDS_SPLAT_FLOOR", 0.0f, 0.0f, 0.12f) : 0.0f;
    if (F.rq && F.wx_floor > 0.0f) ps2_log("clouds far rq: the splat's coverage from opacity %.3f (PS2_RQ_CLOUDS_SPLAT_FLOOR)", (double)F.wx_floor);
    if (F.rq)
        ps2_log("clouds far rq: the static field's splat %s%s (PS2_RQ_CLOUDS_SPLAT_EVERY=%u), the splat %s (PS2_RQ_CLOUDS_SPLAT_CULL)",
                F.wx_every ? "on a list change at most every " : "on every list change", F.wx_every ? "N frames, the far map's refresh not restarted by it" : "",
                F.wx_every, F.p_weather_rq ? "culled per 8 x 8 texels" : "over the whole list");
    if (F.rq)
        ps2_log("clouds far rq: %s (PS2_RQ_CLOUDS_FARSWAP%s)", F.rq_swap ? "the visible tiles re-marched in sweeps from one origin each, staged and put in at once" :
                "the visible tiles re-marched one by one by the travel rule, each from its own origin", F.rq_swap ? "; at most PS2_RQ_CLOUDS_FARSWAP_MAX frames a sweep" : "=0");
#endif
    F.ready = 1;
    return 0;
}

/* Overcast-type weather (C1 type < 0.3, or a high unmasked coverage): soft veils, many semi-transparent layers along
   each ray, so the march is dearer and the detail matters less -> a smaller used map and a step cap. */
static int overcast(const ps2_cloud_env *e) {
    return e && (e->valid & CL_ENV_WEATHER) && (e->type < 0.3f || e->coverage_unmasked > 0.45f);
}
/* The cloud-list path (S1's list, with its tracked fades) instead of the procedural field. Bug #33 (2026-10-22): S1
   publishes an EMPTY list whenever no cloud of the game's field passes its drop threshold (M2: 0-4 of 150 kept while
   flying, 0 for seconds at a time). The old test (ncloud > 0) treated that as "no list": the tracked clouds still
   fading out were dropped at once, the weather map became the procedural field and the far map's used size changed
   (a new generation), and the next listed cloud was "taken as it is" -- full opacity in one frame (M2 field 1978: a
   whole cumulus popped in; V1 09:51:58). ACZ_VR_CLOUD_LIST_KEEP=1 (default): a valid list is the list path even when
   empty (nothing to splat = clear sky; fades continue); 0 = the old test. */
static int list_path(const ps2_cloud_env *e) {
    if (!e || !(e->valid & CL_ENV_LIST)) return 0;
    return e->ncloud > 0 || F.keep;
}
/* The far map's used size N_eff for this frame (ps2_clouds.c puts it into CloudFrame sizes.x before the hooks, and
   cl_dir_tex_uv scales the composite's uv by N_eff / N): ACZ_VR_CLOUD_FAR_RES_LIST (1024) with the game's cloud list
   (S1's soft veils: about 5.7 texels per degree at the horizon are enough), else ACZ_VR_CLOUD_FAR_RES_OVC (1536) for
   overcast weather, else the whole map (the procedural field's cumulus). */
uint32_t ps2_clouds_far_map_size(const ps2_cloud_env *e, uint32_t dir_res) {
    uint32_t n = dir_res;
    int list = list_path(e);
    if (F.ready && F.far_ready && dir_res >= 64u && F.ovc_res >= 64 && overcast(e) && (uint32_t)F.ovc_res < dir_res)
        n = (uint32_t)F.ovc_res;
    if (F.ready && F.far_ready && list && F.list_res >= 64 && (uint32_t)F.list_res < n)
        n = (uint32_t)F.list_res;
    F.n_ubo = n; F.ubo_asked = 1;
    return n;
}

static far_params *slot_params(int fi) { return (far_params *)(F.host + (size_t)HOST_SLOT * (uint32_t)fi + HOST_PARAMS); }

/* the weather window centre relative to the eye, and whether nothing lies beyond it (the cloud-list path) */
/* the per-cloud optical depth for this weather: ACZ_VR_CLOUD_OD for the overcast veils (C1 type <= 0.2), _OD_CU for
   cumulus-like weather (type >= 0.6), smoothly in between; 0 in either = full density */
static float od_eff(const ps2_cloud_env *e) {
    float od = F.par.fine[1], cu = F.od_cu;
    if (od <= 0.0f || cu <= 0.0f) return 0.0f;
    float ty = e && (e->valid & CL_ENV_WEATHER) ? e->type : 0.7f;
    float w = (ty - 0.2f) / 0.4f;
    w = w < 0.0f ? 0.0f : w > 1.0f ? 1.0f : w;
    w = w * w * (3.0f - 2.0f * w);
    return od + (cu - od) * w;
}
/* the march range with the game's cloud list: its clouds fade out by the stage visibility (S1 folds the game's far fade
   into their opacity), so 1.1 x C1 fog_far_m (the bodies reach past their centres), at most ACZ_VR_CLOUD_FAR_MAX_LIST */
static float list_range(const ps2_cloud_env *e) {
    float r = e && (e->valid & CL_ENV_COLOURS) && e->fog_far_m > 1000.0f ? 1.1f * e->fog_far_m : F.max_m_list;
    return r < F.max_m_list ? r : F.max_m_list;
}
static void fill_params(int fi, const ps2_cloud_env *e, const ps2_cloud_view *v) {
    far_params *p = slot_params(fi);
    *p = F.par;
    p->fine[1] = od_eff(e);
    /* thin-medium steps for cumulus-like weather (the list's faint clouds would use up the step budget) */
    {   float ty = e && (e->valid & CL_ENV_WEATHER) ? e->type : 0.7f, w = (ty - 0.2f) / 0.4f;
        w = w < 0.0f ? 0.0f : w > 1.0f ? 1.0f : w;
        p->haze[0] = rq_one_thin(w > 0.5f ? F.thin : 0.0f); }
    int list = list_path(e);
    /* ext.w: the governor's level 1 caps only the tiles outside the view (ACZ_VR_GOV_FAR_MODE=1; PASS_ARGS) */
    {   int gon, grelax; far_gov_state(F.frames, &gon, &grelax); p->ext[3] = gon && grelax ? 1.0f : 0.0f; }
    if (F.have_splat) {
        p->win[0] = (float)(F.splat_origin[0] - v->cloud_origin_m[0]);
        p->win[1] = (float)(F.splat_origin[2] - v->cloud_origin_m[2]);
    }
    p->win[2] = list ? 1.0f : 0.0f;
    if (list) { p->haze[1] = 1.0e9f; p->haze[2] = 2.0e9f; }   /* no distance fade: S1's opacity carries the game's */
#if PS2_RQ_SUPPORT
    /* (clouds7, ACZ_VR_CLOUD_STATIC: the list's opacities carry no far fade; each sample fades by its distance, full to half the
       game's cloud far, gone at it -- rq_cloud_near.comp rq_game_fade the same) */
    if (list && F.rq && e->list_static && e->cloud_far_m > 100.0f) { p->haze[1] = 0.5f * e->cloud_far_m; p->haze[2] = e->cloud_far_m; }
#endif
}

static void barrier2(VkCommandBuffer cmd, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkMemoryBarrier2 mb; memset(&mb, 0, sizeof mb);
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    mb.srcStageMask = ss; mb.srcAccessMask = sa; mb.dstStageMask = ds; mb.dstAccessMask = da;
    VkDependencyInfo di; memset(&di, 0, sizeof di);
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO; di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &di);
}
#define CS VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
#define XS VK_PIPELINE_STAGE_2_TRANSFER_BIT
#define RW (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT)

/* A tracked cloud's new target opacity. ACZ_VR_CLOUD_LIST_RAMP > 0 (bug #33, 2026-10-22): the change runs as a
   smoothstep over ramp x min(1, |change| / 0.1) seconds (at least the constant-rate time below), restarted from the
   current value: a cloud entering or leaving the list (S1's threshold: opacity 0.1, which our volume shows far more
   strongly than the game's 13/128 sprite alpha -- seen edge-on a wide cumulus at 0.125 is mostly opaque) takes the
   whole ramp; S1's 1/64 steps take a fraction of it. 0 = the constant rate of before: ACZ_VR_CLOUD_LIST_FADE seconds
   for a full 0 -> 1 change, so 0 -> 0.125 took 0.125 s (M2: a whole cumulus "appeared from nowhere" in 15 frames). */
static void track_target(far_track *t, float tgt) {
    if (tgt == t->tgt && t->age >= 0.0f) return;
    float d = fabsf(tgt - t->cur);
    float lin = F.fade_s > 0.0f ? d * F.fade_s : 0.0f;
    float dur = F.ramp_s * (d < 0.1f ? d / 0.1f : 1.0f);
    t->from = t->cur; t->tgt = tgt; t->age = 0.0f; t->dur = dur > lin ? dur : lin;
}
/* ACZ_DEBUG_CLOUDS (cl_storm, bug #159): a tracked cloud's surface distance from the eye (m; < 0 inside its body,
   0.9 x its semi-axes as the near gate) and a log line when a cloud near the eye gets a new target */
static double trk_surface(const far_track *t, const ps2_cloud_view *v) {
    double dx = t->x - v->cloud_origin_m[0], dz = t->z - v->cloud_origin_m[2], dy = (double)t->y - v->cloud_origin_m[1];
    double a = 0.9 * t->r, b = t->h > 0.0f ? 0.9 * t->h : a;
    if (a < 1.0) a = 1.0;
    if (b < 1.0) b = 1.0;
    double k = sqrt((dx * dx + dz * dz) / (a * a) + dy * dy / (b * b));
    return (k - 1.0) * (a < b ? a : b);
}
static void trk_note(const far_track *t, const ps2_cloud_view *v, float tgt, const char *why) {
    double s = trk_surface(t, v);
    if (s > 1500.0 || fabsf(tgt - t->tgt) < 0.03f) return;
    double dx = t->x - v->cloud_origin_m[0], dz = t->z - v->cloud_origin_m[2];
    ps2_log("clouds near-change: field %llu: cloud R %.0f H %.0f alt %.0f (centre %.0f m away, surface %.0f m) opacity "
            "%.3f (now %.3f) -> %.3f %s", (unsigned long long)ps2_kernel_vblank_count(), (double)t->r, (double)t->h,
            (double)t->y, sqrt(dx * dx + dz * dz), s, (double)t->tgt, (double)t->cur, (double)tgt, why);
}

/* ACZ_VR_CLOUD_LIST_EASE=1 (bug #159, cl_storm 2026-10-28): the opacity follows C1 as a critically damped spring
   (continuous in value and rate; exact for a constant target over the step), instead of a smoothstep restarted from
   rest on every C1 change. The game's sprite alpha follows the coverage map with no easing at all, and in a windy stage
   C1 changes a cloud every few frames: M8 (Merlon, wind 133 m/s, clouds 3-6 km across) steps a cloud the eye is in by
   0.06-0.13 every 0.3 s, 0 -> 1 or 1 -> 0 in 2-3 s (the clouds drift over the game's fixed coverage map; checked on the
   game's own coverage samples). The restarted ramp (a full 2 s smoothstep from zero rate each time: ~6 % of the gap per
   0.3 s) lagged up to 0.8 behind for seconds and then closed the gap in one 2 s S-curve after the game's change had
   stopped (up to 0.56 in a second, all clouds of a gust together) -- a delayed "reload" of the cloud around the eye the
   PS2 never shows. The spring keeps the list ramp for clouds entering or leaving (from or to below 0.1: the whole cumulus
   still fades in over ACZ_VR_CLOUD_LIST_RAMP, bug #33) and follows changes within the list with a 95 % time of
   ACZ_VR_CLOUD_LIST_FOLLOW (1 s; a steady fade lags 0.42 s behind the game's). ACZ_VR_CLOUD_AROUND_RATE=r (opt-in,
   0 = off): a cloud whose body is within ACZ_VR_CLOUD_AROUND_M of the eye (or around it) changes at most r per
   second -- not the game's timing; it spreads the game's quick fades of the cloud you fly in over 1 / r seconds. */
static void track_follow(far_track *t, float dt, const ps2_cloud_view *v) {
    float w = 4.744f / (t->tgt <= 0.0f || t->cur < 0.1f ? F.ramp_s : F.follow_s);   /* 95 % of a step in that time */
    float x = t->cur - t->tgt, e = expf(-w * dt), k = (t->vel + w * x) * dt;
    float nx = (x + k) * e, nv = (t->vel - w * k) * e;
    if (F.near_rate > 0.0f && trk_surface(t, v) < (double)F.near_m) {   /* the opt-in cap near the eye */
        float lim = F.near_rate * dt, d = (t->tgt + nx) - t->cur;
        if (d > lim) { nx = t->cur + lim - t->tgt; nv = F.near_rate; }
        else if (d < -lim) { nx = t->cur - lim - t->tgt; nv = -F.near_rate; }
    }
    t->cur = t->tgt + nx; t->vel = nv;
    if (t->cur < 0.0f) { t->cur = 0.0f; if (t->vel < 0.0f) t->vel = 0.0f; }
    if (t->cur > 1.0f) { t->cur = 1.0f; if (t->vel > 0.0f) t->vel = 0.0f; }
    if (fabsf(t->cur - t->tgt) < 0.004f && fabsf(t->vel) < 0.02f) { t->cur = t->tgt; t->vel = 0.0f; }   /* settled */
}
/* Ease each cloud's splat opacity toward C1's value (track_target): S1 adds or drops a
   cloud and steps its opacity (1/64) as the clouds drift over the game's coverage map, which the splat followed at
   once (popping; the game's own sprites change gradually). On a new list_seq the published clouds are matched to the
   tracked ones by cloud-frame position and radius: new ones fade in, dropped ones fade out. The first list or a rebase
   are taken as they are (and, with ACZ_VR_CLOUD_LIST_KEEP=0, a list after an empty spell). Returns 1 while anything is
   still fading. */
static int track_update(const ps2_cloud_env *e, const ps2_cloud_view *v) {
    double dt = F.trk_ok ? v->time_s - F.trk_time : 0.0;
    F.trk_time = v->time_s;
    if (dt < 0.0) dt = 0.0;
    if (dt > 0.25) dt = 0.25;                       /* a hitch or a pause: at most a quarter second per frame */
    int reset = !F.trk_ok || (F.splat_rebase != v->rebase && !rq_one_rebase(v));
    if (reset || F.splat_seq != e->list_seq) {
        uint32_t nc = e->ncloud < CL_MAX_CLOUDS ? e->ncloud : CL_MAX_CLOUDS;
        if (F.debug > 0 && (reset || nc != F.ntrk))
            ps2_log("clouds track: field %llu: list_seq %u, %u published, %u tracked%s", (unsigned long long)ps2_kernel_vblank_count(), e->list_seq, nc, F.ntrk,
                    !reset ? "" : F.trk_ok ? " -- RESET by a rebase (taken as they are)" : " -- RESET (taken as they are)");
        if (reset) F.ntrk = 0;
        for (uint32_t i = 0; i < F.ntrk; i++) F.trk[i].seen = 0;
        for (uint32_t k = 0; k < nc; k++) {
            double x = e->origin_m[0] + (double)e->cloud[k].pos_m[0], z = e->origin_m[2] + (double)e->cloud[k].pos_m[2];
            float r = e->cloud[k].radius_m, op = e->cloud[k].opacity;
            far_track *t = NULL;
            int fresh = 0;
            for (uint32_t i = 0; i < F.ntrk && !t; i++)
                if (!F.trk[i].seen && fabs(F.trk[i].x - x) < 2.0 && fabs(F.trk[i].z - z) < 2.0 && fabsf(F.trk[i].r - r) < 2.0f)
                    t = &F.trk[i];
            if (!t) {
                if (F.ntrk >= FAR_MAXCL) continue;
                t = &F.trk[F.ntrk++];
                fresh = 1;
                t->cur = reset ? op : 0.0f;          /* a cloud new to the list fades in */
                t->tgt = t->from = t->cur; t->age = -1.0f; t->dur = 0.0f;   /* (track_target below starts its ramp) */
                t->vel = 0.0f;
                if (F.debug > 0) {
                    double ex = x - v->cloud_origin_m[0], ez = z - v->cloud_origin_m[2];
                    ps2_log("clouds track: + cloud R %.0f m alt %.0f m opacity %.3f at %.0f m from the eye: %s", (double)r,
                            (double)e->cloud[k].pos_m[1], (double)op, sqrt(ex * ex + ez * ez), reset ? "at once" : "fading in");
                }
                /* the seed of its envelope puffs: from where it is (the same after a reset at the same place) */
                uint32_t hs = (uint32_t)(int64_t)floor(x) * 0x9E3779B1u ^ (uint32_t)(int64_t)floor(z) * 0x85EBCA77u;
                hs ^= hs >> 15; hs *= 0x2C1B3C6Du; hs ^= hs >> 12;
                t->seed = 1u + (hs & 0xFFFFFEu);                /* 1 .. 2^24 - 1: exact in a float */
            }
            t->x = x; t->z = z; t->y = e->cloud[k].pos_m[1]; t->r = r; t->var = e->cloud[k].variant;
#ifdef CL_CLOUD_SPRITE
            t->h = e->cloud[k].height_m;             /* the body's vertical semi-axis (S1) */
#else
            t->h = 0.0f;                             /* the shader's aspect spl.x */
#endif
            CL_FAR_LIST_NOTE(F.list_dbg && !reset && (fresh || (t->tgt <= 0.0f && op > 0.0f)), t, v, e, op, fresh ? '+' : 'r'); if (F.debug > 0 && !reset) trk_note(t, v, op, fresh ? "(new)" : "");
            if (F.ramp_s > 0.0f && !F.ease) track_target(t, op); else t->tgt = op;
            t->seen = 1;
#if PS2_RQ_SUPPORT
            F.trk_hold[t - F.trk] = 0.0f;             /* (ACZ_VR_CLOUD_LIST_HOLD: in the list again, its published opacity) */
#endif
        }
        for (uint32_t i = 0; i < F.ntrk; i++)                 /* dropped: fade out */
            if (!F.trk[i].seen) {
                CL_FAR_LIST_NOTE(F.list_dbg && F.trk[i].tgt > 0.0f && !(F.list_hold && F.trk_hold[i] > 0.0f), &F.trk[i], v, e, F.trk[i].tgt, '-'); if (F.debug > 0) trk_note(&F.trk[i], v, 0.0f, "(dropped)");
#if PS2_RQ_SUPPORT
                /* ACZ_VR_CLOUD_LIST_HOLD (clouds6 2026-11-11, A.4 P4, R-83; default 0, the Quest 1): the game drops a cloud when its opacity
                   (coverage x the far fade by its CENTRE's distance) falls under ~0.1; M2's bodies are 4-8 km across, so their near surface
                   is often 1 km away when that happens and the whole body faded out over the list's ramp ("vanishes"). Held: it keeps its
                   last opacity while its own SURFACE is within half the fog far and fades out by that surface's distance to the fog far
                   (below, every frame) */
                /* (clouds7 t11a2, R-116: not with the centre-faded static field, list_static 2 -- its bodies leave at opacity ~0, and the
                   held ones piled up: 30 -> 141 tracks against ~32 published on the 11-11 M10 tape, each one in the splat) */
                if (F.list_hold && e->list_static != 2u && rq_one_cfg() < 2 && (F.trk_hold[i] > 0.0f || F.trk[i].tgt > 0.0f)) {
                    if (F.trk_hold[i] <= 0.0f) F.trk_hold[i] = F.trk[i].tgt;
                    continue;
                }
#endif
                if (F.ramp_s > 0.0f && !F.ease) track_target(&F.trk[i], 0.0f); else F.trk[i].tgt = 0.0f;
            }
        F.trk_ok = 1;
    }
    CL_FAR_LIST_SUM(F.list_dbg); float step = F.fade_s > 0.0f ? (float)dt / F.fade_s : 1.0f;
#if PS2_RQ_SUPPORT
    if (F.list_hold) {                                  /* (ACZ_VR_CLOUD_LIST_HOLD: a held cloud's target by its surface's distance, every frame) */
        double far = e->fog_far_m > 100.0f ? (double)e->fog_far_m : 8000.0;
        for (uint32_t i = 0; i < F.ntrk; i++) {
            if (F.trk_hold[i] <= 0.0f) continue;
            double sd = trk_surface(&F.trk[i], v), k = (far - sd) / (0.5 * far);
            k = k < 0.0 ? 0.0 : k > 1.0 ? 1.0 : k;
            float tg = (float)(F.trk_hold[i] * k);
            if (tg <= 0.0f) F.trk_hold[i] = 0.0f;
            if (F.ramp_s > 0.0f && !F.ease) { if (fabsf(tg - F.trk[i].tgt) > 0.004f || tg <= 0.0f) track_target(&F.trk[i], tg); }
            else F.trk[i].tgt = tg;
        }
    }
#endif
    int fading = 0; uint32_t m = 0; rq_one_snap();
    for (uint32_t i = 0; i < F.ntrk; i++) {
        far_track *t = &F.trk[i];
        if (F.ramp_s > 0.0f && F.ease) {
            if (t->cur != t->tgt || t->vel != 0.0f) track_follow(t, (float)dt, v);
        } else if (F.ramp_s > 0.0f) {                          /* the smoothstep ramp of track_target */
            t->age += (float)dt;
            float a = t->dur > 0.0f ? t->age / t->dur : 1.0f;
            t->cur = a >= 1.0f ? t->tgt : t->from + (t->tgt - t->from) * a * a * (3.0f - 2.0f * a);
        } else {
            float d = t->tgt - t->cur;
            t->cur = fabsf(d) <= step ? t->tgt : t->cur + (d > 0.0f ? step : -step);
        }
        if (t->cur != t->tgt || t->vel != 0.0f) fading = 1;
#if PS2_RQ_SUPPORT
        if (t->cur > 0.0f || t->tgt > 0.0f) { F.trk_hold[m] = F.trk_hold[i]; F.trk[m++] = *t; }   /* drop the faded-out */
#else
        if (t->cur > 0.0f || t->tgt > 0.0f) F.trk[m++] = *t;        /* drop the faded-out */
#endif
    }
    F.ntrk = m;
    return fading;
}

/* The near gate's cloud set (ps2_clouds_near.c near_active) with ACZ_VR_CLOUD_LIST_KEEP: the clouds the weather map
   holds this frame -- the tracked list, including clouds still fading out after S1 dropped them -- instead of only C1's
   published list (an empty C1 list made the gate march the whole band; a cloud fading out near the eye had no near
   field). Returns -1 when not tracking (the caller keeps C1's list), else 1 if some body (0.9 x its semi-axes) comes
   within range + pad, 0 if none; *nearest = the conservative distance bound near_active uses (1e30 = none). */
int ps2_clouds_far_near_gate(const ps2_cloud_view *v, float range, float pad, float *nearest) {
    *nearest = 1e30f;
    if (!F.ready || !F.keep || !F.trk_ok) return -1;
    double alt = v->cloud_origin_m[1];
    float best = 1e30f;
    for (uint32_t i = 0; i < F.ntrk; i++) {
        const far_track *t = &F.trk[i];
        if (t->cur <= 0.0f) continue;
#if PS2_RQ_SUPPORT
        if (F.wx_floor > 0.0f && t->cur <= F.wx_floor) continue;   /* (R-119: under the splat's floor it has no coverage: nothing to march) */
#endif
        double dx = t->x - v->cloud_origin_m[0], dz = t->z - v->cloud_origin_m[2], dy = (double)t->y - alt;
        double a = 0.9 * t->r * F.size_k, b = t->h > 0.0f ? 0.9 * t->h * F.size_k : a;
        if (a < 1.0) a = 1.0;
        if (b < 1.0) b = 1.0;
        double k = sqrt((dx * dx + dz * dz) / (a * a) + dy * dy / (b * b));
        float d = (float)((k - 1.0) * (a < b ? a : b));
        if (d < best) best = d;
    }
    *nearest = best;
    return best < range + pad;
}

/* bx_immerse (the cloud feel's in-cloud value, bug #34): the clouds the weather splat draws -- the tracked list, fading
   ones included, at their eased opacity -- for the CPU copy of the splat in ps2_video.c (cloud_body). Per cloud 8
   floats: cloud-frame x, z relative to org, altitude, R and H (x ACZ_VR_CLOUD_SIZE), opacity x (1 - transp) (the
   splat's info.y), the puff seed, 0; par[10]: od_eff, core fill ext.x, splat mix ext.z, billows fine.z, puffs fine.w,
   spl.x..w, keep. Renderer thread (as the splat). Returns the count, -1 off the list path or before tracking starts. */
int ps2_clouds_far_splat_list(const ps2_cloud_env *e, const double org[3], float *out, int max, float par[10]) {
    if (!F.ready || !list_path(e) || !F.trk_ok) return -1;
    float tr = 1.0f - (e->valid & CL_ENV_WEATHER ? e->transp : 0.0f);
    int n = 0;
    for (uint32_t i = 0; i < F.ntrk && n < max; i++) {
        const far_track *t = &F.trk[i];
        if (t->cur <= 0.0f) continue;
        float *o = out + 8 * n++;
        o[0] = rq_one_wrapf(t->x - org[0]); o[1] = rq_one_wrapf(t->z - org[2]); o[2] = t->y; o[3] = t->r * F.size_k;
        o[4] = t->h * F.size_k; o[5] = t->cur * tr; o[6] = (float)t->seed; o[7] = 0.0f;
    }
    par[0] = od_eff(e); par[1] = F.par.ext[0]; par[2] = F.par.ext[2]; par[3] = F.par.fine[2]; par[4] = F.par.fine[3];
    par[5] = F.par.spl[0]; par[6] = F.par.spl[1]; par[7] = F.par.spl[2]; par[8] = F.par.spl[3]; par[9] = (float)F.keep;
    return n;
}

int ps2_clouds_weather_init(const ps2_cloud_gpu *g) {
    if (common_init(g) != 0) { ps2_log("clouds: far module init failed (weather)"); return -1; }
    return 0;
}

/* Re-splat when the eye has moved a weather texel in the cloud frame, the list changed (list_seq, membership, valid
   bits), the band changed (the splat normalises the blobs' base/top by it), the procedural inputs changed, or the
   cloud frame was rebased. Returns 1: S2 owns the weather map. */
int ps2_clouds_weather_record(const ps2_cloud_gpu *g, VkCommandBuffer cmd, const ps2_cloud_rec *r) {
    if (!F.ready || !F.p_weather) return 0;
    const ps2_cloud_env *e = r->env;
    const ps2_cloud_view *v = r->view;
    int fi = r->frame_index;
    if (fi < 0 || fi >= (int)FAR_FIF) return 0;
    if (F.wq) {                         /* this slot's previous splat time (its fence has passed); logged per 600 calls */
        F.wq_calls++;
        uint64_t ts[2];
        if (F.wq_used[fi] && vkGetQueryPoolResults(F.dev, F.wq, 2u * (uint32_t)fi, 2, sizeof ts, ts, 8,
                                                   VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            F.wq_ms += (double)(ts[1] - ts[0]) * F.wq_ns * 1e-6; F.wq_n++;
        }
        F.wq_used[fi] = 0;
        if (F.wq_calls % 600u == 0u && F.wq_n) {
            ps2_log("clouds weather: splat %.3f ms GPU (%u splats in the last 600 frames = %.3f ms a frame; %s splat)",
                    F.wq_ms / F.wq_n, F.wq_n, F.wq_ms / 600.0, F.par.ext[2] > 0.5f ? "opacity-weighted" : "old");
            F.wq_ms = 0.0; F.wq_n = 0;
#if PS2_RQ_SUPPORT
            if (F.rq)
                ps2_log("clouds weather rq: the last 600 frames' splats by cause: new/rebase/band/valid %u, list %u, fade %u, window move %u, "
                        "held list changes %u (PS2_RQ_CLOUDS_SPLAT_EVERY=%u), fired after a hold %u", F.wx_why[0], F.wx_why[1], F.wx_why[2],
                        F.wx_why[3], F.wx_why[4], F.wx_every, F.wx_why[5]);
            memset(F.wx_why, 0, sizeof F.wx_why);
#endif
        }
    }
    int list = list_path(e);
    const double T = CL_WEATHER_M / (double)CL_WEATHER_RES;
    float key[4] = { e->base_m, e->top_m, list ? 0.0f : e->coverage, list ? od_eff(e) : e->type };
    int fading = list && track_update(e, v);
    /* a fade re-splats every 3rd frame and once when it ends (the pass costs ~0.12 ms with M1's ~100 clouds on the
       3090 Ti; a 1 s fade is 40 steps of 2.5 %) */
#if PS2_RQ_SUPPORT
    /* PS2_RQ_CLOUDS: the splat follows the eye only every ACZ_VR_CLOUD_RQ_WX_MOVE metres (150; the window's 200 m ring beyond the
       march's range stays unread) and a fade re-splats every 8th frame: the splat's cost is only in the frames that do it */
    const double Tm = rq_one_cfg() >= 2 ? 1.0e30 : F.rq ? (double)env_f("ACZ_VR_CLOUD_RQ_WX_MOVE", 150.0f, 39.0f, 200.0f) : T;   /* (PS2_RQ_CLOUDS_ONE=2: the period's torus, no window to follow) */
    int fade_due = fading ? (++F.fade_ctr % (F.rq ? 8u : 3u)) == 0u : F.fade_was;
#else
    const double Tm = T;
    int fade_due = fading ? (++F.fade_ctr % 3u) == 0u : F.fade_was;
#endif
    F.fade_was = fading;
    /* ACZ_VR_CLOUD_FADE_TEST=from,to (fields; test, cl_pop 2026-10-31): every listed cloud's opacity x a linear ramp 0 -> 1
       from field `from` to `to` (0 before), re-splat every 3rd frame inside the window as a list fade: a repeatable fade-in
       of everything in view for the weather map's precision (ACZ_VR_CLOUD_WX_SQRT A/B) */
    float ftk = 1.0f;
    {   static int ft_init; static float ft[2];
        if (!ft_init) {
            ft_init = 1;
            const char *fe = getenv("ACZ_VR_CLOUD_FADE_TEST");
            if (fe && sscanf(fe, "%f,%f", &ft[0], &ft[1]) == 2 && ft[1] > ft[0])
                ps2_log("clouds: ACZ_VR_CLOUD_FADE_TEST: the list's opacities ramp 0 -> 1 from field %.0f to %.0f", (double)ft[0], (double)ft[1]);
            else ft[0] = ft[1] = 0.0f;
        }
        if (ft[1] > ft[0]) {
            float f = (float)ps2_kernel_vblank_count();
            ftk = f <= ft[0] ? 0.0f : f >= ft[1] ? 1.0f : (f - ft[0]) / (ft[1] - ft[0]);
            static float ft_last = -1.0f;
            if (ftk != ft_last && (fade_due || ft_last < 0.0f || ftk >= 1.0f || (++F.fade_ctr % 3u) == 0u)) { fade_due = 1; ft_last = ftk; }
        }
    }
    int need = !F.have_splat || F.splat_rebase != v->rebase || memcmp(key, F.splat_key, sizeof key) != 0
            || (list && F.splat_seq != e->list_seq) || F.splat_valid != (e->valid & CL_ENV_LIST) || fade_due
            || fabs(v->cloud_origin_m[0] - F.splat_origin[0]) >= Tm || fabs(v->cloud_origin_m[2] - F.splat_origin[2]) >= Tm;
#if PS2_RQ_SUPPORT
    int rq_soft = 0;                    /* (a static list's small change: no restart of the far map's full refresh) */
    if (F.rq) {
        F.wx_n++;
        int hard = !F.have_splat || F.splat_rebase != v->rebase || memcmp(key, F.splat_key, sizeof key) != 0
                || F.splat_valid != (e->valid & CL_ENV_LIST);
        int mv = fabs(v->cloud_origin_m[0] - F.splat_origin[0]) >= Tm || fabs(v->cloud_origin_m[2] - F.splat_origin[2]) >= Tm;
        int lch = list && F.splat_seq != e->list_seq;
        if (F.wx_every && list && e->list_static && !hard && rq_one_cfg() < 2) {
            if (lch || fade_due) F.wx_pend = 1;
            rq_soft = 1;
            if (F.wx_pend && !mv && F.wx_n - F.wx_last < F.wx_every) { if (lch || fade_due) F.wx_why[4]++; need = 0; }
            else if (F.wx_pend) { need = 1; F.wx_why[5]++; }
        }
        if (need) {
            F.wx_why[hard ? 0 : mv ? 3 : lch ? 1 : 2]++; F.bk_splat_n++; if (hard) F.bk_hard_n++; if (hard || lch || fade_due || F.wx_pend) F.bk_content_n++;   /* (rq7_bake, PS2_RQ_CLOUDS_BAKE: a hard splat re-bakes the volumes whole; one that carries a list or fade change, held or not, in rolling slices; the window's move alone changes nothing the march reads) */
            F.wx_last = F.wx_n; F.wx_pend = 0;
        }
    }
#endif
    if (F.debug > 0 && !list && F.trk_ok)
        ps2_log("clouds track: field %llu: no list path (valid %X, %u clouds): %u tracked clouds dropped at once, the weather map "
                "becomes the procedural field (coverage %.2f)", (unsigned long long)ps2_kernel_vblank_count(), e->valid, e->ncloud, F.ntrk, (double)e->coverage);
    if (!list) F.trk_ok = 0;                       /* a list that comes back later starts without fades */
    if (!need) return 1;
    /* a content change (not only the window following the eye): the far tiles refresh at the full FAR_SLICE rate */
    if (!F.have_splat || F.splat_rebase != v->rebase || memcmp(key, F.splat_key, sizeof key) != 0
        || (list && F.splat_seq != e->list_seq) || F.splat_valid != (e->valid & CL_ENV_LIST) || fade_due)
#if PS2_RQ_SUPPORT
        if (!rq_soft || !F.have_splat || F.splat_rebase != v->rebase || memcmp(key, F.splat_key, sizeof key) != 0
            || F.splat_valid != (e->valid & CL_ENV_LIST))
#endif
        F.wx_quiet = 0;
    uint8_t *slot = F.host + (size_t)HOST_SLOT * (uint32_t)fi;
    uint32_t *hdr = (uint32_t *)(slot + HOST_CLOUDS);
    far_cloud *cl = (far_cloud *)(slot + HOST_CLOUDS + 16);
    uint32_t n = 0;
    /* Only the published copies: S1 publishes each cloud once (the copy within +-10.9 km of origin_m, which stays
       within 2 km of the eye) and drops the copies the game's coverage map masks there. Further copies of the periodic
       list would each need their own coverage lookup; they are not needed, since the cloud-list path fades out by
       ACZ_VR_CLOUD_FAR_MAX_LIST (6 km). So no tiling by list_period_m (the shader's wrap stays off: period 0). */
    float period = rq_one_period(e);
    if (list) {
        float tr = 1.0f - (e->valid & CL_ENV_WEATHER ? e->transp : 0.0f);
        if (F.imp_alpha && (e->valid & CL_ENV_WEATHER) && e->imp_alpha > 0.0f && e->imp_alpha < 1.0f)
            tr *= e->imp_alpha;                    /* (bug #194: the game's impostor alpha scale) */
        for (uint32_t i = 0; i < F.ntrk && n < FAR_MAXCL; i++) {
            const far_track *t = &F.trk[i];
            if (t->cur <= 0.0f) continue;
            /* cloud space of this frame: the tracked cloud-frame position minus the eye's; y stays the altitude */
            far_cloud *c = &cl[n++];
            c->pos_r[0] = (float)(t->x - v->cloud_origin_m[0]); c->pos_r[1] = t->y;
            c->pos_r[2] = (float)(t->z - v->cloud_origin_m[2]); c->pos_r[3] = t->r * F.size_k;
            c->info[0] = (float)t->var; c->info[1] = t->cur * tr * ftk; c->info[2] = t->h * F.size_k; c->info[3] = (float)t->seed;
        }
    }
    /* hdr.z: the list path (with ACZ_VR_CLOUD_LIST_KEEP an empty list splats clear sky, not the procedural field) */
    hdr[0] = n; memcpy(&hdr[1], &period, 4); hdr[2] = list && F.keep ? 1u : 0u; hdr[3] = 0;
    F.splat_origin[0] = v->cloud_origin_m[0]; F.splat_origin[1] = v->cloud_origin_m[1]; F.splat_origin[2] = v->cloud_origin_m[2];
    F.have_splat = 1; F.splat_seq = e->list_seq; F.splat_valid = e->valid & CL_ENV_LIST; F.splat_rebase = v->rebase;
    memcpy(F.splat_key, key, sizeof key);
    fill_params(fi, e, v);
    VkDescriptorSet s = F.set[fi];
#if PS2_RQ_SUPPORT
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, F.p_weather_rq ? F.p_weather_rq : F.p_weather);
#else
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, F.p_weather);
#endif
    ps2_clouds_bind_common(cmd, F.layout);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, F.layout, 1, 1, &s, 0, NULL);
    far_push pc; memset(&pc, 0, sizeof pc);
#if PS2_RQ_SUPPORT
    if (F.p_weather_rq) { pc.cam[0] = F.wx_floor; }   /* (rq_cloud_far_weather.comp: the opacity floor, PS2_RQ_CLOUDS_SPLAT_FLOOR) */ if (F.p_weather_rq && rq_one_period(e) > 0.0f) pc.cam[1] = 1.0f;   /* (PS2_RQ_CLOUDS_ONE=2: the map is the period's torus) */
#endif
    vkCmdPushConstants(cmd, F.layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    if (F.wq) {
        vkCmdResetQueryPool(cmd, F.wq, 2u * (uint32_t)fi, 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, F.wq, 2u * (uint32_t)fi);
    }
    vkCmdDispatch(cmd, CL_WEATHER_RES / 8u, CL_WEATHER_RES / 8u, 1);
    far_tr_flags[fi] |= 1u;
    /* the density softened across the borders between the listed clouds (PASS_WSOFT reads a copy of the splat) */
    if (list && n > 0u && F.p_wsoft && g && g->weather && F.par.ext[1] >= 0.5f) {
        barrier2(cmd, CS, RW, XS, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy rg; memset(&rg, 0, sizeof rg);
        rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rg.imageSubresource.layerCount = 1;
        rg.imageExtent.width = rg.imageExtent.height = CL_WEATHER_RES; rg.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(cmd, g->weather, VK_IMAGE_LAYOUT_GENERAL, F.wxc, 1, &rg);
        barrier2(cmd, XS, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, RW);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, F.p_wsoft);
        vkCmdDispatch(cmd, CL_WEATHER_RES / 16u, CL_WEATHER_RES / 16u, 1);
        far_tr_flags[fi] |= 2u;
    }
    if (F.wq) { vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, F.wq, 2u * (uint32_t)fi + 1u); F.wq_used[fi] = 1; }
#if PS2_RQ_SUPPORT
    if (F.rq) bk_splat_note(cl, n, v);   /* (rq7_bake, PS2_RQ_CLOUDS_BAKE: where this splat changed the weather map) */
#endif
    return 1;
}

int ps2_clouds_far_init(const ps2_cloud_gpu *g) {
    if (common_init(g) != 0) return -1;
    if (F.n != g->dir_res) { ps2_log("clouds: far map size changed after init"); return -1; }
    F.p_tiles = make_pass(g, "cloud_far_tiles.comp.spv");
    F.p_args = make_pass(g, "cloud_far_args.comp.spv");
    F.p_march = make_pass(g, "cloud_far_march.comp.spv");
    if (!F.p_tiles || !F.p_args || !F.p_march) return -1;
    F.have_anchor = 0; F.cleared = 0; F.gen = 1; F.frames = 0;
    if (far_ab() > 0) {
        VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(g->phys, &pp);
        F.wq_ns = pp.limits.timestampPeriod;
        VkQueryPoolCreateInfo qc; memset(&qc, 0, sizeof qc);
        qc.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO; qc.queryType = VK_QUERY_TYPE_TIMESTAMP; qc.queryCount = 2u * FAR_FIF;
        if (vkCreateQueryPool(g->dev, &qc, NULL, &F.mq) != VK_SUCCESS) F.mq = VK_NULL_HANDLE;
        for (uint32_t i = 0; i < FAR_FIF; i++) F.mq_phase[i] = -1;
        ps2_log("clouds far: A/B of the governor's far-map states every %d frames (level 0 / level 1 relaxed / level 1 capped)", far_ab());
    }
#if PS2_RQ_SUPPORT
    if (F.rq) {
        F.p_tiles_rq = make_pass(g, "rq_cloud_far_tiles.comp.spv");
        F.p_args_rq = make_pass(g, "rq_cloud_far_args.comp.spv");
        F.p_march_rq = make_pass(g, "rq_cloud_far_march.comp.spv");
        if (!F.p_tiles_rq || !F.p_args_rq || !F.p_march_rq) { ps2_log("clouds far: PS2_RQ_CLOUDS: shaders missing (rq_cloud_far_*): off"); F.rq = 0; }
        if (F.rq_swap) F.p_copy_rq = make_pass(g, "rq_cloud_far_copy.comp.spv");
        if (F.sh_on) {   /* (clouds7, PS2_RQ_CLOUDS_FARSHOWN: the resolve pass; the sweeps off in the shaders: rq.x 0) */
            F.p_resolve_rq = make_pass(g, "rq_cloud_far_resolve.comp.spv");
            if (!F.p_resolve_rq) { ps2_log("clouds far rq: PS2_RQ_CLOUDS_FARSHOWN: rq_cloud_far_resolve.comp missing: off"); F.sh_on = 0; }
            else {
                F.par.rq3[0] = 1.0f; F.par.rq3[1] = env_f("PS2_RQ_CLOUDS_FARSHOWN_STEPS", 3.0f, 1.0f, 3.0f);
                F.par.rq3[2] = env_f("PS2_RQ_CLOUDS_FARSHOWN_DEPTH", 0.0f, 0.0f, 1.0f);   /* (R-118: 1 = the resolve by the first cloud) */
                F.par.rq3[3] = env_f("PS2_RQ_CLOUDS_FARSHOWN_FADE", 1.0f, 0.0f, 1.0f) > 0.5f ? 0.0f : 1.0f;   /* (0: no disagreement fade) */
                /* (PS2_RQ_CLOUDS_FARSHOWN_SWEEPS=1: the sweeps stay -- every tile in view of one sweep from one origin, so the tiles agree
                   with each other -- and the shown map resolves them from that origin per texel instead of FARWARP's corner warp) */
                int sw = F.rq_swap && env_f("PS2_RQ_CLOUDS_FARSHOWN_SWEEPS", 0.0f, 0.0f, 1.0f) > 0.5f;
                if (!sw) F.par.rq[0] = 0.0f;
                ps2_log("clouds far rq: the parallax resolved per texel once a frame into the shown map, %d step(s); the tiles re-marched "
                        "%s (PS2_RQ_CLOUDS_FARSHOWN, _SWEEPS)", (int)F.par.rq3[1], sw ? "in sweeps from one origin each" : "by the travel rule from the eye, no sweeps");
            }
        }
        if (F.rq_swap && !F.p_copy_rq) { ps2_log("clouds far: PS2_RQ_CLOUDS_FARSWAP: rq_cloud_far_copy.comp missing: off"); F.rq_swap = 0; F.par.rq[0] = 0.0f; }
        F.rq_tn = F.rq ? (int)env_f("PS2_RQ_CLOUDS_FARTIME", 0.0f, 0.0f, 100000.0f) : 0;
        /* [test] PS2_RQ_CLOUDS_FARSTUB (agent clouds6 2026-11-11, A.5 item 4; rq_cloud_far.glsl CL_FAR_STUB): the price of a cached
           sample. 1 = in alternate blocks of PS2_RQ_CLOUDS_FARTIME frames (120 when unset) the march with every density and the fine
           sample's light from ONE read of the light grid's texture (rq_cloud_far_march_stub.comp; the picture is wrong) and the real
           march (rq_cloud_far_march_cnt.comp), both counting their samples; the "clouds far rq stub:" line gives per arm the far
           update's and the march's own GPU time, the samples a frame and the time a sample. 2 = the stub every frame. Wants
           ACZ_VR_CLOUD_LGRID=1 (else the texture is a placeholder). [debug] ACZ_DEBUG_CLOUD_LIST=1: trk_list_note. */
        F.rq_stub = F.rq ? (int)env_f("PS2_RQ_CLOUDS_FARSTUB", 0.0f, 0.0f, 2.0f) : 0;
        if (F.rq_stub) {
            F.p_march_cnt = make_pass(g, "rq_cloud_far_march_cnt.comp.spv");
            F.p_march_stub = make_pass(g, "rq_cloud_far_march_stub.comp.spv");
            if (!F.p_march_cnt || !F.p_march_stub) { ps2_log("clouds far rq: PS2_RQ_CLOUDS_FARSTUB: rq_cloud_far_march_cnt / _stub missing: off"); F.rq_stub = 0; }
            else {
                if (F.rq_tn <= 0) F.rq_tn = 120;
                const char *lg = getenv("ACZ_VR_CLOUD_LGRID");
                ps2_log("clouds far rq: [test] PS2_RQ_CLOUDS_FARSTUB=%d: %s; every density and the fine sample's light from one read of the "
                        "light grid's texture (the picture is wrong, the time per sample counts)%s", F.rq_stub,
                        F.rq_stub == 1 ? "the stub and the real march with sample counters in alternate blocks" : "the stub every frame",
                        lg && *lg == '1' ? "" : " -- ACZ_VR_CLOUD_LGRID is off: the texture is a placeholder, set it to 1");
            }
        }
        F.list_dbg = (int)env_f("ACZ_DEBUG_CLOUD_LIST", 0.0f, 0.0f, 1.0f);
        if (F.list_dbg) ps2_log("clouds list: [debug] the tracked list's entries and exits with distance and bearing (ACZ_DEBUG_CLOUD_LIST)");
        if (F.rq_tn > 0) {
            VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(g->phys, &pp);
            F.wq_ns = pp.limits.timestampPeriod;
            VkQueryPoolCreateInfo qc; memset(&qc, 0, sizeof qc);
            qc.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO; qc.queryType = VK_QUERY_TYPE_TIMESTAMP; qc.queryCount = 3u * FAR_FIF;   /* (3 a slot: start, the march, end) */
            if (vkCreateQueryPool(g->dev, &qc, NULL, &F.rq_tq) != VK_SUCCESS) F.rq_tq = VK_NULL_HANDLE;
            for (uint32_t i = 0; i < FAR_FIF; i++) F.rq_tarm[i] = -1;
            F.rq_tab = F.rq_swap && !F.rq_stub && env_f("PS2_RQ_CLOUDS_FARFAIR_AB", 0.0f, 0.0f, 1.0f) > 0.5f;
            ps2_log("clouds far rq: [measurement] the far update's GPU time every %d frames (PS2_RQ_CLOUDS_FARTIME)%s", F.rq_tn,
                    F.rq_tab ? "; PS2_RQ_CLOUDS_FARFAIR_AB: FARFAIR + the slots + the lead on / off in alternate blocks of as many frames" : "");
        }
        /* R-70 (agent clouds5, 2026-11-11; with FARSWAP; rq_cloud_far.comp): PS2_RQ_CLOUDS_FARFAIR (default 0): 1 = the tiles the last
           sweep missed go first in the next one; 2 = also the tiles coming into view stale marched at once, within the budget.
           PS2_RQ_CLOUDS_FARSWAP_SLOTS (common_init; 4096): the staging's slots = the most tiles one sweep takes (dbg.w, RQ_SW_MAX).
           PS2_RQ_CLOUDS_FARLEAD (0): a sweep's origin led by this many sweeps of the eye's motion (1.5: where the eye is when the sweep
           is half shown; pred, set per frame in rq_far_record). [debug] PS2_RQ_CLOUDS_FARALL: every tile marched every frame (the
           reference). [measurement] PS2_RQ_CLOUDS_FARTIME / _FARFAIR_AB above. */
        F.par.rq[3] = F.rq && F.rq_swap ? (float)(int)env_f("PS2_RQ_CLOUDS_FARFAIR", 0.0f, 0.0f, 2.0f) : 0.0f;
        /* (clouds7 t11b, R-113: with the shown map and its sweeps, FARFAIR's level 2 -- a tile coming into view stale marched at once from
           the shown origin, for the corner warp's sake -- acts as level 1: the shown map resolves any origin per texel, and level 2's direct
           marches made M10's dark edge blocks (f2225); PS2_RQ_CLOUDS_FARSHOWN_FAIR2=1 keeps level 2) */
        if (F.sh_on && F.par.rq[0] > 0.5f && F.par.rq[3] > 1.5f && env_f("PS2_RQ_CLOUDS_FARSHOWN_FAIR2", 0.0f, 0.0f, 1.0f) < 0.5f) {
            F.par.rq[3] = 1.0f;
            ps2_log("clouds far rq: PS2_RQ_CLOUDS_FARFAIR=2 acts as 1 with the shown map's sweeps (PS2_RQ_CLOUDS_FARSHOWN_FAIR2)");
        }
        F.par.dbg[3] = F.rq && F.rq_swap ? (float)F.rq_slots : 0.0f;
        F.rq_all = F.rq ? (int)(env_f("PS2_RQ_CLOUDS_FARALL", 0.0f, 0.0f, 2.0f) + 0.5f) : 0;   /* (2: the tiles in view only, prompt 9 step 1) */
        F.rq_lead = F.rq && F.rq_swap ? env_f("PS2_RQ_CLOUDS_FARLEAD", 0.0f, 0.0f, 3.0f) : 0.0f;
        /* (clouds6 2026-11-11, A.4 P1a / piece 5) PS2_RQ_CLOUDS_FARONE (default 0, the Quest 1): rq_cloud_far.comp's never-marched and outside
           tiles from the shown sweep's origin. ACZ_VR_CLOUD_LGRID_DENS: the march variant whose samples within the density grid's radius
           are one read of it (rq_cloud_far_march_lg.comp; ps2_clouds.c builds the grid) */
        F.par.rq2[0] = F.rq && F.rq_swap && env_f("PS2_RQ_CLOUDS_FARONE", 0.0f, 0.0f, 1.0f) > 0.5f ? 1.0f : 0.0f;
        /* (clouds6, the light variant) PS2_RQ_CLOUDS_FARWARP_ONE: the composite's offset from the shown sweep's origin for every tile
           (rq_clouds.vert; PASS_TILES writes that origin) */
        F.par.rq2[3] = F.rq && F.rq_swap && env_f("PS2_RQ_CLOUDS_FARWARP_ONE", 0.0f, 0.0f, 1.0f) > 0.5f ? 1.0f : 0.0f;
        if (F.par.rq2[3] > 0.5f) ps2_log("clouds far rq: every tile's parallax from the shown sweep's origin (PS2_RQ_CLOUDS_FARWARP_ONE)");
        F.list_hold = (int)env_f("ACZ_VR_CLOUD_LIST_HOLD", 0.0f, 0.0f, 1.0f);
        if (F.list_hold) ps2_log("clouds far: a cloud the game drops is held while its surface is within half the fog far and fades by that surface's distance (ACZ_VR_CLOUD_LIST_HOLD)");
        if (F.rq && env_f("ACZ_VR_CLOUD_LGRID_DENS", 0.0f, 0.0f, 1.0f) > 0.5f) {
            F.p_march_lg = make_pass(g, "rq_cloud_far_march_lg.comp.spv");
            ps2_log("clouds far rq: the far march's samples within the density grid's radius are one read of it (ACZ_VR_CLOUD_LGRID_DENS)%s",
                    F.p_march_lg ? "" : ": rq_cloud_far_march_lg.comp missing -- the real samples");
        }
        if (F.par.rq2[0] > 0.5f) ps2_log("clouds far rq: the never-marched and outside tiles are marched from the shown sweep's origin (PS2_RQ_CLOUDS_FARONE)");
        if (F.rq && F.coarse_k > 0) {
            F.p_coarse_rq = make_pass(g, "rq_cloud_far_coarse.comp.spv");
            if (F.p_coarse_rq) { F.par.rq2[1] = (float)(F.rq_slots * 65u); F.par.rq2[2] = (float)F.coarse_k; }
            ps2_log("clouds far rq: a new generation's coarse level, one ray per tile corner over %d frame(s), fills the never-marched tiles in "
                    "view until their own march (PS2_RQ_CLOUDS_FARCOARSE)%s", F.coarse_k, F.p_coarse_rq ? "" : ": rq_cloud_far_coarse.comp missing -- off");
        }
        if (F.rq_lead > 0.0f)
            ps2_log("clouds far rq: a sweep's origin is led by %.2f sweeps of the eye's motion (at most 40 %% of the march start; PS2_RQ_CLOUDS_FARLEAD)",
                    (double)F.rq_lead);
        if (F.rq && F.rq_swap)
            ps2_log("clouds far rq: a sweep takes at most %u tiles (PS2_RQ_CLOUDS_FARSWAP_SLOTS, %.1f MB of staging); %s%s (PS2_RQ_CLOUDS_FARFAIR=%d)",
                    F.rq_slots, (double)F.rq_slots * 65.0 * 16.0 / 1048576.0, F.par.rq[3] > 0.5f ?
                    "the tiles in view the last sweep missed go first (the debug line's x16 / x8 / x4 / x2 = missed / cloud / beside cloud / due)" :
                    "one fixed order every sweep (a sweep cut by the slots or PS2_RQ_CLOUDS_FARSWAP_MAX misses the same tiles every time)",
                    F.par.rq[3] > 1.5f ? "; a tile coming into view stale is marched at once from the eye, first in the budget (x1 = those)" : "",
                    (int)F.par.rq[3]);
        if (F.rq) bk_init(g);   /* (rq7_bake: the 3D formats a smaller volume could use, logged on every PS2_RQ_CLOUDS run; PS2_RQ_CLOUDS_BAKE's passes) */
        if (F.rq_all) ps2_log("clouds far rq: [debug] PS2_RQ_CLOUDS_FARALL=%d: every tile %s marched every frame from the eye (%s)", F.rq_all,
                              F.rq_all > 1 ? "in view (with the head-motion margin)" : "of the map",
                              F.rq_all > 1 ? "the truth in view: no scheduler, the tiles outside the view not marched" : "the reference; slow");
    }
#endif
    F.far_ready = 1;
    const ps2_cloud_cfg *c = ps2_clouds_cfg();
    ps2_log("clouds: far map %ux%u (%u tiles), %d steps, refresh every %d frames, error limit %.2f eye px%s",
            F.n, F.n, F.nt, c->far_steps, c->far_slice, (double)F.err_px, F.cap ? ", capped" : "");
    return 0;
}

#if PS2_RQ_SUPPORT/* ---- PS2_RQ_CLOUDS_BAKE (rq7_bake 2026-11-14, prompt 9 steps 2-3; rq_cloud_bake.comp, rq_cloud_far.glsl CL_FAR_BAKE) --------------
   The far march's truth in view (PS2_RQ_CLOUDS_FARALL=2: every far tile in view marched every frame from the eye, no history) with its
   samples read from small volumes baked once instead of the weather, the noise, the probe and the shadow map per sample (prompt 9: the
   procedural sample is what does not fit on the Adreno; one read a sample, the stub, priced the march at 1.19 ms at 512^2). With the
   static field (ACZ_VR_CLOUD_STATIC) the density and the light of a place do not change within a mission. The volumes are the weather
   map's torus (20 km of cloud-frame x / z, REPEAT) across the C1 band:
     density   PS2_RQ_CLOUDS_BAKE_RES x _RES_Y x _RES texels (256 x 32 x 256 = 2 MiB R8: 78 m cells), sqrt of the density without the
               detail octave, the base noise prefiltered at the cell;
     light     the density grid / PS2_RQ_CLOUDS_BAKE_LIGHT_DIV (2: 1 MiB RGBA8): "above", the cast shadow's transmittance (the map's edge
               fade applied by the march from its own eye), the local height;
     distance  PS2_RQ_CLOUDS_BAKE_DF (1): the L-inf distance in cells of 4 x 4 x 4 density texels to the nearest cell holding cloud,
               capped at PS2_RQ_CLOUDS_BAKE_DF_R + 1 (7 + 1; 64 x 8 x 64 = 32 KiB R8): the march skips clear air without a read.
   Baked whole at the first frame, a rebase, a band / key change (the splat's hard causes); after a content splat (the list's small
   opacity steps; the window's move alone changes nothing a march reads) the density is re-baked in rolling slices of z rows over
   PS2_RQ_CLOUDS_BAKE_ROLL frames (16) and the distance field after each slice; the light rolls over PS2_RQ_CLOUDS_BAKE_LROLL frames
   (64: it follows the shadow map's own 32-frame rebuild; 0 = only after a content splat) and is baked whole again three frames
   after a hard one (the shadow map is shown from the frame after its full build). [test] PS2_RQ_CLOUDS_BAKE_FREEZE=1: no re-bake
   after the first (the march's price alone). [debug] PS2_RQ_CLOUDS_BAKE_COUNT=1: the march counts its reads (rq_cloud_far_march_bakecnt).
   PS2_RQ_CLOUDS_BAKE_DETAIL=1: the detail octave as an erosion of the baked density within the far march's detail range. */
#define BK_NIMG 6
static struct {
    int on, df, detail, count, freeze, ldiv, roll, lroll, R, fmt_logged;
    uint32_t nx, ny, nl, nly, nc, ncy;                  /* texels: density x (= z), across the band; light; the distance field's cells */
    VkImage img[BK_NIMG]; VkDeviceMemory mem[BK_NIMG]; VkImageView view[BK_NIMG];   /* density, light, occupancy, df x, df y, df */
    VkSampler s_lin, s_near;
    VkPipeline p_d, p_l, p_df, p_march, p_march_cnt;
    int inited, valid; uint32_t hard_seen, content_seen, rebase; float base, top;
    int one_mode; uint32_t one_key;     /* (rq8_onelayer, PS2_RQ_CLOUDS_ONE=2: baked over the period's torus; its key, rq_one_key) */
    uint32_t d_row, d_left, l_row, l_left, l_again;
    uint64_t full_n, d_rows_n, l_rows_n, work_n, frames, d_texels_n, df_n;
    VkQueryPool q; float q_ns; int8_t q_kind[FAR_FIF]; double q_ms[2]; uint32_t q_n[2];
    double smp[4]; uint32_t smp_n; double chk[8];
    /* PS2_RQ_CLOUDS_BAKE_DIRTY (1): a splat's changes re-bake only the boxes of the bodies that changed (x0, width, z0, width in density
       texels, multiples of 4, wrapping; rect_done = the box's z rows done); more than BK_MAXR boxes: the whole roll as without it */
    int above;                        /* PS2_RQ_CLOUDS_BAKE_ABOVE: "above" from the density volume (fp.bk3.z) */
    int alt;                          /* [test] PS2_RQ_CLOUDS_BAKE_ALT: odd fields march the procedural samples (the truth a field apart) */
    int dirty, have_prev, df_due, roll_again; uint32_t nrect, rect[16][4], rect_done, nprev;
    uint64_t note_n, note_dirty_bodies, note_full, note_texels;
} BK;
#define BK_MAXR 16u
#define BK_MAXB 512u
static struct { double x, z; float y, r, h, op; } bk_prev[BK_MAXB], bk_cur[BK_MAXB];
typedef struct { int32_t i0[4]; float f0[4]; int32_t i2[4]; int32_t i1[4]; } bk_push;   /* rq_cloud_bake.comp BkPC */
_Static_assert(sizeof(far_params) <= HOST_CLOUDS, "far params: the cloud list follows at HOST_CLOUDS");
_Static_assert(sizeof(bk_push) <= CL_PUSH_BYTES, "bake push constants");
static void bk_cfg(void) {
    memset(&BK, 0, sizeof BK);
    BK.on = ps2_clouds_rq_on() && env_f("PS2_RQ_CLOUDS_BAKE", 0.0f, 0.0f, 1.0f) > 0.5f;
    if (!BK.on) return;
    BK.nx = (uint32_t)env_f("PS2_RQ_CLOUDS_BAKE_RES", 256.0f, 64.0f, 1024.0f) & ~15u;
    BK.ny = (uint32_t)env_f("PS2_RQ_CLOUDS_BAKE_RES_Y", 32.0f, 8.0f, 128.0f) & ~7u;
    BK.ldiv = (int)env_f("PS2_RQ_CLOUDS_BAKE_LIGHT_DIV", 2.0f, 1.0f, 4.0f);
    if (BK.ldiv == 3) BK.ldiv = 2;
    BK.nl = BK.nx / (uint32_t)BK.ldiv; BK.nly = BK.ny / (uint32_t)BK.ldiv;
    BK.nc = BK.nx / 4u; BK.ncy = BK.ny / 4u;
    BK.roll = (int)env_f("PS2_RQ_CLOUDS_BAKE_ROLL", 16.0f, 1.0f, 256.0f);
    BK.lroll = (int)env_f("PS2_RQ_CLOUDS_BAKE_LROLL", 64.0f, 0.0f, 1024.0f);
    BK.df = env_f("PS2_RQ_CLOUDS_BAKE_DF", 1.0f, 0.0f, 1.0f) > 0.5f;
    BK.R = (int)env_f("PS2_RQ_CLOUDS_BAKE_DF_R", 7.0f, 1.0f, 30.0f);
    BK.detail = env_f("PS2_RQ_CLOUDS_BAKE_DETAIL", 1.0f, 0.0f, 1.0f) > 0.5f;   /* (on: the procedural march erodes every body by it within haze.w) */
    BK.count = (int)(env_f("PS2_RQ_CLOUDS_BAKE_COUNT", 0.0f, 0.0f, 2.0f) + 0.5f);   /* (2: the check variant) */
    BK.freeze = env_f("PS2_RQ_CLOUDS_BAKE_FREEZE", 0.0f, 0.0f, 1.0f) > 0.5f;
    BK.dirty = env_f("PS2_RQ_CLOUDS_BAKE_DIRTY", 1.0f, 0.0f, 1.0f) > 0.5f;
    BK.alt = env_f("PS2_RQ_CLOUDS_BAKE_ALT", 0.0f, 0.0f, 1.0f) > 0.5f;
    BK.above = env_f("PS2_RQ_CLOUDS_BAKE_ABOVE", 0.0f, 0.0f, 1.0f) > 0.5f;
    for (int i = 0; i < (int)FAR_FIF; i++) BK.q_kind[i] = -1;
}
/* ---- PS2_RQ_CLOUDS_ONE (rq8_onelayer 2026-11-14, prompt 10 steps 2-3; default 0; needs PS2_RQ_CLOUDS_BAKE=1, read once) --------------
   1 = step 2, ONE cloud layer: the baked far march (PS2_RQ_CLOUDS_FARALL=2: every texel in view every frame from the eye) starts at
       PS2_RQ_CLOUDS_ONE_START (4 m) instead of the near range, with no near / far cross-fade (fp.bk3.w); the procedural near march, its
       composite and its cell mask are off (ps2_clouds.c ps2_clouds_rq_compute); no shown map and no sweeps (every tile is marched from
       the eye of its frame, so the composite's per-corner parallax is each eye's own offset: rq_clouds.vert, FARWARP's corners); the
       composite cuts and fades per pixel by its own scene depth inside the cloud's span (PS2_RQ_CLOUDS_SOFT forced, rq_clouds.inc): the
       march takes no scene depth; the march's smallest step near the eye PS2_RQ_CLOUDS_ONE_DTMIN.
   2 = also step 3 (the static field for real; see rq_one_period and the lines after it). */
static int rq_one_cfg(void) {
    static int v = -1;
    if (v < 0) {
        float o = env_f("PS2_RQ_CLOUDS_ONE", 0.0f, 0.0f, 2.0f);
        v = ps2_clouds_rq_on() && env_f("PS2_RQ_CLOUDS_BAKE", 0.0f, 0.0f, 1.0f) > 0.5f ? (int)(o + 0.5f) : 0;
        if (o > 0.5f && !v) ps2_log("clouds far rq one: PS2_RQ_CLOUDS_ONE needs PS2_RQ_CLOUDS and PS2_RQ_CLOUDS_BAKE: off");
    }
    return v;
}
static int rq_one(void) { return BK.on ? rq_one_cfg() : 0; }   /* (the bake may have turned itself off: then the old layers) */
/* [test] PS2_RQ_CLOUDS_ONE_ALT=1 (with PS2_RQ_CLOUDS_ONE=1): odd GS frames -- the eye dumps' numbers -- draw t12b's two layers instead (the
   near march to 1500 m and its composite, the baked far march from 1300 m with the cross-fade band, no SOFT), so consecutive dumps of ONE
   run compare the two at the same camera, a frame of motion apart (cross-run dumps are not one camera: R-135). Not with =2 (the period's
   torus and the frozen table are the whole run's) */
static int rq_one_alt(void) {
    static int v = -1;
    if (v < 0) { v = rq_one_cfg() == 1 && env_f("PS2_RQ_CLOUDS_ONE_ALT", 0.0f, 0.0f, 1.0f) > 0.5f;
                 if (v) ps2_log("clouds far rq one: [test] PS2_RQ_CLOUDS_ONE_ALT: odd GS frames draw t12b's two layers (near march + far from 1300 m)"); }
    return v;
}
static int rq_one_now(void) {                            /* (this frame: ONE, or with ONE_ALT on an odd GS frame t12b's layers) */
    if (!rq_one()) return 0;
    if (rq_one_alt()) { extern u32 ps2_gs_frame_count; if (ps2_gs_frame_count & 1u) return 0; }
    return rq_one();
}
int ps2_clouds_far_rq_one(void) { return rq_one_now(); }
int ps2_clouds_far_rq_one_alt(void) { return rq_one() && rq_one_alt(); }
int ps2_clouds_far_rq_one_cfg(void) { return rq_one(); }   /* (the run's setting; ps2_clouds_far_rq_one: this frame's) */
static float rq_one_start(void) { static float v = -1.0f; if (v < 0.0f) v = env_f("PS2_RQ_CLOUDS_ONE_START", 4.0f, 1.0f, 2000.0f); return v; }
static float rq_one_dtmin(void) { static float v = -1.0f; if (v < 0.0f) v = env_f("PS2_RQ_CLOUDS_ONE_DTMIN", 10.0f, 2.0f, 60.0f); return v; }
/* PS2_RQ_CLOUDS_ONE_STEPS (64): the march's step cap raised by this many (the far steps, 64, were sized for a march from 1300 m: from the eye a
   ray through the haze around the aircraft spent them before it reached the cloud behind, and showed the sky through it -- dark dots of
   sky over the cloud, M10 f5930); PS2_RQ_CLOUDS_ONE_NEAR (1000 m): the range of the full-noise coverage near the eye (fp.mz.w) */
static float rq_one_steps(void) { static float v = -1.0f; if (v < 0.0f) v = (float)(int)env_f("PS2_RQ_CLOUDS_ONE_STEPS", 64.0f, 0.0f, 192.0f); return v; }
static float rq_one_near(void) { static float v = -1.0f; if (v < 0.0f) v = env_f("PS2_RQ_CLOUDS_ONE_NEAR", 1000.0f, 0.0f, 3000.0f); return v; }
/* PS2_RQ_CLOUDS_ONE_SPLIT (1500 m): the end of the ray's front part. The march stores the opacity it gathered up to there (prof.w), and the
   composite (rq_clouds_fd.frag) fades that front and the rest by the scene's distance apart -- one (first, mean) pair for the whole ray let a
   wisp near the eye spread a far cloud's opacity over the terrain (a veil), and a "first" moved past the wisp cut the haze around the aircraft */
static float rq_one_split(void) { static float v = -1.0f; if (v < 0.0f) v = env_f("PS2_RQ_CLOUDS_ONE_SPLIT", 1500.0f, 100.0f, 9000.0f); return v; }
float ps2_clouds_far_rq_split(void) { return rq_one() ? rq_one_split() : 1.0e9f; }
/* (round 2, the device 2026-11-15: fe040c2's one layer took 0.65-1.0 ms out of the cloud brackets and App gained 1.3 ms -- the composite in
   the eye pass.) PS2_RQ_CLOUDS_ONE_FD (0): 1 = fe040c2's far tiles, the quad at the near plane and each fragment's depth written at its own
   first cloud (rq_clouds_fd.frag): a shader that writes depth loses the early depth test, so every fragment of a tile with cloud ran, the
   ones behind the cockpit too; 0 = the quad at the tile's (and its neighbours') nearest cloud seen from the eye, 3 % nearer (early depth
   keeps the cockpit and the nearer scene out), the per-pixel front / back fade (rq_clouds_fs.frag) as before. [test] PS2_RQ_CLOUDS_ONE_AB
   with PS2_RQ_ABSKIP=N (the `rq-ab:` lines: the frame's GPU time with the part and without it, blocks of N eye-pass lists): 1 = the
   per-pixel scene fade (SOFT) left out (the far tiles' plain shader), 2 = the near full-noise coverage term (_ONE_NEAR) left out of the
   march, 3 = the extra steps (_ONE_STEPS) left out, 4 = the left-out blocks the other way of _ONE_FD; PS2_RQ_SKIPC=2 leaves the whole far
   composite out */
static int rq_one_fd(void) { static int v = -1; if (v < 0) v = env_f("PS2_RQ_CLOUDS_ONE_FD", 0.0f, 0.0f, 1.0f) > 0.5f; return v; }
static int rq_one_ab(void) {
    static int v = -1;
    if (v < 0) { v = rq_one_cfg() ? (int)env_f("PS2_RQ_CLOUDS_ONE_AB", 0.0f, 0.0f, 4.0f) : 0;
                 if (v) ps2_log("clouds far rq one: [test] PS2_RQ_CLOUDS_ONE_AB=%d: left out in every second block of PS2_RQ_ABSKIP lists: %s", v,
                                v == 1 ? "the per-pixel scene fade" : v == 2 ? "the near full-noise coverage" : v == 3 ? "the extra steps" : "the other _ONE_FD"); }
    return v;
}
static int rq_one_left_out(int part) { extern int ps2_rq_ab_left_out(void); return rq_one_ab() == part && ps2_rq_ab_left_out(); }
int ps2_clouds_far_rq_one_fd(void) { return rq_one_fd(); }
/* (round 3, the device 2026-11-15 04:23-06:15: inside a body ONE=2's march took ~74 fine samples a ray -- M4 26 s under 60, its cloud bracket
   6.00 ms. From the eye the fine step is clamp(t x 0.045, _ONE_DTMIN, 800) x 0.5: 5 m to 222 m; the thin-medium step (ACZ_VR_CLOUD_FAR_THIN,
   the step of 0.35 optical depth up to the empty-space step) was on only for cumulus-like weather (C1 type >= 0.4: M10 0.55, M4 0.32).)
   PS2_RQ_CLOUDS_ONE_THIN (1.0): with the one layer the thin-medium step is on in every weather, at least this factor: inside a faint body
   the march steps by optical depth (0.35 a step x this) instead of by distance; a dense body keeps the short steps it needs */
static float rq_one_thin(float t) {
    static float v = -1.0f; if (v < 0.0f) v = env_f("PS2_RQ_CLOUDS_ONE_THIN", 1.0f, 0.0f, 4.0f);
    return rq_one_now() && v > t ? v : t;
}
int ps2_clouds_far_rq_one_ab(void) { return rq_one_ab(); }
/* PS2_RQ_CLOUDS_ONE=2, step 3, the static field for real: zero_clouds.c publishes the mission's whole table, every body once at an opacity
   frozen when the table appears (no fade by the centre's distance); here
     - the weather map covers the cloud list's PERIOD as a torus (21.8 km on Zero, CL_WEATHER_RES texels): CloudFrame wx is that torus's
       mapping (ps2_clouds_far_rq_wx, from ps2_clouds.c fill_ubo_at), the splat writes texel i at the period's position i / N (its texels'
       place does not depend on the eye) with the list's wrap, so it is complete everywhere and splatted ONCE: no window to follow
       (ACZ_VR_CLOUD_RQ_WX_MOVE), no splat at a rebase, no fades (the tracker takes the list as it is: rq_one_snap), no held bodies;
     - the shadow map keeps its own 20 km torus (rq_cloud_shadow.comp: the eye's place in it from the cloud-frame origin, not wx);
     - the volumes are baked over the period's torus (bp.i2.z; rq_cloud_bake.comp): the base noise periodic in the period (an integer number
       of its own periods over it, ~9 % larger than the shape's), the light's cast shadow marched through the weather's envelope toward the
       shadow map's light (the map's model and strength) instead of read from the map; baked whole only when the table, the band or the
       light changes (rq_one_key): never in flight, never at a camera cut; the march reads them without the window's edge fade and without
       the map's edge weight (fp.bk3.w 2). */
static float rq_one_p;                                   /* (=2: the cloud list's period, m; 0 = none seen yet) */
static uint32_t rq_one_bakes, rq_one_tables; static unsigned long long rq_one_bake_field;   /* (=2: whole bakes so far, the field of the last; tables taken) */
static float rq_one_period(const ps2_cloud_env *e) {
    if (rq_one() < 2 || !F.p_weather_rq || !e || !(e->valid & CL_ENV_LIST) || !(e->list_period_m > 1000.0f)) return 0.0f;
    rq_one_p = e->list_period_m;
    return rq_one_p;
}
static float rq_one_wrapf(double d) {                    /* (the CPU splat copy, cloud_body: the body's copy nearest the eye) */
    if (rq_one() >= 2 && rq_one_p > 0.0f) d -= (double)rq_one_p * floor(d / (double)rq_one_p + 0.5);
    return (float)d;
}
static void rq_one_snap(void) {                          /* (track_update: no ramps, no follower, no holds) */
    if (rq_one() < 2) return;
    for (uint32_t i = 0; i < F.ntrk; i++) { F.trk[i].cur = F.trk[i].tgt; F.trk[i].vel = 0.0f; F.trk_hold[i] = 0.0f; }
}
static int rq_one_rebase(const ps2_cloud_view *v) {      /* (track_update: a rebase changes nothing of the period's field) */
    if (rq_one() < 2) return 0;
    F.splat_rebase = v->rebase;
    return 1;
}
/* (ps2_clouds.c fill_ubo_at, every CloudFrame of the frame) =2: the weather map's mapping is the period's torus; the shadow map's build takes
   its variant first (rq_cloud_shadow.comp) */
void ps2_clouds_far_rq_wx(float wx[4], const ps2_cloud_view *v, const ps2_cloud_env *e) {
    float P = rq_one_period(e);
    if (P <= 0.0f) return;
    extern int ps2_clouds_shadow_rq_sh20(void);
    ps2_clouds_shadow_rq_sh20();
    double wu = fmod(v->cloud_origin_m[0] / (double)P, 1.0), wv = fmod(v->cloud_origin_m[2] / (double)P, 1.0);
    wx[0] = (float)(wu < 0.0 ? wu + 1.0 : wu); wx[1] = (float)(wv < 0.0 ? wv + 1.0 : wv); wx[2] = 1.0f / P;
}
/* the bake's key at =2: the splatted table (the list_seq of the last splat, its key), the band, the period, the light (the cast shadow's
   direction and strength); anything else -- the eye, a rebase, the drift (the cloud frame), the window -- does not re-bake */
static uint32_t rq_one_key(const ps2_cloud_env *e, const float L[3], float k) {
    float w[10] = { (float)F.splat_seq, e->base_m, e->top_m, rq_one_p, roundf(L[0] * 1000.0f), roundf(L[1] * 1000.0f), roundf(L[2] * 1000.0f), k,
                    F.splat_key[2], F.splat_key[3] };
    uint32_t h = 2166136261u;
    const uint8_t *b = (const uint8_t *)w;
    for (size_t i = 0; i < sizeof w; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}
/* one 3D image in GENERAL use (storage + sampled + clear), device-local; 0 = made */
static int bk_image(VkPhysicalDevice phys, int k, VkFormat fmt, uint32_t w, uint32_t h, uint32_t d) {
    VkDevice dev = F.dev;
    VkImageCreateInfo ii; memset(&ii, 0, sizeof ii);
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ii.imageType = VK_IMAGE_TYPE_3D; ii.format = fmt;
    ii.extent.width = w; ii.extent.height = h; ii.extent.depth = d; ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &ii, NULL, &BK.img[k]) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, BK.img[k], &mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    uint32_t mt = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < mp.memoryTypeCount && mt == 0xFFFFFFFFu; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) mt = i;
    VkMemoryAllocateInfo ma; memset(&ma, 0, sizeof ma);
    ma.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ma.allocationSize = mr.size; ma.memoryTypeIndex = mt;
    if (mt == 0xFFFFFFFFu || vkAllocateMemory(dev, &ma, NULL, &BK.mem[k]) != VK_SUCCESS || vkBindImageMemory(dev, BK.img[k], BK.mem[k], 0) != VK_SUCCESS) return -1;
    VkImageViewCreateInfo vi; memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = BK.img[k]; vi.viewType = VK_IMAGE_VIEW_TYPE_3D; vi.format = fmt;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
    return vkCreateImageView(dev, &vi, NULL, &BK.view[k]) == VK_SUCCESS ? 0 : -1;
}
/* the volumes and the samplers (common_init, before set 1's writes): the configured sizes with the switch on, 1 x 1 x 1 placeholders
   (the bindings stay valid) without it or where the device lacks R8 / RGBA8 3D storage with linear filtering */
static int bk_resources(VkPhysicalDevice phys) {
    if (BK.on) {
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        VkFormatProperties f8, f32; vkGetPhysicalDeviceFormatProperties(phys, VK_FORMAT_R8_UNORM, &f8);
        vkGetPhysicalDeviceFormatProperties(phys, VK_FORMAT_R8G8B8A8_UNORM, &f32);
        if ((f8.optimalTilingFeatures & need) != need || (f32.optimalTilingFeatures & need) != need) {
            ps2_log("clouds far rq bake: R8 / RGBA8 storage images with linear filtering missing here: PS2_RQ_CLOUDS_BAKE off");
            BK.on = 0;
        }
    }
    uint32_t sz[BK_NIMG][3];
    for (int k = 0; k < BK_NIMG; k++) sz[k][0] = sz[k][1] = sz[k][2] = 1u;
    if (BK.on) {
        sz[0][0] = BK.nx; sz[0][1] = BK.ny; sz[0][2] = BK.nx;
        sz[1][0] = BK.nl; sz[1][1] = BK.nly; sz[1][2] = BK.nl;
        for (int k = 2; k < BK_NIMG; k++) { sz[k][0] = BK.nc; sz[k][1] = BK.ncy; sz[k][2] = BK.nc; }
    }
    for (int k = 0; k < BK_NIMG; k++)
        if (bk_image(phys, k, k == 1 || !BK.on ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R8_UNORM, sz[k][0], sz[k][1], sz[k][2]) != 0) return -1;
    VkSamplerCreateInfo si; memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO; si.magFilter = si.minFilter = VK_FILTER_LINEAR; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT; si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f; si.maxAnisotropy = 1.0f;
    if (vkCreateSampler(F.dev, &si, NULL, &BK.s_lin) != VK_SUCCESS) return -1;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(F.dev, &si, NULL, &BK.s_near) != VK_SUCCESS) return -1;
    return 0;
}
/* (the brief: the coordinator reads this line on the headset) whether the device takes 3D images of the compressed single-channel formats
   a smaller density volume could use: vkGetPhysicalDeviceImageFormatProperties2, sampled, optimal tiling */
static void bk_formats_log(VkPhysicalDevice phys) {
    static const struct { VkFormat f; const char *n; } fm[] = {
        { VK_FORMAT_R8_UNORM, "R8" }, { VK_FORMAT_BC4_UNORM_BLOCK, "BC4" }, { VK_FORMAT_EAC_R11_UNORM_BLOCK, "EAC_R11" },
        { VK_FORMAT_ASTC_4x4_UNORM_BLOCK, "ASTC_4x4" }, { VK_FORMAT_ASTC_8x8_UNORM_BLOCK, "ASTC_8x8" }, { VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, "ETC2_RGB8" } };
    char ln[900]; int n = 0;
    for (size_t i = 0; i < sizeof fm / sizeof fm[0] && n < (int)sizeof ln - 120; i++) {
        VkPhysicalDeviceImageFormatInfo2 fi; memset(&fi, 0, sizeof fi);
        fi.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2; fi.format = fm[i].f; fi.type = VK_IMAGE_TYPE_3D;
        fi.tiling = VK_IMAGE_TILING_OPTIMAL; fi.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkImageFormatProperties2 pr; memset(&pr, 0, sizeof pr); pr.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
        VkResult r = vkGetPhysicalDeviceImageFormatProperties2(phys, &fi, &pr);
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(phys, fm[i].f, &fp);
        int lin = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
        if (r == VK_SUCCESS)
            n += snprintf(ln + n, sizeof ln - (size_t)n, "%s%s 3D yes (max %ux%ux%u, linear %s)", i ? "; " : "", fm[i].n,
                          pr.imageFormatProperties.maxExtent.width, pr.imageFormatProperties.maxExtent.height,
                          pr.imageFormatProperties.maxExtent.depth, lin ? "yes" : "no");
        else n += snprintf(ln + n, sizeof ln - (size_t)n, "%s%s 3D no (%d; 2D linear %s)", i ? "; " : "", fm[i].n, (int)r, lin ? "yes" : "no");
    }
    ps2_log("clouds far rq bake: 3D image formats here (vkGetPhysicalDeviceImageFormatProperties2, sampled, optimal): %s", ln);
}
/* (far init) the formats line; PS2_RQ_CLOUDS_BAKE: the bake's passes and the march that reads the volumes, FARALL=2, the log */
static void bk_init(const ps2_cloud_gpu *g) {
    bk_formats_log(g->phys);
    if (BK.on) {
        BK.p_d = make_pass(g, "rq_cloud_bake_d.comp.spv");
        BK.p_l = make_pass(g, "rq_cloud_bake_l.comp.spv");
        BK.p_df = BK.df ? make_pass(g, "rq_cloud_bake_df.comp.spv") : VK_NULL_HANDLE;
        BK.p_march = make_pass(g, "rq_cloud_far_march_bake.comp.spv");
        BK.p_march_cnt = BK.count ? make_pass(g, BK.count > 1 ? "rq_cloud_far_march_bakechk.comp.spv" : "rq_cloud_far_march_bakecnt.comp.spv") : VK_NULL_HANDLE;
        if (!BK.p_d || !BK.p_l || !BK.p_march) { ps2_log("clouds far rq bake: rq_cloud_bake_* / rq_cloud_far_march_bake missing: PS2_RQ_CLOUDS_BAKE off"); BK.on = 0; }
        else {
            if (BK.df && !BK.p_df) { ps2_log("clouds far rq bake: rq_cloud_bake_df.comp missing: no distance field"); BK.df = 0; }
            if (BK.count && !BK.p_march_cnt) BK.count = 0;
            const char *fa = getenv("PS2_RQ_CLOUDS_FARALL");
            if (!fa || !*fa) F.rq_all = 2;      /* (the truth in view; an explicit PS2_RQ_CLOUDS_FARALL wins) */
            F.par.bk3[0] = BK.detail ? 1.0f : 0.0f; F.par.bk3[2] = BK.above ? 1.0f : 0.0f;
            double mb_d = (double)BK.nx * BK.ny * BK.nx / 1048576.0, mb_l = (double)BK.nl * BK.nly * BK.nl * 4.0 / 1048576.0;
            double kb_c = (double)BK.nc * BK.ncy * BK.nc / 1024.0;
            ps2_log("clouds far rq bake: PS2_RQ_CLOUDS_BAKE on: the far march (PS2_RQ_CLOUDS_FARALL=%d: %s) reads volumes baked once over the "
                    "weather map's 20 km torus: density %ux%ux%u R8 (%.0f m cells, %.2f MiB), light %ux%ux%u RGBA8 (%.2f MiB), distance field "
                    "%s (%ux%ux%u R8 cells of %.0f m, %.1f KiB, capped at %d); a content splat re-bakes the density over %d frames, the light "
                    "rolls over %d frames%s%s%s", F.rq_all, F.rq_all > 1 ? "every tile in view every frame from the eye" : "every tile",
                    BK.nx, BK.ny, BK.nx, (double)CL_WEATHER_M / BK.nx, mb_d, BK.nl, BK.nly, BK.nl, mb_l, BK.df ? "on" : "off",
                    BK.nc, BK.ncy, BK.nc, (double)CL_WEATHER_M / BK.nc, kb_c, BK.R + 1, BK.roll, BK.lroll,
                    BK.detail ? "; the detail octave on the baked coverage within the detail range (PS2_RQ_CLOUDS_BAKE_DETAIL)" : "; NO detail octave (PS2_RQ_CLOUDS_BAKE_DETAIL=0)",
                    BK.freeze ? "; [test] FROZEN after the first bake" : "", BK.count ? "; [debug] the march counts its reads" :
                    BK.alt ? "; [test] PS2_RQ_CLOUDS_BAKE_ALT: odd fields march the procedural samples" : "");
            if (BK.above) ps2_log("clouds far rq bake: \"above\" from the density volume at the probe point (PS2_RQ_CLOUDS_BAKE_ABOVE)");
            if (rq_one_cfg())
                ps2_log("clouds far rq one: ONE cloud layer (PS2_RQ_CLOUDS_ONE=%d): the baked march from %.0f m (PS2_RQ_CLOUDS_ONE_START), its "
                        "smallest step %.0f m (PS2_RQ_CLOUDS_ONE_DTMIN), no near / far cross-fade; the near march, the shown map and the sweeps "
                        "off; the composite cut per pixel by the scene depth (PS2_RQ_CLOUDS_SOFT)", rq_one_cfg(), (double)rq_one_start(),
                        (double)rq_one_dtmin());
            if (F.debug > 0) {   /* (ACZ_DEBUG_CLOUDS: the bake's own GPU time, 2 stamps a slot) */
                VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(g->phys, &pp);
                BK.q_ns = pp.limits.timestampPeriod;
                VkQueryPoolCreateInfo qc; memset(&qc, 0, sizeof qc);
                qc.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO; qc.queryType = VK_QUERY_TYPE_TIMESTAMP; qc.queryCount = 2u * FAR_FIF;
                if (vkCreateQueryPool(g->dev, &qc, NULL, &BK.q) != VK_SUCCESS) BK.q = VK_NULL_HANDLE;
            }
        }
    }
}
/* (PS2_RQ_CLOUDS_BAKE_DIRTY) a splat happened: compare its bodies with the last splat's (cloud-frame place, radius, height, opacity) and
   queue the density boxes of those that changed, appeared or went (their disc plus the splat's softening and a cell); the window's move
   alone changes no body: nothing to re-bake */
static void bk_add_rect(double x, double z, float r) {
    double cell = CL_WEATHER_M / (double)BK.nx;
    double u = fmod(x / CL_WEATHER_M, 1.0), w = fmod(z / CL_WEATHER_M, 1.0);
    if (u < 0.0) u += 1.0;
    if (w < 0.0) w += 1.0;
    double rt = ((double)r + (double)F.soft_m + 100.0) / cell + 2.0;
    uint32_t N = BK.nx, b[4];
    double c[2] = { u * N, w * N };
    for (int a = 0; a < 2; a++) {
        double lo = floor((c[a] - rt) / 4.0) * 4.0, hi = ceil((c[a] + rt) / 4.0) * 4.0;
        uint32_t wd = hi - lo >= (double)N ? N : (uint32_t)(hi - lo);
        long l0 = (long)lo % (long)N; if (l0 < 0) l0 += (long)N;
        b[2 * a] = wd == N ? 0u : (uint32_t)l0; b[2 * a + 1] = wd;
    }
    if (BK.nrect >= BK_MAXR) {                                   /* (too many: the whole roll; one more after a running one) */
        BK.nrect = 0; BK.rect_done = 0; BK.note_full++;
        if (BK.d_left) BK.roll_again = 1; else BK.d_left = BK.nx;
        return;
    }
    if (BK.d_left) { BK.roll_again = 1; return; }               /* (a whole roll is on: one more whole roll after it covers this) */
    memcpy(BK.rect[BK.nrect++], b, sizeof b);
    BK.note_texels += (uint64_t)b[1] * b[3] * BK.ny;
}
static void bk_splat_note(const void *clv, uint32_t n, const ps2_cloud_view *v) {
    const far_cloud *cl = (const far_cloud *)clv;
    if (!BK.on || !BK.dirty || rq_one() >= 2) return;    /* (PS2_RQ_CLOUDS_ONE=2: no re-bake by boxes; the table is baked whole once) */
    uint32_t m = n < BK_MAXB ? n : BK_MAXB;
    for (uint32_t i = 0; i < m; i++) {
        bk_cur[i].x = (double)cl[i].pos_r[0] + v->cloud_origin_m[0]; bk_cur[i].z = (double)cl[i].pos_r[2] + v->cloud_origin_m[2];
        bk_cur[i].y = cl[i].pos_r[1]; bk_cur[i].r = cl[i].pos_r[3]; bk_cur[i].h = cl[i].info[2]; bk_cur[i].op = cl[i].info[1];
    }
    BK.note_n++;
    if (BK.have_prev) {
        uint8_t used[BK_MAXB]; memset(used, 0, sizeof used);
        for (uint32_t i = 0; i < m; i++) {
            int hit = -1;
            for (uint32_t j = 0; j < BK.nprev && hit < 0; j++)
                if (!used[j] && fabs(bk_cur[i].x - bk_prev[j].x) < 1.0 && fabs(bk_cur[i].z - bk_prev[j].z) < 1.0 && fabsf(bk_cur[i].y - bk_prev[j].y) < 1.0f
                    && fabsf(bk_cur[i].r - bk_prev[j].r) < 0.5f && fabsf(bk_cur[i].h - bk_prev[j].h) < 1.0f) hit = (int)j;
            if (hit >= 0) used[hit] = 1;
            if (hit < 0 || fabsf(bk_cur[i].op - bk_prev[hit].op) > 1.0e-6f) {
                bk_add_rect(bk_cur[i].x, bk_cur[i].z, bk_cur[i].r); BK.note_dirty_bodies++;
                if (hit >= 0 && (fabs(bk_cur[i].x - bk_prev[hit].x) > 0.0 || fabs(bk_cur[i].z - bk_prev[hit].z) > 0.0)) bk_add_rect(bk_prev[hit].x, bk_prev[hit].z, bk_prev[hit].r);
            }
        }
        for (uint32_t j = 0; j < BK.nprev; j++) if (!used[j]) { bk_add_rect(bk_prev[j].x, bk_prev[j].z, bk_prev[j].r); BK.note_dirty_bodies++; }
    }
    memcpy(bk_prev, bk_cur, sizeof bk_cur[0] * m); BK.nprev = m; BK.have_prev = 1;
}
static VkImageView bk_view(int k) { return BK.view[k]; }
static VkSampler bk_sampler(int lin) { return lin ? BK.s_lin : BK.s_near; }
/* PS2_RQ_CLOUDS_BAKE (rq7_bake): this frame's bake work, after the weather splat and the shadow slice, before the far tiles; sets 0 / 1 bound */
static void bk_record(VkCommandBuffer cmd, int fi, const ps2_cloud_env *e, const ps2_cloud_view *v) {
    extern void ps2_clouds_fence(VkCommandBuffer cmd);
    BK.frames++;
    if (!BK.inited) {                     /* (GENERAL once, cleared: an unbaked texel reads as no cloud) */
        VkImageMemoryBarrier ib[BK_NIMG]; memset(ib, 0, sizeof ib);
        for (int k = 0; k < BK_NIMG; k++) {
            ib[k].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; ib[k].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; ib[k].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            ib[k].srcQueueFamilyIndex = ib[k].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; ib[k].image = BK.img[k];
            ib[k].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ib[k].subresourceRange.levelCount = 1; ib[k].subresourceRange.layerCount = 1;
            ib[k].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, BK_NIMG, ib);
        VkClearColorValue z; memset(&z, 0, sizeof z);
        VkImageSubresourceRange rr; memset(&rr, 0, sizeof rr); rr.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rr.levelCount = 1; rr.layerCount = 1;
        for (int k = 0; k < BK_NIMG; k++) vkCmdClearColorImage(cmd, BK.img[k], VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rr);
        barrier2(cmd, XS, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, RW);
        BK.inited = 1;
    }
    if (BK.q && BK.q_kind[fi] >= 0) {     /* (this slot's previous bake, its fence passed) */
        uint64_t ts[2];
        if (vkGetQueryPoolResults(F.dev, BK.q, 2u * (uint32_t)fi, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            int k = BK.q_kind[fi];
            BK.q_ms[k] += (double)(ts[1] - ts[0]) * BK.q_ns * 1e-6; BK.q_n[k]++;
        }
        BK.q_kind[fi] = -1;
    }
    if (BK.q && F.debug > 0 && BK.frames % 600u == 0u) {
        ps2_log("clouds far rq bake: in 600 frames %u with rolling work at %.3f ms GPU each, %u whole bakes at %.3f ms (%llu in all); so far %.2f "
                "density volumes re-baked (%llu rows), %llu light rows, %llu distance fields; splats seen %u, content %u, hard %u; the boxes "
                "(PS2_RQ_CLOUDS_BAKE_DIRTY): %llu splats compared, %llu bodies changed, %llu whole rolls (too many), %.2f volumes queued",
                BK.q_n[0], BK.q_n[0] ? BK.q_ms[0] / BK.q_n[0] : 0.0, BK.q_n[1], BK.q_n[1] ? BK.q_ms[1] / BK.q_n[1] : 0.0,
                (unsigned long long)BK.full_n, (double)BK.d_texels_n / ((double)BK.nx * BK.nx * BK.ny), (unsigned long long)BK.d_rows_n,
                (unsigned long long)BK.l_rows_n, (unsigned long long)BK.df_n, F.bk_splat_n, F.bk_content_n, F.bk_hard_n,
                (unsigned long long)BK.note_n, (unsigned long long)BK.note_dirty_bodies, (unsigned long long)BK.note_full,
                (double)BK.note_texels / ((double)BK.nx * BK.nx * BK.ny));
        BK.q_ms[0] = BK.q_ms[1] = 0.0; BK.q_n[0] = BK.q_n[1] = 0;
    }
    if (F.debug > 0 && BK.frames % 600u == 0u && rq_one() >= 2)   /* (PS2_RQ_CLOUDS_ONE=2: the gate's counters, ACZ_DEBUG_CLOUDS) */
        ps2_log("clouds far rq one: so far %u whole bakes of the period's torus (the last at field %llu), %u splats of the weather map, the "
                "table taken %u times (list_seq %u); field %llu", rq_one_bakes, rq_one_bake_field, F.bk_splat_n, rq_one_tables, F.splat_seq,
                (unsigned long long)ps2_kernel_vblank_count());
    int hard = !BK.valid || BK.hard_seen != F.bk_hard_n || BK.rebase != v->rebase;
    if (BK.freeze && BK.valid) hard = 0;
    /* (rq8_onelayer, PS2_RQ_CLOUDS_ONE=2: the period's torus, baked whole when its key changes -- the splatted table, the band, the light --
       and never otherwise: no rolls, no boxes, no light roll; a switch between the two tori bakes whole) */
    int one2 = rq_one() >= 2 && rq_one_p > 0.0f;
    if (one2) {
        extern int ps2_clouds_shadow_rq_light(const ps2_cloud_env *e, float L[3], float *k);
        float L[3] = { 0.0f, 1.0f, 0.0f }, k = 0.0f;
        if (!ps2_clouds_shadow_rq_light(e, L, &k)) k = 0.0f;
        uint32_t key = rq_one_key(e, L, k);
        {   static uint32_t lastseq; static int have; if (!have || F.splat_seq != lastseq) { lastseq = F.splat_seq; have = 1; rq_one_tables++; } }   /* (the splatted table's list_seq changes) */
        hard = !BK.valid || !BK.one_mode || key != BK.one_key;
        if (hard) {
            BK.one_key = key; BK.one_mode = 1; rq_one_bakes++; rq_one_bake_field = (unsigned long long)ps2_kernel_vblank_count();
            ps2_log("clouds far rq one: the period's torus baked whole (bake %u, field %llu; period %.1f m, band %.0f-%.0f m, light %.3f %.3f %.3f x%.2f)",
                    rq_one_bakes, rq_one_bake_field, (double)rq_one_p, (double)e->base_m, (double)e->top_m, (double)L[0], (double)L[1], (double)L[2], (double)k);
        }
    } else if (BK.one_mode) { hard = 1; BK.one_mode = 0; }
    uint32_t d0 = 0, dn = 0, l0 = 0, ln = 0, dx0 = 0, dxn = BK.nx; int dfn = 0;
    if (hard) {
        BK.hard_seen = F.bk_hard_n; BK.content_seen = F.bk_content_n; BK.rebase = v->rebase;
        BK.base = e->base_m; BK.top = e->top_m > e->base_m + 1.0f ? e->top_m : e->base_m + 1.0f;
        dn = BK.nx; ln = BK.nl; BK.d_row = BK.d_left = 0; BK.l_row = BK.l_left = 0; BK.l_again = one2 ? 0 : 3; BK.full_n++;
        BK.nrect = 0; BK.rect_done = 0; BK.df_due = 1; BK.roll_again = 0;
    } else if (!BK.freeze && !one2) {
        if (BK.content_seen != F.bk_content_n) {
            BK.content_seen = F.bk_content_n; BK.l_left = BK.nl;
            if (!BK.dirty) { if (BK.d_left) BK.roll_again = 1; else BK.d_left = BK.nx; }   /* (a running roll completes first) */
        }
        if (BK.d_left) {                  /* (whole cells of the distance field: rows by 4) */
            uint32_t per = ((BK.nx + (uint32_t)BK.roll - 1u) / (uint32_t)BK.roll + 3u) & ~3u;
            if (BK.d_row >= BK.nx) BK.d_row = 0;
            dn = per < BK.nx - BK.d_row ? per : BK.nx - BK.d_row;
            d0 = BK.d_row; BK.d_row += dn; if (BK.d_row >= BK.nx) BK.d_row = 0;
            BK.d_left -= dn < BK.d_left ? dn : BK.d_left;
            if (!BK.d_left) { BK.df_due = 1; if (BK.roll_again) { BK.roll_again = 0; BK.d_left = BK.nx; } }
        } else if (BK.nrect) {            /* (PS2_RQ_CLOUDS_BAKE_DIRTY: the first queued box, at most a roll's share of the volume a frame) */
            uint32_t *rc = BK.rect[0];
            uint32_t per = (uint32_t)(((uint64_t)BK.nx * BK.nx / (uint32_t)BK.roll) / (rc[1] ? rc[1] : 1u));
            per = per < 4u ? 4u : per & ~3u;
            dn = per < rc[3] - BK.rect_done ? per : rc[3] - BK.rect_done;
            dx0 = rc[0]; dxn = rc[1]; d0 = (rc[2] + BK.rect_done) % BK.nx;
            BK.rect_done += dn;
            if (BK.rect_done >= rc[3]) { BK.nrect--; memmove(BK.rect[0], BK.rect[1], sizeof BK.rect[0] * BK.nrect); BK.rect_done = 0; BK.df_due = 1; }
        }
        if (BK.l_again && --BK.l_again == 0) { l0 = 0; ln = BK.nl; BK.l_row = 0; }
        else if (BK.lroll > 0 || BK.l_left) {
            uint32_t lr = (uint32_t)(BK.lroll > 0 ? BK.lroll : BK.roll), per = (BK.nl + lr - 1u) / lr;
            if (BK.l_row >= BK.nl) BK.l_row = 0;
            ln = per < BK.nl - BK.l_row ? per : BK.nl - BK.l_row;
            l0 = BK.l_row; BK.l_row += ln; if (BK.l_row >= BK.nl) BK.l_row = 0;
            BK.l_left -= ln < BK.l_left ? ln : BK.l_left;
        }
    }
    if (BK.df_due) { dfn = 1; BK.df_due = 0; }
    if (!dn && !ln && !dfn) return;
    if (BK.q) { vkCmdResetQueryPool(cmd, BK.q, 2u * (uint32_t)fi, 2); ps2_clouds_fence(cmd); vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, BK.q, 2u * (uint32_t)fi); }
    barrier2(cmd, CS, RW, CS, RW);        /* (the splat and the shadow slice written; the last frame's march read the volumes) */
    bk_push bp; memset(&bp, 0, sizeof bp);
    bp.i1[0] = (int32_t)BK.nc; bp.i1[1] = (int32_t)BK.ncy; bp.i1[3] = BK.R;
    bp.f0[1] = BK.base; bp.f0[2] = BK.top; bp.f0[3] = 0.02f;
    double W = one2 ? (double)rq_one_p : (double)CL_WEATHER_M;   /* (PS2_RQ_CLOUDS_ONE=2: the period's torus, bp.i2.z) */
    bp.i2[2] = one2 ? 1 : 0;
    if (dn) {
        bp.i0[0] = (int32_t)BK.nx; bp.i0[1] = (int32_t)BK.ny; bp.i0[2] = (int32_t)d0; bp.i0[3] = (int32_t)dn; bp.f0[0] = (float)(W / BK.nx);
        bp.i2[0] = (int32_t)dx0; bp.i2[1] = (int32_t)dxn;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, BK.p_d);
        vkCmdPushConstants(cmd, F.layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof bp, &bp);
        vkCmdDispatch(cmd, dxn / 4u, BK.ny / 4u, dn / 4u);
        BK.d_rows_n += dn; BK.d_texels_n += (uint64_t)dxn * dn * BK.ny;
    }
    if (ln) {
        bp.i0[0] = (int32_t)BK.nl; bp.i0[1] = (int32_t)BK.nly; bp.i0[2] = (int32_t)l0; bp.i0[3] = (int32_t)ln; bp.f0[0] = (float)(W / BK.nl);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, BK.p_l);
        vkCmdPushConstants(cmd, F.layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof bp, &bp);
        vkCmdDispatch(cmd, (BK.nl + 7u) / 8u, (BK.nly + 3u) / 4u, (ln + 3u) / 4u);
        BK.l_rows_n += ln;
    }
    if (dfn && BK.df && BK.p_df) {        /* (the distance field from the occupancy, three separable passes: after a whole bake and when a roll
                                             or the queued boxes are done -- a change seen up to a roll late: a 1/64 opacity step) */
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, BK.p_df);
        for (int ax = 0; ax < 3; ax++) {
            barrier2(cmd, CS, RW, CS, RW);
            bp.i1[2] = ax;
            vkCmdPushConstants(cmd, F.layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof bp, &bp);
            vkCmdDispatch(cmd, (BK.nc + 3u) / 4u, (BK.ncy + 3u) / 4u, (BK.nc + 3u) / 4u);
        }
        BK.df_n++;
    }
    barrier2(cmd, CS, RW, CS, RW);        /* (the march reads them) */
    if (BK.q) { ps2_clouds_fence(cmd); vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, BK.q, 2u * (uint32_t)fi + 1u); BK.q_kind[fi] = (int8_t)(hard ? 1 : 0); }
    BK.valid = 1; BK.work_n++;
}
#endif
#if PS2_RQ_SUPPORT
#include "rq/rq_thin.inc"   /* PS2_RQ_CLOUDS: the far update of the Quest eye pass (rq_cloud_far.comp): the travel rule, the tile budget, the draw list (rq20_edges: rq_thin.inc, PS2_RQ_CLOUDS_THIN_MAX) */
static int rq_far_record(const ps2_cloud_gpu *g, VkCommandBuffer cmd, const ps2_cloud_rec *r) {
    (void)g;
    const ps2_cloud_env *e = r->env;
    const ps2_cloud_view *v = r->view;
    const ps2_cloud_cfg *cfg = ps2_clouds_cfg();
    int fi = r->frame_index;
    uint8_t *slot = F.host + (size_t)HOST_SLOT * (uint32_t)fi;
    if (F.rq_tq && F.rq_tarm[fi] >= 0) {   /* PS2_RQ_CLOUDS_FARTIME: this slot's previous far update (its fence has passed) */
        uint64_t ts[3];
        int a = F.rq_tarm[fi];
        if (vkGetQueryPoolResults(F.dev, F.rq_tq, 3u * (uint32_t)fi, 3, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            F.rq_tms[a] += (double)(ts[2] - ts[0]) * F.wq_ns * 1e-6; F.rq_tcnt[a]++;
            F.rq_tmm[a] += (double)(ts[2] - ts[1]) * F.wq_ns * 1e-6;
            if (F.rq_stub) {                   /* (PS2_RQ_CLOUDS_FARSTUB: the slot's sample counters, copied after its march) */
                const uint32_t *sc = (const uint32_t *)(slot + HOST_COUNTS + 128);
                for (int k = 0; k < 3; k++) F.rq_smp[a][k] += (double)sc[k];
            }
        }
        F.rq_tarm[fi] = -1;
        int two = F.rq_tab || F.rq_stub == 1 || rq_thin_ab();
        if (F.rq_tcnt[0] + F.rq_tcnt[1] >= (uint32_t)F.rq_tn * (two ? 2u : 1u) && (!two || (F.rq_tcnt[0] && F.rq_tcnt[1]))) {
            if (F.rq_stub) {
                double ns[2], sm[2];
                for (int k = 0; k < 2; k++) {
                    double n = F.rq_tcnt[k] ? (double)F.rq_tcnt[k] : 1.0;
                    sm[k] = (F.rq_smp[k][0] + F.rq_smp[k][1]) / n;
                    ns[k] = sm[k] > 0.0 ? F.rq_tmm[k] / n * 1e6 / sm[k] : 0.0;
                }
                ps2_log("clouds far rq stub: the stub march %.3f ms a frame (far update %.3f; %u frames; %.0f coarse + %.0f fine samples, "
                        "%.1f tiles a frame): %.2f ns a sample; the real march %.3f ms (far update %.3f; %u frames; %.0f + %.0f samples, %.1f "
                        "tiles): %.2f ns a sample; a cached sample %.2fx cheaper (PS2_RQ_CLOUDS_FARSTUB)",
                        F.rq_tmm[1] / (F.rq_tcnt[1] ? F.rq_tcnt[1] : 1), F.rq_tms[1] / (F.rq_tcnt[1] ? F.rq_tcnt[1] : 1), F.rq_tcnt[1],
                        F.rq_smp[1][0] / (F.rq_tcnt[1] ? F.rq_tcnt[1] : 1), F.rq_smp[1][1] / (F.rq_tcnt[1] ? F.rq_tcnt[1] : 1),
                        F.rq_smp[1][2] / (F.rq_tcnt[1] ? F.rq_tcnt[1] : 1), ns[1],
                        F.rq_tmm[0] / (F.rq_tcnt[0] ? F.rq_tcnt[0] : 1), F.rq_tms[0] / (F.rq_tcnt[0] ? F.rq_tcnt[0] : 1), F.rq_tcnt[0],
                        F.rq_smp[0][0] / (F.rq_tcnt[0] ? F.rq_tcnt[0] : 1), F.rq_smp[0][1] / (F.rq_tcnt[0] ? F.rq_tcnt[0] : 1),
                        F.rq_smp[0][2] / (F.rq_tcnt[0] ? F.rq_tcnt[0] : 1), ns[0], ns[1] > 0.0 ? ns[0] / ns[1] : 0.0);
                memset(F.rq_smp, 0, sizeof F.rq_smp);
            } else if (rq_thin_ab()) rq_thin_log(F.rq_tms, F.rq_tmm, F.rq_tcnt); else if (F.rq_tab)
                ps2_log("clouds far rq time: far update %.3f ms with PS2_RQ_CLOUDS_FARFAIR + %u slots (%u frames), %.3f ms with neither (%u): "
                        "the switch costs %.3f ms", F.rq_tms[1] / F.rq_tcnt[1], F.rq_slots, F.rq_tcnt[1], F.rq_tms[0] / F.rq_tcnt[0], F.rq_tcnt[0],
                        F.rq_tms[1] / F.rq_tcnt[1] - F.rq_tms[0] / F.rq_tcnt[0]);
            else {
                uint32_t n = F.rq_tcnt[0] + F.rq_tcnt[1];
                ps2_log("clouds far rq time: far update %.3f ms GPU a frame (%u frames; the march %.3f ms)", (F.rq_tms[0] + F.rq_tms[1]) / n, n,
                        (F.rq_tmm[0] + F.rq_tmm[1]) / n);
            }
            F.rq_tms[0] = F.rq_tms[1] = 0.0; F.rq_tcnt[0] = F.rq_tcnt[1] = 0u; F.rq_tmm[0] = F.rq_tmm[1] = 0.0;
        }
    }
    if (BK.on && BK.count && F.frames >= FAR_FIF) {   /* (rq7_bake, PS2_RQ_CLOUDS_BAKE_COUNT: this slot's march's reads, FAR_FIF frames ago) */
        const uint32_t *sc = (const uint32_t *)(slot + HOST_COUNTS + 128);
        BK.smp[0] += sc[0]; BK.smp[1] += sc[1]; BK.smp[2] += sc[2]; BK.smp[3] += sc[7]; BK.smp_n++;
        if (BK.count > 1) {           /* (the check variant's sums: sc[0..7]) */
            for (int k = 0; k < 8; k++) BK.chk[k] += sc[k];
            if (BK.smp_n >= 600u) {
                ps2_log("clouds far rq bake: [debug] CHECK the baked against the procedural sample at the baked march's fine samples: density "
                        "near (t < the detail range) %.4f / %.4f = %.3f over %.0f samples, far %.4f / %.4f = %.3f over %.0f; mean |above| %.4f, "
                        "|cast shadow| %.4f (%u frames)", BK.chk[4] > 0 ? BK.chk[0] / 1000.0 / BK.chk[4] : 0.0, BK.chk[4] > 0 ? BK.chk[1] / 1000.0 / BK.chk[4] : 0.0,
                        BK.chk[1] > 0 ? BK.chk[0] / BK.chk[1] : 0.0, BK.chk[4], BK.chk[5] > 0 ? BK.chk[2] / 1000.0 / BK.chk[5] : 0.0,
                        BK.chk[5] > 0 ? BK.chk[3] / 1000.0 / BK.chk[5] : 0.0, BK.chk[3] > 0 ? BK.chk[2] / BK.chk[3] : 0.0, BK.chk[5],
                        BK.chk[4] + BK.chk[5] > 0 ? BK.chk[6] / 1000.0 / (BK.chk[4] + BK.chk[5]) : 0.0,
                        BK.chk[4] + BK.chk[5] > 0 ? BK.chk[7] / 1000.0 / (BK.chk[4] + BK.chk[5]) : 0.0, BK.smp_n);
                memset(BK.chk, 0, sizeof BK.chk); BK.smp_n = 0;
            }
        } else
        if (BK.smp_n >= 600u) {
            double rays = BK.smp[2] * 64.0;
            ps2_log("clouds far rq bake: reads a ray %.2f density in clear air (the entry's bisection included) + %.2f distance field + %.2f "
                    "fine samples (each a density and a light read) = %.2f; %.0f rays (%.1f tiles) a frame over %u frames",
                    rays > 0.0 ? BK.smp[0] / rays : 0.0, rays > 0.0 ? BK.smp[3] / rays : 0.0, rays > 0.0 ? BK.smp[1] / rays : 0.0,
                    rays > 0.0 ? (BK.smp[0] + BK.smp[3] + 2.0 * BK.smp[1]) / rays : 0.0, rays / BK.smp_n, BK.smp[2] / BK.smp_n, BK.smp_n);
            memset(BK.smp, 0, sizeof BK.smp); BK.smp_n = 0;
        }
    }
    if (F.debug && F.frames >= FAR_FIF) {   /* (ACZ_DEBUG_CLOUDS: a new generation's never-marched tiles, frame by frame: PS2_RQ_CLOUDS_COLD's burst) */
        const uint32_t *cnt = (const uint32_t *)(slot + HOST_COUNTS);
        if (cnt[0] > 0u) { F.cold_n++; F.cold_all += cnt[0]; if (cnt[0] > F.cold_max) F.cold_max = cnt[0]; }
        else if (F.cold_n) {
            ps2_log("clouds far rq: a new generation's tiles marched over %u frames, %u in all, at most %u in one frame (PS2_RQ_CLOUDS_COLD %d; the "
                    "tiles in view first, %u a frame at most with the switch)", F.cold_n, F.cold_all, F.cold_max, (int)F.par.rq[2], (unsigned)F.par.rq[2]);
            F.cold_n = F.cold_all = F.cold_max = 0;
        }
    }
    if (F.debug && F.frames > 0 && F.frames % (uint32_t)F.debug == 0) {
        const uint32_t *cnt = (const uint32_t *)(slot + HOST_COUNTS);     /* this slot's frame, FAR_FIF frames ago */
        ps2_log("clouds far rq: tiles marched %u (never %u, x16 %u, x8 %u, x4 %u, x2 %u, x1 %u, refresh %u, outside %u), tiles drawn %u; "
                "visible tiles due by travel %u (the backlog), the most flown since a drawn tile's march %u m",
                cnt[8], cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], cnt[5], cnt[6], cnt[7], cnt[9], cnt[10], cnt[11]);
        {   /* (clouds6, rq_cloud_far.comp PASS_TILES: the A.7 gate's structure counts of this slot's frame) */
            const uint32_t *sc = (const uint32_t *)(slot + HOST_COUNTS + 128);
            ps2_log("clouds far rq parallax: drawn %u tiles, %u of them more than 1 far texel (0.2 deg) of uncorrected parallax at their first "
                    "cloud, %u more than 8; %u beside a drawn tile of another origin (over 1 texel apart); never-marched tiles in view: %u "
                    "drawn from the coarse level, %u not drawn (holes)", cnt[9], sc[3], sc[4], sc[5], sc[6], sc[7]);
        }
        if (F.rq_swap) {
            const uint32_t *sw = (const uint32_t *)(slot + HOST_COUNTS + 64);
            ps2_log("clouds far rq: sweep %u, %u tiles staged, %u still due; sweeps put in %u, the last %u tiles over %u frames (PS2_RQ_CLOUDS_FARSWAP)",
                    sw[8], sw[9], cnt[10], sw[13], sw[14], sw[15]);
#if PS2_RQ_SUPPORT
            {   /* (rq4_views 2026-11-11, R-99 [debug]: the composite's parallax offset |off| = |eye - tile origin| (rq_clouds.vert) for the
                   drawn sweep, and the running sweep's lead; the cockpit's against the chase view's) */
                const float *o = (const float *)sw;
                float d0[3] = { F.rq_cam[0] - o[4], F.rq_cam[1] - o[5], F.rq_cam[2] - o[6] }, d1[3] = { o[0] - o[4], o[1] - o[5], o[2] - o[6] };
                float vl = sqrtf(F.rq_vel[0] * F.rq_vel[0] + F.rq_vel[1] * F.rq_vel[1] + F.rq_vel[2] * F.rq_vel[2]);
                extern volatile int ps2_vk_cockpit_view;
                ps2_log("clouds far rq: off %.1f m (the eye from the drawn sweep's origin), the running sweep's origin %.1f m from it; the eye's "
                        "smoothed motion %.2f m a frame, lead %.0f m (%s view)", sqrtf(d0[0] * d0[0] + d0[1] * d0[1] + d0[2] * d0[2]),
                        sqrtf(d1[0] * d1[0] + d1[1] * d1[1] + d1[2] * d1[2]), vl, vl * F.rq_lead * (float)(sw[15] ? sw[15] : 1u),
                        ps2_vk_cockpit_view ? "cockpit" : "outside");
            }
#endif
        }
    }
    /* the anchor: tile origins are float32 relative to it; only a rebase (a jump of the world, the mission start) moves
       it: 2 mm of precision at 20 km, 16 mm at 150 km */
    double dx = v->cloud_origin_m[0] - F.anchor[0], dy = v->cloud_origin_m[1] - F.anchor[1], dz = v->cloud_origin_m[2] - F.anchor[2];
    if (F.have_anchor && F.rebase != v->rebase && rq_one()) {
        /* (rq8_onelayer, PS2_RQ_CLOUDS_ONE: every tile in view is marched every frame from the eye, so a camera cut needs no new generation --
           it made every tile "never marched" and the frame drew no cloud; the anchor stays: float32 holds a 100 km offset to 8 mm) */
        if (F.debug > 0) ps2_log("clouds far rq one: a rebase (camera cut): no new generation, the anchor kept");
        F.rebase = v->rebase;
    }
    if (!F.have_anchor || F.rebase != v->rebase) {
        if (F.debug > 0 && F.have_anchor) ps2_log("clouds far rq: new generation (rebase): every tile re-marched");
        memcpy(F.anchor, v->cloud_origin_m, sizeof F.anchor); F.have_anchor = 1; F.rebase = v->rebase; F.gen++;
        far_tr_flags[fi] |= 4u;
        dx = dy = dz = 0.0;
    }
    fill_params(fi, e, v);
    int list = list_path(e);
    far_push pc; memset(&pc, 0, sizeof pc);
    pc.cam[0] = (float)dx; pc.cam[1] = (float)dy; pc.cam[2] = (float)dz;
    F.rq_cam[0] = pc.cam[0]; F.rq_cam[1] = pc.cam[1]; F.rq_cam[2] = pc.cam[2];
    /* the far part starts below the near range by the cross-fade band (rq_cloud_far.glsl rq_far_w) */
    float band = cfg->near_on ? F.rq_band : 0.0f;
    pc.cam[3] = F.start_m >= 0.0f ? F.start_m : cfg->near_on ? cfg->near_range_m - band : 300.0f;
    pc.march[0] = list ? list_range(e) : F.max_m; pc.march[1] = F.dt_min; pc.march[2] = F.rq_kstep;   /* (a shorter range start: longer steps with the distance) */
    pc.march[3] = 0.0f;
    if (rq_one_now()) { pc.cam[3] = rq_one_start(); pc.march[1] = rq_one_dtmin(); pc.march[3] = rq_one_left_out(3) ? 0.0f : rq_one_steps(); }   /* (PS2_RQ_CLOUDS_ONE: from the eye, one layer; more steps) */
    slot_params(fi)->fine[0] = F.rq_fine; rq_thin_frame(fi, &pc);   /* (rq20_edges: PS2_RQ_CLOUDS_THIN_MAX, [test] _TERM_ALT) */
    float slice = (float)cfg->far_slice * (F.wx_quiet < 120u ? 1.0f : F.quiet_k);
    if (F.wx_quiet < 0xFFFFFFu) F.wx_quiet++;
    int gon, grelax;
    far_gov_state(F.frames, &gon, &grelax);
    uint32_t budget = (uint32_t)(F.rq_budget > 0 ? F.rq_budget : 0);
    {   /* PS2_RQ_CLOUDS_TILES_AB=N [measurement] (clouds6 2026-11-11, prompt 6 step 5; with PS2_RQ_ABSKIP=K): the tile budget N in the
           "left out" blocks of K eye-pass lists, ACZ_VR_CLOUD_RQ_TILES in the others; the `rq-ab:` lines give the frame's GPU time per
           arm (with PS2_RQ_STAMP_FENCE the stamps land after the work): the marginal tiles' price inside one flight */
        static int tab = -2;
        if (tab == -2) { const char *e = getenv("PS2_RQ_CLOUDS_TILES_AB"); tab = e && *e ? atoi(e) : -1;
                         if (tab >= 0) ps2_log("clouds far rq: [measurement] %d tiles a frame in the left-out blocks of PS2_RQ_ABSKIP (PS2_RQ_CLOUDS_TILES_AB)", tab); }
        extern int ps2_rq_ab_left_out(void);
        if (tab >= 0 && ps2_rq_ab_left_out()) budget = (uint32_t)tab;
    }
    if (gon && budget > 48u) budget = 48u;                  /* (the fallback ladder's first rung) */
    if (gon) slice *= 2.0f;
    (void)slice;
    pc.sched[0] = F.rq_travel; pc.sched[1] = F.rq_period * (gon ? 2.0f : 1.0f); pc.sched[2] = (float)budget;
    /* the near field's frames: every tile in view is drawn (rq_cloud_far.comp) */
    float nearest;
    int gate = cfg->near_on && !rq_one_now() ? ps2_clouds_far_near_gate(v, cfg->near_range_m, 60.0f, &nearest) : 0;
    pc.sched[3] = gate > 0 ? 1.0f : 0.0f;
    {   /* (clouds6, ACZ_VR_CLOUD_NEAR_RANGE_COCKPIT: the near range changed with the view -- the tiles' march start --: a new generation;
           FARCOARSE fills the view at once) */
        static float last_r = -1.0f;
        if (cfg->near_on && last_r > 0.0f && cfg->near_range_m != last_r) {
            if (F.debug > 0) ps2_log("clouds far rq: new generation (the near range %.0f -> %.0f m): every tile re-marched", (double)last_r, (double)cfg->near_range_m);
            F.gen++; far_tr_flags[fi] |= 4u;
        }
        last_r = cfg->near_range_m;
    }
    uint32_t ne = F.ubo_asked && F.n_ubo <= F.n && F.n_ubo >= 64u ? F.n_ubo : F.n;
    if (ne != F.n_used) {
        if (F.debug > 0 && F.n_used) ps2_log("clouds far rq: new generation (used size %u -> %u): every tile re-marched", F.n_used, ne);
        F.n_used = ne; F.gen++; far_tr_flags[fi] |= 4u;
    }
    uint32_t ncap = overcast(e) && F.ovc_steps > 0 ? (uint32_t)F.ovc_steps : 0u;
    if (F.debug) {   /* (ACZ_DEBUG_CLOUDS, R-70: how often the march's per-frame inputs flip -- the overcast step cap, the thin steps) */
        static uint32_t last_cap = 0xFFFFFFFFu, flips, frames; static float last_thin = -1.0f; static uint32_t thin_flips;
        if (last_cap != 0xFFFFFFFFu && ncap != last_cap) flips++;
        if (last_thin >= 0.0f && slot_params(fi)->haze[0] != last_thin) thin_flips++;
        last_cap = ncap; last_thin = slot_params(fi)->haze[0];
        if (++frames % (uint32_t)F.debug == 0u) {
            ps2_log("clouds far rq: march inputs over %d frames: the overcast step cap flipped %u times (now %u), the thin steps %u times; "
                    "type %.3f, coverage unmasked %.3f, march range %.0f m", F.debug, flips, ncap, thin_flips,
                    (double)e->type, (double)e->coverage_unmasked, (double)pc.march[0]);
            flips = thin_flips = 0;
        }
    }
    uint32_t tpr = ne / 8u, nt = tpr * tpr;
    pc.fr[0] = F.frames; pc.fr[1] = ne; pc.fr[2] = tpr; pc.fr[3] = (ncap & 0xFFu) << 1 | (F.gen & 0x3FFFFFu) << 9;
    if (F.rq_all) pc.fr[3] |= 1u;                               /* ([debug] PS2_RQ_CLOUDS_FARALL: bit 0, every tile into bucket 0) */
    if (F.rq_all > 1) pc.fr[3] |= 0x80000000u;                  /* ([test] =2: bit 31, only the tiles in view; rq_cloud_far.comp FR_GEN) */
    {   /* [debug] (clouds7, R-111) PS2_RQ_CLOUDS_FARPC_LOG=F: the march inputs of far frames F .. F + 11, one line each (the slot's params
           as a checksum): what differs between two marches from one point in a still scene */
        static long pcl = -2;
        if (pcl == -2) { const char *s = getenv("PS2_RQ_CLOUDS_FARPC_LOG"); pcl = s && *s ? atol(s) : -1; }
        if (pcl >= 0 && F.frames >= (uint32_t)pcl && F.frames < (uint32_t)pcl + 12u) {
            const uint32_t *w = (const uint32_t *)slot_params(fi); uint32_t h = 2166136261u;
            for (size_t k = 0; k < sizeof(far_params) / 4u; k++) { h ^= w[k]; h *= 16777619u; }
            ps2_log("clouds far rq pc: frame %u slot %d field %llu cam %.3f %.3f %.3f start %.1f march %.1f %.3f %.4f %.1f sched %.3f %.1f %.0f %.0f "
                    "fr.w %08X params %08X near_on %d", F.frames, fi, (unsigned long long)ps2_kernel_vblank_count(), (double)pc.cam[0], (double)pc.cam[1],
                    (double)pc.cam[2], (double)pc.cam[3], (double)pc.march[0], (double)pc.march[1], (double)pc.march[2], (double)pc.march[3],
                    (double)pc.sched[0], (double)pc.sched[1], (double)pc.sched[2], (double)pc.sched[3], pc.fr[3], h, cfg->near_on);
        }
    }
    if (F.rq_lead > 0.0f) {                                     /* PS2_RQ_CLOUDS_FARLEAD: the eye's motion per far frame, smoothed */
        if (F.rq_pv_ok && F.rq_pv_ok == (int)(F.gen & 0x3FFFFFFFu) + 1) {     /* (the same generation: the anchor did not move) */
            float d[3] = { pc.cam[0] - F.rq_pv[0], pc.cam[1] - F.rq_pv[1], pc.cam[2] - F.rq_pv[2] };
            if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 50.0f * 50.0f)          /* (a jump: kept out) */
                for (int k = 0; k < 3; k++) F.rq_vel[k] += 0.1f * (d[k] - F.rq_vel[k]);
        } else memset(F.rq_vel, 0, sizeof F.rq_vel);
        memcpy(F.rq_pv, pc.cam, sizeof F.rq_pv); F.rq_pv_ok = (int)(F.gen & 0x3FFFFFFFu) + 1;
        far_params *sp = slot_params(fi);
        sp->pred[0] = F.rq_vel[0]; sp->pred[1] = F.rq_vel[1]; sp->pred[2] = F.rq_vel[2]; sp->pred[3] = F.rq_lead;
    }
    int tarm = rq_thin_arm();   /* (rq20_edges: PS2_RQ_CLOUDS_THIN_MAX_AB's arm, 0 otherwise) */
    if (F.rq_tq) {                                              /* PS2_RQ_CLOUDS_FARTIME (and _FARFAIR_AB: the arm of this block) */
        if (F.rq_tab) {
            tarm = (int)((F.frames / (uint32_t)F.rq_tn) & 1u);
            slot_params(fi)->rq[3] = tarm ? (F.par.rq[3] > 0.5f ? F.par.rq[3] : 1.0f) : 0.0f;
            slot_params(fi)->dbg[3] = tarm || F.rq_slots < 4096u ? (float)F.rq_slots : 4096.0f;
            slot_params(fi)->pred[3] = tarm ? F.rq_lead : 0.0f;
        }
        if (F.rq_stub) tarm = F.rq_stub == 2 ? 1 : (int)((F.frames / (uint32_t)F.rq_tn) & 1u);   /* (PS2_RQ_CLOUDS_FARSTUB: 1 = the stub) */
        vkCmdResetQueryPool(cmd, F.rq_tq, 3u * (uint32_t)fi, 3);
        extern void ps2_clouds_fence(VkCommandBuffer cmd);   /* (PS2_RQ_STAMP_FENCE) */
        ps2_clouds_fence(cmd);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, F.rq_tq, 3u * (uint32_t)fi);
    }
    VkDescriptorSet s = F.set[fi];
    barrier2(cmd, CS | XS, RW | VK_ACCESS_2_TRANSFER_READ_BIT, XS, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (!F.cleared) { vkCmdFillBuffer(cmd, F.tiles, 0, VK_WHOLE_SIZE, 0); vkCmdFillBuffer(cmd, F.rq_sweep, 0, VK_WHOLE_SIZE, 0); F.cleared = 1; }   /* generation 0 = never; sweep 0 = none */
    vkCmdFillBuffer(cmd, F.list, 0, 80, 0);                     /* (cnt[12], args[4], dargs[4]) */
    if (F.rq_stub || F.debug || BK.count) vkCmdFillBuffer(cmd, F.list, 80 + (VkDeviceSize)8 * nt * 4, 32, 0);   /* (the list's 8 spare words list[8 NT ..]:
                                                                   PS2_RQ_CLOUDS_FARSTUB's sample counters, ACZ_DEBUG_CLOUDS' structure counts) */
    barrier2(cmd, XS, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, RW);
    ps2_clouds_bind_common(cmd, F.layout);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, F.layout, 1, 1, &s, 0, NULL);
    vkCmdPushConstants(cmd, F.layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    if (BK.on) {                                                /* (rq7_bake, PS2_RQ_CLOUDS_BAKE: the volumes, then this frame's params of them) */
        bk_record(cmd, fi, e, v);
        far_params *sp = slot_params(fi);
        sp->bk[0] = BK.valid ? 1.0f : 0.0f; sp->bk[1] = BK.base; sp->bk[2] = 1.0f / (BK.top - BK.base); sp->bk[3] = BK.df && BK.p_df ? 1.0f : 0.0f;
        sp->bk2[0] = (float)(CL_WEATHER_M / BK.nc); sp->bk2[1] = (BK.top - BK.base) / (float)BK.ncy; sp->bk2[2] = (float)BK.nc; sp->bk2[3] = (float)BK.ncy;
        int one2 = rq_one() >= 2 && rq_one_p > 0.0f && BK.one_mode;   /* (PS2_RQ_CLOUDS_ONE=2: the period's torus) */
        double W = one2 ? (double)rq_one_p : (double)CL_WEATHER_M;
        sp->bk2[0] = (float)(W / BK.nc);
        sp->bk3[1] = (float)(W / BK.nx); { extern int ps2_rq_sun_tile(void); sp->dbg[0] = (float)ps2_rq_sun_tile(); }   /* (rq10_artefacts round 2, PS2_RQ_CLOUDS_SUN_T: rq_cloud_far.comp) */
        sp->bk3[3] = one2 ? 2.0f : (float)rq_one_now();           /* (PS2_RQ_CLOUDS_ONE: 1 the march from the eye, no cross-fade; 2 also the torus) */
        sp->mz[3] = rq_one_now() && !rq_one_left_out(2) ? rq_one_near() : 0.0f;   /* (PS2_RQ_CLOUDS_ONE: the full-noise coverage near the eye, rq_cloud_far.glsl bk_near_w) */
        if (rq_one_now()) sp->sch[1] = rq_one_split();           /* (PS2_RQ_CLOUDS_ONE: the front part's end; sch.y is the PC scheduler's only) */
        /* (PS2_RQ_CLOUDS_ONE=1: the distance field is rebuilt only when a roll of the density ends; while one runs, a body that grew in the
           re-baked slices is not in the field yet and the march skipped through it -- dark dots of sky inside the cloud, in the baked frames
           only (PS2_RQ_CLOUDS_BAKE_ALT, M10 f6060): no skips until the field is current again. =2 never rolls. Rebuilding the field after
           every slice instead cost more: M10 tape far 0.200 against 0.174 ms, 66 against 49 reads a ray) */
        if (rq_one() && (BK.d_left || BK.nrect || BK.df_due)) sp->bk[3] = 0.0f;
        if (one2) {                                            /* (the bake's light pass: the cast shadow's light and strength, fp.pred) */
            extern int ps2_clouds_shadow_rq_light(const ps2_cloud_env *e, float L[3], float *k);
            float L[3] = { 0.0f, 1.0f, 0.0f }, k = 0.0f;
            if (!ps2_clouds_shadow_rq_light(e, L, &k)) k = 0.0f;
            sp->pred[0] = L[0]; sp->pred[1] = L[1]; sp->pred[2] = L[2]; sp->pred[3] = k;
        }
        vkCmdPushConstants(cmd, F.layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    }
    if (F.p_coarse_rq && !F.rq_all) {   /* PS2_RQ_CLOUDS_FARCOARSE: a new generation's coarse level, one chunk a frame, before the tiles pass */
        if (F.cg_gen != F.gen) { F.cg_gen = F.gen; F.cg_k = 0; memcpy(F.cg_org, pc.cam, sizeof F.cg_org); }
        if (F.cg_k < (uint32_t)F.coarse_k) {
            uint32_t nc = (tpr + 1u) * (tpr + 1u), per = (nc + (uint32_t)F.coarse_k - 1u) / (uint32_t)F.coarse_k;
            far_push cp = pc;
            memcpy(cp.sched, F.cg_org, sizeof F.cg_org); cp.sched[3] = F.cg_k + 1u == (uint32_t)F.coarse_k ? 1.0f : 0.0f;
            cp.march[3] = (float)(F.cg_k * per);
            vkCmdPushConstants(cmd, F.layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof cp, &cp);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, F.p_coarse_rq);
            vkCmdDispatch(cmd, (per + 63u) / 64u, 1, 1);
            barrier2(cmd, CS, RW, CS, RW);
            vkCmdPushConstants(cmd, F.layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
            F.cg_k++;
        }
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, F.p_tiles_rq);
    vkCmdDispatch(cmd, (nt + 63u) / 64u, 1, 1);
    barrier2(cmd, CS, RW, CS, RW);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, F.p_args_rq);
    vkCmdDispatch(cmd, 1, 1, 1);
    barrier2(cmd, CS, VK_ACCESS_2_SHADER_WRITE_BIT, CS | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
             VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    if (F.rq_swap) {   /* PS2_RQ_CLOUDS_FARSWAP: a finished sweep into the map (0 work groups in every other frame), before this frame's march */
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, F.p_copy_rq);
        vkCmdDispatchIndirect(cmd, F.rq_sweep, 64);
        barrier2(cmd, CS, RW, CS, RW);
    }
    {   extern void ps2_clouds_fence(VkCommandBuffer cmd);   /* (PS2_RQ_STAMP_FENCE: the stamps after the work, ps2_clouds.c) */
        if (F.rq_tq) { ps2_clouds_fence(cmd); vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, F.rq_tq, 3u * (uint32_t)fi + 1u); } }   /* (the march's own time) */
    int bk_proc = 0;                                            /* ([test] PS2_RQ_CLOUDS_BAKE_ALT: odd GS frames -- the eye dumps' numbers -- procedural) */
    if (BK.on && BK.alt) {
        extern u32 ps2_gs_frame_count;
        bk_proc = (int)(ps2_gs_frame_count & 1u);
        if ((ps2_gs_frame_count & 63u) < 2u) ps2_log("clouds far rq bake: [test] ALT: GS frame %u marches the %s samples", (unsigned)ps2_gs_frame_count, bk_proc ? "procedural" : "baked");
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, BK.on && BK.valid && !bk_proc ?   /* (rq7_bake) */
                      (BK.count ? BK.p_march_cnt : BK.p_march) :
                      F.rq_stub ? (tarm ? F.p_march_stub : F.p_march_cnt) : F.p_march_lg ? F.p_march_lg : F.p_march_rq);
    vkCmdDispatchIndirect(cmd, F.list, 48);
    far_tr_flags[fi] |= 8u;
    if (!F.sh_init && F.sh_img[0] && F.sh_img[1]) {   /* (the shown images to GENERAL once: the composite and the near set hold them) */
        {
            VkImageMemoryBarrier ib[2]; memset(ib, 0, sizeof ib);
            for (int k = 0; k < 2; k++) {
                ib[k].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; ib[k].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; ib[k].newLayout = VK_IMAGE_LAYOUT_GENERAL;
                ib[k].srcQueueFamilyIndex = ib[k].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; ib[k].image = F.sh_img[k];
                ib[k].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ib[k].subresourceRange.levelCount = 1; ib[k].subresourceRange.layerCount = 1;
                ib[k].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            }
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, ib);
            /* (cleared once: a texel the resolve never wrote reads as no content and no handover shift, not as whatever the memory held) */
            VkClearColorValue z; memset(&z, 0, sizeof z);
            VkImageSubresourceRange rr; memset(&rr, 0, sizeof rr); rr.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rr.levelCount = 1; rr.layerCount = 1;
            for (int k = 0; k < 2; k++) vkCmdClearColorImage(cmd, F.sh_img[k], VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rr);
            barrier2(cmd, XS, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, RW);
            F.sh_init = 1;
        }
    }
    if (F.sh_on && F.p_resolve_rq) {   /* (clouds7, PS2_RQ_CLOUDS_FARSHOWN: the shown map from this frame's map, for the eye pass's composite) */
        barrier2(cmd, CS, RW, CS, RW);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, F.p_resolve_rq);
        vkCmdDispatchIndirect(cmd, F.list, 80 + (VkDeviceSize)8 * nt * 4 + 32);
        barrier2(cmd, CS, RW, CS | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                 VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    }
    if (F.rq_tq) { extern void ps2_clouds_fence(VkCommandBuffer cmd); ps2_clouds_fence(cmd); vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, F.rq_tq, 3u * (uint32_t)fi + 2u); F.rq_tarm[fi] = (int8_t)tarm; }
    if (F.rq_stub || F.debug || BK.count) {   /* (this frame's spare-word counters into its slot, read FAR_FIF frames later) */
        barrier2(cmd, CS, VK_ACCESS_2_SHADER_WRITE_BIT, XS, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferCopy sc; sc.srcOffset = 80 + (VkDeviceSize)8 * nt * 4; sc.dstOffset = (VkDeviceSize)HOST_SLOT * (uint32_t)fi + HOST_COUNTS + 128; sc.size = 32;
        vkCmdCopyBuffer(cmd, F.list, F.host_buf, 1, &sc);
    }
    if (F.debug || far_trace_on()) {          /* the counters into this slot (read FAR_FIF frames later) */
        barrier2(cmd, CS, VK_ACCESS_2_SHADER_WRITE_BIT, XS, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferCopy bc; bc.srcOffset = 0; bc.dstOffset = (VkDeviceSize)HOST_SLOT * (uint32_t)fi + HOST_COUNTS; bc.size = 48;
        vkCmdCopyBuffer(cmd, F.list, F.host_buf, 1, &bc);
        if (F.rq_swap) { bc.dstOffset += 64; bc.size = 64; vkCmdCopyBuffer(cmd, F.rq_sweep, F.host_buf, 1, &bc); }   /* (the sweep's header) */
    }
    F.frames++;
    return 1;
}
/* the buffers of the composite (rq_clouds.vert): the tiles and the draw list, the list with its draw arguments at offset 64 */
void ps2_clouds_far_rq_buffers(VkBuffer *tiles, VkBuffer *draw, VkBuffer *list) { *tiles = F.tiles; *draw = F.rq_draw; *list = F.list; }
void ps2_clouds_far_rq_cam(float cam[3]) { memcpy(cam, F.rq_cam, sizeof F.rq_cam); }
/* (clouds7, PS2_RQ_CLOUDS_FARSHOWN) the shown map's views (colour, depth data) for the composite's set; 1 = on */
int ps2_clouds_far_rq_shown(VkImageView *c, VkImageView *p) { *c = F.sh_view[0]; *p = F.sh_view[1]; return F.sh_on; }
float ps2_clouds_far_rq_band(void) { return rq_one_now() ? 0.0f : F.rq_band; }   /* (rq8_onelayer, PS2_RQ_CLOUDS_ONE: no near march, no cross-fade band) */
#endif

int ps2_clouds_far_record(const ps2_cloud_gpu *g, VkCommandBuffer cmd, const ps2_cloud_rec *r) {
    (void)g;
    if (!F.far_ready) return 0;
    const ps2_cloud_env *e = r->env;
    const ps2_cloud_view *v = r->view;
    const ps2_cloud_cfg *cfg = ps2_clouds_cfg();
    int fi = r->frame_index;
    if (fi < 0 || fi >= (int)FAR_FIF) return 0;
#if PS2_RQ_SUPPORT
    if (F.rq) return rq_far_record(g, cmd, r);
#endif
    uint8_t *slot = F.host + (size_t)HOST_SLOT * (uint32_t)fi;
    if (F.mq && F.mq_phase[fi] >= 0) {  /* this slot's previous far update (its fence has passed): ACZ_VR_GOV_FAR_AB */
        uint64_t ts[2];
        int ph = F.mq_phase[fi], n = far_ab();
        if (F.mq_frame[fi] % (uint32_t)n >= (uint32_t)n / 4u
            && vkGetQueryPoolResults(F.dev, F.mq, 2u * (uint32_t)fi, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            F.mq_ms[ph] += (double)(ts[1] - ts[0]) * F.wq_ns * 1e-6; F.mq_n[ph]++;
            if (F.debug) F.mq_tiles[ph] += ((const uint32_t *)(slot + HOST_COUNTS))[3];
        }
        F.mq_phase[fi] = -1;
        if (F.mq_n[0] >= 600u && F.mq_n[1] >= 600u && F.mq_n[2] >= 600u) {
            ps2_log("clouds far A/B: far update %.3f ms (level 0) / %.3f ms (level 1 relaxed) / %.3f ms (level 1 capped) GPU, "
                    "own timestamps over %u / %u / %u frames%s; tiles a frame %.0f / %.0f / %.0f", F.mq_ms[0] / F.mq_n[0],
                    F.mq_ms[1] / F.mq_n[1], F.mq_ms[2] / F.mq_n[2], F.mq_n[0], F.mq_n[1], F.mq_n[2],
                    F.debug ? "" : " (tiles need ACZ_DEBUG_CLOUDS)", (double)F.mq_tiles[0] / F.mq_n[0],
                    (double)F.mq_tiles[1] / F.mq_n[1], (double)F.mq_tiles[2] / F.mq_n[2]);
            memset(F.mq_ms, 0, sizeof F.mq_ms); memset(F.mq_n, 0, sizeof F.mq_n); memset(F.mq_tiles, 0, sizeof F.mq_tiles);
        }
    }
    if (F.debug && F.frames > 0 && F.frames % (uint32_t)F.debug == 0) {
        const uint32_t *cnt = (const uint32_t *)(slot + HOST_COUNTS);     /* this slot's frame, FAR_FIF frames ago */
        ps2_log("clouds far: tiles marched %u of %u (must %u, view %u, outside %u)", cnt[3], F.nt, cnt[0], cnt[1], cnt[2]);
    }
    if (F.stale > 0 && F.st_used[fi]) {     /* ACZ_DEBUG_CLOUD_FAR_STALE: this slot's frame (FAR_FIF frames ago) */
        const uint32_t *c = (const uint32_t *)(slot + HOST_STALE);
        for (int k = 0; k < 6; k++) F.st_sum[k] += c[k];
        F.st_frames++;
        uint32_t pop = c[0] + c[1];
        if (pop > F.st_worst) { F.st_worst = pop; F.st_worst_field = F.st_field[fi]; }
        if (pop >= 4u)
            ps2_log("clouds far stale: field %u: %u visible tiles left alone show a cloud appear / vanish against a march "
                    "now (%u of them relaxed by the near fog), %u + %u step > 16 levels, of %u checked (%u relaxed)",
                    F.st_field[fi], pop, c[0], c[2], c[3], c[4], c[5]);
        if (F.st_frames % (uint64_t)F.stale == 0u)
            ps2_log("clouds far stale: %llu frames: per frame %.2f tiles appear / vanish (%.2f relaxed by the near fog, %.2f "
                    "not), %.2f / %.2f step > 16 levels, %.1f checked (%.1f relaxed); worst %llu tiles at field %u",
                    (unsigned long long)F.st_frames, (double)(F.st_sum[0] + F.st_sum[1]) / (double)F.st_frames,
                    (double)F.st_sum[0] / (double)F.st_frames, (double)F.st_sum[1] / (double)F.st_frames,
                    (double)F.st_sum[2] / (double)F.st_frames, (double)F.st_sum[3] / (double)F.st_frames,
                    (double)F.st_sum[4] / (double)F.st_frames, (double)F.st_sum[5] / (double)F.st_frames,
                    (unsigned long long)F.st_worst, F.st_worst_field);
        F.st_used[fi] = 0;
    }
    /* the scheduler's anchor: tiles store eye positions relative to it; a new anchor = a new generation (all tiles
       re-marched, spread by the cap if one is set) */
    double dx = v->cloud_origin_m[0] - F.anchor[0], dy = v->cloud_origin_m[1] - F.anchor[1], dz = v->cloud_origin_m[2] - F.anchor[2];
    if (!F.have_anchor || F.rebase != v->rebase || dx * dx + dz * dz > 20000.0 * 20000.0) {
        if (F.debug > 0 && F.have_anchor) ps2_log("clouds far: new generation (re-anchor%s): every tile re-marched", F.rebase != v->rebase ? ", rebase" : "");
        memcpy(F.anchor, v->cloud_origin_m, sizeof F.anchor); F.have_anchor = 1; F.rebase = v->rebase; F.gen++;
        far_tr_flags[fi] |= 4u;
        dx = dy = dz = 0.0;
    }
    fill_params(fi, e, v);
    int list = list_path(e);
    far_push pc; memset(&pc, 0, sizeof pc);
    pc.cam[0] = (float)dx; pc.cam[1] = (float)dy; pc.cam[2] = (float)dz;
    pc.cam[3] = F.start_m >= 0.0f ? F.start_m : cfg->near_on ? cfg->near_range_m : 300.0f;
    pc.march[0] = list ? list_range(e) : F.max_m; pc.march[1] = F.dt_min; pc.march[2] = F.k_step;
    pc.march[3] = v->proj[0] > 0.0f ? v->proj[0] : 745.0f;           /* eye pixels per radian at the view centre */
    /* the periodic refresh is for content changes the translation error does not see (list, fades, band, colours):
       FAR_SLICE for a second after the weather changed, ACZ_VR_CLOUD_FAR_QUIET x slower while it stays the same */
    float slice = (float)cfg->far_slice * (F.wx_quiet < 120u ? 1.0f : F.quiet_k);
    if (F.wx_quiet < 0xFFFFFFu) F.wx_quiet++;
    /* the renderer's frame-time governor (ps2_clouds_far_gov), ACZ_VR_GOV_FAR_MODE: 1 = every tile's error limit
       x ACZ_VR_GOV_FAR_ERR and the periodic refresh half as often, ACZ_VR_GOV_FAR_CAP tiles a frame only of those outside
       both eyes' views (PASS_ARGS, ext.w); 0 = at most ACZ_VR_GOV_FAR_CAP tiles a frame of all lists (the must list
       first, then the visible, then the rest; what is left stays due) and the refresh half as often */
    int gcap = far_gov_cap();
    uint32_t cap = F.cap;
    int gon, grelax;
    far_gov_state(F.frames, &gon, &grelax);
    far_tr_flags[fi] = (uint8_t)((far_tr_flags[fi] & 0x0Fu) | (uint32_t)(gon ? (grelax ? 2 : 3) : 1) << 4);
    if (gon && gcap > 0 && (!cap || cap > (uint32_t)gcap)) cap = (uint32_t)gcap;
    if (gon) slice *= 2.0f;
    pc.sched[0] = gon && grelax ? F.err_px * far_gov_err() : F.err_px; pc.sched[1] = slice; pc.sched[2] = (float)cap;
    pc.sched[3] = cfg->near_on && F.start_m < 0.0f ? 1.0f : 0.0f;    /* the near field covers 0 .. start */
    uint32_t ne = F.ubo_asked && F.n_ubo <= F.n && F.n_ubo >= 64u ? F.n_ubo : F.n;   /* = this pass's sizes.x */
    if (ne != F.n_used) {                                          /* a new used size: re-march every tile */
        if (F.debug > 0 && F.n_used) ps2_log("clouds far: new generation (used size %u -> %u): every tile re-marched", F.n_used, ne);
        F.n_used = ne; F.gen++; far_tr_flags[fi] |= 4u;
    }
    uint32_t ncap = overcast(e) && F.ovc_steps > 0 ? (uint32_t)F.ovc_steps : 0u;
    uint32_t tpr = ne / 8u, nt = tpr * tpr;
    pc.fr[0] = F.frames; pc.fr[1] = ne; pc.fr[2] = tpr; pc.fr[3] = (ncap & 0xFFu) << 1 | (F.gen & 0x7FFFFFu) << 9;
    VkDescriptorSet s = F.set[fi];
    /* the previous frame's march (and the debug copy) read the counters; this frame's fill writes them */
    barrier2(cmd, CS | XS, RW | VK_ACCESS_2_TRANSFER_READ_BIT, XS, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (!F.cleared) { vkCmdFillBuffer(cmd, F.tiles, 0, VK_WHOLE_SIZE, 0); F.cleared = 1; }   /* generation 0 = never */
    vkCmdFillBuffer(cmd, F.list, 0, 32, 0);
    if (F.stale > 0) vkCmdFillBuffer(cmd, F.list, 32u + (VkDeviceSize)3u * nt * 4u, 32, 0);   /* (the stale counters) */
    barrier2(cmd, XS, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, RW);
    ps2_clouds_bind_common(cmd, F.layout);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, F.layout, 1, 1, &s, 0, NULL);
    vkCmdPushConstants(cmd, F.layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    if (F.mq) {
        vkCmdResetQueryPool(cmd, F.mq, 2u * (uint32_t)fi, 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, F.mq, 2u * (uint32_t)fi);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, F.p_tiles);
    vkCmdDispatch(cmd, (nt + 63u) / 64u, 1, 1);
    barrier2(cmd, CS, RW, CS, RW);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, F.p_args);
    vkCmdDispatch(cmd, 1, 1, 1);
    barrier2(cmd, CS, VK_ACCESS_2_SHADER_WRITE_BIT, CS | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
             VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, F.p_march);
    vkCmdDispatchIndirect(cmd, F.list, 16);
    far_tr_flags[fi] |= 8u;
    if (F.mq) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, F.mq, 2u * (uint32_t)fi + 1u);
        F.mq_phase[fi] = (int8_t)far_ab_phase(F.frames); F.mq_frame[fi] = F.frames;
    }
    if (F.debug || far_trace_on()) {          /* the counters into this slot (read FAR_FIF frames later) */
        barrier2(cmd, CS, VK_ACCESS_2_SHADER_WRITE_BIT, XS, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferCopy bc; bc.srcOffset = 0; bc.dstOffset = (VkDeviceSize)HOST_SLOT * (uint32_t)fi + HOST_COUNTS; bc.size = 16;
        vkCmdCopyBuffer(cmd, F.list, F.host_buf, 1, &bc);
    }
    if (F.stale > 0) {                        /* ACZ_DEBUG_CLOUD_FAR_STALE: the counters into this slot */
        barrier2(cmd, CS, VK_ACCESS_2_SHADER_WRITE_BIT, XS, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferCopy bc; bc.srcOffset = 32u + (VkDeviceSize)3u * nt * 4u;
        bc.dstOffset = (VkDeviceSize)HOST_SLOT * (uint32_t)fi + HOST_STALE; bc.size = 32;
        vkCmdCopyBuffer(cmd, F.list, F.host_buf, 1, &bc);
        F.st_used[fi] = 1; F.st_nt[fi] = nt; F.st_field[fi] = (uint32_t)ps2_kernel_vblank_count();
    }
    F.frames++;
    return 1;
}

/* PS2_GPU_TRACE: frame slot fi's far record (its fence has passed): out[0..3] = the tile lists must / visible /
   outside and the tiles marched, out[4] = the flags (far_tr_flags); the flags are cleared for the slot's next frame */
int ps2_clouds_far_trace(int fi, uint32_t out[5]) {
    memset(out, 0, 5 * sizeof out[0]);
    if (!F.ready || fi < 0 || fi >= (int)FAR_FIF) return 0;
    out[4] = far_tr_flags[fi];
    if (far_tr_flags[fi] & 8u) memcpy(out, F.host + (size_t)HOST_SLOT * (uint32_t)fi + HOST_COUNTS, 4 * sizeof out[0]);
    far_tr_flags[fi] = 0;
    return 1;
}
/* the renderer's frame-time governor (ps2_video.c hgov, level 1): fewer far tiles re-marched a frame while on, and the
   cloud shadow map rebuilt in half slices (twice as long; invisible) */
void ps2_clouds_far_gov(int on) { far_gov_on = on != 0; ps2_clouds_shadow_gov(on); }
void ps2_clouds_far_shutdown(const ps2_cloud_gpu *g) {
    VkDevice dev = g->dev;
    if (!F.ready) return;
    VkPipeline ps[5] = { F.p_weather, F.p_wsoft, F.p_tiles, F.p_args, F.p_march };
    for (int i = 0; i < 5; i++) if (ps[i]) vkDestroyPipeline(dev, ps[i], NULL);
#if PS2_RQ_SUPPORT
    VkPipeline prq[5] = { F.p_tiles_rq, F.p_args_rq, F.p_march_rq, F.p_copy_rq, F.p_weather_rq };
    for (int i = 0; i < 5; i++) if (prq[i]) vkDestroyPipeline(dev, prq[i], NULL);
    if (F.rq_draw) { vkDestroyBuffer(dev, F.rq_draw, NULL); vkFreeMemory(dev, F.rq_draw_mem, NULL); }
    for (int k = 0; k < 2; k++) {   /* (clouds7: the shown map) */
        if (F.sh_view[k]) vkDestroyImageView(dev, F.sh_view[k], NULL);
        if (F.sh_img[k]) vkDestroyImage(dev, F.sh_img[k], NULL);
        if (F.sh_mem[k]) vkFreeMemory(dev, F.sh_mem[k], NULL);
    }
    if (F.p_resolve_rq) vkDestroyPipeline(dev, F.p_resolve_rq, NULL);
    if (F.rq_sweep) { vkDestroyBuffer(dev, F.rq_sweep, NULL); vkFreeMemory(dev, F.rq_sweep_mem, NULL); }
    if (F.rq_stage) { vkDestroyBuffer(dev, F.rq_stage, NULL); vkFreeMemory(dev, F.rq_stage_mem, NULL); }
    if (F.rq_tq) vkDestroyQueryPool(dev, F.rq_tq, NULL);
    {   /* (rq7_bake) */
        VkPipeline pb[5] = { BK.p_d, BK.p_l, BK.p_df, BK.p_march, BK.p_march_cnt };
        for (int i = 0; i < 5; i++) if (pb[i]) vkDestroyPipeline(dev, pb[i], NULL);
        for (int k = 0; k < BK_NIMG; k++) {
            if (BK.view[k]) vkDestroyImageView(dev, BK.view[k], NULL);
            if (BK.img[k]) vkDestroyImage(dev, BK.img[k], NULL);
            if (BK.mem[k]) vkFreeMemory(dev, BK.mem[k], NULL);
        }
        if (BK.s_lin) vkDestroySampler(dev, BK.s_lin, NULL);
        if (BK.s_near) vkDestroySampler(dev, BK.s_near, NULL);
        if (BK.q) vkDestroyQueryPool(dev, BK.q, NULL);
        memset(&BK, 0, sizeof BK);
    }
#endif
    for (int i = 0; i < F.nlayouts; i++) if (F.layouts[i]) vkDestroyPipelineLayout(dev, F.layouts[i], NULL);
    if (F.wq) vkDestroyQueryPool(dev, F.wq, NULL);
    if (F.mq) vkDestroyQueryPool(dev, F.mq, NULL);
    if (F.pool) vkDestroyDescriptorPool(dev, F.pool, NULL);
    if (F.set_layout) vkDestroyDescriptorSetLayout(dev, F.set_layout, NULL);
    if (F.host_buf) { vkDestroyBuffer(dev, F.host_buf, NULL); vkFreeMemory(dev, F.host_mem, NULL); }
    if (F.tiles) { vkDestroyBuffer(dev, F.tiles, NULL); vkFreeMemory(dev, F.tiles_mem, NULL); }
    if (F.list) { vkDestroyBuffer(dev, F.list, NULL); vkFreeMemory(dev, F.list_mem, NULL); }
    if (F.wxc) { vkDestroyBuffer(dev, F.wxc, NULL); vkFreeMemory(dev, F.wxc_mem, NULL); }
    memset(&F, 0, sizeof F);
}

#if PS2_RQ_SUPPORT
/* [debug] ACZ_DEBUG_CLOUD_LIST=1 (agent clouds6 2026-11-11, A.5 item 5 of PROMPT_2026-11-11_flights2_fixes.md): every entry of a
   cloud into the game's published list ('+', fading in), every exit ('-', fading out) and every re-entry of a cloud still fading
   out ('r'), one line each: the field; the distance from the eye to its centre (horizontal and slant) and to its body's surface
   (0.9 x its semi-axes, < 0 inside); the bearing of its centre right of the view's forward ("view", the head-turned camera) and
   right of the aircraft's nose ("nose", the game camera without the head, ps2_xr_camera_base_of), both in the horizontal plane,
   + = right; its elevation; whether its centre lies inside the view's image ("in") and the angle of the body's radius; R / H /
   alt; the published opacity (exits: the last one); the list's size and C1's fog far (the game's far fade ends there). A list
   culled by the view would show its entries and exits at the view's edge (|view| near the half field of view), one culled by the
   aircraft's yaw at a fixed |nose|, one culled by distance at a fixed distance whatever the bearing. Every 1200 fields a summary
   line: entries / exits, how many with the centre in view, nearer than 3 km (surface), ahead of the nose (|nose| < 60). */
void ps2_clouds_far_list_note(const void *trk, const ps2_cloud_view *v, const ps2_cloud_env *e, float op, char sign) {
    const far_track *t = (const far_track *)trk;
    extern int ps2_xr_camera_base_of(uint32_t seq, float eye[3], float rot[9]);
    double dx = t->x - v->cloud_origin_m[0], dz = t->z - v->cloud_origin_m[2], dy = (double)t->y - v->cloud_origin_m[1];
    double dh = sqrt(dx * dx + dz * dz), d3 = sqrt(dh * dh + dy * dy), s = trk_surface(t, v);
    /* the view's heading in the horizontal plane: its forward, or (pitched beyond ~75 deg) the image's bottom / top edge's
       direction, which then points where the face was turned */
    double fx = -v->cam_z[0], fz = -v->cam_z[2];
    if (fx * fx + fz * fz < 0.0625) { double k = v->cam_z[1] < 0.0f ? -1.0 : 1.0; fx = k * v->cam_y[0]; fz = k * v->cam_y[2]; }
    double bv = atan2(dx * -fz + dz * fx, dx * fx + dz * fz) * 57.29578;   /* (+ = right: right of forward (fx, fz) is (-fz, fx)) */
    double bn = 999.0;
    float be[3], R[9];
    if (ps2_xr_camera_base_of(v->seq, be, R)) {                    /* (world = R view: the base camera's back axis (R[2], R[5], R[8])) */
        double nx = -R[2], nz = -R[8];
        if (nx * nx + nz * nz < 0.0625) { double k = R[5] < 0.0f ? -1.0 : 1.0; nx = k * R[1]; nz = k * R[7]; }
        bn = atan2(dx * -nz + dz * nx, dx * nx + dz * nz) * 57.29578;
    }
    double vx = dx * v->cam_x[0] + dy * v->cam_x[1] + dz * v->cam_x[2], vy = dx * v->cam_y[0] + dy * v->cam_y[1] + dz * v->cam_y[2];
    double vz = -(dx * v->cam_z[0] + dy * v->cam_z[1] + dz * v->cam_z[2]);
    int in = vz > 1.0 && vx / vz >= v->tan_l && vx / vz <= v->tan_r && vy / vz <= v->tan_u && vy / vz >= v->tan_d;
    double dd = d3 > 1.0 ? d3 : 1.0, ca = vz / dd;
    double off = acos(ca < -1.0 ? -1.0 : ca > 1.0 ? 1.0 : ca) * 57.29578;
    double ar = atan(0.9 * t->r / dd) * 57.29578;
    /* (clouds7) box: the distance to the body's box (0.9 x its semi-axes; never more than its true surface distance: the static
       list's reach test, zero_clouds.c) -- the ellipsoid estimate "surface" reads low for flat bodies; cloud far: the game's */
    double gh = dh - 0.9 * t->r, gv = fabs(dy) - 0.9 * (t->h > 0.0f ? t->h : t->r);
    gh = gh > 0.0 ? gh : 0.0; gv = gv > 0.0 ? gv : 0.0;
    ps2_log("clouds list: %c field %llu d %.0f m (slant %.0f, surface %.0f, box %.0f) view %+.0f nose %+.0f el %+.0f deg; in %d (off-axis %.0f, "
            "body radius %.0f deg); R %.0f H %.0f alt %.0f op %.3f; published %u, fog far %.0f m, cloud far %.0f m%s", sign,
            (unsigned long long)ps2_kernel_vblank_count(), dh, d3, s, sqrt(gh * gh + gv * gv), bv, bn, atan2(dy, dh) * 57.29578, in, off, ar, (double)t->r,
            (double)t->h, (double)t->y, (double)op, e->ncloud, (double)e->fog_far_m, (double)e->cloud_far_m, e->list_static ? ", static" : "");
    int k = sign == '-' ? 1 : 0;
    F.ls_n[k]++; F.ls_view[k] += (uint32_t)in; F.ls_near[k] += s < 3000.0 ? 1u : 0u; F.ls_ahead[k] += bn < 60.0 && bn > -60.0 ? 1u : 0u;
}
void ps2_clouds_far_list_summary(void) {
    uint64_t f = ps2_kernel_vblank_count();
    if (f < F.ls_last + 1200u) return;
    if (F.ls_last)
        ps2_log("clouds list: fields %llu-%llu: %u entries (centre in view %u, surface nearer than 3 km %u, ahead of the nose %u), %u exits "
                "(in view %u, nearer than 3 km %u, ahead %u) (ACZ_DEBUG_CLOUD_LIST)", (unsigned long long)F.ls_last, (unsigned long long)f,
                F.ls_n[0], F.ls_view[0], F.ls_near[0], F.ls_ahead[0], F.ls_n[1], F.ls_view[1], F.ls_near[1], F.ls_ahead[1]);
    F.ls_last = f;
    memset(F.ls_n, 0, sizeof F.ls_n); memset(F.ls_view, 0, sizeof F.ls_view); memset(F.ls_near, 0, sizeof F.ls_near);
    memset(F.ls_ahead, 0, sizeof F.ls_ahead);
}
#endif
