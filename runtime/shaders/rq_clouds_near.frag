#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_control_flow_attributes : require
/* rq_clouds_near.frag (plan 4.2; rq_clouds.inc): the near part of our clouds in the eye pass. Without -DEDGE: one bilinear read of this eye's
   near buffer. -DEDGE (cells where the scene lies inside the near range): the scene distance from one gather of the depth source,
   and beyond the range the same bilinear read; inside it the near texel whose clip distance is closest to the scene's
   (cloud_comp.frag's nearest-depth rule). Mediump except the depth linearisation and the coordinates.
   PS2_RQ_CLOUDS_OPTICS (B-6, R-56; specialization constant OPT): the glory and the spectre on near cloud inside their box
   (cl_optics_cloud with near = 1, as cloud_comp.frag), the target pixel from the near uv (no fragment-position read). */
precision highp float;
#include "cloud_common.glsl"
precision mediump float;
layout(location = 0) out vec4 o;
/* T2 (ACZ_VR_CLOUD_RQ_NEARFS=2): the near colour holds no transmittance: it is the G channel of the near depth image */
layout(constant_id = 0) const int T2 = 0;
layout(constant_id = 1) const int OPT = 0;
layout(push_constant) uniform PC { highp vec4 rqx; highp vec2 rqy; highp vec2 rqsz; highp vec2 nuv; highp vec2 nsz; highp float ndiv; highp float zpre; highp float flags; } pc;   /* (zpre: PS2_RQ_ZPRE; flags bit 0: PS2_RQ_CLOUDS_NEAR_BILAT) */
layout(location = 0) in highp vec3 v_nuv;
vec4 near_optics(vec4 c) {
    if (OPT == 0 || !(c.a < 0.999) || !cl_opt_any()) return c;
    int eye = int(v_nuv.z + 0.5);
    highp vec2 tgt = v_nuv.xy / pc.nuv;
    vec4 bx = cl.opt[21 + eye];
    if (tgt.x < bx.x || tgt.x > bx.z || tgt.y < bx.y || tgt.y > bx.w) return c;
    return cl_optics_cloud(c, cl_world_dir(tgt), CL_DEPTH_NONE, eye, 1);
}
#ifndef EDGE
void main() {
    vec4 c = textureLod(cl_near_tex, v_nuv, 0.0);
    if (T2 != 0) c.a = textureLod(cl_near_depth_tex, v_nuv, 0.0).g;
    o = near_optics(c);
}
#else
layout(location = 1) in highp vec2 v_tgt;
void main() {
    int eye = int(v_nuv.z + 0.5);
    highp vec2 st = v_tgt / vec2(cl.sizes.w, cl.sizes2.x);
    highp vec4 zg = textureGather(cl_scene_depth, vec3(st, float(cl_depth_layer(eye))), 0);
    highp float rl = length(cl_view_ray(v_tgt)), sd = CL_INF;
    /* PS2_RQ_ZPRE: the depth's cockpit band (the previous frame's coverage, z >= 0.99) stops the near march but is no occluder here: the eye pass's own 4x depth cuts the cloud
       at the cockpit's real edge. As an occluder it left a stair-stepped cloud-free halo of coarse texels along the canopy frame. ckn: beside those texels the march was clipped: take the unclipped neighbour */
    bool ckn = pc.zpre > 0.5 && max(zg[0], max(zg[1], max(zg[2], zg[3]))) >= 0.99;
    [[unroll]] for (int i = 0; i < 4; i++) {
        highp float sv = pc.zpre > 0.5 && zg[i] >= 0.99 ? CL_INF : cl_view_depth_m(zg[i]);
        if (sv < CL_INF) sd = min(sd, sv * rl);
    }
    if (!(sd < cl.cfg.z * 1.05) && !ckn) {
        o = textureLod(cl_near_tex, v_nuv, 0.0);
        if (T2 != 0) o.a = textureLod(cl_near_depth_tex, v_nuv, 0.0).g;
        o = near_optics(o);
        return;
    }
    highp vec2 lp = v_tgt / pc.ndiv - 0.5;
    ivec2 b = ivec2(floor(lp)), lim = ivec2(pc.nsz) - 1;
    highp float sdc = min(sd, cl.cfg.z), best = 1.0e30;
    int bi = 0;
    bool agree = true;
    vec4 s[4];
    highp vec4 dk;
    [[unroll]] for (int k = 0; k < 4; k++) {
        ivec2 q = clamp(b + ivec2(k & 1, k >> 1), ivec2(0), lim);
        s[k] = texelFetch(cl_near_tex, ivec3(q, eye), 0);
        highp vec2 nd = texelFetch(cl_near_depth_tex, ivec3(q, eye), 0).rg;
        if (T2 != 0) s[k].a = nd.g;
        highp float dd = abs(nd.r - sdc);
        dk[k] = dd;
        if (dd > 0.1 * sdc + 1.0) agree = false;
        if (dd < best) { best = dd; bi = k; }
    }
    vec2 f = lp - vec2(b);
    if (agree || int(pc.flags) % 2 == 0) { o = near_optics(agree ? mix(mix(s[0], s[1], f.x), mix(s[2], s[3], f.x), f.y) : s[bi]); return; }
    /* PS2_RQ_CLOUDS_NEAR_BILAT (B-3, R-53): where the four texels disagree in clip distance, the four bilinear weights times their
       agreement with this pixel's scene distance (a smooth weight, so the result moves continuously with the pixel instead of
       one texel's colour over a whole 12 x 14 px block); if none agrees at all, the nearest one in depth as before */
    highp float tol = 0.1 * sdc + 1.0;
    vec4 bw = vec4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y), acc = vec4(0.0);
    highp float ws = 0.0;
    [[unroll]] for (int k = 0; k < 4; k++) {
        highp float dd = dk[k] / tol;
        highp float w = bw[k] / (1.0 + dd * dd * dd * dd) + 1.0e-6 * float(k == bi);
        acc += w * s[k]; ws += w;
    }
    o = near_optics(acc / ws);
}
#endif
