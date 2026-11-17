/* zero_clouds.c -- Ace Combat Zero game data for our own clouds (CLOUDS_3D_RESEARCH_2026-10-20 section 8, stream S1):
   the game's world-space cloud list, its coverage map, the stage environment set and the sun, published once per game
   frame into C1 (ps2_cloud_env, ps2_clouds.h). Kept out of zero_vr.c (other streams edit that file). One hook: after
   the sky update 0x1604A0 (a shared site; zero_vr.c has a before-hook there); everything is read from the sky object.

   The game's data (SLUS-21346; the cloud list was found by the cloudfeel agent, the rest by S1's probe runs):
   - sky update 0x1604A0 (a0 = sky object, once per frame in flight). sky +4 = the stage environment set + 0x50 (the
     864-byte ACZGCT set; "file" offsets below count from the set's start); sky +0x10/+0x14 = the precipitation band
     (camera heights: full precipitation below, none above; blended from the blocks *(sky +0x9F8 / +0x9FC)); sky +0xB30
     = the blend between the set's halves A (file +0x50) and B (file +0xF0); sky +0xB8C = precipitation at the camera;
     sky +0xBB0 -> the sky camera (eye at +0x10/+0x14/+0x18); sky +0x240 = the sun object (+0x78 elevation, +0x7C
     azimuth: the set's angles); sky +0x7F0 = the cloud object.
   - environment set, file offsets: +0x50/+0x54 fog near/far (units), +0x58/+0x5C sun elevation/azimuth (rad), +0xD4
     precipitation amount (A; +0x174 B), +0xD8 kind (>= 0.5 snow, else rain), +0x190 the cloud descriptor:
       +0x190 type word (bit 1 = cloud field drawn, bit 0x100 = 2 impostor variants instead of 4), +0x198 a distance,
       +0x1A0/+0x1A4 wind x/z (units/s), +0x1A8 heading (rad), +0x1AC speed, +0x1B0 4 x 4 impostor build parameters,
       +0x1F4 shadow colour, +0x1F8 lit colour (0x00BBGGRR), +0x1FC alpha (128 = opaque), +0x200 u16 cloud count,
       +0x210 the variants, 16 bytes each {base Y, spread, size scale, flag}.
   - cloud object (sky +0x7F0): +0 -> the descriptor, +4 -> the records, +8 -> the coverage map, +0xD0 the frame
     counter (bumped by the sky update), +0xE0/+0xE8 the map offset. The cloud field 0x16A6C0 draws from it: count u16
     desc +0x70 (<= 400), variant v = desc +0x80 + 16 (i & mask), mask = 1 if type & 0x100 else 3; record i (8 bytes):
     s16 x, u16 y (low 12 bits, 0x800 = 0), s16 z, s16 size. Cloud frame (the clouds stand still in it): X0 = 2 x +
     65536, Z0 = 2 z + 65536 (the field places a cloud at ((2 x + offset) & 0x1FFFF) - 0x10000 from its window: half a
     period from 2 x; checked against the game's own positions and coverage samples, M3 field 1500: all 200 clouds),
     repeating every 131072 units in x and z; world = cloud frame + drift, drift = wind * counter / 60.
     Y = v[0] - v[1] ((y & 0xFFF) - 0x800); the screen sprite's half-size S = size * 2304 * v[2] / 206.6 units (B: all
     15 sprites of an M1 frame, 1.3-5.7 km, fit tan = S / forward distance within 1 %; the sprite maps the whole 128 x
     128 impostor). The impostor's alpha (the visible cloud) covers only part of the quad: wide and flat, 50 % of the
     peak at 0.45 S across (0.19 S for build parameter v[-0x60] 0.5) and 0.18 S up/down, 10 % at 0.57 S / 0.27 S
     (11 impostors of M1, M3, M12, M16 in level flight; the content turns with the view's roll). Coverage map: 256 x
     256 bytes, 2048-unit cells, periodic, bilinear (0x16A590) at WORLD (X + obj[0xE0], Z + obj[0xE8]) / 255 (the map is
     geography: clouds drift across it). A cloud's sprite alpha is 128 x coverage x distance fade: skipped nearer than
     4096 units, faded in to 12288, full to far / 2 and gone at far, far = min(*(obj +0xD4) +0x278, 139264) (the
     stage's visibility, = the environment's fog far: M1 51200 = 8533 m, M13 35840 = 5973 m); the game drops a cloud
     whose alpha is below 12 + alpha / 108.
   - The "cloud deck/ceiling planes" (tex 395, drawn by 0x1615E8 through 0x164658) are not cloud layers: a quad of
     half-size sky[+0xB54] * 8192 units at 4096 units from the camera in the sun's direction (the sun object's angles),
     coloured by file +0x30. No altitude in them.

   C1 (ps2_clouds.h): cloud frame = world - drift_m; origin_m = cloud-frame x/z (y 0) of a point re-centred on the
   camera when the camera gets more than 2 km away from it; cloud[].pos_m x/z relative to it (the copy of the list
   nearest to it), y = altitude; the visible body is an ellipsoid with the profile full to 0.45 and gone at 0.9 of
   its semi-axes, which put its half density where the impostor's alpha is 50 %: radius_m = horizontal semi-axis =
   0.667 S fw (fw = min(1, v[-0x60] / 1.2), the variant's build width), height_m = vertical semi-axis = 0.262 S,
   sprite_m = S (the raw sprite half-size); opacity = the coverage map at that copy's world position x the game's far
   fade at the copy's distance from the camera (full to far / 2, 0 at far; the near fade is left to our renderers),
   in steps of 1/64; clouds the game drops are not published (in M13 the game shows none: the covered ones are beyond
   its 5973 m); re-evaluated after 40 m of drift or 100 m of camera travel; list_seq bumps on any change of cloud[].
   base_m/top_m = 10th/90th percentile of Y -/+ k height over the list (k = ACZ_VR_CLOUD_BODY, 0.675 = the profile's
   half density; top_m at least the highest body top Y + 0.9 height, ACZ_VR_CLOUD_BAND_TOP); coverage = 1 - exp(-sum(pi (k radius)^2 opacity) / period^2), each cloud's opacity averaged over the
   map's 4 x 4 copies of the list (coverage_unmasked: without the map); type 0 stratus .. 1 cumulus from the median
   S / 8.85 (1000 m -> 0, 350 m -> 1); transp 0 (the game's alpha only raises its drop threshold); precip = sky +0xB8C;
   in_cloud = each published cloud a smaller ball, radius ACZ_VR_CLOUD_INCLOUD x S (0.20: the size that follows the
   picture best on the cloudcheck fly_m1 climb without firing in clear air; the full visible body fires everywhere),
   full to 0.45 and gone at 0.9 of it, times its opacity, all together 1 - product (1 - each), at the sky camera;
   sun_dir = (-cos e sin a, sin e,
   -cos e cos a) from the sun object (valid with the sun off screen); cov_map = a copy of the coverage map (u8 [z][x]),
   cov_origin_m = this frame's cloud-frame position of sample (0, 0) (world - drift: it moves with the drift).

   Switches: ACZ_VR_CLOUD_DATA=0 / 1 (default on: the cloud feel's in-cloud input, also without ACZ_VR_CLOUDS),
   ACZ_VR_CLOUD_BODY=k (0.675), ACZ_VR_CLOUD_INCLOUD=k (0.20, the in-cloud ball's radius / sprite half-size),
   ACZ_VR_CLOUD_BAND_TOP=0 / 1 (default 1: the band top reaches the highest body top; 0: the 90th percentile),
   ACZ_DEBUG_CLOUD_ENV=N (a C1 summary every N fields; the environment set, the cloud list and the coverage map when
   they change: the input of the mission table config_zero/cloud_missions.json).
   ACZ_VR_CLOUD_STATIC (clouds7 2026-11-12, prompt 8 step 1, R-104; PS2_RQ_SUPPORT builds with PS2_RQ_CLOUDS only; default 0, the
   Quest 1): a STATIC field. The list is published by the place of each body, not by the camera: every copy of the periodic list
   whose body (its box of 0.9 x the semi-axes) comes within the game's cloud far + ACZ_VR_CLOUD_STATIC_MARGIN (1500 m) of the
   camera, with the opacity of the coverage map alone (no far fade by the CENTRE's distance, no drop by distance; under the game's
   coverage threshold c1 ~0.10 the opacity tapers to 0 as cov x smoothstep(0, c1, cov) instead of dropping), up to C1's 400 by the nearest surface. The
   game's far fade moves into the marches: each sample's density x (1 - smoothstep(far / 2, far, its distance)), the game's fog
   colour as before (cl_shade_s); C1 cloud_far_m carries far, list_static says the list is static. A body then enters and leaves
   the list only beyond the far range, where every sample of it is faded out: nothing appears, vanishes or ramps in view.
   ACZ_VR_CLOUD_STATIC_FADE (clouds7 2026-11-12, t11a2, R-116; default 1 with ACZ_VR_CLOUD_STATIC): the game's far fade by the body's
   CENTRE distance stays, as a continuous factor of the opacity (full to far / 2, 0 at far; no threshold, so a body enters and leaves
   at opacity ~0 as its centre crosses far), the per-sample fade in the marches on top. Without it (=0, t11a) the near parts of big,
   far-centred bodies came into the near range at the full coverage opacity -- parts the game never showed (it faded the whole body
   by its centre's distance) -- and the near march marched them: M4 / M10 / M11's lists 7-38 bodies against t10i's 1-17, the near
   pass 3-6x on the device. */
#include "ps2_runtime.h"
#include "ps2_menv.h"
#include "ps2_hook.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#if defined(__has_include)
#if __has_include("ps2_clouds.h")
#include "ps2_clouds.h"
#define ZC_C1 1
#endif
#endif

