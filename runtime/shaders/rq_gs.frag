#version 450
#extension GL_EXT_multiview : require
/* rq_gs: the lean GS fragment shader of the eye-sized pass (PS2_RQ_LEAN, rq_video.inc). The draws of the game's GS path that
   use gs.frag's ordinary path only -- no DATE, no shuffle, no replacement image, no read of an upscaled target, no soft
   effect edges, no target-marker shapes (those need pc.scale > 1 and never run in this pass at 1:1) -- with the same
   results as gs.frag in those cases: the same wrap, sprite rounding, point / bilinear fetch, TFX, fog, alpha test and
   dual-source blend output, statement for statement. The shared block: gs.frag's push constants under -DMV -DRQ. */
#ifdef MP
precision mediump float;   /* (PS2_RQ_MP: relaxed precision; coordinates, depth and flags stay highp, named below) */
#endif
layout(location = 0) in vec4 v_color;
layout(location = 1) in highp vec3 v_stq;
layout(location = 2) in float v_fog;
layout(location = 3) flat in uint v_round_uv;
#ifdef RQCUT
layout(location = 4) in highp vec4 v_cut;  /* PS2_RQ_CLOUDS_CUT (rq_cut.glsl, gs.vert): the cloud in front of this vertex, rgb L_d, a T_d */
layout(location = 5) flat in highp vec4 v_cutf;   /* (round 2) the provoking vertex's: a card's one value (mv_flags bit 26) */
#endif

layout(location = 0, index = 0) out vec4 out_color;
#ifndef TREE_SINGLE   /* (R-241: rq_gs_tree_sd.frag, the near tree cards' module, has no second source: their blend is off) */
layout(location = 0, index = 1) out vec4 out_blend;
#endif

layout(set = 0, binding = 0) uniform mediump sampler2DArray tex;
#ifdef DATE
layout(input_attachment_index = 0, set = 1, binding = 0) uniform subpassInputMS dstin;   /* (-DDATE: the 4x colour, sample 0, as gs.frag -DRQ_MS) */
#endif

layout(push_constant) uniform PC {
    highp vec2  inv_size;
    highp vec2  tex_size;
    uvec4 flags;
    vec4  fogcol;
    uvec4 clamp_uv;
    highp vec2  img_size;
    highp float scale;
    highp float point_size;
    highp float tex_scale;
    highp float rep_scale;
    uint  mesh_block;
    uint  mv_flags;
    highp vec4  rp[4];
    highp vec4  rp2;
    highp vec4  fogr;
    highp vec4  soft;
    highp vec4  fxv;
    highp vec4  rqx;
    highp vec2  rqy;
    highp vec2  rqsz;
} pc;

#define TL (((pc.mv_flags & 1u) != 0u) ? gl_ViewIndex : 0)
#define FETCH_T(p) texelFetch(tex, ivec3(p, TL), 0)

const uint RUV_SPRITE = 1u << 29;

highp float gs_wrap(highp float t, uint mode, highp float lo, highp float hi, highp float size, bool own) {
    if (mode == 0u) return own ? mod(t, max(size, 1.0)) : t;
    if (mode == 1u) return clamp(t, 0.0, max(size - 1.0, 0.0));
    if (mode == 2u) return clamp(t, lo, max(hi, lo));
    return float((int(floor(t)) & int(lo)) | int(hi));
}

highp float sprite_round(highp float t, uint mode, int origin, int pixel) {
    if (mode == 0u) return t + 1.0 / 1024.0;
    highp float halftexel = floor(t * 2.0 + 0.5) * 0.5;
    if (abs(t - halftexel) > 1.0 / 64.0) return t;
    return halftexel + ((mode == 1u || pixel == origin) ? 1.0 : -1.0) / 64.0;
}

