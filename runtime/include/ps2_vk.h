#ifndef PS2_VK_H
#define PS2_VK_H

#include "ps2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps2_vk_vertex {
    float x, y, z;
    float r, g, b, a;
    float s, t, q;
    float fog;
    u32 round_uv;
    float xr_dx;     /* synchronous stereo: right-eye x shift in PS2 pixels (left eye uses -xr_dx); not read by
                        shaders, the renderer applies it per eye pass. 0 for 2D/HUD and off-screen targets. */
    float xr_iw;     /* 1 / view depth (game units) of a world vertex, for reprojecting a frame to another camera
                        (frames between game frames); 0 = at infinity, < 0 = never reprojected (HUD, 2D, cockpit) */
} ps2_vk_vertex;

#define PS2_VK_RUV_SPRITE (1u << 29)

typedef struct ps2_vk_state {
    u32 fb_w, fb_h;
    u32 fb_psm;
    u32 fb_mask;
    int shuffle_rg;
    u32 shuffle_alpha;
    s32 scissor[4];

    u32 ztst;
    int zwrite;

    int abe;
    u32 alpha_a, alpha_b, alpha_c, alpha_d, alpha_fix;

    int tme;
    u32 tex_index;
    float tex_w, tex_h;
    u32 tfx;
    int tcc;

    u32 rt;
    u32 tex_rt;
    int tex_depth;
    int tex_self;
    int fst;
    u32 tex_lod;
    int tex_point;

    u32 wms, wmt;
    u32 minu, maxu, minv, maxv;

    int ate;
    u32 atst, aref;
    u32 afail;
    int zte;

    int fge;
    u32 fogcol;
    int fba;
    int date, datm;
    u32 zrt;
    int zcopy;
    int colclip;
    u32 emitter;
    int native_region;
} ps2_vk_state;

enum { PS2_VK_POINTS = 0, PS2_VK_LINES = 1, PS2_VK_TRIANGLES = 2 };

int  ps2_vk_enabled(void);
int  ps2_vk_closed(void);

void ps2_vk_draw(int topology, const ps2_vk_state *st,
                 const ps2_vk_vertex *v, int n);

enum { PS2_VK_NATIVE_NONE = 0, PS2_VK_NATIVE_SUN_FLARE = 1,
       PS2_VK_NATIVE_SUN_GHOST = 2,
       PS2_VK_NATIVE_2D = 3 };

void ps2_vk_draw_2d(int topology, const ps2_vk_state *st,
                    const ps2_vk_vertex *v, int n);
typedef struct ps2_vk_native {
    u32 pass;
    u32 rt;
    u32 zrt;
    u32 tex;
    float tex_w, tex_h;
    int tex_point;
    u32 wrap;
    float box[4];
    float sun_z;
    s32 scissor[4];
} ps2_vk_native;

void ps2_vk_native_draw(const ps2_vk_native *nd, const ps2_vk_vertex *v, int n);

typedef struct ps2_vk_mesh_block {
    float m[16];
    float c[16];
    float l[12];
    float r[16];
    float e[12];
    float fog0, fog1;
    float zscale, zmax;
    float xoff, yoff;
    float spec_alpha;
    float xr_k;        /* stereo (set by the renderer): eye shift = -xr_k / max(w, xr_wmin) PS2 pixels; 0 = none */
    u32 mode;
    u32 pass;
    u32 clip;
    u32 pad1;
    u32 prog;
    float xr_wmin;
    u32 xr_reproj;     /* frames between game frames: 0 never (cockpit, 2D), 1 as static world (camera
                          extrapolation), 2 own motion (m extrapolated from mp, the same object one game frame earlier) */
    u32 pad4;
    float p[16];
    float mp[16];      /* xr_reproj 2: the object's m in the previous game frame */
} ps2_vk_mesh_block;

enum { PS2_VK_NATIVE_MESH = 4 };

/* The rear-view mirrors' live map (ps2_video.c mlv_*, ACZ_VR_COCKPIT_MIRRORS_LIVE): the graphics worker's side
   (rn_vp_model.c mlv_batch). The rear camera of the game frame being recorded: head view -> rear view (rows of T,
   translation t, game units), the game frame's projection (hf, hc: x = hc0 + hf0 X / -Z, y = hc1 - hf1 Y / -Z, GS
   pixels) and the map's (rf, rc, map pixels), the rear frustum's planes (n.v + d >= 0 inside, rear view), the range,
   the coarse test's radius, the own aircraft's margin and the near plane (game units); identity 1 / 2: the test modes
   (the head camera itself; 2 = the rows as they come). NULL: no map for this game frame. */