u64 ps2_kernel_vblank_count(void);

#define ZC_SKY_UPDATE 0x001604A0u          /* sky update: a0 = sky object */
#define ZC_SKY_SUN    0x240u               /* the sun object: +0x78 elevation, +0x7C azimuth */
#define ZC_SKY_CLOUD  0x7F0u               /* the cloud object */
#define ZC_SKY_CAM    0xBB0u               /* -> the sky camera, eye at +0x10 */
#define ZC_MAXC 400
#define ZC_PERIOD 131072.0                 /* the cloud list repeats every 131072 units in x and z */
#define ZC_SPRITE_DIV 206.6f               /* sprite half-size S = size * 2304 * v[2] / 206.6 units */
#define ZC_BODY_W 0.667f                   /* body semi-axes / S: horizontal (x fw), vertical; the profile's half density */
#define ZC_BODY_H 0.262f                   /* (0.675) lands on the impostor's 50 % alpha: 0.45 S across, 0.177 S up/down */
#define ZC_HALF 65536.0                    /* list x/z -> cloud frame: 2 x + half a period (the field's & 0x1FFFF - 0x10000) */

static float zc_f(u32 w) { float f; memcpy(&f, &w, 4); return f; }
static int zc_ok(u32 p) { return p >= 0x100000u && p < 0x2000000u; }
static float zc_rf(u32 a) { return zc_f(ps2_r32(a)); }
static int zc_cmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }
static float zc_map_at(u32 map, float x, float z) {   /* as 0x16A590 (bilinear, 2048-unit cells, 256 x 256, periodic) */
    int ix = (int)x, iz = (int)z;
    float fx = (float)(ix & 0x7FF) * (1.0f / 2048.0f), fz = (float)(iz & 0x7FF) * (1.0f / 2048.0f);
    u32 cx = (u32)((ix + 0x40000) >> 11) & 0xFFu, cz = (u32)((iz + 0x40000) >> 11) & 0xFFu;
    u32 cx1 = (cx + 1u) & 0xFFu, cz1 = (cz + 1u) & 0xFFu;
    float a = (float)ps2_r8(map + cz * 256u + cx), b = (float)ps2_r8(map + cz * 256u + cx1);
    float c = (float)ps2_r8(map + cz1 * 256u + cx), d = (float)ps2_r8(map + cz1 * 256u + cx1);
    return ((a * (1.0f - fx) + b * fx) * (1.0f - fz) + (c * (1.0f - fx) + d * fx) * fz) * (1.0f / 255.0f);
}
static double zc_wrap(double d) {          /* to [-65536, 65536) */
    d = fmod(d + 0.5 * ZC_PERIOD, ZC_PERIOD);
    if (d < 0.0) d += ZC_PERIOD;
    return d - 0.5 * ZC_PERIOD;
}

static int zc_on, zc_dbg, zc_band_top = 1;
/* ACZ_VR_CLOUD_GROUND_CLEAR=m (bug #95, 2026-10-23; default 680, 0 = off): while the aircraft stands or rolls on
   the ground (FM+0x236 bit 0, published by zero_vr.c's instrument hook) the band's base is held at least m above
   the camera, for the rest of that cloud list (the stage). Mission 2's take-off set is a stratus deck of 10-12 km
   sprites (S 39000-74000 units) centred 1530-2420 m up; their bodies (vertical semi-axis 0.262 S) put the band's
   10th-percentile base at -248 m, so the runway (eye 308 m) sat inside our clouds (C1 in_cloud 0.35, our body
   0.40: about 3.8 km visibility; haze over the runway and the mountains, the glass wet). The game itself never
   draws a cloud within 4096 units (683 m) of its camera and fades them in to 12288: the 2006 airfield is clear air
   under a grey deck. The floor is the eye's altitude on the ground + m (M2: 988 m; the set's own weather band
   starts at 853 m), C1 in_cloud fades to 0 over the 150 m below it; flight-start stages never touch it. */
static float zc_clear_m = 680.0f, zc_floor_m = -1.0e9f;
volatile u64 ps2_z_ground_field;          /* zero_vr.c: the last flight frame with the aircraft on the ground */
static float zc_upm = 6.0f, zc_kbody = 0.675f, zc_kin = 0.20f;
/* ACZ_VR_CLOUD_DRIFT (pt_excal 2026-10-28, bug #187: M8 "why do I feel I am being carried away somewhere, or are the
   clouds like that?"): how much of the game's wind drift our clouds (C1: the far map, the near volume, their shadows,
   the in-cloud feel) take. The game moves its cloud field with the stage's wind at full speed -- M8 / M9 800 units/s (120 m/s at the
   6.65 units/m that 324 kt gives; 72 % of that speed), M1, M2, M5, M10, M11, M15, M16 500 units/s: a jet stream at
   0.6-4.5 km -- while the aircraft flies over the ground and never drifts
   with that air: relative to the aircraft a whole cloud deck slides sideways at the wind speed. In 2006 the game's
   sprites were culled within 680 m and faded in to 2 km; our clouds pass right by the canopy, so that slide becomes
   strong optic flow of false sideways motion. 1 = the game's drift (default), 0 = clouds fixed over the ground, between:
   a slower drift. The game's own cloud sprites (ACZ_VR_CLOUDS off) keep the game's drift. */
static float zc_kdrift = 1.0f;
static struct {                            /* this frame's inputs, read from the sky object */
    u32 sky, env, obj, desc, rec, map, n, mask, tcount, alpha, type;
    float wind[2], eye[3], moff[2], far;   /* far: the game's cloud distance limit (units), 0 = unknown */
    int list;                              /* the cloud field is on and the list is readable */
} zf;
static struct {                            /* per cloud list (identity rec / desc / n / mask): fixed values */
    u32 rec, desc, n, mask;
    float base_m, top_m, coverage, coverage_unmasked, type, r50_m;
} zl;
typedef struct { float pos[3], s, rh, rv, op; u32 var; } zc_cloud;   /* metres: sprite half-size, body semi-axes */
static double zc_org[2];                   /* cloud-frame x, z (units) that the published positions are relative to */
static int zc_org_ok;
static u32 zc_list_seq, zc_npub, zc_recentres;
#if PS2_RQ_SUPPORT
#define ZC_MAXP (4 * ZC_MAXC)              /* (ACZ_VR_CLOUD_STATIC: a record's copies within reach, before the cap at C1's 400) */
static float zc_sd[ZC_MAXP];               /* (ACZ_VR_CLOUD_STATIC: a copy's surface distance from the camera, m) */
static int zc_static = -1; static float zc_static_m = 1500.0f; static int zc_static_fade = 1;
/* (rq8_onelayer 2026-11-14, prompt 10 step 3: PS2_RQ_CLOUDS_ONE=2, the whole table frozen; see zc_build_one) */
static int zc_one = -1; static float zc_one_amt = 1.0f; static u32 zc_one_key[4]; static int zc_one_have, zc_one_bump; static float zc_one_op[ZC_MAXC];
static u64 zc_one_logf;
/* (round 2, PS2_RQ_CLOUDS_ONE_RULE / _WINDOW: the coverage's motion under the bodies, s = the drift + the map's offset, sampled every 16
   fields while a list exists -- its velocity for the rule; a table met without a history re-freezes once when one exists) */
static int zc_one_rule = 3; static float zc_one_win = 600.0f, zc_one_reach = 2000.0f; static u64 zc_one_hf[8]; static float zc_one_hs[8][2]; static u32 zc_one_hm[8], zc_one_hn;
static int zc_one_redo, zc_one_force; static float zc_one_v[2];
static float zc_one_vl[2]; static u64 zc_one_vlf;   /* (the last motion measured, kept over a stage's new map: its table is re-taken at once) */
static float zc_one_mean[ZC_MAXC], zc_one_area[ZC_MAXC]; static u32 zc_one_ord[ZC_MAXC];
static int zc_one_cmp_mean(const void *a, const void *b) {   /* (rule 3: the most often present first) */
    float x = zc_one_mean[*(const u32 *)a], y = zc_one_mean[*(const u32 *)b];
    return x > y ? -1 : x < y ? 1 : (int)(*(const u32 *)a) - (int)(*(const u32 *)b);
}
#else
#define ZC_MAXP ZC_MAXC
#endif
static zc_cloud zc_pub[ZC_MAXP], zc_new[ZC_MAXP];
static float zc_in_cloud, zc_precip, zc_sun[3], zc_sun_ang[2], zc_cam[3];
static float zc_pr_amt, zc_pr_snow, zc_pr_lo, zc_pr_hi;     /* the precipitation volume (C1 CL_ENV_PRECIP) */
static int zc_pr_ok;
static int zc_sun_ok;
static double zc_drift[2];
#ifdef ZC_C1
static u8 zc_map[65536];                   /* a copy of the game's coverage map (C1 cov_map) */
static u32 zc_map_src, zc_map_hash;
#endif