vec4 sample_region_repeat(highp vec2 tc) {
    highp vec2 lo = floor(tc), hi = lo + 1.0, w = fract(tc);
    ivec2 a, b, size = textureSize(tex, 0).xy;
    a.x = int(gs_wrap(lo.x, pc.flags.z & 3u, float(pc.clamp_uv.x), float(pc.clamp_uv.y), pc.tex_size.x, true));
    b.x = int(gs_wrap(hi.x, pc.flags.z & 3u, float(pc.clamp_uv.x), float(pc.clamp_uv.y), pc.tex_size.x, true));
    a.y = int(gs_wrap(lo.y, (pc.flags.z >> 2) & 3u, float(pc.clamp_uv.z), float(pc.clamp_uv.w), pc.tex_size.y, true));
    b.y = int(gs_wrap(hi.y, (pc.flags.z >> 2) & 3u, float(pc.clamp_uv.z), float(pc.clamp_uv.w), pc.tex_size.y, true));
    a = ((a % size) + size) % size;
    b = ((b % size) + size) % size;
    return mix(mix(FETCH_T(a), FETCH_T(ivec2(b.x, a.y)), w.x),
               mix(FETCH_T(ivec2(a.x, b.y)), FETCH_T(b), w.x), w.y);
}

#if defined(TREE_AA) && !defined(TREE_MIP)   /* (-DTREE_MIP, bit 16's twins rq_gs_tree_nm_mip / _sd_mip: no box read compiled) */
/* -DTREE_AA (rq_gs_tree.frag; PS2_RQ_TREE_AA, rq_order.inc, BUGS_RQ R-185): the game's tree cards. pc.flags.w bit 0: the impostor
   read box-filtered over the pixel's footprint -- up to 4 x 4 bilinear taps spread over the screen derivatives of the texel
   coordinate (gx, gy = texels a pixel along x and y), each clamped as the draw's wrap, colour weighted by alpha (a prefiltered read:
   what a mip chain gives; the target has one level). Bit 1: the alpha test GEQUAL / GREATER as coverage of the 4 samples -- the
   alpha sharpened by its screen derivative, (a - ref) / fwidth(a) + 0.5, sets gl_SampleMask (k of 4 samples, the pattern turned by
   the pixel) instead of a discard; the colour's alpha stays as the game writes it (FBA). */
vec4 tree_box(highp vec2 tc, highp vec2 gx, highp vec2 gy) {
    int nx = int(clamp(ceil(length(gx)), 1.0, 4.0)), ny = int(clamp(ceil(length(gy)), 1.0, 4.0));
    vec4 acc = vec4(0.0);
    for (int j = 0; j < 4; j++) {
        if (j >= ny) break;
        for (int i = 0; i < 4; i++) {
            if (i >= nx) break;
            highp vec2 p = tc + gx * ((float(i) + 0.5) / float(nx) - 0.5) + gy * ((float(j) + 0.5) / float(ny) - 0.5);
            p.x = gs_wrap(p.x, pc.flags.z & 3u, float(pc.clamp_uv.x), float(pc.clamp_uv.y), pc.tex_size.x, true);
            p.y = gs_wrap(p.y, (pc.flags.z >> 2) & 3u, float(pc.clamp_uv.z), float(pc.clamp_uv.w), pc.tex_size.y, true);
            vec4 s = textureLod(tex, vec3((p + 0.5) / pc.img_size, float(TL)), 0.0);
            acc += vec4(s.rgb * s.a, s.a);
        }
    }
    return vec4(acc.a > 1e-5 ? acc.rgb / acc.a : vec3(0.0), acc.a / float(nx * ny));
}
#endif