typedef struct ps2_vk_mlv_cam {
    float T[9], t[3];
    float hf[2], hc[2], rf[2], rc[2];
    float pl[5][4];
    float range, rmax, own, near_u;
    float wR[9], we[3];        /* the head camera in the world (world = wR view + we, game units): the height test */
    float ground, agl;         /* ground objects (nearer than agl to the ground, units): their range (0 = left out) */
    int identity;
    u32 seq;                   /* the game frame */
    u32 gmax;                  /* ground objects' strips a frame at most */
} ps2_vk_mlv_cam;
const ps2_vk_mlv_cam *ps2_vk_mlv_cam_now(void);
int  ps2_vk_mlv_room(u32 n_verts, u32 n_indices, u32 n_draws, int same_object);
void ps2_vk_mlv_mesh(const ps2_vk_state *st, const ps2_vk_mesh_block *b, const u8 *recs, u32 n_verts,
                     const u32 *indices, u32 n_indices, int cull, int pass, int same);
int  ps2_gs_native_state_ad(const u64 *ad, u32 n, u32 prim, ps2_vk_state *st, float *xoff, float *yoff, float *zmax);

int  ps2_vk_mesh_available(void);
int  ps2_vk_mesh_material_ok(const ps2_vk_state *st);
int  ps2_vk_mesh_store(const u8 *records, u32 n_verts, const u32 *indices,
                       u32 n_indices, u32 *vbase, u32 *ifirst);
int  ps2_vk_mesh_room(u32 n_verts, u32 n_indices, u32 n_draws);
enum { PS2_VK_MESH_CULL = 1, PS2_VK_MESH_GSFRAG = 4 };
void ps2_vk_draw_mesh(const ps2_vk_state *st, const ps2_vk_mesh_block *b,
                      u32 vbase, u32 ifirst, u32 n_indices, int cull);
void ps2_vk_draw_mesh_topo(const ps2_vk_state *st, const ps2_vk_mesh_block *b,
                           u32 vbase, u32 ifirst, u32 n_indices, int cull,
                           int topology);
extern void (*ps2_vk_before_record)(void);

u32  ps2_vk_texture(u64 key, u32 hash, const u8 *rgba, u32 w, u32 h);
u32  ps2_vk_texture_scaled(u64 key, u32 hash, const u8 *rgba, u32 w, u32 h, u32 scale);
u32  ps2_vk_texture_scale(u32 idx);
int  ps2_vk_texture_is(u32 idx, u64 key, u32 hash);

u32  ps2_vk_texture_native(u32 src, u32 x, u32 y, u32 w, u32 h);
u32  ps2_vk_texture_native_find(u64 key, u32 hash, u32 w, u32 h);
u32  ps2_vk_texture_native_rgba(u64 key, u32 hash, const u8 *rgba, u32 w, u32 h);
const u8 *ps2_vk_texture_pixels(u32 idx);
void ps2_vk_texture_size(u32 idx, u32 *w, u32 *h);

void ps2_vk_invalidate(u32 base, u32 size);

#define PS2_VK_PRESENT_BLANK 0xFFFFFFFFu
int  ps2_vk_present(u32 disp_x, u32 disp_y, u32 disp_w, u32 disp_h, u32 rt);

extern int ps2_vk_lockstep;

int  ps2_vk_screenshot(const char *path);

extern float ps2_vk_display_aspect;

void ps2_vk_stats(u64 *frames, u64 *draws, u64 *verts, u64 *textures);

/* Enclosure (tunnel_light 2026-11-02, bug #267; ps2_video.c): what of the sun and the sky reaches the eye, from a game
   module's rays through its own world (Zero: zero_vr.c, the game's segment test). The game module publishes the raw
   values once a game frame; the renderer smooths them in display time and its sun- and sky-lit effects follow. */
int   ps2_vk_enclosure_wanted(void);                 /* ACZ_VR_TUNNEL_LIGHT > 0: worth casting the rays */
void  ps2_vk_enclosure_set(float sun_vis, float sky_open, const float eye_units[3], float units_per_m);
float ps2_vk_enclosure_sun(void);                    /* the sun at the eye: 1 open .. 0 hidden (smoothed, x strength) */
float ps2_vk_enclosure_closed(void);                 /* 0 open sky .. 1 enclosed (a tunnel, a roof) */
float ps2_vk_enclosure_sun_at(const float w_units[3]);    /* the same near the eye, 1 from ~600 m away */
float ps2_vk_enclosure_closed_at(const float w_units[3]);

