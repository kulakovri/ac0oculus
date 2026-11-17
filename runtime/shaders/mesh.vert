#version 450
#ifdef MV
#extension GL_EXT_multiview : require
#endif

#if defined(BATCH) && defined(RQT)
#extension GL_EXT_nonuniform_qualifier : require
#endif
#ifdef BATCH
/* PS2_RN_BATCH (quest S3b; the variants mesh_b*.vert.spv, ps2_video.c rnb_*): consecutive native-mesh draws with the
   same state are one indexed draw. Each index carries its draw's record (bits 17..31: the record, 0..16: the vertex
   record of the frame's mesh vertex buffer), so the vertices are read here from that buffer (binding 7: the same
   bytes the vertex attributes read) and the draw's own push-constant head (inv_size .. mesh_pad: its block, texture
   sizes, flags, clamp, fog colour) from its record (binding 8); the rest of the push constants is the batch's. */
vec4  in_pos;
ivec4 in_nrm;
ivec4 in_col;
vec4  in_uv;
layout(location = 12) flat out uint v_rec;
struct MeshHead {
    vec2  inv_size;
    vec2  tex_size;
    uvec4 flags;
    vec4  fogcol;
    uvec4 clamp_uv;
    vec2  img_size;
    float scale;
    float point_size;
    float tex_scale;
    float pad0;
    uint  mesh_block;
    uint  mesh_pad;
    uvec4 tslot;           /* (mesh.frag: the texture's slot in the table) */
};
layout(std430, set = 2, binding = 7) readonly buffer MeshVerts { uvec4 mvx[]; };
layout(std430, set = 2, binding = 8) readonly buffer MeshRecs { MeshHead rec[]; };
#else
layout(location = 0) in vec4  in_pos;
layout(location = 1) in ivec4 in_nrm;
layout(location = 2) in ivec4 in_col;
layout(location = 3) in vec4  in_uv;
#endif

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec3 v_stq;
layout(location = 2) out float v_fog;
layout(location = 3) flat out uint v_round_uv;
layout(location = 4) out vec3 v_terr;      /* realistic terrain: map uv of the relief texture, z = 1 to apply it */
/* shader water (terrain_water.glsl; zero_water.c): tile texel position 0..1, mask layer, class (0 = no water); the
   fragment relative to the centre eye (units) + our clouds' sun transmittance; its world position (units) */
layout(location = 5) out vec4 v_wat;
layout(location = 6) out vec4 v_wrel;
layout(location = 7) out vec3 v_wabs;
/* near-ground materials (zero_terrain_mat.c, terrain_detail.glsl): per vertex the smooth ground's slope (from the
   record's second quadword), the slow strength variation, the sun light gain of the detail slope */
layout(location = 8) out vec4 v_tmat;
/* city facades (zero_props.c, block pad4 bit 30, record quadword 1 = (the building's centre x, z, its base y * 4,
   kind | seed << 8), kind 1 = with windows, 2 = without; the sun's horizontal direction in the block's r[0].xy): the
   fragment's offset from the centre (units), its height above the base (m), kind + seed / 256; v_wabs = its world
   position, v_wrel.xy = the sun (mesh.frag's facade light and windows) */
layout(location = 9) out vec4 v_fac;
/* the city's detailed look (ACZ_TERRAIN_CITY_LOOK, push-constant bit 524288; zero_props.c, bug #261): the walls' and
   roofs' building data -- the wall's top and foot relative to the base (m: the position's w, top x 16 x 2048 + foot
   x 16), the half length and width (m: the record's quadword 1 w bits 16-23 / 24-31, 0.5 m steps) -- and the fragment
   relative to the eye (units; 0 without one). Roofs (kinds 3 flat, 4 pitched) carry their corner's local position along
   the length / width (m x 16) in quadword 1 x / y instead of the building's centre; v_wrel.z = the sun's height */
layout(location = 10) out vec4 v_fac2;
layout(location = 11) out vec3 v_view;
#ifdef RQCUT
#include "rq_cut.glsl"
layout(location = 22) out vec4 v_cut;      /* PS2_RQ_CLOUDS_CUT (rq_cut.glsl, the eye pass's cut_*.vert twins): the merged terrain (rq_terrain.frag
                                              -DRQO): xyz its place in cloud space (m), w the far fade's weight (-1: no cloud applied); the other
                                              natives and the unmerged terrain: rq_cut.glsl rqc_native's code (round 2: per fragment) */
layout(location = 23) flat out float v_cutk;   /* (round 2) the band test, the provoking vertex's: 0 = no cloud can lie in front (rqc_need) */
#endif

out gl_PerVertex {
    vec4 gl_Position;
    float gl_PointSize;
    float gl_ClipDistance[1];
#ifdef ZCULL
    float gl_CullDistance[1];
#endif
};

layout(push_constant) uniform PC {
    vec2  inv_size;
    vec2  tex_size;
    uvec4 flags;
    vec4  fogcol;
    uvec4 clamp_uv;
    vec2  img_size;
    float scale;
    float point_size;
    float tex_scale;
    float pad0;
    uint  mesh_block;
    uint  mesh_pad;
    vec4  rp[4];           /* reprojection to another camera (see gs.vert) */
    vec4  rp2;             /* x: fraction of a game frame to extrapolate objects with their own motion; z: 1 = static
                              ground without a match (3) takes the matched path (bug #39, ps2_video.c) */
    vec4  fogr;            /* radial fog (ACZ_VR_FOG_RADIAL, ps2_video.c fogr_*): the game frame's camera cx, cy, fx, fy
                              (GS pixels) for head-tracked world meshes; fx = 0: the game's view-depth fog */
#if defined(RQ) || defined(RQT)
    vec4  rq_pad0, rq_pad1;   /* (soft, fxv of gs_push) */
    vec4  rqx;                /* R1 (NOTES_RQ): the per-view crop (gs.vert) */
    vec2  rqy;
    vec2  rqsz;
#endif
#ifdef BATCH
} pcu;
struct MeshPC {            /* (the draw's head from its record, the batch's tail from the push constants) */
    vec2  inv_size;
    vec2  tex_size;
    uvec4 flags;
    vec4  fogcol;
    uvec4 clamp_uv;
    vec2  img_size;
    float scale;
    float point_size;
    float tex_scale;
    float pad0;
    uint  mesh_block;
    uint  mesh_pad;
    vec4  rp[4];
    vec4  rp2;
    vec4  fogr;
};
MeshPC pc;
#else
} pc;
#endif
/* A world vertex at screen (x, y) with inverse view depth iw, seen from another camera: view direction at unit depth
   d = ((x - cx) / fx, -(y - cy) / fy, -1); new view point (scaled by iw) p = R d + t iw. Returns the new screen
   position and the depth ratio w / w' (for perspective texture coordinates). */
vec3 reproject(float x, float y, float iw) {
    vec3 d = vec3((x - pc.rp[0].w) / pc.rp[2].w, -(y - pc.rp[1].w) / pc.rp[3].w, -1.0);
    vec3 p = vec3(dot(pc.rp[0].xyz, d), dot(pc.rp[1].xyz, d), dot(pc.rp[2].xyz, d)) + pc.rp[3].xyz * iw;
    float pz = -p.z;
    if (pz < 1e-4) return vec3(x, y, 1.0);
    return vec3(pc.rp[0].w + pc.rp[2].w * p.x / pz, pc.rp[1].w - pc.rp[3].w * p.y / pz, 1.0 / pz);
}