static void zc_read(u32 s) {
    memset(&zf, 0, sizeof zf);
    zf.sky = s;
    zf.env = ps2_r32(s + 4u);
    u32 sc = ps2_r32(s + ZC_SKY_CAM);
    if (zc_ok(sc)) for (u32 i = 0; i < 3; i++) zf.eye[i] = zc_rf(sc + 0x10u + 4u * i);
    u32 obj = s + ZC_SKY_CLOUD, desc = ps2_r32(obj), rec = ps2_r32(obj + 4u);
    if (!zc_ok(desc) || !zc_ok(rec) || !zc_ok(sc)) return;
    zf.obj = obj; zf.desc = desc; zf.rec = rec;
    zf.type = ps2_r32(desc);
    zf.map = ps2_r32(obj + 8u);
    if (!(zc_ok(zf.map) && zf.map < 0x2000000u - 0x10000u)) zf.map = 0u;
    u32 n = ps2_r16(desc + 0x70u);
    zf.n = n > ZC_MAXC ? ZC_MAXC : n;
    zf.mask = zf.type & 0x100u ? 1u : 3u;
    zf.tcount = ps2_r32(obj + 0xD0u);
    zf.alpha = ps2_r16(desc + 0x6Cu);
    {   /* the clouds' LIGHT colour (file +0x1F8 = descriptor +0x68, 0x00BBGGRR) as luminance: what the stage's art
           says about direct sunlight (the optics' gate ACZ_VR_OPTICS_SUNLIT, ps2_clouds_optics.c; M7's storm 0.47) */
        extern volatile float ps2_clouds_game_lit_y;
        u32 c = ps2_r32(desc + 0x68u);
        if (c & 0xFFFFFFu)
            ps2_clouds_game_lit_y = (0.299f * (float)(c & 0xFFu) + 0.587f * (float)(c >> 8 & 0xFFu)
                                     + 0.114f * (float)(c >> 16 & 0xFFu)) * (1.0f / 255.0f);
    }
    zf.wind[0] = zc_rf(desc + 0x10u); zf.wind[1] = zc_rf(desc + 0x14u);
    zf.moff[0] = zc_rf(obj + 0xE0u); zf.moff[1] = zc_rf(obj + 0xE8u);
    {   u32 sc = ps2_r32(obj + 0xD4u);     /* as the field: min(*(obj +0xD4) +0x278, 139264) */
        float far = zc_ok(sc) ? zc_rf(sc + 0x278u) : 0.0f;
        zf.far = far == far && far > 4096.0f ? (far < 139264.0f ? far : 139264.0f) : 0.0f;
    }
    zf.list = (zf.type & 2u) && zf.n > 0u && zf.wind[0] == zf.wind[0] && zf.wind[1] == zf.wind[1];
}

/* cloud i: altitude Y and sprite half-size S (units) */
static void zc_cloud_at(u32 i, float *Y, float *S) {
    u32 r = zf.rec + 8u * i, v = zf.desc + 0x80u + 16u * (i & zf.mask);
    *Y = zc_rf(v) - zc_rf(v + 4u) * (float)((int)(ps2_r16(r + 2u) & 0xFFFu) - 0x800);
    *S = (float)(s16)ps2_r16(r + 6u) * 2304.0f * zc_rf(v + 8u) * (1.0f / ZC_SPRITE_DIV);
}
/* the variant's build width (descriptor +0x20 + 16 v, the first of its four impostor build parameters: 0.5 builds a
   small round cloud, 1.2-1.6 a wide one) as a factor of the horizontal body axis */
static float zc_fw(u32 var) {
    float p0 = zc_rf(zf.desc + 0x20u + 16u * var);
    float fw = p0 / 1.2f;
    return fw == fw ? (fw > 1.0f ? 1.0f : fw < 0.2f ? 0.2f : fw) : 1.0f;
}

/* fixed per list: band, coverage, type (see the header) */
static void zc_list_stats(void) {
    static float lo[ZC_MAXC], hi[ZC_MAXC], rr[ZC_MAXC];
    u32 m = 0;
    double area = 0.0, area_all = 0.0;
    float top_full = -1e30f;               /* the highest body top as S2's splat draws it (0.9 x the vertical semi-axis) */
    for (u32 i = 0; i < zf.n; i++) {
        float Y, S;
        zc_cloud_at(i, &Y, &S);
        if (!(S > 1.0f) || !(Y == Y)) continue;
        float Rh = ZC_BODY_W * S * zc_fw(i & zf.mask), Rv = ZC_BODY_H * S;
        u32 r = zf.rec + 8u * i;
        float c = 1.0f;
        if (zf.map) {
            c = 0.0f;
            for (int a = 0; a < 4; a++)
                for (int b = 0; b < 4; b++)
                    c += zc_map_at(zf.map, 2.0f * (float)(s16)ps2_r16(r) + (float)ZC_HALF + (float)a * 131072.0f + zf.moff[0],
                                   2.0f * (float)(s16)ps2_r16(r + 4u) + (float)ZC_HALF + (float)b * 131072.0f + zf.moff[1]);
            c *= 1.0f / 16.0f;
        }
        area += 3.14159265 * (double)(zc_kbody * Rh) * (double)(zc_kbody * Rh) * (double)c;
        area_all += 3.14159265 * (double)(zc_kbody * Rh) * (double)(zc_kbody * Rh);
        lo[m] = Y - zc_kbody * Rv; hi[m] = Y + zc_kbody * Rv; rr[m] = S * (1.0f / 8.85f); m++;
        if (Y + 0.9f * Rv > top_full) top_full = Y + 0.9f * Rv;
    }
    zl.rec = zf.rec; zl.desc = zf.desc; zl.n = zf.n; zl.mask = zf.mask;
    if (!m) { zl.base_m = zl.top_m = 0.0f; zl.coverage = zl.coverage_unmasked = 0.0f; zl.type = 0.5f; zl.r50_m = 0.0f; return; }
    qsort(lo, m, sizeof lo[0], zc_cmpf); qsort(hi, m, sizeof hi[0], zc_cmpf); qsort(rr, m, sizeof rr[0], zc_cmpf);
    zl.base_m = lo[m / 10u] / zc_upm;
    zl.top_m = hi[(m * 9u) / 10u] / zc_upm;
    /* ACZ_VR_CLOUD_BAND_TOP=1 (bug #40, 2026-10-22): the band reaches the highest body top. The 90th percentile of
       Y + 0.675 h cut every body poking out of it (S2's splat draws them to Y + 0.9 h): M2 60 of 150 tops (band top 4193 m,
       bodies to 4994 m), M1 141 of 280 -- flat lids at one altitude, "a loaf of bread", a long flat-topped slab. The
       band only bounds our renderer's density (C1 band: S2/S3/the shadow map); 0 = the percentile of before. */
    if (zc_band_top && top_full > -1e29f && top_full / zc_upm > zl.top_m) zl.top_m = top_full / zc_upm;
    zl.coverage = (float)(1.0 - exp(-area / (ZC_PERIOD * ZC_PERIOD)));
    zl.coverage_unmasked = (float)(1.0 - exp(-area_all / (ZC_PERIOD * ZC_PERIOD)));
    zl.r50_m = rr[m / 2u] / zc_upm;
    float ty = (1000.0f - zl.r50_m) / (1000.0f - 350.0f);
    zl.type = ty < 0.0f ? 0.0f : ty > 1.0f ? 1.0f : ty;
}

/* the list relative to the cloud-frame origin, the copy nearest to it, without the clouds the game drops */
static u32 zc_build(double dx, double dz, double cx, double cy, double cz) {
    u32 k = 0;
    float thr = 12.0f + (float)zf.alpha / 108.0f;
    for (u32 i = 0; i < zf.n; i++) {
        float Y, S;
        zc_cloud_at(i, &Y, &S);
        if (!(S > 1.0f) || !(Y == Y)) continue;
        u32 r = zf.rec + 8u * i;
        double ox = zc_wrap(2.0 * (double)(s16)ps2_r16(r) + ZC_HALF - zc_org[0]);
        double oz = zc_wrap(2.0 * (double)(s16)ps2_r16(r + 4u) + ZC_HALF - zc_org[1]);
        float cov = zf.map ? zc_map_at(zf.map, (float)(zc_org[0] + ox + dx) + zf.moff[0],
                                       (float)(zc_org[1] + oz + dz) + zf.moff[1]) : 1.0f;
        if (zf.far > 0.0f) {               /* the game's far fade: full to far / 2, 0 at far */
            double ex = zc_org[0] + ox - cx, ey = (double)Y - cy, ez = zc_org[1] + oz - cz;
            float d = (float)sqrt(ex * ex + ey * ey + ez * ez), h = 0.5f * zf.far;
            if (d > h) cov *= d >= zf.far ? 0.0f : (zf.far - d) / h;
        }
        if (cov * 128.0f < thr) continue;
        zc_new[k].pos[0] = (float)(ox / zc_upm); zc_new[k].pos[1] = Y / zc_upm; zc_new[k].pos[2] = (float)(oz / zc_upm);
        zc_new[k].s = S / zc_upm;
        zc_new[k].rh = ZC_BODY_W * S * zc_fw(i & zf.mask) / zc_upm;
        zc_new[k].rv = ZC_BODY_H * S / zc_upm;
        zc_new[k].op = floorf(cov * 64.0f + 0.5f) * (1.0f / 64.0f);   /* steps of 1/64: list_seq moves with real changes */
        zc_new[k].var = i & zf.mask;
        k++;
    }
    return k;
}

#if PS2_RQ_SUPPORT
/* ACZ_VR_CLOUD_STATIC (see the header): every copy whose body's box comes within reach of the camera, opacity = the coverage map
   alone (tapered to 0 under the game's threshold), the nearest CL_MAX_CLOUDS by the surface's distance */
static int zc_cmp_sd(const void *a, const void *b) {
    float x = zc_sd[*(const u32 *)a], y = zc_sd[*(const u32 *)b];
    return (x > y) - (x < y);
}
/* PS2_RQ_CLOUDS_ONE=2 (rq8_onelayer 2026-11-14, prompt 10 step 3: the static field for real; with ACZ_VR_CLOUD_STATIC and
   PS2_RQ_CLOUDS_BAKE): the mission's WHOLE table, every record once (its copy nearest the cloud-frame origin; the renderer splats the
   list with the period's wrap, so every copy everywhere is the same body), each at an opacity FROZEN when the table first appears --
   the coverage map at that copy times PS2_RQ_CLOUDS_ONE_AMOUNT (the whole mission's amount; 1 = the game's coverage), tapered under
   the game's threshold as ACZ_VR_CLOUD_STATIC does --, no fade by the centre's distance, no drop by distance, no drift of the opacity:
   nothing about a body depends on the camera or the time. The game's fade by distance is the marches' per sample (list_static 1).
   list_seq moves only when the table itself changes (a new stage's list); a re-centre of the origin re-publishes the same bodies
   relative to it without a new list_seq. Not the game's: a copy far from the mission's start takes the start copy's opacity (the
   coverage map repeats every 4 list periods), and the game's opacities follow the coverage under the drifting bodies over time. */
