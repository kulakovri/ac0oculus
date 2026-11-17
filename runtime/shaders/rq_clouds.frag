#version 450
#extension GL_GOOGLE_include_directive : require
/* rq_clouds.frag (plan 4.2; rq_clouds.inc rq_clouds_composite): the far part of our clouds in the eye pass: one read of the panorama at
   the uv the vertex shader corrected for parallax; dst = L + dst T (premultiplied, RGB only). The near part is its own draw
   (rq_clouds_near.frag) over the cells of the near buffer that hold cloud.
   PS2_RQ_CLOUDS_SOFT (B-2, R-52; pc.flags bit 1): where this pixel holds cloud and the scene is not sky, the share of the cloud in
   front of the scene, softly: the scene's distance from one gather of the 1x depth source (the near march's, rq_feed.depth) at the
   interpolated target pixel (v_th, a varying: no fragment-position read), the cloud's first and mean distance from the profile
   image (bilinear, normalised by its z: the weight of the texels with cloud), k = smoothstep(first, 2 mean - first, scene); the
   vertex shader put the quad at the tile's nearest cloud, so the hardware test only cut where the scene lies in front of all of it.
   PS2_RQ_CLOUDS_OPTICS (B-6, R-56; specialization constant OPT, so the pipeline without it has none of the code): our halo, glory and
   spectre (cloud_optics.glsl cl_optics_cloud, the PC composite's own functions and CloudFrame opt[]) on the far content, the pixel's
   direction from the interpolated target pixel (no fragment-position read). */
precision highp float;
#include "cloud_common.glsl"
precision mediump float;
layout(push_constant) uniform PC { highp vec4 rqx; highp vec2 rqy; highp vec2 rqsz; highp vec2 nuv; highp vec2 nsz; highp float ndiv; highp float zpre; highp float flags; highp float dsplit; } pc;
layout(location = 0) in highp vec2 v_uv;
layout(location = 1) flat in vec4 v_id;
layout(location = 2) in highp vec3 v_th;
layout(location = 3) flat in int v_eye;
layout(location = 4) in highp float v_ke;
layout(location = 5) in highp vec3 v_dir;
layout(location = 6) flat in highp vec4 v_off;
layout(location = 7) in highp float v_dist;
layout(location = 8) flat in highp vec4 v_tb;
/* PS2_RQ_CLOUDS_FARSHOWN (clouds7 2026-11-12, prompt 8 step 3, R-113; specialization constant 4): the far colour and its eye-relative depth
   data from the SHOWN map (rq_cloud_far.comp PASS_RESOLVE: the parallax resolved per texel once a frame; the composite's set 1 bindings
   5 / 6): one bilinear read at the tile's own uv, clamped inside the tile, no resolve and no warp here */
layout(constant_id = 4) const int SHOWN = 0;
layout(set = 1, binding = 5) uniform sampler2D shown_tex;
layout(set = 1, binding = 6) uniform sampler2D shown_prof_tex;
layout(location = 0) out vec4 o;
layout(constant_id = 1) const int OPT = 0;
layout(constant_id = 2) const int SOFT = 0;   /* (PS2_RQ_CLOUDS_SOFT: a specialization constant too, so the default pipeline has none of its code) */
/* PS2_RQ_CLOUDS_SOFT_CUT (rq10_artefacts 2026-11-15, A2 of prompt 11, BUGS_RQ R-157; specialization constant 5, with SOFT): the scene's cut
   of the cloud is the pass's own depth test wherever it can be, the coarse depth only fades what stands inside the cloud's span. The SOFT
   fade took the NEAREST of the four depth texels round the pixel (the 1x feed's depth pre-pass, ~3 eye pixels a texel): along a ridge
   in front of a cloud the texels of the ridge cut the cloud from the pixels beside it, in steps of the texel grid (the user's M10 flight,
   80 s: "stairs, stairs"). Now a depth texel nearer than the quad itself (v_qm: the composite's own depth at this pixel, the tile's
   nearest cloud) is left out -- if this pixel shows that surface, the pass's depth test already drops the fragment, per sample; if it
   does not, the texel says nothing about this pixel -- and the texels behind the quad fade by their bilinear weights (a texel-wide soft
   edge instead of a texel step where the scene stands inside the cloud) */