void main() {
#if defined(TREE_AA) && !defined(TREE_NOMASK)   /* (R-241: rq_gs_tree_nm.frag never names the sample mask) */
    gl_SampleMask[0] = -1;
#endif
    vec4 c = v_color;
    uint f = pc.flags.x;
    uint tfx = f & 3u;
    bool ate = ((f >> 3) & 1u) != 0u;
    bool fge = ((f >> 16) & 1u) != 0u;
    bool tcc = ((f >> 17) & 1u) != 0u;
    bool afkeep = ((f >> 18) & 1u) != 0u;
    bool point = ((f >> 19) & 1u) != 0u;
    bool afonly = ((f >> 20) & 1u) != 0u;
    bool fba = ((f >> 21) & 1u) != 0u;
#ifdef DATE
    if (((f >> 22) & 1u) != 0u) {
#ifdef DATE_FORCE
        float ad = DATE_FORCE;          /* (PS2_RQ_DATE_FORCE [test], note 6 E1: no subpassLoad of the foveated 4x colour) */
#else
        float ad = subpassLoad(dstin, 0).a;
#endif
        if ((ad >= 1.0) != (((f >> 23) & 1u) != 0u)) discard;
    }
#endif

    if (pc.flags.y != 0u) {
        highp vec2 tc = ((f >> 2) & 1u) != 0u ? v_stq.xy : (v_stq.xy / max(v_stq.z, 1e-9)) * pc.tex_size;
        uint ruv = v_round_uv & (RUV_SPRITE - 1u);
        if (point && ruv != 0u) {
            /* the game's pixel of this eye-image pixel (the inverse of the crop) */
            highp float rqA = gl_ViewIndex == 0 ? pc.rqx.x : pc.rqx.z, rqB = gl_ViewIndex == 0 ? pc.rqx.y : pc.rqx.w;
            highp vec2 e = gl_FragCoord.xy / pc.rqsz * 2.0 - 1.0;
            highp vec2 n = vec2((e.x - rqB) / rqA, (e.y - pc.rqy.y) / pc.rqy.x);
            ivec2 pix = ivec2(floor((n + 1.0) / pc.inv_size));
            if (((f >> 29) & 1u) == 0u)
                tc.x = sprite_round(tc.x, ruv & 3u, int((ruv >> 4) & 4095u) - 2048, pix.x);
            tc.y = sprite_round(tc.y, (ruv >> 2) & 3u, int((ruv >> 16) & 4095u) - 2048, pix.y);
        }
        if (!point && (f & (1u << 25)) != 0u) tc -= 0.5;
        highp vec2 unwrapped_tc = tc;
        highp vec2 gdx = dFdx(unwrapped_tc) / pc.img_size, gdy = dFdy(unwrapped_tc) / pc.img_size;
        bool own = any(notEqual(pc.img_size, pc.tex_size));
        tc.x = gs_wrap(tc.x, pc.flags.z & 3u, float(pc.clamp_uv.x), float(pc.clamp_uv.y), pc.tex_size.x, own);
        tc.y = gs_wrap(tc.y, (pc.flags.z >> 2) & 3u, float(pc.clamp_uv.z), float(pc.clamp_uv.w), pc.tex_size.y, own);
        vec4 t;
        if (point) {
            ivec2 size = textureSize(tex, 0).xy;
            ivec2 p = ivec2(floor(tc + (ruv == 0u ? 1.0 / 32.0 : 0.0)));
            p = ((p % size) + size) % size;
            t = FETCH_T(p);
        } else if ((pc.flags.z & 3u) == 3u || ((pc.flags.z >> 2) & 3u) == 3u
                   || (own && ((pc.flags.z & 3u) == 0u || ((pc.flags.z >> 2) & 3u) == 0u))) {
            t = sample_region_repeat(unwrapped_tc);
#ifdef TREE_AA
        } else if ((pc.flags.w & 4u) != 0u) {
            /* PS2_RQ_TREE_AA bit 16 (R-242, rq_treemip.inc): the impostor's premultiplied mip chain is bound (img_size = its region),
               ONE trilinear tap over the footprint (soft.x scales the derivatives: PS2_RQ_TREE_MIP_SHARP), the colour divided by the
               mean alpha -- the box read's alpha-weighted colour and mean alpha, without its taps */
            highp float k = pc.soft.x > 0.0 ? pc.soft.x : 1.0;
            t = textureGrad(tex, vec3((tc + 0.5) / pc.img_size, float(TL)), gdx * k, gdy * k);
            t.rgb = min(t.rgb / max(t.a, 1.0 / 1024.0), vec3(1.0));
#ifndef TREE_MIP
        } else if ((pc.flags.w & 1u) != 0u) {
            t = tree_box(unwrapped_tc, gdx * pc.img_size, gdy * pc.img_size);
#endif
#endif
        } else {
            bool whole = (pc.flags.z & 3u) < 2u && ((pc.flags.z >> 2) & 3u) < 2u;
            t = textureGrad(tex, vec3((tc + 0.5) / pc.img_size, float(TL)), whole ? gdx : vec2(0.0), whole ? gdy : vec2(0.0));
        }
        float af = c.a;
        vec3 rgb;
        float a;
        if (tfx == 1u) {
            rgb = t.rgb;
            a = tcc ? t.a : af;
        } else if (tfx == 0u) {
            rgb = t.rgb * c.rgb;
            a = tcc ? t.a * af : af;
        } else if (tfx == 2u) {
            rgb = t.rgb * c.rgb + af * (128.0 / 255.0);
            a = tcc ? t.a + af : af;
        } else {
            rgb = t.rgb * c.rgb + af * (128.0 / 255.0);
            a = tcc ? t.a : af;
        }
        c = vec4(rgb, a);
    } else {
        c.rgb *= 128.0 / 255.0;
    }

    if (fge) c.rgb = mix(pc.fogcol.rgb, c.rgb, clamp(v_fog, 0.0, 1.0));

    bool apass = true;
#if defined(TREE_AA) && !defined(TREE_NOMASK)
    if (ate && (pc.flags.w & 2u) != 0u) {
        float aref = float((f >> 8) & 0xFFu) / 128.0;
        float cov = clamp((c.a - aref) / max(fwidth(c.a), 1.0 / 256.0) + 0.5, 0.0, 1.0);
        uint k = uint(cov * 4.0 + 0.5);
        if (k == 0u) discard;
        uint r = (uint(gl_FragCoord.x) + 2u * uint(gl_FragCoord.y)) & 3u;
        uint m = (1u << k) - 1u;
        gl_SampleMask[0] = int(((m << r) | (m >> (4u - r))) & 15u);
    } else
#endif
    if (ate) {
        uint atst = (f >> 4) & 7u;
        float aref = float((f >> 8) & 0xFFu) / 128.0;
        bool pass;
        if      (atst == 0u) pass = false;
        else if (atst == 1u) pass = true;
        else if (atst == 2u) pass = c.a <  aref;
        else if (atst == 3u) pass = c.a <= aref;
        else if (atst == 4u) pass = abs(c.a - aref) < (0.5 / 128.0);
        else if (atst == 5u) pass = c.a >= aref;
        else if (atst == 6u) pass = c.a >  aref;
        else                 pass = abs(c.a - aref) >= (0.5 / 128.0);
        apass = pass;
        if (afonly ? pass : (!pass && afkeep)) discard;
    }

#ifndef TREE_SINGLE
    if (((f >> 30) & 1u) != 0u) out_blend = vec4(vec3(clamp(c.a, 0.0, 1.0)), apass ? 1.0 : 0.0);
    else out_blend = vec4(0.0, 0.0, 0.0, clamp(c.a, 0.0, 1.0));
#endif
    if (fba) c.a = max(c.a, 1.0);
#ifdef RQCUT
    {   highp vec4 rqc = (pc.mv_flags & 67108864u) != 0u ? v_cutf : v_cut;   /* (PS2_RQ_CLOUDS_CUT: the cloud in front, applied by the object; clamped) */
        c.rgb = c.rgb * clamp(rqc.a, 0.0, 1.0) + max(rqc.rgb, vec3(0.0)); }
#endif
    out_color = clamp(c, vec4(0.0), vec4(2.0));
}