/* (round 2, PS2_RQ_CLOUDS_ONE_RULE: 0 = each body at the game's opacity when its table appears (fe040c2: a body the coverage takes in later,
   M10's 97-105 s cumulus, is never there); 1 = the mean, 2 = the peak of the game's opacity for that body over the next _ONE_WINDOW
   seconds (600) of the coverage's motion under it -- the bodies drift with the wind and the map's offset scrolls: s(t) = s(now) + v t,
   v measured from the last ~2 s; 48 samples. Still one value per body for the whole table: nothing changes in flight. Without a
   motion history (the mission's first table) the table is published as rule 0 and taken once more, with the rule, as soon as 30 fields
   of history exist -- one more whole bake at the load) */
static int zc_one_vel(u64 fnow, const float s[2], float v[2]) {
    int best = -1;
    for (u32 k = 0; k < 8u && k < zc_one_hn; k++)
        if (zc_one_hm[k] == zf.map && fnow >= zc_one_hf[k] + 30u && fnow <= zc_one_hf[k] + 160u && (best < 0 || zc_one_hf[k] < zc_one_hf[best])) best = (int)k;
    if (best < 0) return 0;
    for (int a = 0; a < 2; a++) {
        double d = (double)s[a] - (double)zc_one_hs[best][a];
        d = fmod(d + 262144.0, 524288.0); if (d < 0.0) d += 524288.0; d -= 262144.0;   /* (the map's period, 256 x 2048 units) */
        v[a] = (float)(d / (double)(fnow - zc_one_hf[best]));
    }
    return 1;
}
static u32 zc_build_one(double dx, double dz) {
    u32 key[4] = { zf.rec, zf.desc, zf.n, zf.mask };
    u32 nmax = zf.n < (u32)ZC_MAXC ? zf.n : (u32)ZC_MAXC;
    u64 fnow = ps2_kernel_vblank_count();
    float s[2] = { (float)dx + zf.moff[0], (float)dz + zf.moff[1] };
    zc_one_force = 0;
    if (zc_one_redo && zc_one_have && memcmp(key, zc_one_key, sizeof key) == 0) {
        float v[2]; if (zc_one_vel(fnow, s, v)) { zc_one_have = 0; zc_one_redo = 0; }   /* (the history exists now: the table once more, with the rule) */
    }
    if (!zc_one_have || memcmp(key, zc_one_key, sizeof key) != 0) {
        memcpy(zc_one_key, key, sizeof key); zc_one_have = 1; zc_one_bump = 1;
        float c1 = (12.0f + (float)zf.alpha / 108.0f) / 128.0f;
        u32 nz = 0; double sum = 0.0;
        int rule = zc_one_rule;
        zc_one_v[0] = zc_one_v[1] = 0.0f;
        if (rule > 0 && !zc_one_vel(fnow, s, zc_one_v)) {
            if (zc_one_vlf && fnow < zc_one_vlf + 1200u) { zc_one_v[0] = zc_one_vl[0]; zc_one_v[1] = zc_one_vl[1]; zc_one_redo = 0; }   /* (a stage's new map) */
            else { rule = 0; zc_one_redo = 1; }
        }
        /* (the window ends after _ONE_WINDOW seconds or when the coverage has moved _ONE_REACH metres under the bodies, whichever is first:
           M2's wind moves it ~8.6 units a field, 25x M10's -- over 600 s every body met full coverage and the peak was 1 for all of them) */
        float vv = sqrtf(zc_one_v[0] * zc_one_v[0] + zc_one_v[1] * zc_one_v[1]), tw = zc_one_win * 60.0f;
        if (vv > 1.0e-3f && zc_one_reach * zc_upm / vv < tw) tw = zc_one_reach * zc_upm / vv;
        const int NS = 48; float dt = tw / (float)(NS - 1);
        for (u32 i = 0; i < nmax; i++) {
            float Y, S;
            zc_cloud_at(i, &Y, &S);
            zc_one_op[i] = 0.0f;
            if (!(S > 1.0f) || !(Y == Y)) continue;
            u32 r = zf.rec + 8u * i;
            double ox = zc_wrap(2.0 * (double)(s16)ps2_r16(r) + ZC_HALF - zc_org[0]);
            double oz = zc_wrap(2.0 * (double)(s16)ps2_r16(r + 4u) + ZC_HALF - zc_org[1]);
            float op = 0.0f, pk = 0.0f;
            for (int n = 0; n < (rule > 0 ? NS : 1); n++) {
                float t = (float)n * dt;
                float cov = zf.map ? zc_map_at(zf.map, (float)(zc_org[0] + ox + dx) + zf.moff[0] + zc_one_v[0] * t,
                                               (float)(zc_org[1] + oz + dz) + zf.moff[1] + zc_one_v[1] * t) : 1.0f;
                cov *= zc_one_amt;
                if (cov > 1.0f) cov = 1.0f;
                float x = cov >= c1 ? 1.0f : cov / c1, o = cov * x * x * (3.0f - 2.0f * x);
                op += o; if (o > pk) pk = o;
            }
            if (rule > 0) op *= 1.0f / (float)NS;
            {   float a = ZC_BODY_W * S * zc_fw(i & zf.mask); zc_one_mean[i] = op; zc_one_area[i] = a * a; }
            if (rule >= 2) op = pk;
            zc_one_op[i] = floorf(op * 64.0f + 0.5f) * (1.0f / 64.0f);
        }
        /* (rule 3: the game's amount on average with whole bodies -- the bodies most often present over the window, each at its peak, until
           the sum of peak x area reaches the sum of the mean opacity x area over the whole table; the rest none) */
        if (rule == 3) {
            double tgt = 0.0, acc = 0.0; u32 m = 0;
            for (u32 i = 0; i < nmax; i++) if (zc_one_op[i] > 0.0f) { tgt += (double)zc_one_mean[i] * zc_one_area[i]; zc_one_ord[m++] = i; }
            qsort(zc_one_ord, m, sizeof zc_one_ord[0], zc_one_cmp_mean);
            for (u32 j = 0; j < m; j++) {
                u32 i = zc_one_ord[j];
                if (acc >= tgt) { zc_one_op[i] = 0.0f; continue; }
                acc += (double)zc_one_op[i] * zc_one_area[i];
            }
        }
        for (u32 i = 0; i < nmax; i++) if (zc_one_op[i] > 0.0f) { nz++; sum += zc_one_op[i]; }
        ps2_log("zero-clouds: PS2_RQ_CLOUDS_ONE=2: the whole table frozen (field %llu): %u records, %u with an opacity > 0 (mean %.3f), "
                "amount x%.2f (PS2_RQ_CLOUDS_ONE_AMOUNT); rule %d (%s; the coverage's motion %.2f %.2f units a field, the window %.0f s)%s; no fade by the centre's "
                "distance, no drift of the opacities", (unsigned long long)fnow, nmax, nz, nz ? sum / nz : 0.0, (double)zc_one_amt, rule,
                rule == 1 ? "the mean over the window" : rule == 2 ? "the peak over the window" : rule == 3 ? "the most often present at their peak, the mean's amount" : "the opacity now", (double)zc_one_v[0], (double)zc_one_v[1],
                (double)(rule > 0 ? tw / 60.0f : 0.0f),
                zc_one_redo ? ": no motion history yet, taken once more with the rule when there is one" : "");
    }
    u32 k = 0;
    for (u32 i = 0; i < nmax && k < (u32)CL_MAX_CLOUDS; i++) {
        if (!(zc_one_op[i] > 0.0f)) continue;
        float Y, S;
        zc_cloud_at(i, &Y, &S);
        if (!(S > 1.0f) || !(Y == Y)) continue;
        u32 r = zf.rec + 8u * i;
        double ox = zc_wrap(2.0 * (double)(s16)ps2_r16(r) + ZC_HALF - zc_org[0]);
        double oz = zc_wrap(2.0 * (double)(s16)ps2_r16(r + 4u) + ZC_HALF - zc_org[1]);
        zc_new[k].pos[0] = (float)(ox / zc_upm); zc_new[k].pos[1] = Y / zc_upm; zc_new[k].pos[2] = (float)(oz / zc_upm);
        zc_new[k].s = S / zc_upm;
        zc_new[k].rh = ZC_BODY_W * S * zc_fw(i & zf.mask) / zc_upm;
        zc_new[k].rv = ZC_BODY_H * S / zc_upm;
        zc_new[k].op = zc_one_op[i];
        zc_new[k].var = i & zf.mask;
        k++;
    }
    return k;
}
/* (PS2_RQ_CLOUDS_ONE=2, ACZ_DEBUG_CLOUDS) how the frozen whole table's amount compares with the game's 2006 field and with t12b's (the
   static field with the fade by the centre's distance): over the copies (3 x 3 each) whose body's box lies within the game's cloud far of
   the camera, the sum of opacity x the body's disc (0.9 x the horizontal semi-axis); the game: the coverage now x its fade by the centre's
   distance, dropped under its threshold; t12b: the coverage tapered x the same fade */
