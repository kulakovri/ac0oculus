/* rq_water.glsl -- the shader water of terrain_water.glsl (zero_water.c) for the Quest's eye pass (PS2_RQ_WATER, rq_order.inc; included by rq_terrain.frag -DRQO -DWATER): the same look
   (waves, gloss, the sun's glitter, the reflection of the game's sky and of our clouds' underside). What depends only on the eye direction moves to the vertex (mesh.vert -DRQT, bit 4194304):
   the view vector, the distance fade, the wave layers' uv, the sun's half vector, the body's terms, the cloud reflection; the frame's constants come as push constants (rp, rp2, soft, fxv: rq_order.inc
   rqo_water_push), no storage buffer per fragment. Per fragment: the mask of partial tiles, the two wave reads, the facet's reflection.
   One model from near to far (PS2_RQ_WFAR): the wave reads take a mip bias growing from half that distance (mesh.vert), so the mips move the slopes' energy into the variance the glitter
   lobe reads (the textures hold the slopes' second moments); at that distance they return the texture's mean, which is what S_WF evaluates per vertex: the far class is the near one's limit. */
#ifndef RQ_WATER_GLSL
#define RQ_WATER_GLSL
layout(push_constant) uniform PC {
    vec2  inv_size; vec2 tex_size; uvec4 flags; vec4 fogcol; uvec4 clamp_uv; vec2 img_size;
    float scale; float point_size; float tex_scale; float pad0; uint mesh_block; uint mv_flags;
    vec4  rp[4];   /* RA, RB (the wave layers' rotation back to world axes); (RMS slope A, RMS slope B, the unresolved slope variance, body strength); (glitter colour, glitter on) */
    vec4  rp2;     /* the game's sky at the zenith, reflection strength */
    vec4  fogr;
    vec4  soft;    /* the game's horizon sky, F0 */
    vec4  fxv;     /* a cloud's underside colour */
} pc;
layout(location = 5) in vec4 v_wat;              /* mask uv, layer, class (0 none, 1 partial, 2 full) */
layout(location = 6) in mediump vec4 v_wrel;     /* towards the eye (xyz), our clouds' sun transmittance */
layout(location = 8) in mediump vec4 v_tmat;     /* x: the distance fade; y: the cloud reflection's weight */
layout(location = 11) in mediump vec3 v_view;    /* the sun's half vector */
layout(location = 16) in vec4 v_wa;              /* the two wave layers' uv */
layout(location = 17) in mediump vec4 v_wb;      /* the body: 1 / (1 - k0), k0 sky0 / (1 - k0) */
layout(set = 2, binding = 3) uniform mediump sampler2DArray wat_mask;    /* the tiles' water masks */
layout(set = 2, binding = 4) uniform mediump sampler2D wat_waves;        /* slopes s_x / 8 + 0.5, s_z / 8 + 0.5, s_x^2 / 16, s_z^2 / 16 */

mediump float wat_fresnel(mediump float f0, mediump float c) { mediump float x = 1.0 - clamp(c, 0.0, 1.0); mediump float x2 = x * x; return f0 + (1.0 - f0) * x2 * x2 * x; }
/* (PS2_RQ_WFP16 [test]) a value as the Quest's mediump holds it: rounded to half precision, denormals flushed to zero */
mediump float wat_h16(mediump float x) { highp float y = unpackHalf2x16(packHalf2x16(vec2(x, 0.0))).x; return abs(y) < 6.1035e-5 ? 0.0 : y; }