layout(constant_id = 5) const int CUT = 0;
layout(location = 9) in highp float v_qm;
/* PS2_RQ_CLOUDS_FARRESOLVE (clouds6 2026-11-11, A.4 piece 4, R-80): the parallax per pixel. The content the map holds at direction dO
   and distance D from the tile's march origin O lies on the eye's ray where the ray meets the sphere |E - O + de s| = D: a fixed
   point D = D_map(dO(D)). The first guess is the map's own distance in the pixel's direction (per pixel: a guess interpolated over
   the quad's triangles drew their straight edges as teeth and sheets wherever the iteration landed on another cloud, third person
   M10); then up to three steps, each one read of the depth data, until the distance read agrees with the sphere's within 2 %. A
   pixel whose step does not settle (a disocclusion: two clouds at different distances compete for it) fades out with the
   disagreement instead of showing the other cloud. No corner moves, so nothing folds or tears (the corner warp's holes, slabs and
   fans). Typically two reads of the depth data and one of the colour a pixel. */
layout(constant_id = 3) const int RES = 0;
highp float rq_dist(highp vec2 uv) {            /* the map's mean cloud distance about uv (the four texels with cloud), 0 = none */
    highp vec4 g = textureGather(cl_dir_prof_tex, uv, 0);              /* (1 / mean distance of the four texels; 0 = no cloud) */
    highp vec4 ok = vec4(greaterThan(g, vec4(1.0e-6)));
    highp float n = ok.x + ok.y + ok.z + ok.w;
    return n < 0.5 ? 0.0 : n / dot(g, ok);
}
highp vec2 rq_dir_uv(highp vec3 d) {
    d /= abs(d.x) + abs(d.y) + abs(d.z);
    highp vec2 p = d.xz;
    if (d.y < 0.0) p = (1.0 - abs(p.yx)) * vec2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    return p * 0.5 + 0.5;
}
highp vec2 rq_resolve(highp vec3 de, highp vec3 off, highp float Dc, highp float sc, out float w) {
    highp float D = rq_dist(rq_dir_uv(de) * sc);
    if (D <= 0.0) D = Dc;                                             /* (no cloud in the pixel's own direction: the corner's) */
    highp vec2 uv = v_uv;
    w = 1.0;
    for (int k = 0; k < RES; k++) {                                  /* (RES = the steps, PS2_RQ_CLOUDS_FARRESOLVE_STEPS) */
        highp float b = dot(off, de), cc = dot(off, off) - D * D, disc = b * b - cc;
        highp float s = disc > 0.0 ? -b + sqrt(disc) : max(-b, 0.0);
        uv = rq_dir_uv(normalize(off + de * s)) * sc;
        highp float Dn = rq_dist(uv);
        if (Dn <= 0.0) { w = 1.0; break; }                            /* (no cloud there: the colour read is clear sky) */
        highp float e = abs(Dn - D) / D;
        w = 1.0 - smoothstep(0.05, 0.25, e);
        if (e < 0.02) break;
        D = Dn;
    }
    return uv;
}
void main() {
    float rw = 1.0;
    highp vec2 ruv = SHOWN != 0 ? clamp(v_uv, v_tb.xy, v_tb.zw) : RES != 0 ? rq_resolve(normalize(v_dir), v_off.xyz, v_dist, v_off.w, rw) : v_uv;
    vec4 c = SHOWN != 0 ? textureLod(shown_tex, ruv, 0.0) : textureLod(cl_dir_tex, ruv, 0.0);
    if (RES != 0 && SHOWN == 0) c = vec4(c.rgb * rw, mix(1.0, c.a, rw));
    if (OPT != 0 && c.a < 0.999 && cl_opt_any()) {
        highp vec4 pr = SHOWN != 0 ? textureLod(shown_prof_tex, ruv, 0.0) : textureLod(cl_dir_prof_tex, ruv, 0.0);
        highp float cd = pr.z > 1.0e-3 && pr.x > 1.0e-9 ? pr.z / pr.x * (SHOWN != 0 ? 1.0 : v_ke) : CL_DEPTH_NONE;
        c = cl_optics_cloud(c, cl_world_dir(v_th.xy / v_th.z), cd, v_eye, 0);
    }
    if (SOFT != 0 && CUT != 0 && c.a < 0.998) {
        highp vec2 tgt = v_th.xy / v_th.z;
        highp vec2 guv = tgt / vec2(cl.sizes.w, cl.sizes2.x);
        highp vec4 zg = textureGather(cl_scene_depth, vec3(guv, float(cl_depth_layer(v_eye))), 0);
        highp vec4 pr = SHOWN != 0 ? textureLod(shown_prof_tex, ruv, 0.0) : textureLod(cl_dir_prof_tex, ruv, 0.0);
        if (pr.z > 1.0e-3 && pr.x > 1.0e-9) {
            highp float ke = SHOWN != 0 ? 1.0 : v_ke;
            highp float mean = pr.z / pr.x * ke, first = min(pr.z / max(pr.y, 1.0e-9) * ke, mean);
#if defined(FDEPTH) || defined(FSPLIT)
            highp float A = 1.0 - c.a, as = clamp(pr.w, 0.0, A), ds = pc.dsplit;
            highp float lastf = clamp(2.0 * mean - first, first + 30.0, max(ds, first + 30.0));
            highp float fb = max(first, ds), lastb = max(2.0 * mean - fb, fb + 30.0);
#else
            highp float last = max(2.0 * mean - first, first + 30.0);
#endif
            highp vec2 f = fract(guv * vec2(textureSize(cl_scene_depth, 0).xy) - 0.5);
            highp vec4 w = vec4((1.0 - f.x) * f.y, f.x * f.y, f.x * (1.0 - f.y), (1.0 - f.x) * (1.0 - f.y));   /* (textureGather's order) */
            highp float rl = length(cl_view_ray(tgt)), kk = 0.0, ww = 0.0;
            for (int i = 0; i < 4; i++) {
                if (pc.zpre > 0.5 && zg[i] >= 0.99) continue;              /* (PS2_RQ_ZPRE's cockpit band: the pass's own depth cuts there) */
                highp float sm = cl_view_depth_m(zg[i]);
                if (sm < 0.999 * v_qm) continue;                           /* (in front of the quad: the pass's depth test's) */
                float ki = 1.0;
                if (sm < CL_INF) {
                    highp float sd = sm * rl;
#if defined(FDEPTH) || defined(FSPLIT)
                    ki = A > 1.0e-4 ? (as * smoothstep(first, lastf, sd) + (A - as) * smoothstep(fb, lastb, sd)) / A : 1.0;
#else
                    ki = smoothstep(first, last, sd);
#endif
                }
                kk += w[i] * ki; ww += w[i];
            }
            float k = ww > 1.0e-4 ? kk / ww : 1.0;
            c = vec4(c.rgb * k, mix(1.0, c.a, k));
        }
    } else
    if (SOFT != 0 && c.a < 0.998) {
        highp vec2 tgt = v_th.xy / v_th.z;
        highp vec4 zg = textureGather(cl_scene_depth, vec3(tgt / vec2(cl.sizes.w, cl.sizes2.x), float(cl_depth_layer(v_eye))), 0);
        highp float sv = CL_INF;
        for (int i = 0; i < 4; i++)                                     /* (PS2_RQ_ZPRE's cockpit band is no occluder: the pass's own depth cuts there) */
            if (!(pc.zpre > 0.5 && zg[i] >= 0.99)) sv = min(sv, cl_view_depth_m(zg[i]));
        if (sv < CL_INF) {
            highp float sd = sv * length(cl_view_ray(tgt));
            highp vec4 pr = SHOWN != 0 ? textureLod(shown_prof_tex, ruv, 0.0) : textureLod(cl_dir_prof_tex, ruv, 0.0);
            highp float ke = SHOWN != 0 ? 1.0 : v_ke;                   /* (the shown map's depth data is the eye's already) */
            if (pr.z > 1.0e-3 && pr.x > 1.0e-9) {
                highp float mean = pr.z / pr.x * ke, first = min(pr.z / max(pr.y, 1.0e-9) * ke, mean);
#if defined(FDEPTH) || defined(FSPLIT)
                /* (rq8_onelayer, PS2_RQ_CLOUDS_ONE: the ray's front part -- to dsplit, its opacity in prof.w, bilinear as the colour's -- and the
                   back fade apart: the front over [first, min(2 mean - first, dsplit)], the back from max(first, dsplit) to 2 mean - that) */
                highp float A = 1.0 - c.a, as = clamp(pr.w, 0.0, A), ds = pc.dsplit;
                highp float lastf = clamp(2.0 * mean - first, first + 30.0, max(ds, first + 30.0));
                highp float fb = max(first, ds), lastb = max(2.0 * mean - fb, fb + 30.0);
                float k = A > 1.0e-4 ? (as * smoothstep(first, lastf, sd) + (A - as) * smoothstep(fb, lastb, sd)) / A : 1.0;
#else
                highp float last = max(2.0 * mean - first, first + 30.0);
                float k = smoothstep(first, last, sd);
#endif
                c = vec4(c.rgb * k, mix(1.0, c.a, k));
            }
        }
    }
    o = c;
    if (v_id.a > 0.5) o = vec4(v_id.rgb, 0.0);      /* [debug] PS2_RQ_CLOUDS_VIEW=3: the tile ids (dst = id) */
#ifdef FDEPTH
    /* (rq8_onelayer, PS2_RQ_CLOUDS_ONE: the variant rq_clouds_fd.frag, the one layer's far tiles) the fragment's depth is its own first cloud,
       3 % nearer: the depth test (late, against the eye pass's whole depth -- the far terrain the depth pre-pass lacks included) cuts the
       cloud per pixel where the scene stands in front of all of it; SOFT above fades what lies partly in front by the pre-pass's depth.
       A quad at the tile's nearest cloud cut along its two triangles, and with the quad at the near plane the far terrain outside the
       pre-pass took the whole ray's cloud: a row of teeth along the horizon, M10 f5936. No cloud here: the near plane's value (it blends
       nothing) */
    {
        highp float zd = 0.9955;
        highp vec4 pr = SHOWN != 0 ? textureLod(shown_prof_tex, ruv, 0.0) : textureLod(cl_dir_prof_tex, ruv, 0.0);
        if (c.a < 0.999 && pr.z > 1.0e-3 && pr.y > 1.0e-9) {
            highp float ke = SHOWN != 0 ? 1.0 : v_ke;
            highp float first = min(pr.z / max(pr.y, 1.0e-9) * ke, pr.x > 1.0e-9 ? pr.z / pr.x * ke : CL_DEPTH_NONE);
            highp vec2 tgt = v_th.xy / v_th.z;
            highp float vm = max(0.97 * first / length(cl_view_ray(tgt)), 0.05);          /* (m along the view axis) */
            zd = clamp((cl.depth.z / (vm * cl.misc.x) / cl.depth.x - cl.depth.y) / cl.depth.w, 0.0, 0.9955);
        }
        gl_FragDepth = zd;
    }
#endif
}