static void zc_one_amount_log(double cx, double cy, double cz) {
    if (!zf.list || zf.far <= 0.0f) return;
    float c1 = (12.0f + (float)zf.alpha / 108.0f) / 128.0f, thr = 12.0f + (float)zf.alpha / 108.0f;
    double far = (double)zf.far, a_one = 0.0, a_onef = 0.0, a_game = 0.0, a_t12b = 0.0, near_one = 0.0, near_game = 0.0;
    u32 nmax = zf.n < (u32)ZC_MAXC ? zf.n : (u32)ZC_MAXC;
    for (u32 i = 0; i < nmax; i++) {
        float Y, S;
        zc_cloud_at(i, &Y, &S);
        if (!(S > 1.0f) || !(Y == Y)) continue;
        u32 r = zf.rec + 8u * i;
        double ox0 = zc_wrap(2.0 * (double)(s16)ps2_r16(r) + ZC_HALF - zc_org[0]);
        double oz0 = zc_wrap(2.0 * (double)(s16)ps2_r16(r + 4u) + ZC_HALF - zc_org[1]);
        double a = 0.9 * ZC_BODY_W * S * zc_fw(i & zf.mask), b = 0.9 * ZC_BODY_H * S, disc = 3.14159265 * a * a;
        double gv = fabs((double)Y - cy) - b;
        if (gv < 0.0) gv = 0.0;
        for (int ia = -1; ia <= 1; ia++)
            for (int ib = -1; ib <= 1; ib++) {
                double ox = ox0 + ia * ZC_PERIOD, oz = oz0 + ib * ZC_PERIOD;
                double ex = zc_org[0] + ox - cx, ez = zc_org[1] + oz - cz;
                double gh = sqrt(ex * ex + ez * ez) - a;
                if (gh < 0.0) gh = 0.0;
                double sd = sqrt(gh * gh + gv * gv);
                if (sd > far) continue;
                float cov = zf.map ? zc_map_at(zf.map, (float)(zc_org[0] + ox + zc_drift[0]) + zf.moff[0],
                                               (float)(zc_org[1] + oz + zc_drift[1]) + zf.moff[1]) : 1.0f;
                double ey = (double)Y - cy, dc = sqrt(ex * ex + ey * ey + ez * ez), h = 0.5 * far;
                double fade = dc <= h ? 1.0 : dc >= far ? 0.0 : (far - dc) / h;
                float x = cov >= c1 ? 1.0f : cov / c1, tap = cov * x * x * (3.0f - 2.0f * x);
                double og = cov * fade; if (og * 128.0 < thr) og = 0.0;
                a_one += zc_one_op[i] * disc; a_onef += zc_one_op[i] * fade * disc; a_game += og * disc; a_t12b += tap * fade * disc;
                if (sd < 0.25 * far) { near_one += zc_one_op[i] * disc; near_game += og * disc; }
            }
    }
    ps2_log("zero-clouds: PS2_RQ_CLOUDS_ONE=2 amount (field %llu): sum of opacity x disc within the cloud far (%.0f m) of the camera: "
            "frozen %.1f km2, the game's %.1f km2 (x%.2f), t12b's %.1f km2 (x%.2f); the frozen opacities weighed by the game's fade by the centre "
            "(the opacity alone against the game's) %.1f km2 (x%.2f); bodies within a quarter of it: frozen %.1f, the game's %.1f km2",
            (unsigned long long)ps2_kernel_vblank_count(), far / zc_upm, a_one / (zc_upm * zc_upm * 1e6), a_game / (zc_upm * zc_upm * 1e6),
            a_game > 0.0 ? a_one / a_game : 0.0, a_t12b / (zc_upm * zc_upm * 1e6), a_game > 0.0 ? a_t12b / a_game : 0.0,
            a_onef / (zc_upm * zc_upm * 1e6), a_game > 0.0 ? a_onef / a_game : 0.0,
            near_one / (zc_upm * zc_upm * 1e6), near_game / (zc_upm * zc_upm * 1e6));
}
static u32 zc_build_static(double dx, double dz, double cx, double cy, double cz) {
    static zc_cloud tmp[ZC_MAXP]; static float tsd[ZC_MAXP]; static u32 ord[ZC_MAXP];
    if (zc_one > 0) return zc_build_one(dx, dz);           /* (rq8_onelayer: PS2_RQ_CLOUDS_ONE=2, the whole table frozen) */
    u32 k = 0;
    float c1 = (12.0f + (float)zf.alpha / 108.0f) / 128.0f;   /* (the game's threshold, ~0.10: tapered to 0 below it) */
    double far = zf.far > 0.0f ? (double)zf.far : 51200.0, reach = far + (double)zc_static_m * zc_upm;
    for (u32 i = 0; i < zf.n; i++) {
        float Y, S;
        zc_cloud_at(i, &Y, &S);
        if (!(S > 1.0f) || !(Y == Y)) continue;
        u32 r = zf.rec + 8u * i;
        double ox0 = zc_wrap(2.0 * (double)(s16)ps2_r16(r) + ZC_HALF - zc_org[0]);
        double oz0 = zc_wrap(2.0 * (double)(s16)ps2_r16(r + 4u) + ZC_HALF - zc_org[1]);
        double a = 0.9 * ZC_BODY_W * S * zc_fw(i & zf.mask), b = 0.9 * ZC_BODY_H * S;
        double gv = fabs((double)Y - cy) - b;
        if (gv < 0.0) gv = 0.0;
        if (gv > reach) continue;
        for (int ia = -1; ia <= 1; ia++)
            for (int ib = -1; ib <= 1; ib++) {
                if (k >= ZC_MAXP) break;
                double ox = ox0 + ia * ZC_PERIOD, oz = oz0 + ib * ZC_PERIOD;
                double ex = zc_org[0] + ox - cx, ez = zc_org[1] + oz - cz;
                double gh = sqrt(ex * ex + ez * ez) - a;
                if (gh < 0.0) gh = 0.0;
                double sd = sqrt(gh * gh + gv * gv);               /* (the box's distance: never more than the body's) */
                if (sd > reach) continue;
                float cov = zf.map ? zc_map_at(zf.map, (float)(zc_org[0] + ox + dx) + zf.moff[0],
                                               (float)(zc_org[1] + oz + dz) + zf.moff[1]) : 1.0f;
                /* (under the game's threshold c1 the opacity goes to 0 as cov x smoothstep(0, c1, cov): continuous with a slope of at
                   most ~1.5, so a 10 m drift step over the coverage map (<= 0.03) moves it by <= ~0.05 and a body enters at 1/64) */
                float x = cov >= c1 ? 1.0f : cov / c1, op = cov * x * x * (3.0f - 2.0f * x);
                if (zc_static_fade && zf.far > 0.0f) {   /* (ACZ_VR_CLOUD_STATIC_FADE: the game's fade by the centre's distance, no threshold) */
                    double ey = (double)Y - cy, dc = sqrt(ex * ex + ey * ey + ez * ez), h = 0.5 * far;
                    if (dc > h) op *= dc >= far ? 0.0f : (float)((far - dc) / h);
                }
                op = floorf(op * 64.0f + 0.5f) * (1.0f / 64.0f);   /* steps of 1/64: list_seq moves with real changes */
                if (op <= 0.0f) continue;
                tmp[k].pos[0] = (float)(ox / zc_upm); tmp[k].pos[1] = Y / zc_upm; tmp[k].pos[2] = (float)(oz / zc_upm);
                tmp[k].s = S / zc_upm;
                tmp[k].rh = ZC_BODY_W * S * zc_fw(i & zf.mask) / zc_upm;
                tmp[k].rv = ZC_BODY_H * S / zc_upm;
                tmp[k].op = op;
                tmp[k].var = i & zf.mask;
                tsd[k] = (float)(sd / zc_upm);
                k++;
            }
    }
    u32 m = k;
    if (k > CL_MAX_CLOUDS) {                   /* more than C1 holds: the nearest surfaces (logged once a list) */
        static u32 warned_n;
        for (u32 j = 0; j < k; j++) { ord[j] = j; zc_sd[j] = tsd[j]; }
        qsort(ord, k, sizeof ord[0], zc_cmp_sd);
        m = CL_MAX_CLOUDS;
        if (warned_n != zf.n) { warned_n = zf.n; ps2_log("zero-clouds: ACZ_VR_CLOUD_STATIC: %u copies within reach, C1 holds %u: the nearest kept (the farthest surface kept %.0f m)", k, (u32)CL_MAX_CLOUDS, (double)zc_sd[ord[m - 1]]); }
        for (u32 j = 0; j < m; j++) zc_new[j] = tmp[ord[j]];
    } else
        memcpy(zc_new, tmp, (size_t)k * sizeof tmp[0]);
    return m;
}
#endif

static float zc_incloud(const float cam[3]) {
    float keep = 1.0f;
    for (u32 i = 0; i < zc_npub; i++) {
        float a = zc_pub[i].pos[0] - cam[0], b = zc_pub[i].pos[1] - cam[1], c = zc_pub[i].pos[2] - cam[2];
        float d2 = a * a + b * b + c * c, R = zc_kin * zc_pub[i].s;
        if (d2 >= 0.81f * R * R) continue;
        float q = sqrtf(d2) / R, s = q <= 0.45f ? 1.0f : 1.0f - (q - 0.45f) / 0.45f;
        s = s * s * (3.0f - 2.0f * s);
        keep *= 1.0f - zc_pub[i].op * s;
    }
    return 1.0f - keep;
}

#if defined(ZC_C1) && CL_MAX_CLOUDS < ZC_MAXC
static int zc_cmp_dist(const void *pa, const void *pb) {
    const zc_cloud *a = (const zc_cloud *)pa, *b = (const zc_cloud *)pb;
    float ax = a->pos[0] - zc_cam[0], az = a->pos[2] - zc_cam[2], bx = b->pos[0] - zc_cam[0], bz = b->pos[2] - zc_cam[2];
    float da = ax * ax + az * az, db = bx * bx + bz * bz;
    return (da > db) - (da < db);
}
#endif