#ifdef __cplusplus
}
#endif

/* the interior cube's extra usage in ps2_video.c gfx_refl_init and the hook after its bake (each on the line it extends, so the PC
   object keeps its line numbers): the Quest eye pass's readable glass reads the cube back once (rq/rq_glass.inc rqgx_cube_read);
   nothing without PS2_RQ_SUPPORT */
#if PS2_RQ_SUPPORT
#define PS2_RQ_CUBE_USAGE VK_IMAGE_USAGE_TRANSFER_SRC_BIT
void ps2_rq_glass_cube_read(void *model);          /* (right after the bake, on the cockpit loader's thread: the model being loaded) */
#define PS2_RQ_REFL_HOOK() ps2_rq_glass_cube_read(&cpit)
/* (R-85, ACZ_VR_RQ_COCKPIT_SELFLIT_MESH: the loader's mesh, its file and its 32-byte name; rq/rq_cockpit.inc rqsl_mark) */
void ps2_rq_selflit_mark(void *mesh, const char *path, const char *name32, unsigned idx);
#define PS2_RQ_SELFLIT_HOOK(m, path, hdr, i) ps2_rq_selflit_mark((m), (path), (hdr), (i))
/* (rq21_screens 2026-11-16, BUGS_RQ R-280, ACZ_VR_MFD_FRONT / _MODEL: the loader's image after the LOD side file's check value -- a listed
   model's named parts become display screens; ps2_video.c cpit_load_one on cpit_lod_prehash's line, rq/rq_cockpit.inc) */
void ps2_rq_screens_assign(unsigned char *d, size_t n, const char *path);
/* (rq10_artefacts 2026-11-15, [test] PS2_XR_DUMP_AT="a[-b[/s]],...": eye dumps at a list of fields and ranges instead of
   PS2_XR_DUMP_EYES / _COUNT / _EVERY; ps2_video.c eyedump_want and lw_dump_soon, each hook on the line it extends; rq/rq_video.inc
   ps2_rq_dumpat_next: the next listed field after `after`, -1 without the list, LONG_MAX when none is left) */
long ps2_rq_dumpat_next(long after);
extern long ps2_rq_dumpat_last;
/* (rq17_menus 2026-11-16, from rq3_phases: ACZ_TEST_PAD_ON's DUMP, zero_vr.c, sets ps2_vk_eyedump_req: one eye dump at the next frame
   drawn for a game frame, whatever PS2_XR_DUMP_EYES / _AT say -- a dump per game phase on a device whose phases come at other fields) */
extern volatile int ps2_vk_eyedump_req;
#define PS2_RQ_DUMPAT_WANT(fc) { if (ps2_vk_eyedump_req && !eyedump_seqk && !eyedump_pair && !eyedump_pending && !xr_rerender) { \
    ps2_vk_eyedump_req = 0; return 1; } \
    long nx_ = ps2_rq_dumpat_next(ps2_rq_dumpat_last); if (nx_ >= 0) { if (nx_ == LONG_MAX || (long)(fc) < nx_) return 0; \
    ps2_rq_dumpat_last = (long)(fc); if (eyedump_seqk) { eyedump_seq_i = 0; return 1; } if (!eyedump_pair) eyedump_done++; return 1; } }
#define PS2_RQ_DUMPAT_SOON(fc) { if (ps2_vk_eyedump_req) return 1; long nx_ = ps2_rq_dumpat_next(ps2_rq_dumpat_last); if (nx_ >= 0) { \
    if (eyedump_seq_i >= 0 || eyedump_pair_seq || eyedump_pending) return 1; return nx_ != LONG_MAX && (long)(fc) + 4 >= nx_; } }
/* (rq10_artefacts, PS2_RQ_CLOUDS_SUN_T: ps2_video.c render_list, after the in-cloud fade of the sun's class-5 draws; rq/rq_fxfade.inc) */
#define PS2_RQ_SUN_FADE(fr, ca, hk) rqsun_fade((fr), (ca), (hk));
/* (rq10_artefacts, PS2_RQ_GSHADOW_FILTER: ps2_video.c gsh_frame, the ground shadow's filter reach into the block's c[8]; rq/rq_clouds.inc) */
#define PS2_RQ_GSH_HOOK(gb) { extern float ps2_rq_gsh_filter_m(void); (gb).c[8] = ps2_rq_gsh_filter_m(); }
/* (rq13_cut, PS2_RQ_CLOUDS_CUT: ps2_video.c's mesh set -- two more layout bindings (9 the far map, 10 its profile) only with the switch, their
   stand-in at the set's allocation (the relief), the pool's room, and the far map's views at a frame slot's start; rq/rq_clouds.inc) */