struct Block {
    vec4  m[4];
    vec4  c[4];
    vec4  l[3];
    vec4  r[4];
    vec4  e[3];
    float fog0, fog1, zscale, zmax;
    float xoff, yoff, spec_alpha, xr_k;
    uint  mode, pass, clip, pad1;
    uint  prog;
    float xr_wmin;
    uint  xr_reproj, pad4;
    vec4  p[4];
    vec4  mp[4];
};

layout(std430, set = 2, binding = 0) readonly buffer Blocks { Block b[]; };
/* realistic terrain (zero_terrain.c): one light ratio per world texel over the map, 128 = 1, mipmapped */
layout(set = 2, binding = 1) uniform sampler2D relief;

/* Cloud shadows on the ground (ACZ_VR_CLOUD_SHADOW_GROUND; ps2_video.c gsh_*): a copy of the shown cloud shadow map
   of our clouds (ps2_clouds_shadow.c, lookup cloud_shadow.glsl; LINEAR + REPEAT, toroidal) made at the end of the
   previous command buffer, and its parameters in the reserved block GSH_BLOCK (= ps2_video.c MESH_GSHADOW_BLOCK):
   p[0..2] = s0, s1, s2 (ps2_clouds_shadow_ubo, as the hook returns them); p[3] = the centre eye they were made for, in
   world metres (xyz), and 1 / units per metre; c[0] = strengths k (x terrain, z objects; w the debug view), c[1] =
   the give-back n of the area's mean shadow (same order); m[0..2] = the rows of the inverse of the game camera's
   world -> screen x/y/w rows, m[3].xyz = their offsets (models: screen -> world). Push-constant bit 2048 of mesh_pad:
   on for this frame's native meshes. Per vertex (78 m map texels), before the game's fog, world-fixed and the same in
   both eyes. */
#include "cloud_shadow.glsl"
layout(set = 2, binding = 2) uniform sampler2D cl_ground_map;
#define GSH_BLOCK 16382u
#ifdef RQ
/* PS2_RQ_GSHADOW_FILTER (rq10_artefacts 2026-11-15, A5 of prompt 11, BUGS_RQ R-160; the Quest eye pass's variants only): the ground's cloud
   shadow is read per VERTEX, and the terrain's vertices lie a few map texels (78-85 m) apart: one vertex that hits a small gap in the
   map lights the whole fan of triangles round it -- a bright blotch with soft branches along the triangles' edges (the user's M10 flight
   on t13a, 289 s). c[2].x = the filter's reach (m, 0 = one read as before): the centre and four taps at +-reach along x and z (weights
   0.4 / 0.15 each), so a vertex sees the shadow as wide as the vertices' spacing */
float gsh_t(vec3 pm) {
    vec4 s0 = b[GSH_BLOCK].p[0], s1 = b[GSH_BLOCK].p[1], s2 = b[GSH_BLOCK].p[2];
    float t = cl_sun_shadow_map(cl_ground_map, s0, s1, s2, pm), r = b[GSH_BLOCK].c[2].x;
    if (r > 0.0)
        t = 0.4 * t + 0.15 * (cl_sun_shadow_map(cl_ground_map, s0, s1, s2, pm + vec3(r, 0.0, 0.0)) + cl_sun_shadow_map(cl_ground_map, s0, s1, s2, pm - vec3(r, 0.0, 0.0))
                            + cl_sun_shadow_map(cl_ground_map, s0, s1, s2, pm + vec3(0.0, 0.0, r)) + cl_sun_shadow_map(cl_ground_map, s0, s1, s2, pm - vec3(0.0, 0.0, r)));
    return t;
}
#endif
/* light factor at a game-world point (units): min(1, (1 - k (1 - T)) n), T = the sun's transmittance through our
   clouds there */
float gsh_light(vec3 pw, float k, float n) {
    vec4 o = b[GSH_BLOCK].p[3];
#ifdef RQ
    float t = gsh_t(pw * o.w - o.xyz);
#else
    float t = cl_sun_shadow_map(cl_ground_map, b[GSH_BLOCK].p[0], b[GSH_BLOCK].p[1], b[GSH_BLOCK].p[2],
                                pw * o.w - o.xyz);
#endif
    return min(1.0, (1.0 - k * (1.0 - t)) * n);
}
/* a model vertex's world position from its game-frame screen position s (x, y, w rows; no division) */
vec3 gsh_world(vec4 s) {
    vec3 d = vec3(s.x, s.y, s.w) - b[GSH_BLOCK].m[3].xyz;
    return vec3(dot(b[GSH_BLOCK].m[0].xyz, d), dot(b[GSH_BLOCK].m[1].xyz, d), dot(b[GSH_BLOCK].m[2].xyz, d));
}
/* ACZ_DEBUG_GSHADOW_TINT=2: a smooth world pattern (8192-unit period; per vertex it must stay well sampled): an object
   has the shade of the ground under it if its reconstructed position is right */
float gsh_check(vec3 pw) { vec2 a = pw.xz * (6.2831853 / 8192.0); return 0.55 + 0.45 * sin(a.x) * sin(a.y); }
/* the factor of a model vertex: pad4 bit 0 = a ground object, bit 1 = the part program (debug tint only);
   c[0].w = ACZ_DEBUG_GSHADOW_TINT (1: ground objects red, parts blue; 2: the world pattern) */
vec3 gsh_object(vec4 s, uint f) {
    vec4 gk = b[GSH_BLOCK].c[0];
    if (gk.w > 1.5) return vec3((f & 1u) != 0u ? gsh_check(gsh_world(s)) : 1.0);
    if (gk.w > 0.5) return (f & 1u) != 0u ? vec3(1.0, 0.35, 0.35) : vec3(0.35, 0.35, 1.0);
    return vec3((f & 1u) != 0u ? gsh_light(gsh_world(s), gk.z, b[GSH_BLOCK].c[1].z) : 1.0);
}

/* terrain detail textures (passes 1, 2: subtracted from the atlas colour, the game's fine ground pattern): the game
   fades them linearly from the eye to 0 at its distance D (inv = 1/D; 9140 units in missions 2-4). ACZ_TERRAIN_DETAIL_HOLD
   (rn_vp_terrain.c, block pad4 bits 16-23 = h x 255; 0 = the game's fade) keeps them at full strength over the first
   share h of D and fades over the rest, still to 0 at D (the game draws the passes only on its near patches) */
float detail_fade(float D, float inv, float dw, uint pad4) {
    float h = float((pad4 >> 16) & 255u) / 255.0;
    if (h <= 0.0) return clamp((D - dw) * inv * 128.0, 0.0, 128.0);
    return 128.0 * (1.0 - clamp((dw * inv - h) / (1.0 - h), 0.0, 1.0));
}

/* the fog distance of a game-frame screen position s (x, y, w rows; no division): the game's view depth w, or with
   pc.fogr the distance from the eye |view position| = |((x - cx w) / fx, (y - cy w) / fy, w)|, which head turns do
   not change (in VR the view depth of a mountain is its distance times the cosine of its angle off the view axis) */
float fog_dist(vec4 s) {
    if (pc.fogr.z <= 0.0) return s.w;
    return length(vec3((s.x - pc.fogr.x * s.w) / pc.fogr.z, (s.y - pc.fogr.y * s.w) / pc.fogr.w, s.w));
}