static void zc_update(void) {
    zc_drift[0] = zc_drift[1] = 0.0;
    if (zf.list) {
        if (zf.rec != zl.rec || zf.desc != zl.desc || zf.n != zl.n || zf.mask != zl.mask) {
            zc_list_stats();
            zc_org_ok = 0;
            zc_floor_m = -1.0e9f;              /* (a new stage: no runway floor yet) */
        }
        double t = (double)zf.tcount * (1.0 / 60.0) * (double)zc_kdrift;       /* (ACZ_VR_CLOUD_DRIFT) */
        zc_drift[0] = (double)zf.wind[0] * t; zc_drift[1] = (double)zf.wind[1] * t;
        double cx = (double)zf.eye[0] - zc_drift[0], cz = (double)zf.eye[2] - zc_drift[1];   /* camera, cloud frame */
        if (!zc_org_ok || fabs(cx - zc_org[0]) > 2000.0 * zc_upm || fabs(cz - zc_org[1]) > 2000.0 * zc_upm) {
            zc_org[0] = cx; zc_org[1] = cz; zc_org_ok = 1; zc_recentres++;
        }
        /* the opacities change only as the clouds drift over the coverage map: rebuild after a re-centre, a new list
           or 40 m of drift (1/8 of a map cell), so list_seq bumps a few times a second at most */
        static double built_drift[2], built_cam[3];
        static u32 built_recentres = ~0u, built_seq_list;
        double bd = zf.far > 0.0f ? 100.0 * zc_upm : 1e30;   /* the far fade follows the camera */
#if PS2_RQ_SUPPORT
        /* (ACZ_VR_CLOUD_STATIC: the opacities follow the coverage under the drifting bodies in steps of 10 m of drift instead of 40, so a
           step is at most ~1/32 of opacity -- the follower eases it -- and a body crossing the coverage threshold enters at ~1/64) */
#define ZC_DRIFT_STEP (zc_one_force ? -1.0 : zc_static > 0 ? 10.0 : 40.0)   /* (zc_one_force: PS2_RQ_CLOUDS_ONE_RULE's re-take, zc_one_tick) */
#else
#define ZC_DRIFT_STEP 40.0
#endif
        if (zc_recentres != built_recentres || built_seq_list != zl.rec + zl.n + zl.desc
            || fabs(zc_drift[0] - built_drift[0]) > ZC_DRIFT_STEP * zc_upm || fabs(zc_drift[1] - built_drift[1]) > ZC_DRIFT_STEP * zc_upm
            || fabs(cx - built_cam[0]) > bd || fabs((double)zf.eye[1] - built_cam[1]) > bd || fabs(cz - built_cam[2]) > bd) {
            built_recentres = zc_recentres; built_seq_list = zl.rec + zl.n + zl.desc;
            built_drift[0] = zc_drift[0]; built_drift[1] = zc_drift[1];
            built_cam[0] = cx; built_cam[1] = zf.eye[1]; built_cam[2] = cz;
#if PS2_RQ_SUPPORT
            if (zc_static < 0) {                   /* (ACZ_VR_CLOUD_STATIC: with our Quest clouds only; their marches fade by distance) */
                extern int ps2_clouds_rq_on(void);
                const char *se = getenv("ACZ_VR_CLOUD_STATIC"), *sm = getenv("ACZ_VR_CLOUD_STATIC_MARGIN");
                zc_static = se && *se && *se != '0' && ps2_clouds_rq_on() ? 1 : 0;
                if (sm && *sm) { float m = (float)atof(sm); zc_static_m = m >= 0.0f && m <= 20000.0f ? m : 1500.0f; }
                { const char *sf = getenv("ACZ_VR_CLOUD_STATIC_FADE"); if (sf && *sf) zc_static_fade = *sf != '0'; }
                {   /* (rq8_onelayer, PS2_RQ_CLOUDS_ONE=2 with PS2_RQ_CLOUDS_BAKE: the whole table frozen, no fade by the centre's distance) */
                    const char *o = getenv("PS2_RQ_CLOUDS_ONE"), *bk = getenv("PS2_RQ_CLOUDS_BAKE"), *am = getenv("PS2_RQ_CLOUDS_ONE_AMOUNT");
                    zc_one = zc_static && o && atoi(o) >= 2 && bk && *bk == '1' ? 1 : 0;
                    if (am && *am) { float a = (float)atof(am); zc_one_amt = a >= 0.0f && a <= 4.0f ? a : 1.0f; }
                    {   const char *ru = getenv("PS2_RQ_CLOUDS_ONE_RULE"), *wi = getenv("PS2_RQ_CLOUDS_ONE_WINDOW");   /* (round 2) */
                        if (ru && *ru) { int r = atoi(ru); zc_one_rule = r >= 0 && r <= 3 ? r : 0; }
                        if (wi && *wi) { float w = (float)atof(wi); zc_one_win = w >= 10.0f && w <= 3600.0f ? w : 600.0f; }
                        const char *re = getenv("PS2_RQ_CLOUDS_ONE_REACH");
                        if (re && *re) { float r = (float)atof(re); zc_one_reach = r >= 100.0f && r <= 100000.0f ? r : 2000.0f; } }
                    if (zc_one) { zc_static_fade = 0; ps2_log("zero-clouds: PS2_RQ_CLOUDS_ONE=2: the mission's whole cloud table, each body once at an opacity frozen at its first appearance (amount x%.2f)", (double)zc_one_amt); }
                }
                if (zc_static) ps2_log("zero-clouds: a static field: every body within the game's cloud far + %.0f m by its place, at the coverage "
                                       "map's opacity%s; the far fade per sample in the marches (ACZ_VR_CLOUD_STATIC, _MARGIN, _FADE)", (double)zc_static_m,
                                       zc_static_fade ? " x the game's fade by its centre's distance (continuous, no threshold)" : "");
            }
            u32 k = zc_static ? zc_build_static(zc_drift[0], zc_drift[1], cx, (double)zf.eye[1], cz)
                              : zc_build(zc_drift[0], zc_drift[1], cx, (double)zf.eye[1], cz);
#else
            u32 k = zc_build(zc_drift[0], zc_drift[1], cx, (double)zf.eye[1], cz);
#endif
            if (k != zc_npub || memcmp(zc_new, zc_pub, (size_t)k * sizeof zc_new[0])) {
                memcpy(zc_pub, zc_new, (size_t)k * sizeof zc_new[0]);
                zc_npub = k;
#if PS2_RQ_SUPPORT
                if (zc_one <= 0 || zc_one_bump) zc_list_seq++;   /* (PS2_RQ_CLOUDS_ONE=2: only a new table; a re-centre re-publishes it) */
                zc_one_bump = 0;
#else
                zc_list_seq++;
#endif
            }
#if PS2_RQ_SUPPORT
            {   static int dbg = -1; if (dbg < 0) { const char *d = getenv("ACZ_DEBUG_CLOUDS"); dbg = d && *d && atoi(d) > 0; }
                u64 fnow = ps2_kernel_vblank_count();
                if (zc_one > 0 && dbg && fnow >= zc_one_logf + 600u) { zc_one_logf = fnow; zc_one_amount_log(cx, (double)zf.eye[1], cz); } }
#endif
        }
        zc_cam[0] = (float)((cx - zc_org[0]) / zc_upm); zc_cam[1] = zf.eye[1] / zc_upm;
        zc_cam[2] = (float)((cz - zc_org[1]) / zc_upm);
        {   /* ACZ_VR_CLOUD_GROUND_CLEAR: on the ground, the band's base at least that far above the camera */
            u64 fn = ps2_kernel_vblank_count(), g = ps2_z_ground_field;
            if (zc_clear_m > 0.0f && g && fn >= g && fn - g <= 8u) {
                float fl = zc_cam[1] + zc_clear_m;
                if (fl > zc_floor_m + 1.0f) {
                    if (zc_floor_m < -1.0e8f)
                        ps2_log("zero-clouds: on the ground (eye %.0f m): the cloud band's base held at %.0f m (was %.0f m) "
                                "for this stage (ACZ_VR_CLOUD_GROUND_CLEAR=%.0f)", zc_cam[1], fl, zl.base_m, zc_clear_m);
                    zc_floor_m = fl;
                }
            }
        }
        zc_in_cloud = zc_incloud(zc_cam);
        if (zc_floor_m > zl.base_m && zc_floor_m < zl.top_m - 200.0f) {   /* none below the floor: fade over 150 m */
            float t = (zc_cam[1] - (zc_floor_m - 150.0f)) / 150.0f;
            zc_in_cloud *= t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
        }
    } else if (zc_npub) {
        zc_npub = 0; zc_list_seq++; zc_in_cloud = 0.0f;
    }
    zc_precip = zc_rf(zf.sky + 0xB8Cu);
    {   /* the precipitation volume (the optics' rainbow): the set's amount (halves A/B blended), its kind, the band */
        float t = zc_rf(zf.sky + 0xB30u), ka = zc_rf(zf.sky + 0xB90u);
        float lo = zc_rf(zf.sky + 0x10u), hi = zc_rf(zf.sky + 0x14u);
        float a = zc_ok(zf.env) ? zc_rf(zf.env + 0x84u) : 0.0f, b = zc_ok(zf.env) ? zc_rf(zf.env + 0x124u) : 0.0f;
        if (!(t >= 0.0f && t <= 1.0f)) t = 0.0f;
        if (!(a >= 0.0f && a <= 4.0f)) a = 0.0f;
        if (!(b >= 0.0f && b <= 4.0f)) b = a;
        zc_pr_ok = zc_ok(zf.env) && lo == lo && hi == hi && fabsf(lo) < 1.0e6f && fabsf(hi) < 1.0e6f;
        zc_pr_amt = a + (b - a) * t;
        zc_pr_snow = ka == ka && ka >= 0.4f ? 1.0f : 0.0f;
        zc_pr_lo = lo / zc_upm; zc_pr_hi = hi / zc_upm;
        if (zc_pr_hi < zc_pr_lo + 1.0f) zc_pr_hi = zc_pr_lo + 1.0f;
    }
    float el = zc_rf(zf.sky + ZC_SKY_SUN + 0x78u), az = zc_rf(zf.sky + ZC_SKY_SUN + 0x7Cu);
    zc_sun_ok = el == el && az == az && fabsf(el) <= 1.5708f && fabsf(az) <= 6.2832f;
    if (zc_sun_ok) {
        zc_sun[0] = -cosf(el) * sinf(az); zc_sun[1] = sinf(el); zc_sun[2] = -cosf(el) * cosf(az);
        zc_sun_ang[0] = el; zc_sun_ang[1] = az;
    }
}