#define PS2_RQ_MSET_EXTRA 2u
#define PS2_RQ_MSET_BIND(mb, cnt) { extern void ps2_rq_mset_bind(VkDescriptorSetLayoutBinding *mb_, uint32_t *count_); ps2_rq_mset_bind((mb), &(cnt)); }
#define PS2_RQ_MSET_INIT(w, smp, view) { extern int ps2_rq_mset_extra(void); if (ps2_rq_mset_extra()) { VkDescriptorImageInfo ri_; ri_.sampler = (smp); \
    ri_.imageView = (view); ri_.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; VkWriteDescriptorSet w_ = (w); w_.pImageInfo = &ri_; \
    w_.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w_.pBufferInfo = NULL; w_.dstBinding = 9; vkUpdateDescriptorSets(dev, 1, &w_, 0, NULL); \
    w_.dstBinding = 10; vkUpdateDescriptorSets(dev, 1, &w_, 0, NULL); } }
#define PS2_RQ_FRAME_BEGIN(fr) { extern void ps2_rq_cut_frame(VkDescriptorSet set_, int fi_); ps2_rq_cut_frame((fr)->mset, (int)((fr) - frames)); }
/* (rq13_cut round 4, BUGS_RQ R-123, PS2_RQ_RTFIT_1X: ps2_video.c create_targets, after each slot's extent for the new size -- the new scale,
   which rtz_resize_extent cannot see yet (rt_scale is set after the loop); rq/rq_video.inc) */
#define PS2_RQ_RTZ_NEW_SCALE(s, ew, eh) { extern void ps2_rq_rtz_new_scale(unsigned s_, unsigned short *ew_, unsigned short *eh_); \
    ps2_rq_rtz_new_scale((s), (ew), (eh)); }
/* (rq17_menus 2026-11-16, [test] ACZ_TEST_PAD_ON: ps2_video.c, before ps2_quit_pad on the line it extends: while our ENHANCEMENTS page
   or TOUCH CONTROLS card is up the phase-timed presses reach it (ps2_hle_pad.c keeps them from the game then); nothing without them) */
#define PS2_RQ_PAGE_FEED(st) { extern volatile u16 ps2_pad_test_held; extern volatile int ps2_pad_test_page; \
    extern int ps2_quit_enh_visible(void); extern int ps2_quit_touch_card_visible(void);     ps2_pad_test_page = ps2_quit_enh_visible() || ps2_quit_touch_card_visible(); if (ps2_pad_test_page) (st).buttons |= ps2_pad_test_held; }
/* (rq22_blink 2026-11-17, BUGS_RQ R-290, PS2_RQ_EMPTY_HOLD: ps2_video.c render_list_body -- at its start the list is judged (an empty list
   right after an eye-pass list), and such a list keeps the cockpit screens' last plan (mfd_plan not run); each on the line it extends;
   rq/rq_empty.inc) */
#define PS2_RQ_EMPTY_BEGIN() rq_empty_begin();
#define PS2_RQ_EMPTY_KEEP(main_) (rq_empty_now && (main_))
#else
#define PS2_RQ_CUBE_USAGE 0u
#define PS2_RQ_REFL_HOOK() ((void)0)
#define PS2_RQ_SELFLIT_HOOK(m, path, hdr, i) ((void)0)
#define PS2_RQ_DUMPAT_WANT(fc)
#define PS2_RQ_DUMPAT_SOON(fc)
#define PS2_RQ_SUN_FADE(fr, ca, hk)
#define PS2_RQ_GSH_HOOK(gb)
#define PS2_RQ_MSET_EXTRA 0u
#define PS2_RQ_MSET_BIND(mb, cnt)
#define PS2_RQ_MSET_INIT(w, smp, view)
#define PS2_RQ_FRAME_BEGIN(fr)
#define PS2_RQ_RTZ_NEW_SCALE(s, ew, eh)
#define PS2_RQ_PAGE_FEED(st)
#define PS2_RQ_EMPTY_BEGIN()
#define PS2_RQ_EMPTY_KEEP(main_) 0
#endif

#endif