#ifdef ZCULL
/* PS2_PERF_TERRAIN_ZCULL (perf5_terrain; with it on, every native mesh draws with this variant, mesh_zc*.vert.spv, so the
   terrain's passes keep one vertex program). Push-constant bit 1 << 27 of mesh_pad (ps2_video.c tzc_gate) marks the
   terrain's overlay passes 1-3 whose blend leaves the target as it is where the source colour is 0 (D = Cd, A / B = Cs
   and 0; no depth or alpha write). zc_zero: this vertex's colour makes the fragment shaders' texture function give rgb
   0 whatever the texel (MODULATE t x c, untextured c x 128/255: c.rgb <= 0; HIGHLIGHT t x c + a x 128/255: also
   a <= 0; DECAL never), no GS fog. A triangle of three such vertices interpolates exactly 0 everywhere, so every
   fragment of it would leave the frame unchanged: gl_CullDistance < 0 at all three culls it before rasterisation (a
   cull distance never clips a triangle that has one vertex >= 0) */
bool zc_zero(vec4 c) {
    uint f = pc.flags.x;
    if (((f >> 16) & 1u) != 0u) return false;                       /* (GS fog: the fog colour comes in) */
    if (!(c.r <= 0.0 && c.g <= 0.0 && c.b <= 0.0)) return false;     /* (NaN: kept) */
    if (pc.flags.y == 0u || (f & 3u) == 0u) return true;            /* untextured, MODULATE */
    return (f & 3u) != 1u && c.a <= 0.0;                            /* HIGHLIGHT, HIGHLIGHT2; DECAL never */
}
#endif

#if defined(RQ) || defined(RQT)
#ifdef BATCH
#define RQPC pcu
#else
#define RQPC pc
#endif
#endif
#ifdef RQ
/* PS2_XR_LATE_POSE (rq_late.inc): the eye pass from a pose located right before it: the four rows of reproject in the reserved block RQ_LATE_BLOCK (= MESH_MAX_BLOCKS - 9), mesh_pad bit 20 */
#define RQ_LATE_BLOCK 16375u
#define RQ_LATE_BIT 1048576u
vec3 rq_late(float x, float y, float iw) {
    vec4 r0 = b[RQ_LATE_BLOCK].m[0], r1 = b[RQ_LATE_BLOCK].m[1], r2 = b[RQ_LATE_BLOCK].m[2], r3 = b[RQ_LATE_BLOCK].m[3];
    vec3 d = vec3((x - r0.w) / r2.w, -(y - r1.w) / r3.w, -1.0);
    vec3 q = vec3(dot(r0.xyz, d), dot(r1.xyz, d), dot(r2.xyz, d)) + r3.xyz * iw;
    float pz = -q.z;
    if (pz < 1e-4) return vec3(x, y, 1.0);
    return vec3(r0.w + r2.w * q.x / pz, r1.w - r3.w * q.y / pz, 1.0 / pz);
}
#define RQ_XF(P) { float rqA = gl_ViewIndex == 0 ? RQPC.rqx.x : RQPC.rqx.z, rqB = gl_ViewIndex == 0 ? RQPC.rqx.y : RQPC.rqx.w; \
                   vec4 rq0 = P; P.x = rqA * rq0.x + rqB * rq0.w; P.y = RQPC.rqy.x * rq0.y + RQPC.rqy.y * rq0.w; }