#ifdef ZC_C1
static void zc_publish(void) {
    ps2_cloud_env *e = ps2_cloud_env_edit();
    e->valid &= ~(u32)(CL_ENV_PROFILE | CL_ENV_WEATHER | CL_ENV_LIST | CL_ENV_INCLOUD | CL_ENV_SUN | CL_ENV_COVMAP
                       | CL_ENV_WIND | CL_ENV_DRIFT);
    if (zc_sun_ok) { memcpy(e->sun_dir, zc_sun, sizeof zc_sun); e->valid |= CL_ENV_SUN; }
    e->precip = zc_precip;
    e->valid &= ~(u32)CL_ENV_PRECIP;
    if (zc_pr_ok) {
        e->precip_amt = zc_pr_amt; e->precip_snow = zc_pr_snow; e->precip_lo_m = zc_pr_lo; e->precip_hi_m = zc_pr_hi;
        e->valid |= CL_ENV_PRECIP;
    }
    if (zf.list) {
        e->base_m = zl.base_m; e->top_m = zl.top_m;
        if (zc_floor_m > zl.base_m && zc_floor_m < zl.top_m - 200.0f) e->base_m = zc_floor_m;   /* (runway floor) */
        e->coverage = zl.coverage; e->coverage_unmasked = zl.coverage_unmasked; e->type = zl.type;
        /* the game does not scale its cloud sprites by the descriptor's alpha (+0x6C, env +0x1FC): the field's sprite
           alpha is 128 x distance fade x coverage, and the alpha only raises the drop threshold (12 + alpha / 108,
           0x16A900), which the published list already applies. But the impostor build scales each impostor's own
           alpha by it (night_sky 2026-10-29: the self-copy with vertex colour (0, alpha / 2, 0) after the puffs): C1
           imp_alpha, which ps2_clouds_far.c applies to the drawn list (ACZ_VR_CLOUD_IMP_ALPHA) */
        e->transp = 0.0f;
        e->imp_alpha = zf.alpha > 0u ? (zf.alpha >= 128u ? 1.0f : (float)zf.alpha / 128.0f) : 0.0f;
        e->in_cloud = zc_in_cloud;
        e->wind_mps[0] = zf.wind[0] * zc_kdrift / zc_upm; e->wind_mps[1] = zf.wind[1] * zc_kdrift / zc_upm;
        e->drift_m[0] = (float)(zc_drift[0] / zc_upm); e->drift_m[1] = (float)(zc_drift[1] / zc_upm);
        e->origin_m[0] = zc_org[0] / zc_upm; e->origin_m[1] = 0.0; e->origin_m[2] = zc_org[1] / zc_upm;
        e->list_period_m = (float)(ZC_PERIOD / zc_upm);
        u32 k = zc_npub;
        const zc_cloud *src = zc_pub;
#if CL_MAX_CLOUDS < ZC_MAXC
        if (k > CL_MAX_CLOUDS) {           /* more clouds than C1 holds: the nearest to the camera */
            memcpy(zc_new, zc_pub, (size_t)k * sizeof zc_new[0]);
            qsort(zc_new, k, sizeof zc_new[0], zc_cmp_dist);
            src = zc_new; k = CL_MAX_CLOUDS;
        }
#endif
        for (u32 i = 0; i < k; i++) {
            memcpy(e->cloud[i].pos_m, src[i].pos, sizeof src[i].pos);
            e->cloud[i].radius_m = src[i].rh; e->cloud[i].opacity = src[i].op; e->cloud[i].variant = src[i].var;
#ifdef CL_CLOUD_SPRITE
            e->cloud[i].height_m = src[i].rv; e->cloud[i].sprite_m = src[i].s;
#endif
        }
        e->ncloud = k;
        e->list_seq = zc_list_seq;
#if PS2_RQ_SUPPORT
        e->cloud_far_m = zf.far > 0.0f ? zf.far / zc_upm : 0.0f;   /* (ACZ_VR_CLOUD_STATIC: the marches' per-sample far fade) */
        e->list_static = zc_static > 0 ? (zc_static_fade ? 2u : 1u) : 0u;   /* (2: ACZ_VR_CLOUD_STATIC_FADE) */
#endif
        if (zf.map) {                      /* u8 [z][x], 0..255, periodic; sample (cx, cz) at world (cx * 2048 - 262144
                                              - obj[0xE0], cz * 2048 - 262144 - obj[0xE8]) units; bilinear */
            u32 h = 2166136261u;
            for (u32 i = 0; i < 65536u; i += 61u) { h ^= ps2_r8(zf.map + i); h *= 16777619u; }
            if (zf.map != zc_map_src || h != zc_map_hash) {
                for (u32 i = 0; i < 65536u; i++) zc_map[i] = ps2_r8(zf.map + i);
                zc_map_src = zf.map; zc_map_hash = h;
            }
            e->cov_map = zc_map; e->cov_w = 256u; e->cov_h = 256u; e->cov_fmt = 0u;
            e->cov_cell_m = 2048.0f / zc_upm;
            e->cov_origin_m[0] = (-262144.0 - (double)zf.moff[0] - zc_drift[0]) / zc_upm;   /* cloud frame, this frame */
            e->cov_origin_m[1] = (-262144.0 - (double)zf.moff[1] - zc_drift[1]) / zc_upm;
            e->valid |= CL_ENV_COVMAP;
        }
        e->valid |= CL_ENV_PROFILE | CL_ENV_WEATHER | CL_ENV_LIST | CL_ENV_INCLOUD | CL_ENV_WIND | CL_ENV_DRIFT;
    } else {
        e->ncloud = 0;
        e->list_seq = zc_list_seq;
    }
    ps2_cloud_env_done();
    ps2_cloud_env_publish(0);
}
#endif