mediump vec3 rq_water_shade(mediump vec3 base) {
    if (v_wat.w < 0.5) return base;
    mediump float m = v_wat.w > 1.5 ? 1.0 : texture(wat_mask, vec3(v_wat.xy, v_wat.z)).r;
    mediump float fade = m * v_tmat.x;
    if (fade < 0.004) return base;
    if (S_WF) {                                           /* (PS2_RQ_WFAR: sky reflection (v_wa), its strength (v_wa.w) and glitter (v_view) per vertex) */
        mediump vec3 fbody = pc.rp[2].w > 0.0 ? mix(base, max(base * v_wb.x - v_wb.yzw, 0.3 * base), pc.rp[2].w) : base;
        return mix(base, mix(fbody, v_wa.rgb, v_wa.w) + v_view, fade);
    }
    mediump vec3 V = normalize(v_wrel.xyz);
    /* (R-60, rq_order.inc rqo_wglint_word: fxv.w = the glitter's knee G (0: the clamp at 4 as before) + 8 the edges by the distance fade + 16 the half-precision test) */
    mediump float gw = pc.fxv.w;
    bool wh16 = gw >= 15.5; if (wh16) gw -= 16.0;
    bool wedge = gw >= 7.75; if (wedge) gw -= 8.0;
    mediump float fs = wedge ? v_tmat.x : fade;            /* (the waves' fade: with PS2_RQ_WATER_EDGE the distance only, the mask only in the final mix) */
    const mediump vec4 dk = vec4(8.0, 8.0, 16.0, 16.0), db = vec4(-4.0, -4.0, 0.0, 0.0);
    mediump vec4 ta = texture(wat_waves, v_wa.xy, v_tmat.z) * dk + db;      /* (v_tmat.z: the mip bias towards PS2_RQ_WFAR: the mips move the slopes' energy into the variance, so at that distance this is the far class) */
    mediump vec4 tb = texture(wat_waves, v_wa.zw, v_tmat.z) * dk + db;
    mediump vec2 s = (vec2(dot(pc.rp[0].xy, ta.xy), dot(pc.rp[0].zw, ta.xy)) * pc.rp[2].x + vec2(dot(pc.rp[1].xy, tb.xy), dot(pc.rp[1].zw, tb.xy)) * pc.rp[2].y) * fs;
    mediump vec3 n = normalize(vec3(-s.x, 1.0, -s.y));
    mediump float ndv = dot(n, V);
    mediump float F = wat_fresnel(pc.soft.w, ndv);
    mediump float Ry = abs(2.0 * ndv * n.y - V.y);       /* (the reflected ray's height: the only component used) */
    mediump vec3 sky = mix(pc.soft.rgb, pc.rp2.rgb, sqrt(clamp(Ry, 0.0, 1.0)));
    if (v_tmat.y > 0.0 && Ry > 0.03) sky = mix(sky, pc.fxv.rgb, v_tmat.y);
    sky *= mix(1.0, v_wrel.w, 0.5);
    mediump vec3 body = pc.rp[2].w > 0.0 ? mix(base, max(base * v_wb.x - v_wb.yzw, 0.3 * base), pc.rp[2].w) : base;
    mediump vec3 col = mix(body, sky, clamp(F * pc.rp2.w, 0.0, 1.0));
    if (pc.rp[3].w > 0.5) {                               /* the sun's glitter */
        mediump float va = max(ta.z + ta.w - dot(ta.xy, ta.xy), 0.0) * pc.rp[2].x * pc.rp[2].x, vb = max(tb.z + tb.w - dot(tb.xy, tb.xy), 0.0) * pc.rp[2].y * pc.rp[2].y;
        mediump float var = 0.5 * (va + vb) * fs * fs + pc.rp[2].z;
        mediump float hy = max(v_view.y, 0.1);
        mediump vec2 dd = -v_view.xz / hy - s;
        mediump float D = exp(-0.5 * dot(dd, dd) / var) / (6.2831853 * var);
        mediump float x = wat_fresnel(pc.soft.w, dot(v_view, V)) * D, den = 4.0 * max(V.y, 0.08) * hy * hy * hy * hy;
        if (wh16) { x = wat_h16(x); den = wat_h16(den); }
        mediump float g;
        if (gw > 0.0) g = gw * x / (gw * den + x + 1e-4);    /* (R-60: G g / (G + g), finite where den flushes to 0) */
        else { g = x / den; if (wh16) { g = wat_h16(g); if (isnan(g) || isinf(g)) g = 4.0; } g = min(g, 4.0); }   /* (the test: x / 0 and 0 / 0 at the clamp, as IEEE minNum's min(NaN, 4.0) = 4.0 [inference for the Adreno]) */
        col += pc.rp[3].rgb * g * v_wrel.w;
    }
    return mix(base, col, fade);
}
#endif
