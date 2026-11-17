#version 450
#extension GL_EXT_multiview : require
#ifdef BATCH
#extension GL_EXT_nonuniform_qualifier : require
#endif
#ifdef RQO
/* rq_terrain.frag -DRQO -- PS2_RQ_ORDER (rq_order.inc): one draw per terrain patch, no discard, no blending, no state read per fragment: everything per draw arrives through
   the vertex shader (mesh.vert -DRQT, push-constant bit 2097152). What a patch needs is known per patch: the specialisation constants (PS2_RQ_OCLS, rq_order.inc rqo_class) compile
   out what it does not (DET: the detail passes 1 / 2; L3: the world-XZ layer of pass 3; HZ: the haze ring's closed form, patches beyond the first layer; ALPHA: a texture with
   non-opaque texels, the game's blend over the sky). -DWATER (PS2_RQ_WATER): the shader water of the same patches (rq_water.glsl). */
layout(constant_id = 0) const bool S_DET = true;
layout(constant_id = 1) const bool S_L3 = true;
layout(constant_id = 2) const bool S_HZ = true;
layout(constant_id = 3) const bool S_ALPHA = true;
layout(constant_id = 5) const bool S_MAT = false;  /* near-ground materials (PS2_RQ_MAT): one array read, the classification per vertex (mesh.vert) */
layout(constant_id = 4) const bool S_WF = false;   /* far water (PS2_RQ_WFAR): the flat surface's reflection and glitter per vertex, no wave reads */
layout(constant_id = 6) const bool S_DETW = false; /* PS2_RQ_DET_ALPHA (R-93): a blended texel's detail subtraction divided by its blend weight a (2 - a), not by a */
layout(location = 0) in mediump vec4 v_color;    /* rgb: the vertex colour; without -DWATER times the fog factor */
layout(location = 1) in vec3 v_stq;              /* xy: the uv in the texture (the vertex shader's region arithmetic) */
layout(location = 4) in vec3 v_rqk;              /* x: the alpha reference (-1: no test); y: the fragment's distance / the haze layers' step; z: the layers' base / step */
layout(location = 9) in mediump vec4 v_fc;       /* -DWATER: the fog colour; else the fog's term (fog colour x (1 - fog factor)), minus the layer of pass 3 where it is read per vertex (PS2_RQ_L3V) */
layout(location = 10) in mediump vec4 v_hz;      /* the sky ring colour here (H), 1 inside the haze ring's band */
layout(location = 13) in vec4 v_rqt;
layout(location = 14) in vec4 v_rq3;
layout(location = 15) in mediump float v_rqf3;
layout(location = 0) out mediump vec4 out_color;
#ifdef WATER
#include "rq_water.glsl"
layout(location = 2) in mediump float v_fog;     /* 1 = no fog */
#endif
layout(location = 18) flat in vec4 v_uvc;        /* the uv clamp: lo.xy, hi.xy (the tiles are regions of the game's atlas pages) */
#ifdef BATCH
layout(location = 3) flat in uint v_slot;        /* the texture's slot in the table (mesh.vert) */
layout(set = 3, binding = 0) uniform mediump sampler2DArray texs[1024];
#else
layout(set = 0, binding = 0) uniform mediump sampler2DArray tex;
#endif
layout(set = 4, binding = 0) uniform mediump sampler2DArray rqt_d1;
layout(set = 4, binding = 1) uniform mediump sampler2DArray rqt_d2;
layout(set = 4, binding = 2) uniform mediump sampler2DArray rqt_l3;
#ifndef WATER
layout(location = 19) in vec4 v_ma;              /* PS2_RQ_MAT: world uv of the 32 m tile (highp); the strength x distance fade; the mip bias */
layout(location = 20) in mediump vec2 v_mb;      /* the sun light gain x the sun's XZ */
layout(location = 21) flat in mediump vec3 v_mc; /* the layer, its slope and albedo scales */
layout(set = 2, binding = 5) uniform mediump sampler2DArray td_tex;
#endif
#ifdef RQCUT
#include "rq_cut.glsl"
layout(location = 22) in highp vec4 v_cut;  /* PS2_RQ_CLOUDS_CUT (mesh.vert): xyz the fragment's place in cloud space (m), w the far fade's weight (< 0: none) */
layout(location = 23) flat in highp float v_cutk;   /* (round 2, R-195: the band test -- 0: no cloud can lie between the eye and this triangle) */
#endif
const mediump float RQO_T[9] = float[9](1.0, 1.0, 0.875, 0.65625, 0.41015625, 0.205078125, 0.076904296875, 0.019226074, 0.0024032593);
void main() {
    vec2 uv = clamp(v_stq.xy, v_uvc.xy, v_uvc.zw);
#ifdef BATCH
    mediump vec4 t = texture(texs[nonuniformEXT(v_slot)], vec3(uv, 0.0));
#else
    mediump vec4 t = texture(tex, vec3(uv, 0.0));
#endif
#ifdef WATER
    mediump vec3 rgb = rq_water_shade(t.rgb * v_color.rgb);
    rgb = mix(v_fc.rgb, rgb, v_fog);
#else
    mediump vec3 mbase = t.rgb * v_color.rgb;
    if (S_MAT && v_ma.z > 0.003) {                       /* (terrain_detail.glsl's result with one material, before the fog) */
        mediump vec4 md = texture(td_tex, vec3(v_ma.xy, v_mc.x), v_ma.w) * (255.0 / 127.0) - (128.0 / 127.0);
        mediump float mm = clamp(1.0 + md.w * v_mc.z, 0.6, 1.45) * clamp(1.0 - dot(md.xy * v_mc.y, v_mb), 0.55, 1.6);
        mbase *= 1.0 + v_ma.z * (mm - 1.0);
    }
    mediump vec3 rgb = mbase + v_fc.rgb;
#endif
#ifdef WATER
    if (S_WF && !S_DET) rgb = max(rgb - v_rqt.rgb, vec3(0.0));   /* (the layer of pass 3, read per vertex) */
#endif
    mediump float a = S_ALPHA ? t.a : 1.0;
    if (S_DET || S_L3) {
        mediump vec3 sub = vec3(0.0);
        if (S_DET) {
            if (!S_ALPHA || a > v_rqk.x) { if (v_rq3.z > 0.0) sub += texture(rqt_d1, vec3(v_rqt.xy, 0.0)).rgb * v_rq3.z; }
            else if (v_rq3.w > 0.0) sub += texture(rqt_d2, vec3(v_rqt.zw, 0.0)).rgb * v_rq3.w;
        }
        if (S_L3 && v_rqf3 > 0.0) sub += texture(rqt_l3, vec3(v_rq3.xy, 0.0)).rgb * v_rqf3;
        rgb = max(rgb - (S_ALPHA && a < 0.998 ? sub / max(S_DETW ? a * (2.0 - a) : a, 1.0 / 128.0) : sub), vec3(0.0));
    }
    if (S_ALPHA && a < 0.998) rgb = mix(v_hz.rgb, rgb, a * (2.0 - a));   /* (the game blends these texels over the sky twice: the patch's first draw, without an alpha test, then its fail half: 1 - (1 - a)^2 of the texel) */
    else if (S_HZ && v_hz.w > 0.5 && a > v_rqk.x) {
        float hx = v_rqk.y - abs(v_rqk.z);              /* (the layers nearer than the fragment, in layer steps) */
        mediump float ht;
        if (v_rqk.z < 0.0) {                            /* PS2_RQ_TFADE (mesh.vert: the sign): between the layers' values, no step a frame */
            float hf = clamp(hx, 0.0, 8.0), h0 = floor(hf);
            int i0 = int(h0);
            ht = mix(RQO_T[i0], RQO_T[min(i0 + 1, 8)], hf - h0);
        } else ht = RQO_T[int(clamp(ceil(hx), 0.0, 8.0))];
        rgb = mix(v_hz.rgb, max(rgb, vec3(0.0)), ht);
    }
#ifdef RQCUT
    if (v_cutk < 0.5) { }                          /* (round 2, R-195: no cloud can lie in front of this triangle: none of the code runs) */
    else if (v_cut.w > 1.5) rgb = rgb * clamp(v_cut.w - 2.0, 0.0, 1.0) + max(v_cut.rgb, vec3(0.0));   /* ([test] PS2_RQ_CLOUDS_CUT_PARTS bit 16: the vertex's (L_d, T_d + 2)) */
    else if (v_cut.w > -0.5) {                     /* (PS2_RQ_CLOUDS_CUT: the cloud in front of this fragment, at its own distance; the share faded
                                                      into the dome's haze (PS2_RQ_TFADE) takes the whole ray, as the sky beside it. w is -1 without the
                                                      bits and the fade's weight with them: a pixel on a triangle's edge interpolates it just outside the
                                                      triangle -- a weight that starts at 0 there comes out a hair below 0, and a test against 0 dropped
                                                      those pixels' cloud: a dotted line of dark pixels along the fade's first edge, rq13_cut r1) */
        highp vec4 full, f = rqc_front(v_cut.xyz, full);
        if (RQC_Q(5).x < 1.5) f = mix(f, full, clamp(v_cut.w, 0.0, 1.0));
        rgb = rgb * f.a + f.rgb;
    } else if (RQC_Q(5).x > 1.5) rgb = vec3(0.0, 1.0, 0.0);   /* ([test] =2: a merged patch without the cloud's bits shows green) */
#endif
    out_color = vec4(rgb, a);
}
#else
/* rq_terrain.frag -- R2 (NOTES_RQ): the terrain's first pass with its detail passes folded in, without the rest of mesh.frag's
   branches: only what a terrain patch takes (modulate, the alpha test, the fog, the detail subtractions). Same inputs and push
   constants as mesh.frag -DRQT; used by PS2_RQ_TERRAIN=2 for the patches mesh.frag would draw without a shader variant. */
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec3 v_stq;
layout(location = 2) in float v_fog;
layout(location = 13) in vec4 v_rqt;
layout(location = 14) in vec4 v_rq3;
layout(location = 15) in float v_rqf3;
#ifdef RQCUT
#include "rq_cut.glsl"
layout(location = 22) in highp vec4 v_cut;  /* PS2_RQ_CLOUDS_CUT (rq_cut.glsl rqc_native, mesh.vert): the place and the mode (round 2: per fragment) */
layout(location = 23) flat in highp float v_cutk;
#endif
layout(location = 0, index = 0) out vec4 out_color;
layout(location = 0, index = 1) out vec4 out_blend;
#ifdef BATCH
layout(location = 12) flat in uint v_rec;
layout(set = 0, binding = 0) uniform sampler2DArray tex0;
layout(set = 3, binding = 0) uniform sampler2DArray texs[1024];
#define SAMPLE_T(u) (pc.tslot.x == 0xFFFFFFFFu ? texture(tex0, vec3(u, 0.0)) : texture(texs[nonuniformEXT(pc.tslot.x)], vec3(u, 0.0)))
#else
layout(set = 0, binding = 0) uniform sampler2DArray tex;
#define SAMPLE_T(u) texture(tex, vec3(u, 0.0))
#endif
layout(set = 4, binding = 0) uniform sampler2DArray rqt_d1;
layout(set = 4, binding = 1) uniform sampler2DArray rqt_d2;
layout(set = 4, binding = 2) uniform sampler2DArray rqt_l3;
#ifdef BATCH
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
    uint  mv_flags;
    uvec4 tslot;
};
layout(std430, set = 2, binding = 8) readonly buffer MeshRecs { MeshHead rec[]; };
MeshHead pc;
#else
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
    uint  mv_flags;
} pc;
#endif
void main() {
#ifdef BATCH
    pc = rec[v_rec];
#endif
    uint f = pc.flags.x;
    bool ate = ((f >> 3) & 1u) != 0u;
    float aref = float((f >> 8) & 0xFFu) / 128.0;
    bool fge = ((f >> 16) & 1u) != 0u;
    bool tcc = ((f >> 17) & 1u) != 0u;
    bool afkeep = ((f >> 18) & 1u) != 0u;
    bool afonly = ((f >> 20) & 1u) != 0u;
    bool fba = ((f >> 21) & 1u) != 0u;
    vec2 tc = (v_stq.xy / max(v_stq.z, 1e-9)) * pc.tex_size;
    uint wms = pc.flags.z & 3u, wmt = (pc.flags.z >> 2) & 3u;
    vec2 org = vec2(0.0);
    if (((pc.flags.z >> 8) & 1u) != 0u) {
        if (wms == 2u) org.x = float(pc.clamp_uv.x);
        if (wmt == 2u) org.y = float(pc.clamp_uv.z);
    }
    vec2 uv = (tc - org) / pc.img_size;
    vec2 lo = 0.5 / pc.img_size, hi = (pc.tex_size - 0.5) / pc.img_size;
    if (wms == 2u) { lo.x = (float(pc.clamp_uv.x) - org.x + 0.5) / pc.img_size.x; hi.x = (float(pc.clamp_uv.y) - org.x + 0.5) / pc.img_size.x; }
    if (wmt == 2u) { lo.y = (float(pc.clamp_uv.z) - org.y + 0.5) / pc.img_size.y; hi.y = (float(pc.clamp_uv.w) - org.y + 0.5) / pc.img_size.y; }
    if (wms == 1u || wms == 2u) uv.x = clamp(uv.x, lo.x, hi.x);
    if (wmt == 1u || wmt == 2u) uv.y = clamp(uv.y, lo.y, hi.y);
    vec4 t = SAMPLE_T(uv);
    vec3 rgb = t.rgb * v_color.rgb;
    float a = tcc ? t.a * v_color.a : v_color.a;
    if (fge) rgb = mix(pc.fogcol.rgb, rgb, clamp(v_fog, 0.0, 1.0));
    bool pass = !ate || a > aref;
    if (ate && (afonly ? pass : (!pass && afkeep))) discard;
    if (v_rq3.z > 0.0 || v_rq3.w > 0.0 || v_rqf3 > 0.0) {
        vec3 sub = vec3(0.0);
        if (pass) { if (v_rq3.z > 0.0) sub += texture(rqt_d1, vec3(v_rqt.xy, 0.0)).rgb * v_rq3.z; }
        else if (v_rq3.w > 0.0) sub += texture(rqt_d2, vec3(v_rqt.zw, 0.0)).rgb * v_rq3.w;
        if (v_rqf3 > 0.0) sub += texture(rqt_l3, vec3(v_rq3.xy, 0.0)).rgb * v_rqf3;
        rgb = max(rgb - sub / max(a, 1.0 / 128.0), vec3(0.0));
    }
#ifdef RQCUT
    {   highp vec4 rqc = rqc_native(v_cut, v_cutk);   /* (PS2_RQ_CLOUDS_CUT, per fragment) */
        rgb = rgb * rqc.a + rqc.rgb; }
#endif
    out_blend = vec4(0.0, 0.0, 0.0, clamp(a, 0.0, 1.0));
    out_color = clamp(vec4(rgb, fba ? max(a, 1.0) : a), vec4(0.0), vec4(2.0));
}
#endif