#else
#define RQ_XF(P)
#endif
#ifdef RQT
/* R2a (NOTES_RQ): the terrain's detail passes folded into its first pass: pass 1 / 2 uv, pass 3 uv + the fades */
layout(location = 13) out vec4 v_rqt;
layout(location = 14) out vec4 v_rq3;
layout(location = 15) out float v_rqf3;
/* PS2_RQ_HAZE (rq_order.inc): the sky / haze table: the blocks as an array of vec4 (a block = 30 vec4) */
layout(std430, set = 2, binding = 0) readonly buffer BlocksQ { vec4 bqw[]; };
#define RQO_HZ_BLOCK 16374u
layout(location = 16) out vec4 v_wa;              /* PS2_RQ_WATER: the two wave layers' uv */
layout(location = 17) out vec4 v_wb;              /* ... the body: 1 / (1 - k0), k0 sky0 / (1 - k0) */
layout(location = 18) flat out vec4 v_uvc;        /* the tile's uv clamp */
layout(location = 19) out vec4 v_ma;              /* PS2_RQ_MAT (rq_terrain.frag S_MAT) */
layout(location = 20) out vec2 v_mb;
layout(location = 21) flat out vec3 v_mc;
#ifdef BATCH
layout(set = 3, binding = 0) uniform sampler2DArray texs_v[1024];   /* (the atlas texel for the classification) */
#endif
layout(set = 2, binding = 4) uniform sampler2D wat_waves_v;      /* PS2_RQ_WFAR: the wave texture's mean */
layout(set = 4, binding = 2) uniform sampler2DArray rqt_l3_v;   /* PS2_RQ_L3V: pass 3's layer read per vertex */
#endif
void main() {
#ifdef RQT
    v_rqt = vec4(0.0); v_rq3 = vec4(0.0); v_rqf3 = 0.0;
    v_ma = vec4(0.0); v_mb = vec2(0.0); v_mc = vec3(0.0);
    v_wa = vec4(0.0); v_wb = vec4(0.0); v_uvc = vec4(0.0);
    float rq_tfw = 0.0;                           /* PS2_RQ_TFADE (rq_order.inc rqtd_*): the far fade's weight towards H at this vertex */
    bool rq_tfdbg = false;
#endif
#ifdef BATCH
    {
        uint rv = uint(gl_VertexIndex), vi = (rv & 0x1FFFFu) * 4u;
        v_rec = rv >> 17;
        MeshHead h = rec[v_rec];
        pc.inv_size = h.inv_size; pc.tex_size = h.tex_size; pc.flags = h.flags; pc.fogcol = h.fogcol;
        pc.clamp_uv = h.clamp_uv; pc.img_size = h.img_size; pc.scale = h.scale; pc.point_size = h.point_size;
        pc.tex_scale = h.tex_scale; pc.pad0 = h.pad0; pc.mesh_block = h.mesh_block; pc.mesh_pad = h.mesh_pad;
        pc.rp = pcu.rp; pc.rp2 = pcu.rp2; pc.fogr = pcu.fogr;
        in_pos = uintBitsToFloat(mvx[vi]);
        in_nrm = ivec4(mvx[vi + 1u]);
        in_col = ivec4(mvx[vi + 2u]);
        in_uv = uintBitsToFloat(mvx[vi + 3u]);
    }
#endif
    Block k = b[pc.mesh_block];
    vec3 p = in_pos.xyz;
    vec4 s = k.m[0] * p.x + k.m[1] * p.y + k.m[2] * p.z + k.m[3];
    vec4 sg = s;                              /* (the game frame's screen position: cloud shadows on objects) */
    /* 3: static world without a match, given the camera motion of the frame's terrain as its previous matrix
       (ps2_video.c xr_unify_static, bug #39): the matched path while rp2.z is set (PS2_XR_REPROJ_UNIFY), else 1 */
    uint rk = k.xr_reproj == 3u ? (pc.rp2.z > 0.5 ? 2u : 1u) : k.xr_reproj;
    if ((pc.mesh_pad & 256u) != 0u && rk == 2u) {
        /* frames between game frames: the object continues its own motion relative to the camera */
        vec4 s0 = k.mp[0] * p.x + k.mp[1] * p.y + k.mp[2] * p.z + k.mp[3];
        s += pc.rp2.x * (s - s0);
    }
    float w = s.w;

    float X = (s.x + (0.5 - k.xoff) * w) * pc.inv_size.x - w;
    float Y = (s.y + (0.5 - k.yoff) * w) * pc.inv_size.y - w;
    float zlim = min(16777215.0, k.zmax);
    float Z = min(max(s.z * k.zscale, 0.0), zlim * max(w, 0.0)) / k.zmax;
    /* a vertex behind the eye (w <= 0; push-constant bit 16384, PS2_MESH_BEHIND_Z): its homogeneous GS depth s.z is
       kept as it is (K - C w, linear in clip space) instead of being clamped to 0, so the GPU's clipping of a triangle
       that reaches behind the eye interpolates the true depth up to the eye plane. With the clamp the depth of such a
       triangle sank toward the far end over its whole near part: big ground pieces (the airfield's apron and grass
       quads, 2026-10-23 bug #93) lost the depth test against the terrain whenever they reached behind the eye, so they
       showed when the head looked across them and vanished when it looked along them. */
    if ((pc.mesh_pad & 16384u) != 0u && w <= 0.0) Z = s.z * k.zscale / k.zmax;
    /* stereo: the eye shift of the GS path, -xr_k / max(w, xr_wmin) PS2 pixels (xr_k = m00*h of the eye
       projection, times the cockpit scale for the cockpit), applied in clip space; the left eye (pc.pad0 < 0, or
       view 0) shifts by -dx, the right one by +dx */
#ifdef MV
    float eye = (pc.mesh_pad & 4u) != 0u ? (gl_ViewIndex == 0 ? -1.0 : 1.0) : 0.0;
#else
    float eye = pc.pad0;
#endif
    /* (ACZ_REC_CINE, push bit 131072: the recording's cinematic camera sees every head-tracked mesh -- the world, the
       matched objects, the own aircraft -- from where it is, by its exact w; ps2_video.c cine_*) */
    bool cine = (pc.mesh_pad & 131072u) != 0u && k.xr_k > 0.0;
    if ((pc.mesh_pad & 256u) != 0u && (rk == 1u || cine) && w > 1e-3) {
        /* frames between game frames: the world seen from the extrapolated camera */
        vec3 r = reproject(X / w * (1.0 / pc.inv_size.x) + 1.0 / pc.inv_size.x,
                           Y / w * (1.0 / pc.inv_size.y) + 1.0 / pc.inv_size.y, 1.0 / w);
        float w2 = w / r.z;
        X = (r.x * pc.inv_size.x - 1.0) * w2;
        Y = (r.y * pc.inv_size.y - 1.0) * w2;
        Z = Z / w * w2;
        w = w2;
    }
#ifdef RQ
    if ((pc.mesh_pad & RQ_LATE_BIT) != 0u && k.xr_k > 0.0 && w > 1e-3) {    /* (PS2_XR_LATE_POSE: every tracked mesh, as ACZ_REC_CINE's path) */
        vec3 r = rq_late(X / w * (1.0 / pc.inv_size.x) + 1.0 / pc.inv_size.x, Y / w * (1.0 / pc.inv_size.y) + 1.0 / pc.inv_size.y, 1.0 / w);
        float w2 = w / r.z;
        X = (r.x * pc.inv_size.x - 1.0) * w2;
        Y = (r.y * pc.inv_size.y - 1.0) * w2;
        Z = Z / w * w2;
        w = w2;
    }
#endif
    if (eye != 0.0 && k.xr_k > 0.0)
        X -= eye * k.xr_k * clamp(w / k.xr_wmin, 0.0, 1.0) * pc.inv_size.x;
    gl_Position = vec4(X, Y, Z, w);
#ifdef MV
    if ((pc.mesh_pad & 262144u) != 0u && gl_ViewIndex == 1) gl_Position = vec4(-4.0, -4.0, 0.5, 1.0);   /* (ACZ_REC_CINE_MONO) */
#endif
    RQ_XF(gl_Position);
#ifdef RQCUT
    /* PS2_RQ_CLOUDS_CUT (after the position is final: round 2, R-197 -- placed before it, the twin's compiled position differed by a rounding
       at the far terrain's clamped depth 0 and the dome no longer tied with it: a saw of pale far cells above the cloud, M10 f3565): the vertex's
       place from the game frame's screen position (the target pixel and its view depth, as the GS path's
       x, y and z), the cloud in front of it per vertex, or the place for the fragment (the merged terrain) */
    v_cut = vec4(0.0, 0.0, 0.0, -1.0);                              /* (nothing, in every fragment path's code) */
    v_cutk = 0.0;
    bool rqc_pf = false;
    highp vec4 rqc_full = vec4(0.0, 0.0, 0.0, 1.0);                    /* (the whole ray: the far fade's share, below) */
#ifdef RQT
    rqc_pf = k.prog == 1u && (pc.mesh_pad & 2097152u) != 0u;
#endif
    if ((pc.mesh_pad & (RQC_TL_BIT | RQC_T_BIT)) != 0u && k.prog < 4u) {   /* (any w: a vertex behind the eye too -- its triangle's fragments in front
                                                                               interpolate a place on the triangle's plane) */
        highp vec3 rqc_p = rqc_hom_cloud(vec3(sg.x + (0.5 - k.xoff) * sg.w, sg.y + (0.5 - k.yoff) * sg.w, sg.w));
        uint rqc_parts = uint(RQC_Q(5).y + 0.5);
        v_cutk = rqc_need(rqc_p, k.prog == 1u ? 400.0 : 100.0) ? 1.0 : 0.0;   /* (round 2, R-195: the terrain's triangles up to 400 m tall) */
        if (rqc_pf && (rqc_parts & 16u) != 0u) {                      /* ([test] PS2_RQ_CLOUDS_CUT_PARTS bit 16: the merged terrain per vertex too:
                                                                         (L_d, T_d + 2), rq_terrain.frag -DRQO tells it by w > 1.5) */
            v_cut = rqc_front(rqc_p, rqc_full); v_cut.w += 2.0;
        } else if (!rqc_pf && (rqc_parts & 32u) != 0u) {              /* ([test] bit 32: the other natives per vertex, round 1's way: (L_d, T_d + 4)) */
            v_cut = rqc_vertex(pc.mesh_pad, rqc_p); v_cut.w += 4.0;
        } else v_cut = vec4(rqc_p, rqc_pf || (pc.mesh_pad & RQC_TL_BIT) != 0u ? 0.0 : 2.0);   /* (round 2: per fragment, R-196) */
    }
#endif

    vec4 cc = k.c[0] * p.x + k.c[1] * p.y + k.c[2] * p.z + k.c[3];
    gl_ClipDistance[0] = k.clip != 0u ? cc.z + cc.w : 1.0;
#ifdef ZCULL
    gl_CullDistance[0] = 1.0;
#endif

    float fog = clamp(k.fog0 + (pc.fogr.z > 0.0 ? fog_dist(s) : w) * k.fog1, 0.0, 255.0);
    v_fog = fog / 255.0;
    v_round_uv = 0u;
    v_terr = vec3(0.0);
    v_wat = vec4(0.0); v_wrel = vec4(0.0); v_wabs = vec3(0.0); v_tmat = vec4(0.0); v_fac = vec4(0.0);
    v_fac2 = vec4(0.0); v_view = vec3(0.0);
    gl_PointSize = pc.point_size;

    if (k.prog == 5u) {
        gl_Position = vec4(in_pos.x * pc.inv_size.x - 1.0,
                           in_pos.y * pc.inv_size.y - 1.0, in_pos.z, 1.0);
        RQ_XF(gl_Position);
        gl_ClipDistance[0] = 1.0;
        v_stq = in_uv.xyz;
        v_color = vec4(in_col) / 128.0;
        v_fog = in_uv.w;
        v_round_uv = uint(in_nrm.x);
        return;
    }

    if (k.prog == 3u) {
        float iw = 1.0 / w;
        vec3 c = s.xyz * iw;
#ifdef RQ
        if ((pc.mesh_pad & RQ_LATE_BIT) != 0u && k.xr_k > 0.0 && s.w > 1e-3) {   /* (the billboard's centre moves with the view) */
            vec3 r = rq_late(c.x + 0.5 - k.xoff, c.y + 0.5 - k.yoff, 1.0 / s.w);
            c.xy = r.xy - vec2(0.5 - k.xoff, 0.5 - k.yoff);
        }
#endif
        vec2 corner = c.xy + vec2(in_nrm.xy) * k.p[0].xy * in_uv.z * iw;
        float zg = clamp(floor(floor(c.z * 16.0) / 8.0), k.p[0].z, zlim);
        gl_Position = c.z > 0.0
            ? vec4((corner.x + 0.5 - k.xoff) * pc.inv_size.x - 1.0,
                   (corner.y + 0.5 - k.yoff) * pc.inv_size.y - 1.0, zg / k.zmax, 1.0)
            : vec4(2.0, 2.0, 2.0, 1.0);
        RQ_XF(gl_Position);
        gl_ClipDistance[0] = 1.0;
        v_stq = vec3(in_uv.xy, 1.0);
        v_color = vec4(in_col & 255) / 128.0;
        v_fog = 1.0;
        return;
    }

    if (k.prog == 4u) {
        v_stq = in_uv.xyz;
        v_color = vec4(in_col & 255) / 128.0;
        return;
    }

    if (k.prog == 2u) {
        if (k.pass == 0u) {
            v_stq = vec3(in_uv.xy, 1.0);
            v_color = vec4(in_col & 255) / 128.0;
            if ((pc.mesh_pad & 2048u) != 0u && (k.pad4 & 3u) != 0u)      /* cloud shadows: ground objects */
                v_color.rgb *= gsh_object(sg, k.pad4);
        } else {
            v_stq = vec3(in_uv.xy * 2.0, 1.0);
            v_color = vec4(vec3(clamp((k.p[0].z - cc.w) * k.p[0].w * 128.0, 0.0, 128.0)),
                           128.0) / 128.0;
        }
        return;
    }

    if (k.prog == 1u) {
        vec2 uv;
        vec4 col;
        /* the detail textures fade with the view depth (the game's), or in VR with the distance from the eye
           (pad1 bit 0, eye in p[3].yzw: rn_vp_terrain.c ACZ_VR_TERRAIN_MORPH) so they do not slide with head turns */
        float dw = (k.pad1 & 1u) != 0u ? length(p - k.p[3].yzw) : cc.w;
        if (k.pass == 0u) {
            uv = in_uv.xy + in_uv.zw;
            col = vec4(in_col & 255);
#ifdef RQT
            {   uint rqf = (k.pad1 >> 8) & 7u;                 /* (the patch's pass bits: rn_vp_terrain.c) */
                v_rqt = vec4(in_uv.xy * 16.0, in_uv.xy * 16.0 + k.p[1].xy);
                v_rq3 = vec4((p.xz - k.p[2].xz) * k.p[2].w * RQPC.rqsz,                  /* (the layer's rectangle in its image: rqsz) */
                             (rqf & 1u) != 0u ? detail_fade(k.p[0].z, k.p[0].w, dw, k.pad4) / 128.0 * float((k.pad1 >> 16) & 255u) / 128.0 : 0.0,
                             (rqf & 2u) != 0u ? detail_fade(k.p[1].z, k.p[1].w, dw, k.pad4) / 128.0 * float((k.pad1 >> 24) & 255u) / 128.0 : 0.0);
                v_rqf3 = (rqf & 4u) != 0u ? fog * 0.5 / 128.0 * k.spec_alpha : 0.0;
            }
            if ((pc.mesh_pad & 2097152u) != 0u) {          /* PS2_RQ_HAZE (rq_order.inc): the haze ring's closed form, v_fac2 = H, 1 inside the band; v_view = the distance vector / step;
                                                              v_fac.x = base / step (rq_terrain.frag -DRQO) */
                uint hb = RQO_HZ_BLOCK * 30u;
                vec4 ht0 = bqw[hb], ht1 = bqw[hb + 1u];
                vec3 hd = p - ((k.pad1 & 1u) != 0u ? k.p[3].yzw : vec3(0.0));
                float el = atan(hd.y, length(hd.xz));
                int nr = int(ht0.z + 0.5);
                vec3 hc = bqw[hb + 2u].rgb;
                float elp = bqw[hb + 2u].w;
                if (el < elp) {
                    for (int j = 1; j < nr; j++) {
                        vec4 rr = bqw[hb + 2u + uint(j)];
                        if (el >= rr.w) { hc = mix(rr.rgb, hc, (el - rr.w) / max(elp - rr.w, 1e-6)); break; }
                        hc = rr.rgb; elp = rr.w;
                    }
                }
                vec3 hv = pc.fogr.z > 0.0 ? vec3((s.x - pc.fogr.x * s.w) / pc.fogr.z, (s.y - pc.fogr.y * s.w) / pc.fogr.w, s.w) : vec3(0.0, 0.0, s.w);
                v_fac2 = vec4(hc, el >= ht1.x && el <= ht1.y ? 1.0 : 0.0);
                v_terr.y = length(hv) / ht0.y;
                v_terr.z = ht0.x / ht0.y;
                if (ht1.w != 0.0) {                    /* PS2_RQ_TFADE: from row 1's z (units) over 1 / |w|, a smoothstep, at every elevation (w < 0: debug red) */
                    float tq = clamp((length(hv) - ht1.z) * abs(ht1.w), 0.0, 1.0);
                    rq_tfw = tq * tq * (3.0 - 2.0 * tq);
                    rq_tfdbg = ht1.w < 0.0;
#ifdef RQCUT
                    if (rqc_pf && v_cut.w > 1.5 && RQC_Q(5).x < 1.5) v_cut = vec4(mix(v_cut.rgb, rqc_full.rgb, rq_tfw), mix(v_cut.w - 2.0, rqc_full.a, rq_tfw) + 2.0);
                    else if (rqc_pf && v_cut.w > -0.5 && v_cut.w < 1.5) v_cut.w = rq_tfw;   /* (PS2_RQ_CLOUDS_CUT: the share faded into H takes the whole ray, as the sky beside it) */
#endif
                    v_terr.z = -v_terr.z;               /* (the sign tells rq_terrain.frag: the ring's 8 layers continuous, no steps) */
                }
            }
#endif
            /* (debug ACZ_DEBUG_TERRAIN_TEX_TINT: the game's fixed mip level 0 red, 1 green; detail passes blue) */
            if ((k.pad4 & 0x700u) != 0u)
                col.rgb *= ((k.pad4 & 0x100u) != 0u ? vec3(1.4, 0.7, 0.7) : vec3(0.7, 1.4, 0.7))
                         * ((k.pad4 & 0x400u) != 0u ? vec3(0.8, 0.8, 1.5) : vec3(1.0));
            /* realistic terrain (zero_terrain.c; push-constant bit 512 from ps2_video.c relief_apply): the relief
               texture covers the map (513 x 513 samples 1024 units apart from -262144). Per vertex (default): read
               prefiltered to the vertex spacing (mip 3 = 2048 units; mip 4 from the geomorph distance on, so the fine
               patches' middle vertices agree with their coarse neighbours) and multiplied into the vertex colour,
               which pass 0 modulates the atlas texel with: no fragment cost. Bit 1024 (ACZ_TERRAIN_PIXEL): per pixel
               in mesh.frag, full detail */
            if ((pc.mesh_pad & 512u) != 0u) {
                vec2 ruv = (p.xz + 262144.0) * (1.0 / 524288.0);
                if ((pc.mesh_pad & 1024u) != 0u) {
                    v_terr = vec3(ruv, 1.0);
                } else {
                    float lod = 3.0 + smoothstep(16384.0, 32768.0, dw);
                    col.rgb *= textureLod(relief, ruv, lod).r * (255.0 / 128.0);
                }
            }
            /* cloud shadows (bit 2048; the game's water tiles 0x800 are terrain too: the same strength) */
            if ((pc.mesh_pad & 2048u) != 0u) {
                vec4 gk = b[GSH_BLOCK].c[0], gn = b[GSH_BLOCK].c[1];
                if (gk.w > 2.5) col.rgb = vec3(128.0 * gsh_light(p, 1.0, 1.0));     /* (debug 3: T itself) */
                else col.rgb *= gk.w > 1.5 ? gsh_check(p) : gsh_light(p, gk.x, gn.x);
            }
            /* near-ground materials (block pad4 bit 24, zero_terrain_mat.c): a patch near the eye (the eye in e[2], as
               for the shader water); the fragment relative to the eye, its world position; per vertex the smooth
               ground's slope, the slow strength variation and the sun light gain of the detail slope (sky share 0.45,
               our cloud shadow, the smooth ground's own light) */
            if ((k.pad4 & 0x1000000u) != 0u) {
                float tsun = 1.0;
                if ((pc.mesh_pad & 2048u) != 0u) {
                    vec4 o = b[GSH_BLOCK].p[3];
#ifdef RQ
                    tsun = gsh_t(p * o.w - o.xyz);
#else
                    tsun = cl_sun_shadow_map(cl_ground_map, b[GSH_BLOCK].p[0], b[GSH_BLOCK].p[1], b[GSH_BLOCK].p[2],
                                             p * o.w - o.xyz);
#endif
                }
                vec2 g = vec2(in_nrm.xy) * (1.0 / 4096.0);              /* the smooth ground's slope */
                vec3 L = k.l[0].xyz;                                     /* towards the sun */
                float ly = max(L.y, 0.25), sunk = 0.55 * tsun * smoothstep(0.0, 0.08, L.y);
                float ndl = max(dot(vec3(-g.x, 1.0, -g.y), L) * inversesqrt(1.0 + dot(g, g)), 0.0);
                float gain = sunk / ly * smoothstep(0.0, 0.15, ndl) / max(0.45 + sunk * ndl / ly, 0.2);
                vec2 vq = p.xz * (1.0 / 5400.0), vi = floor(vq), vf = vq - vi;   /* slow variation (~1 km) */
                vf = vf * vf * (3.0 - 2.0 * vf);
                uvec4 hq = (uvec4(ivec4(vec4(vi.x, vi.x + 1.0, vi.x, vi.x + 1.0))) * 1597334673u)
                         ^ (uvec4(ivec4(vec4(vi.y, vi.y, vi.y + 1.0, vi.y + 1.0))) * 3812015801u);
                vec4 hv = vec4((hq * 1597334673u) >> 8u) * (1.0 / 16777216.0);
                float vn = mix(mix(hv.x, hv.y, vf.x), mix(hv.z, hv.w, vf.x), vf.y);
                v_wrel = vec4(p - k.e[2].xyz, tsun);
                v_wabs = p;
                v_tmat = vec4(length(g), 0.75 + 0.5 * vn, gain, 0.0);
            }
            int fk = in_nrm.w & 255;
            bool clook = (pc.mesh_pad & 524288u) != 0u;
            if ((k.pad4 & 0x40000000u) != 0u && (fk == 1 || fk == 2)) {   /* a city facade (zero_props.c) */
                v_fac = vec4(p.x - float(in_nrm.x), p.z - float(in_nrm.y), (p.y - float(in_nrm.z) * 0.25) * (1.0 / 5.12),
                             float(fk) + float((in_nrm.w >> 8) & 255) * (1.0 / 256.0));
                v_wabs = p;
                v_wrel = vec4(k.r[0].xy, clook ? k.r[0].w : 0.0, 0.0);
            } else if (clook && (k.pad4 & 0x40000000u) != 0u && (fk == 3 || fk == 4)) {   /* a city roof (detailed look) */
                v_fac = vec4(vec2(in_nrm.xy) * (1.0 / 16.0), (p.y - float(in_nrm.z) * 0.25) * (1.0 / 5.12),
                             float(fk) + float((in_nrm.w >> 8) & 255) * (1.0 / 256.0));
                v_wabs = p;
                v_wrel = vec4(k.r[0].xy, k.r[0].w, 0.0);
            }
            if (clook && v_fac.w > 0.5) {
                float tq = floor(in_pos.w * (1.0 / 2048.0));
                v_fac2 = vec4(tq * (1.0 / 16.0), (in_pos.w - tq * 2048.0) * (1.0 / 16.0),
                              float((in_nrm.w >> 16) & 255) * 0.5, float((in_nrm.w >> 24) & 255) * 0.5);
                v_view = (k.pad1 & 1u) != 0u ? p - k.p[3].yzw : vec3(0.0);
            }
            /* T5 terrain pack (push-constant bit 32768, ps2_video.c tp_frame; zero_texpack.c marks the block: e[0] = the
               tile's uv corner and 1 / extent, e[1].x = layer, e[1].z = 1): the tile uv and layer for the -DTEXPACK
               fetch in mesh.frag (v_wat.w stays the water class, 0 on land) */
            if ((pc.mesh_pad & 32768u) != 0u && k.e[1].z > 0.5)
                v_wat = vec4((in_uv.xy - k.e[0].xy) * k.e[0].zw, k.e[1].x, (pc.mesh_pad & 4096u) != 0u ? k.e[1].w : 0.0);
            /* shader water (push-constant bit 4096, ps2_video.c wat_frame): a patch whose tile carries water
               (zero_water.c marks the block: e[0] = the tile's uv corner and 1 / extent, e[1] = layer, share, -, class,
               e[2] = the eye) */
            if ((pc.mesh_pad & 4096u) != 0u && k.e[1].w > 0.5) {
                v_wat = vec4((in_uv.xy - k.e[0].xy) * k.e[0].zw, k.e[1].x, k.e[1].w);
                float tsun = 1.0;
                if ((pc.mesh_pad & 2048u) != 0u) {
                    vec4 o = b[GSH_BLOCK].p[3];
#ifdef RQ
                    tsun = gsh_t(p * o.w - o.xyz);
#else
                    tsun = cl_sun_shadow_map(cl_ground_map, b[GSH_BLOCK].p[0], b[GSH_BLOCK].p[1], b[GSH_BLOCK].p[2],
                                             p * o.w - o.xyz);
#endif
                }
                v_wrel = vec4(p - k.e[2].xyz, tsun);
                v_wabs = p;
#ifdef RQT
                /* PS2_RQ_WATER (rq_water.glsl): what depends only on the eye direction */
                if ((pc.mesh_pad & 4194304u) != 0u) {
                    Block wb = b[16381u];
                    vec3 rel = p - k.e[2].xyz;
                    float rd = length(rel);
                    vec3 rV = -rel / max(rd, 1e-3);
                    v_wrel = vec4(-rel / 16384.0, tsun);          /* (towards the eye, linear in the patch: normalised per fragment; Fresnel at grazing angles is too steep for a unit vector's chord) */
                    float rf = 0.0;
                    if ((pc.mesh_pad & 2048u) != 0u && wb.l[1].z > 0.0 && rV.y > 0.03) {
                        vec4 o = b[GSH_BLOCK].p[3], s1 = b[GSH_BLOCK].p[1];
                        vec3 pm = p * o.w - o.xyz;
                        float th = (s1.z - (s1.w + pm.y)) / rV.y;
                        if (th > 0.0) {
                            float wg, odr = cl_shadow_od_map(cl_ground_map, b[GSH_BLOCK].p[0], s1, b[GSH_BLOCK].p[2], pm + vec3(-rV.x, rV.y, -rV.z) * th, wg);
                            rf = (1.0 - cl_shadow_t(odr)) * wg * wb.l[1].z;
                        }
                    }
                    /* PS2_RQ_WFAR (wb.p[0].x, units): the wave reads' mip bias: 0 up to half that distance, the texture's mean at it: one model from near to far */
                    float wbt = wb.p[0].x > 0.0 ? clamp(2.0 * rd / wb.p[0].x - 1.0, 0.0, 1.0) : 0.0;
                    v_tmat = vec4((1.0 - clamp((rd - wb.l[0].x) * wb.l[0].y, 0.0, 1.0)) * wb.l[2].w * clamp(k.e[2].w, 0.0, 1.0), rf, 12.0 * wbt * wbt, 0.0);   /* (e[2].w: the cell's fade-in, PS2_RQ_WATER_FADEIN; 1 before) */
                    v_wa = vec4(vec2(dot(wb.m[0].xy, p.xz), dot(wb.m[0].zw, p.xz)) + wb.m[1].xy, vec2(dot(wb.m[2].xy, p.xz), dot(wb.m[2].zw, p.xz)) + wb.m[3].xy);
                    float rx = 1.0 - clamp(rV.y, 0.0, 1.0), rx2 = rx * rx;
                    float k0 = clamp((wb.c[3].w + (1.0 - wb.c[3].w) * rx2 * rx2 * rx) * wb.c[2].w, 0.0, 0.9);
                    vec3 sky0 = mix(wb.c[3].rgb, wb.c[2].rgb, sqrt(clamp(rV.y, 0.0, 1.0))) * mix(1.0, tsun, 0.5);
                    v_wb = vec4(1.0 / (1.0 - k0), k0 * sky0 / (1.0 - k0));
                    v_view = normalize(wb.c[0].xyz + rV);
                    if ((pc.mesh_pad & 16777216u) != 0u) {     /* PS2_RQ_WFAR: far water: the flat surface (rq_water.glsl S_WF) */
                        vec3 wsky = mix(wb.c[3].rgb, wb.c[2].rgb, sqrt(clamp(rV.y, 0.0, 1.0)));
                        if (rf > 0.0 && rV.y > 0.03) wsky = mix(wsky, wb.l[2].rgb, rf);
                        wsky *= mix(1.0, tsun, 0.5);
                        vec3 wgl = vec3(0.0);
                        if (wb.c[0].w > 0.0) {
                            const vec4 dk = vec4(8.0, 8.0, 16.0, 16.0), db = vec4(-4.0, -4.0, 0.0, 0.0);
                            vec4 wm = textureLod(wat_waves_v, vec2(0.5), 16.0) * dk + db;
                            float wv = max(wm.z + wm.w - dot(wm.xy, wm.xy), 0.0), wf = v_tmat.x;
                            float var = 0.5 * wv * (wb.m[1].z * wb.m[1].z + wb.m[3].z * wb.m[3].z) * wf * wf + wb.c[1].w;
                            vec3 wH = v_view;
                            float hy = max(wH.y, 0.1);
                            vec2 dd = -wH.xz / hy;
                            float D = exp(-0.5 * dot(dd, dd) / var) / (6.2831853 * var);
                            float hx = 1.0 - clamp(dot(wH, rV), 0.0, 1.0), hx2 = hx * hx;
                            float g = (wb.c[3].w + (1.0 - wb.c[3].w) * hx2 * hx2 * hx) * D / (4.0 * max(rV.y, 0.08) * hy * hy * hy * hy);
                            wgl = wb.c[1].rgb * min(g, 4.0) * tsun;
                        }
                        v_wa = vec4(wsky, clamp((wb.c[3].w + (1.0 - wb.c[3].w) * rx2 * rx2 * rx) * wb.c[2].w, 0.0, 1.0));
                        v_view = wgl;
                    }
                }
#endif
            }
        } else if (k.pass == 1u) {
            uv = in_uv.xy * 16.0;
            col = vec4(vec3(detail_fade(k.p[0].z, k.p[0].w, dw, k.pad4)), k.p[3].x);
        } else if (k.pass == 2u) {
            uv = in_uv.xy * 16.0 + k.p[1].xy;
            col = vec4(vec3(detail_fade(k.p[1].z, k.p[1].w, dw, k.pad4)), fog * 0.5);
        } else {
            uv = (p.xz - k.p[2].xz) * k.p[2].w;
            col = vec4(fog * 0.5);
        }
        v_stq = vec3(uv, 1.0);
        v_color = col / 128.0;
#ifdef RQT
        if ((pc.mesh_pad & 2097152u) != 0u) {          /* PS2_RQ_ORDER (rq_terrain.frag -DRQO): what mesh.frag / rq_terrain.frag do per fragment from the draw's state, once per vertex */
            uint wms = pc.flags.z & 3u, wmt = (pc.flags.z >> 2) & 3u;
            vec2 org = vec2(0.0);
            if (((pc.flags.z >> 8) & 1u) != 0u) {
                if (wms == 2u) org.x = float(pc.clamp_uv.x);
                if (wmt == 2u) org.y = float(pc.clamp_uv.z);
            }
            v_stq = vec3((uv * pc.tex_size - org) / pc.img_size, 1.0);
            vec2 lo = 0.5 / pc.img_size, hi = (pc.tex_size - 0.5) / pc.img_size;
            if (wms == 2u) { lo.x = (float(pc.clamp_uv.x) - org.x + 0.5) / pc.img_size.x; hi.x = (float(pc.clamp_uv.y) - org.x + 0.5) / pc.img_size.x; }
            if (wmt == 2u) { lo.y = (float(pc.clamp_uv.z) - org.y + 0.5) / pc.img_size.y; hi.y = (float(pc.clamp_uv.w) - org.y + 0.5) / pc.img_size.y; }
            v_uvc = vec4(wms == 1u || wms == 2u ? lo.x : -1e7, wmt == 1u || wmt == 2u ? lo.y : -1e7, wms == 1u || wms == 2u ? hi.x : 1e7, wmt == 1u || wmt == 2u ? hi.y : 1e7);
            v_terr.x = ((pc.flags.x >> 3) & 1u) != 0u ? float((pc.flags.x >> 8) & 255u) / 128.0 : -1.0;
            float rfog = ((pc.flags.x >> 16) & 1u) != 0u ? v_fog : 1.0;
            if ((pc.mesh_pad & 4194304u) != 0u) { v_fog = rfog; v_fac = vec4(pc.fogcol.rgb, 0.0); }   /* (the water shades the texel before the fog) */
            else { v_color.rgb *= rfog; v_fac = vec4(pc.fogcol.rgb * (1.0 - rfog), 0.0); }             /* (the fog folded per vertex: t c f + F (1 - f)) */
            if ((pc.mesh_pad & 16777216u) != 0u) v_rqt = (pc.mesh_pad & 8388608u) != 0u ? vec4(textureLod(rqt_l3_v, vec3(v_rq3.xy, 0.0), 0.0).rgb * v_rqf3, 0.0) : vec4(0.0);   /* (PS2_RQ_WFAR: the water's layer of pass 3 per vertex) */
            else if ((pc.mesh_pad & 8388608u) != 0u) v_fac.rgb -= textureLod(rqt_l3_v, vec3(v_rq3.xy, 0.0), 0.0).rgb * v_rqf3;   /* (PS2_RQ_L3V: far patches: pass 3's layer per vertex) */
            if (rq_tfw > 0.0) {                        /* PS2_RQ_TFADE: the colour so far mixed towards H (v_fac2.rgb) by rq_tfw, folded into the same terms */
                vec3 th = rq_tfdbg ? vec3(1.0, 0.0, 0.0) : v_fac2.rgb;
                float tk = 1.0 - rq_tfw;
                if ((pc.mesh_pad & 4194304u) != 0u) {   /* (water: mix(F, W, f) -> mix(F', W, f k) with F' (1 - f k) = F (1 - f) k + H w) */
                    float den = 1.0 - v_fog * tk;
                    v_fac.rgb = den > 1e-4 ? (v_fac.rgb * (1.0 - v_fog) * tk + th * rq_tfw) / den : th;
                    v_fog *= tk;
                } else {                                /* (t c f + F (1 - f) -> (t c f + F (1 - f)) k + H w) */
                    v_color.rgb *= tk;
                    v_fac.rgb = v_fac.rgb * tk + th * rq_tfw;
                }
                v_rq3.zw *= tk; v_rqf3 *= tk;           /* (what the fragment subtracts: the detail passes, the layer of pass 3) */
                if ((pc.mesh_pad & 16777216u) != 0u) v_rqt.rgb *= tk;
            }
#ifdef BATCH
            if ((pc.mesh_pad & 33554432u) != 0u) {         /* PS2_RQ_MAT: terrain_detail.glsl's classification and fade per vertex, from the atlas texel under the vertex */
                vec3 ma = textureLod(texs_v[nonuniformEXT(rec[v_rec].tslot.x)], vec3(clamp(v_stq.xy, v_uvc.xy, v_uvc.zw), 0.0), 0.0).rgb;
                vec4 mL0 = k.l[0], mL1 = k.l[1], tm = v_tmat;
                float mY = dot(ma, vec3(0.299, 0.587, 0.114)), mYr = 1.0 / max(mY, 0.04);
                float mhi = max(ma.r, max(ma.g, ma.b)), mlo = min(ma.r, min(ma.g, ma.b)), msat = (mhi - mlo) / max(mhi, 0.02);
                float mgreen = clamp((ma.g - max(ma.r, ma.b)) * mYr * 8.3, 0.0, 1.0);
                float mwarm = clamp(((ma.r - ma.b) * mYr - 0.20) * 4.0, 0.0, 1.0);
                float wn = mL1.z * clamp((mY - 0.48) * 5.5, 0.0, 1.0) * clamp((0.22 - msat) * 7.0, 0.0, 1.0);
                float mgrey = clamp((0.25 - msat) * 6.7, 0.0, 1.0) * clamp((0.42 - mY) * 5.0, 0.0, 1.0);
                float mslope = clamp((tm.x - 0.08) * 8.0, 0.0, 1.0);
                float wr = max(clamp((tm.x - 0.18) * 5.5, 0.0, 1.0), 0.8 * mgrey * (1.0 - mgreen) * max(mL1.z, mslope));
                float ws = mwarm * clamp((mY - 0.42) * 6.2, 0.0, 1.0) * clamp((msat - 0.15) * 6.7, 0.0, 1.0) * clamp((0.42 - msat) * 10.0, 0.0, 1.0) * (1.0 - mgreen);
                float wg = mgreen;
                float wo = (0.35 + 0.65 * clamp((0.30 - mY) * 5.0, 0.0, 1.0) * (1.0 - mgreen)) * (1.0 - max(wg, ws));
                float mkeep = 1.0 - wn, mpaved = 1.0 - 0.6 * mgrey * (1.0 - mL1.z) * (1.0 - mslope);
                wg *= mkeep * (1.0 - 0.8 * wr); ws *= mkeep * (1.0 - 0.6 * wr); wo *= mkeep * (1.0 - 0.8 * wr);
                int mi = 0; float mw = wg;
                if (ws > mw) { mi = 1; mw = ws; }
                if (wr > mw) { mi = 2; mw = wr; }
                if (wn > mw) { mi = 3; mw = wn; }
                if (wo > mw) { mi = 4; mw = wo; }
                vec4 mP = mi < 4 ? k.r[mi] : k.l[2];
                float mf = clamp(1.0 - (length(p - k.e[2].xyz) - mL1.x) * mL1.y, 0.0, 1.0);
                v_ma = vec4(p.xz * (1.0 / 192.0), mL0.w * mf * mf * (3.0 - 2.0 * mf) * tm.y * mpaved, log2(mP.w));
                v_mb = tm.z * mL0.xz;
                v_mc = vec3(float(mi), mP.y, mP.z);
            }
            v_round_uv = rec[v_rec].tslot.x;
#endif
        }
#endif
#ifdef ZCULL
        if ((pc.mesh_pad & 134217728u) != 0u && k.pass != 0u && zc_zero(v_color))
            gl_CullDistance[0] = -1.0;                                          /* (perf5_terrain: zc_zero above) */
#endif
        return;
    }

    if (k.pass == 0u) {
        v_stq = vec3(in_uv.xy, 1.0);
        if (k.mode == 0u) {
            v_color = vec4(in_col & 255) / 128.0;
        } else {
            vec3 n = vec3(in_nrm.xyz) / 4096.0;
            vec4 l = vec4(k.l[0].xyz * n.x + k.l[1].xyz * n.y + k.l[2].xyz * n.z, 1.0);
            l = l * l * l;
            l.xyz *= 2.0;
            l = max(l, 0.0);
            vec4 li = max(k.r[0] * l.x + k.r[1] * l.y + k.r[2] * l.z + k.r[3] * l.w, 0.0);
            vec3 c = min(vec3(in_col.xyz) * li.xyz, vec3(255.0));
            v_color = vec4(c, 128.0) / 128.0;
        }
        /* cloud shadows on ground objects (pad4 bit 0: rn_vp_model.c, not the aircraft program) */
        if ((pc.mesh_pad & 2048u) != 0u && (k.pad4 & 3u) != 0u)
            v_color.rgb *= gsh_object(sg, k.pad4);
    } else {
        vec3 n = vec3(in_nrm.xyz) / 4096.0;
        vec3 nb = k.e[0].xyz * n.x + k.e[1].xyz * n.y + k.e[2].xyz * n.z;
        v_stq = vec3((nb.x + 1.0) * 0.5, (nb.z + 1.0) * 0.5, 1.0);
        float spec = min(max(nb.y, 0.0) * 1280.0, 128.0);
        v_color = vec4(vec3(spec), k.spec_alpha) / 128.0;
    }
}
