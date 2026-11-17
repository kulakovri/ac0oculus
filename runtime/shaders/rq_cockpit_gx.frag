#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_multiview : require
/* The Quest cockpit's readable glass (rq_cockpit.inc rqgx_*, R-57): rq_cockpit_glass.frag (the tint's texture read, the
   interpolated light, rim and reflection, premultiplied); -DWEAR, on the triangles near the sun's image only: ONE read of the
   tiling wear texture (rqgx_wear_bake: hairlines, wiping strokes and polishing arcs with their orientation) lit by
   cockpit.frag glass_light's rule -- a line sends the sun to the eye where its tangent is square to Q = S - D, so the lit
   bits line up on circles round the sun's image and slide with the head -- and the veiling glare's core from the
   interpolated eye ray. No gl_FragCoord, no input attachment (R-45). */
precision mediump float;
precision mediump int;
layout(location = 0) in highp vec2 v_uv;
layout(location = 1) in vec3 v_li;
layout(location = 2) in vec3 v_add;
layout(location = 3) in float v_fa;
layout(location = 4) in float v_ca;
layout(location = 5) flat in uint v_tex;
#ifdef WEAR
layout(location = 6) in highp vec2 v_wt;
layout(location = 7) in vec2 v_q;
layout(location = 8) in vec3 v_sl;
layout(location = 9) in highp vec3 v_pe;
layout(location = 10) in vec3 v_gl;
#endif
#ifdef RFS
#ifndef WEAR
layout(location = 9) in highp vec3 v_pe;
#endif
layout(location = 11) in vec3 v_n;
layout(location = 12) in vec3 v_rim;
layout(location = 13) in float v_lv;
layout(set = 3, binding = 0) uniform samplerCube interior;
#endif
layout(location = 0) out vec4 o;
layout(set = 0, binding = 0) uniform sampler2DArray texs[1024];
layout(push_constant) uniform Pc { vec4 k; } pc;  /* x 1: the new terms alone; y the list (ACZ_VR_RQ_GLASS_SHOW's colour) */
layout(std140, set = 2, binding = 2) uniform Fr {
    vec4 rot[3]; vec4 head; vec4 proj; vec4 misc; vec4 xfx; vec4 xfy; vec4 l[3]; vec4 r[4]; vec4 dome[4]; vec4 cloud;
    vec4 su, sv, sd; vec4 pack; vec4 tub; vec4 flp[4]; vec4 flc[4]; vec4 night; vec4 mir[3]; vec4 mirg; vec4 eyesize;
    vec4 wa, wb, wc, wd, we, wf; vec4 uvk[2]; vec4 wg, wh;
    vec4 gx[8];
} F;
#ifdef WEAR
layout(set = 3, binding = 1) uniform sampler2D wear;
#endif

void main() {
    vec4 t = texture(texs[nonuniformEXT(v_tex)], vec3(v_uv, 0.0));
    float ca = v_ca * t.a;
    float a = clamp(ca + (1.0 - ca) * v_fa, 0.0, 1.0);
    vec3 c = t.rgb * v_li * ca + v_add;
#ifdef RFS
    {   vec3 rim = v_rim;                        /* the interior's reflection per fragment: rq_cockpit_gx.vert's read, here (no position read:
                                                    the eye ray is the interpolated v_pe) */
        if (v_lv > 0.0) {
            float ek = F.gx[3].x > 0.5 ? 0.0 : (gl_ViewIndex == 0 ? -1.0 : 1.0);
            highp vec3 E = F.head.xyz + ek * F.head.w * F.rot[0].xyz, pa = E + v_pe;
            highp vec3 D = normalize(v_pe);
            vec3 n = normalize(v_n);
            float cv = max(dot(n, -D), 0.0), c5 = 1.0 - cv;
            c5 = c5 * c5 * c5 * c5 * c5;
            highp vec3 Rr = reflect(D, n);
            highp float bq = dot(pa, Rr), cq = dot(pa, pa) - F.gx[2].w * F.gx[2].w, disc = bq * bq - cq;
            highp float tt = disc > 0.0 ? (cq > 0.0 ? -bq - sqrt(disc) : -bq + sqrt(disc)) : -1.0;
            vec4 ic = textureLod(interior, tt > 0.0 ? pa + tt * Rr : Rr, 0.0);
            c += ic.rgb * (min(0.04 + 0.96 * c5, 0.15) * v_lv);
            rim *= 1.0 - ic.a * min(F.gx[2].x, 1.0);
        }
        c += rim;
    }
#endif
#ifdef WEAR
    vec4 w = texture(wear, v_wt);                /* r, g: 0.5 + 0.5 I (cos, sin) of twice the line's direction, b: I (x gx[3].w) */
    float I = w.b;
    if (I > 0.004) {
        vec2 c2 = (w.rg * 2.0 - 1.0) / I;
        float cl = length(c2), k = min(cl, 1.0);          /* (how coherent the texel's directions are: its mips average them) */
        vec2 u = cl > 1e-3 ? c2 / cl : vec2(1.0, 0.0);
        float qa = dot(v_q, v_q), qb = v_q.x * v_q.x - v_q.y * v_q.y, qc = 2.0 * v_q.x * v_q.y;
        float gq2 = max(0.5 * (qa + qb * u.x + qc * u.y), 0.0);            /* (Q . t)^2 for the line's tangent t */
        float sg = inversesqrt(F.gx[3].y);
        float g = mix(min(1.0, sg * 0.5642 / max(sqrt(qa), 1e-4)), exp(-gq2 * F.gx[3].y), k);   /* (incoherent: the mean over directions) */
        c += v_sl * (I * F.gx[3].w * g);
    }
    highp vec3 Q = F.gx[0].xyz - normalize(v_pe);
    float th = length(Q);                        /* (2 asin(|Q| / 2) within the glare's 23 deg: under 0.6 % off) */
    c += v_gl * ((0.05 * exp(-th * (1.0 / 0.035)) + 0.022 * exp(-th * (1.0 / 0.15))) * (1.0 - smoothstep(0.3, 0.4, th)));
#endif
    o = vec4(c, a);
    if (F.gx[5].x > 0.5) {                       /* [debug] ACZ_VR_RQ_GLASS_SHOW: the lists' coverage (A green, B red, the wet glass's own terms blue) */
        int li = int(pc.k.y + 0.5);
        o = vec4((li == 0 ? vec3(0.0, 1.0, 0.0) : li == 1 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 0.0, 1.0)) * 0.5, 0.5);
    }
}