/* ---- ACZ_DEBUG_CLOUD_ENV: the data behind config_zero/cloud_missions.json ---- */
static void zc_row(const char *tag, u32 base, u32 o) {
    char ln[200]; int k = snprintf(ln, sizeof ln, "cldata: %s+%03X", tag, (unsigned)o);
    for (u32 i = 0; i < 8u; i++) k += snprintf(ln + k, sizeof ln - (size_t)k, " %08X", ps2_r32(base + o + 4u * i));
    ps2_log("%s", ln);
}
static void zc_debug(u64 f) {
    static u32 env_seen, list_rec, list_n, list_desc, band_seen[2];
    u32 s = zf.sky;
    if (zc_ok(zf.env) && zf.env != env_seen) {     /* the environment set (file layout, exact words) */
        env_seen = zf.env;
        u32 sv = ps2_r8(0x003B3810u + 0xC04Cu) ? 0x003B3810u + 0x5FE4u : 0x003B3810u + 0x50u;
        ps2_log("cldata: env %08X (file %08X) sky %08X field %llu mission index %d stage %d", zf.env, zf.env - 0x50u, s,
                (unsigned long long)f, (int)ps2_r32(sv + 0x5F48u), (int)ps2_r32(sv + 0x5F4Cu));
        for (u32 o = 0; o < 0x360u; o += 0x20u) zc_row("file", zf.env - 0x50u, o);
    }
    for (u32 j = 0; j < 2u; j++) {                 /* the precipitation band blocks */
        u32 b = ps2_r32(s + 0x9F8u + 4u * j);
        if (zc_ok(b) && b != band_seen[j] && f % 60u == 0u) {
            band_seen[j] = b;
            ps2_log("cldata: band block %u %08X field %llu", j, b, (unsigned long long)f);
            for (u32 o = 0; o < 0xC0u; o += 0x20u) zc_row(j ? "bandB" : "bandA", b, o);
        }
    }
    if (zf.list && (zf.rec != list_rec || zf.n != list_n || zf.desc != list_desc)) {   /* the list and the map */
        list_rec = zf.rec; list_n = zf.n; list_desc = zf.desc;
        float t = (float)zf.tcount * (1.0f / 60.0f);
        ps2_log("cldata: list rec %08X map %08X n %u mask %u t %u wind %.3f %.3f map offset %.0f %.0f eye %.0f %.0f %.0f",
                zf.rec, zf.map, zf.n, zf.mask, zf.tcount, zf.wind[0], zf.wind[1], zf.moff[0], zf.moff[1], zf.eye[0],
                zf.eye[1], zf.eye[2]);
        for (u32 i = 0; i < zf.n; i++) {
            u32 r = zf.rec + 8u * i;
            float Y, S;
            zc_cloud_at(i, &Y, &S);
            float R = S * (1.0f / 8.85f);
            float X = 2.0f * (float)(s16)ps2_r16(r) + (float)ZC_HALF + zf.wind[0] * t;
            float Z = 2.0f * (float)(s16)ps2_r16(r + 4u) + (float)ZC_HALF + zf.wind[1] * t;
            float cov = zf.map ? zc_map_at(zf.map, X + zf.moff[0], Z + zf.moff[1]) : 1.0f;
            ps2_log("cldata: cl %u %d %u %d %d | %.0f %.0f %.0f R %.0f var %u cov %.3f S %.0f fw %.2f", i, (s16)ps2_r16(r),
                    ps2_r16(r + 2u), (s16)ps2_r16(r + 4u), (s16)ps2_r16(r + 6u), X, Y, Z, R, i & zf.mask, cov, S,
                    zc_fw(i & zf.mask));
        }
        if (zf.map)
            for (u32 gz = 0; gz < 64u; gz++) {             /* 64 x 64, each the mean of 4 x 4 cells, one hex digit */
                char ln[80]; u32 k = 0;
                for (u32 gx = 0; gx < 64u; gx++) {
                    u32 sum = 0;
                    for (u32 dz = 0; dz < 4u; dz++)
                        for (u32 dx = 0; dx < 4u; dx++) sum += ps2_r8(zf.map + (gz * 4u + dz) * 256u + gx * 4u + dx);
                    ln[k++] = "0123456789ABCDEF"[(sum / 16u) >> 4];
                }
                ln[k] = 0;
                ps2_log("cldata: map %02u %s", gz, ln);
            }
    }
    if (f % (u64)zc_dbg) return;
    float el = zc_ok(zf.env) ? zc_rf(zf.env + 0x08u) : 0.0f, az = zc_ok(zf.env) ? zc_rf(zf.env + 0x0Cu) : 0.0f;
    char sun[200] = "";
#ifdef ZC_C1
    static ps2_cloud_env r;
    if (ps2_cloud_env_read(&r) && (r.valid & CL_ENV_SUN))
        snprintf(sun, sizeof sun, " | C1 sun %.3f %.3f %.3f", r.sun_dir[0], r.sun_dir[1], r.sun_dir[2]);
#endif
    ps2_log("cldata: field %llu camy %.0f eye %.0f %.0f %.0f | band %.0f..%.0f t %.3f pr %.3f (set %.3f %s) | list %s n %u kept %u mask %u "
            "seq %u recentres %u origin %.0f %.0f drift %.0f %.0f wind %.1f %.1f m/s alpha %u far %.0f m | base %.0f top %.0f m cover %.3f/%.3f "
            "type %.2f in-cloud %.2f | sun env %.2f/%.2f sky %.2f/%.2f deg dir %.3f %.3f %.3f%s",
            (unsigned long long)f, zf.eye[1], zf.eye[0], zf.eye[1], zf.eye[2], zc_rf(s + 0x10u), zc_rf(s + 0x14u),
            zc_rf(s + 0xB30u), zc_precip, zc_pr_amt, zc_pr_snow > 0.5f ? "snow" : "rain", zf.list ? "live" : "none", zf.n, zc_npub, zf.mask, zc_list_seq, zc_recentres,
            zc_org[0] / zc_upm, zc_org[1] / zc_upm, zc_drift[0] / zc_upm, zc_drift[1] / zc_upm, zf.wind[0] / zc_upm,
            zf.wind[1] / zc_upm, zf.alpha, zf.far / zc_upm, zl.base_m, zl.top_m, zl.coverage, zl.coverage_unmasked, zl.type, zc_in_cloud, el * 57.29578f,
            az * 57.29578f, zc_sun_ang[0] * 57.29578f, zc_sun_ang[1] * 57.29578f, zc_sun[0], zc_sun[1], zc_sun[2], sun);
}

/* after the sky update (its counter bump and precipitation are done): once per game frame */
static int zc_sky_leave(ps2_ctx *ctx, void *user) {
    (void)ctx; (void)user;
    int ok = 0;
    u32 s = ps2_hook_entry_arg(0, &ok);
    if (!ok || !zc_ok(s)) return 0;
    static u64 last;
    u64 f = ps2_kernel_vblank_count();
    if (f == last) return 0;
    last = f;
    zc_read(s);
#if PS2_RQ_SUPPORT
    if (zc_one > 0 && zf.list) {   /* (round 2, PS2_RQ_CLOUDS_ONE_RULE: the coverage's motion every 16 fields -- the build runs only on a re-centre,
                                      10 m of drift or 100 m of camera; a table taken without a history is re-taken once one exists) */
        static u64 hl;
        float s2[2] = { (float)zc_drift[0] + zf.moff[0], (float)zc_drift[1] + zf.moff[1] };
        if (!zc_one_hn || f >= hl + 16u) { u32 k = zc_one_hn++ & 7u; zc_one_hf[k] = f; zc_one_hs[k][0] = s2[0]; zc_one_hs[k][1] = s2[1]; zc_one_hm[k] = zf.map; hl = f; }
        float v[2];
        if (zc_one_vel(f, s2, v)) { zc_one_vl[0] = v[0]; zc_one_vl[1] = v[1]; zc_one_vlf = f; if (zc_one_redo) zc_one_force = 1; }
    }
    {   /* [debug] ACZ_DEBUG_CLOUD_LIST=1 (clouds7): the coverage map's offset (obj +0xE0 / +0xE8) and pointer when they change -- a step
           of them moves the coverage under every body at once (the static list's opacity steps) */
        static int dbg = -1; static float lm[2]; static u32 lmap;
        if (dbg < 0) { const char *e = getenv("ACZ_DEBUG_CLOUD_LIST"); dbg = e && *e == '1'; }
        /* (the offset scrolls ~2 units a field in M10: logged only on a step over 16 units or a new map) */
        if (dbg && zf.list && (fabsf(zf.moff[0] - lm[0]) > 16.0f || fabsf(zf.moff[1] - lm[1]) > 16.0f || zf.map != lmap)) {
            ps2_log("zero-clouds: [debug] field %llu: the coverage map's offset %.0f %.0f -> %.0f %.0f units, map %08X -> %08X",
                    (unsigned long long)f, (double)lm[0], (double)lm[1], (double)zf.moff[0], (double)zf.moff[1], lmap, zf.map);
        }
        lm[0] = zf.moff[0]; lm[1] = zf.moff[1]; lmap = zf.map;
    }
#endif
    zc_update();
#ifdef ZC_C1
    if (zc_on) zc_publish();
#endif
    if (zc_dbg > 0) zc_debug(f);
    return 0;
}

void zero_clouds_init(void) {
    if (ps2_r32(ZC_SKY_UPDATE) != 0x27BDFFD0u || ps2_r32(0x0016A6C0u) != 0x27BDFEE0u) return;   /* not Zero (NTSC-U) */
    const char *e = getenv("ACZ_DEBUG_CLOUD_ENV");
    zc_dbg = e ? atoi(e) : 0;
    const char *u = getenv("ACZ_VR_UNITS_PER_M");
    float v = u ? (float)atof(u) : 6.0f;
    zc_upm = v > 1.0f && v < 20.0f ? v : 6.0f;
    const char *b = getenv("ACZ_VR_CLOUD_BODY");
    float k = b ? (float)atof(b) : 0.675f;
    zc_kbody = k > 0.1f && k <= 1.0f ? k : 0.675f;
    const char *ki = getenv("ACZ_VR_CLOUD_INCLOUD");
    float kv = ki ? (float)atof(ki) : 0.20f;
    zc_kin = kv > 0.01f && kv <= 1.0f ? kv : 0.20f;
    const char *bt = getenv("ACZ_VR_CLOUD_BAND_TOP");
    zc_band_top = bt && *bt ? *bt != '0' : 1;
    const char *gc = getenv("ACZ_VR_CLOUD_GROUND_CLEAR");
    if (gc && *gc) { float c = (float)atof(gc); zc_clear_m = c >= 0.0f && c <= 5000.0f ? c : 680.0f; }
    const char *dr = ps2_menv("ACZ_VR_CLOUD_DRIFT");
    if (dr && *dr) { float d = (float)atof(dr); zc_kdrift = d >= 0.0f && d <= 1.0f ? d : 1.0f; }
    const char *on = getenv("ACZ_VR_CLOUD_DATA");
#ifdef ZC_C1
    zc_on = on ? *on != '0' : 1;          /* on by default: the cloud feel (immersion) reads C1 in_cloud */
#else
    zc_on = 0;
    (void)on;
#endif
    if (!zc_on && zc_dbg <= 0) return;
    int a = ps2_hook_after(ZC_SKY_UPDATE, zc_sky_leave, NULL, 54, "zero-clouds") >= 0;
    ps2_log("zero-clouds: the game's cloud list, sun and weather -> C1 %s (hook %d; %.2f units/m, body %.3f R; "
            "in-cloud ball %.2f S; on the ground the base %.0f m above the camera; wind drift x%.2f; ACZ_VR_CLOUD_DATA=0 turns it off)%s", zc_on ? "on" : "off (log only)", a, zc_upm, zc_kbody, zc_kin, zc_clear_m, zc_kdrift,
            zc_dbg > 0 ? ", ACZ_DEBUG_CLOUD_ENV log" : "");
}
/* the NEXT MISSION tier (ps2_menv.h): the game thread, at the edge */
void zero_clouds_menv_reload(void) {
    const float a = zc_kdrift;
    const char *dr = ps2_menv("ACZ_VR_CLOUD_DRIFT");
    float d = dr && *dr ? (float)atof(dr) : 1.0f;
    zc_kdrift = d >= 0.0f && d <= 1.0f ? d : 1.0f;
    ps2_menv_note("ACZ_VR_CLOUD_DRIFT", a, zc_kdrift);
}
