#version 450
#extension GL_EXT_multiview : require
#extension GL_EXT_nonuniform_qualifier : require
/* rq_hud.frag: the HUD batcher's fragment shader (PS2_RQ_HUD=batch). rq_gs.frag's ordinary path with the per-draw state from
   flat varyings (v_a = frag flags, wrap, mode, texture slot; v_c = clamp_uv; v_s = tex_size, img_size; v_fc = fog colour,
   FIX), the texture from the frame's table. One pipeline for every blend the HUD uses: dual-source blending, colour ONE +
   SRC1_COLOR x Cd, alpha ONE + SRC1_ALPHA x Ad, so the factors are per draw: mode bits 0-2 the source factor, 3-5 the
   destination's, in the codes 0 zero, 1 one, 2 As, 3 1 - As, 4 FIX, 5 1 - FIX (the shader's clamped As, FIX / 128); bit 6:
   this draw writes the alpha channel (else out1.a = 1 keeps it); bit 7: the texture is layered (view index); bit 8: textured. */
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec3 v_stq;
layout(location = 2) in float v_fog;
layout(location = 3) flat in uint v_round_uv;
layout(location = 4) flat in uvec4 v_a;
layout(location = 5) flat in uvec4 v_c;
layout(location = 6) flat in vec4 v_s;
layout(location = 7) flat in vec4 v_fc;
layout(location = 8) flat in uvec2 v_dr;
layout(location = 9) flat in vec2 v_rs;
layout(location = 10) flat in vec4 v_sc;
layout(location = 11) noperspective in vec2 v_ndc;

layout(location = 0, index = 0) out vec4 out_color;
layout(location = 0, index = 1) out vec4 out_blend;

layout(set = 0, binding = 0) uniform sampler2DArray tab[256];
layout(set = 1, binding = 1, std430) readonly buffer W { vec4 w[]; };   /* DATE writers, newest first, two vec4 each: a rectangle (x0, y0, x1, y1) (-, -, alpha >= 1, 0), a triangle (x0, y0, x1, y1) (x2, y2, alpha >= 1, 1) */

layout(push_constant) uniform PC {
    vec4  rqx;
    vec2  rqy;
    vec2  rqsz;
    vec2  inv_size;
    float halfw;
    uint  flags;
} pc;

#define TEX tab[nonuniformEXT(v_a.w)]
#define TL (((v_a.z >> 7) & 1u) != 0u ? gl_ViewIndex : 0)
#define FETCH_T(p) texelFetch(TEX, ivec3(p, TL), 0)

const uint RUV_SPRITE = 1u << 29;

float gs_wrap(float t, uint mode, float lo, float hi, float size, bool own) {
    if (mode == 0u) return own ? mod(t, max(size, 1.0)) : t;
    if (mode == 1u) return clamp(t, 0.0, max(size - 1.0, 0.0));
    if (mode == 2u) return clamp(t, lo, max(hi, lo));
    return float((int(floor(t)) & int(lo)) | int(hi));
}

float sprite_round(float t, uint mode, int origin, int pixel) {
    if (mode == 0u) return t + 1.0 / 1024.0;
    float halftexel = floor(t * 2.0 + 0.5) * 0.5;
    if (abs(t - halftexel) > 1.0 / 64.0) return t;
    return halftexel + ((mode == 1u || pixel == origin) ? 1.0 : -1.0) / 64.0;
}

vec4 sample_region_repeat(vec2 tc) {
    vec2 lo = floor(tc), hi = lo + 1.0, w = fract(tc);
    ivec2 a, b, size = textureSize(TEX, 0).xy;
    a.x = int(gs_wrap(lo.x, v_a.y & 3u, float(v_c.x), float(v_c.y), v_s.x, true));
    b.x = int(gs_wrap(hi.x, v_a.y & 3u, float(v_c.x), float(v_c.y), v_s.x, true));
    a.y = int(gs_wrap(lo.y, (v_a.y >> 2) & 3u, float(v_c.z), float(v_c.w), v_s.y, true));
    b.y = int(gs_wrap(hi.y, (v_a.y >> 2) & 3u, float(v_c.z), float(v_c.w), v_s.y, true));
    a = ((a % size) + size) % size;
    b = ((b % size) + size) % size;
    return mix(mix(FETCH_T(a), FETCH_T(ivec2(b.x, a.y)), w.x),
               mix(FETCH_T(ivec2(a.x, b.y)), FETCH_T(b), w.x), w.y);
}

int wrap_up(int k, uint mode, int lo, int hi, int size, int s) {
    if (mode == 0u) {
        int period = max(size, 1) * s;
        return k - period * int(floor(float(k) / float(period)));
    }
    if (mode == 1u) return clamp(k, 0, max(size, 1) * s - 1);
    if (mode == 2u) return clamp(k, lo * s, max(hi, lo) * s + s - 1);
    int g = int(floor(float(k) / float(s)));
    return ((g & lo) | hi) * s + (k - g * s);
}
/* bilinear in a replacement image stored at sc x the texture's texel grid, the GS wrap / clamp / region modes per original texel */
vec4 sample_region_repeat_up(vec2 tc, float sc) {
    int s = int(sc + 0.5);
    vec2 c = (tc + 0.5) * sc - 0.5;
    vec2 lo = floor(c), w = c - lo;
    ivec2 size = textureSize(TEX, 0).xy;
    uint mu = v_a.y & 3u, mv = (v_a.y >> 2) & 3u;
    int tw = int(v_s.x), th = int(v_s.y);
    int x0 = int(lo.x), y0 = int(lo.y);
    ivec2 a = ivec2(wrap_up(x0, mu, int(v_c.x), int(v_c.y), tw, s), wrap_up(y0, mv, int(v_c.z), int(v_c.w), th, s));
    ivec2 b = ivec2(wrap_up(x0 + 1, mu, int(v_c.x), int(v_c.y), tw, s), wrap_up(y0 + 1, mv, int(v_c.z), int(v_c.w), th, s));
    a -= size * ivec2(floor(vec2(a) / vec2(size)));
    b -= size * ivec2(floor(vec2(b) / vec2(size)));
    return mix(mix(FETCH_T(a), FETCH_T(ivec2(b.x, a.y)), w.x), mix(FETCH_T(ivec2(a.x, b.y)), FETCH_T(b), w.x), w.y);
}

float factor(uint code, float as, float fix) {
    if (code == 0u) return 0.0;
    if (code == 1u) return 1.0;
    if (code == 2u) return as;
    if (code == 3u) return 1.0 - as;
    if (code == 4u) return fix;
    return 1.0 - fix;
}

/* the fragment's position in eye pixels: PS2_RQ_DATE_FREE (pc.flags bit 1) from the vertices' interpolated NDC, not gl_FragCoord
   ([hypothesis] the foveated pass's reduced bins give a doubled gl_FragCoord on the Quest, as its subpassLoad is wrong there) */
vec2 frag_px() { return (pc.flags & 2u) != 0u ? (v_ndc * 0.5 + 0.5) * pc.rqsz : gl_FragCoord.xy; }

void main() {
    if (v_sc.z > v_sc.x) { vec2 fp = frag_px(); if (fp.x < v_sc.x || fp.x >= v_sc.z || fp.y < v_sc.y || fp.y >= v_sc.w) discard; }   /* (PS2_RQ_DATE_FREE: the draw's scissor) */
    vec4 c = v_color;
    uint f = v_a.x;
    uint tfx = f & 3u;
    bool ate = ((f >> 3) & 1u) != 0u;
    bool fge = ((f >> 16) & 1u) != 0u;
    bool tcc = ((f >> 17) & 1u) != 0u;
    bool afkeep = ((f >> 18) & 1u) != 0u;
    bool point = ((f >> 19) & 1u) != 0u;
    bool afonly = ((f >> 20) & 1u) != 0u;
    bool fba = ((f >> 21) & 1u) != 0u;
    vec2 tsz = v_s.xy;
    bool rep = v_rs.x > 1.0;
    vec2 isz = rep ? v_s.zw / v_rs.x : v_s.zw;

    if (v_dr.y != 0u) {                                  /* DATE by rectangles (rq_hud.inc rqh_date): the alpha the writers left at this game pixel */
        float rqA = gl_ViewIndex == 0 ? pc.rqx.x : pc.rqx.z, rqB = gl_ViewIndex == 0 ? pc.rqx.y : pc.rqx.w;
        vec2 e = frag_px() / pc.rqsz * 2.0 - 1.0;
        vec2 g0 = (vec2((e.x - rqB) / rqA, (e.y - pc.rqy.y) / pc.rqy.x) + 1.0) / pc.inv_size;
        vec2 es = (frag_px() + vec2(-0.125, -0.375)) / pc.rqsz * 2.0 - 1.0;   /* (PS2_RQ_DATE_FREE: at sample 0, where the read it replaces looked: the standard 4x pattern) */
        vec2 gs = (vec2((es.x - rqB) / rqA, (es.y - pc.rqy.y) / pc.rqy.x) + 1.0) / pc.inv_size;
        float sgn = (pc.flags & 1u) != 0u ? (gl_ViewIndex == 0 ? -1.0 : 1.0) : 0.0;
        vec2 g = g0;
        g.x -= sgn * v_rs.y;
        bool a1 = false, hit = false;
        uint np = 0u;
        uint pend[4];                                    /* (PS2_RQ_DATE_FREE: conditional writers above the first plain hit, newest first) */
        for (uint i = 0u; i < v_dr.y; i++) {
            vec4 r = w[2u * (v_dr.x + i)], s = w[2u * (v_dr.x + i) + 1u];
            uint kind = uint(s.w + 0.5);                 /* 0 rectangle, 1 triangle (the reader's eye shift); PS2_RQ_DATE_FREE, in eye space: 2 / 3 a triangle, 4 / 5 a rectangle of view 0 / 1 */
            vec2 p = g;
            if (kind >= 2u) { if ((kind & 1u) != uint(gl_ViewIndex)) continue; p = gs; }
            bool in_w;
            if (kind == 1u || kind == 2u || kind == 3u) {
                vec2 a = r.xy, b = r.zw, c = s.xy;
                float s0 = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
                float s1 = (c.x - b.x) * (p.y - b.y) - (c.y - b.y) * (p.x - b.x);
                float s2 = (a.x - c.x) * (p.y - c.y) - (a.y - c.y) * (p.x - c.x);
                in_w = (s0 >= 0.0 && s1 >= 0.0 && s2 >= 0.0) || (s0 <= 0.0 && s1 <= 0.0 && s2 <= 0.0);
            } else in_w = p.x >= r.x && p.x < r.z && p.y >= r.y && p.y < r.w;
            if (!in_w) continue;
            uint code = uint(s.z + 0.5);                 /* a1 + 2 cond (cond 1 / 2: written only where the alpha below is < 1 / >= 1) */
            if ((code >> 1) != 0u) { if (np < 4u) pend[np++] = code; continue; }
            a1 = (code & 1u) != 0u; hit = true; break;
        }
        bool datm = ((f >> 23) & 1u) != 0u;
        bool cur = hit ? a1 : datm;                      /* (no writer here: the alpha below passes, as before) */
        for (int k = int(np) - 1; k >= 0; k--) {         /* (the masks in their order, over what the scan found below them) */
            uint code = pend[k];
            if (cur == ((code >> 1) == 2u)) cur = (code & 1u) != 0u;
        }
        if (cur != datm) discard;
    }

    if (((v_a.z >> 8) & 1u) != 0u) {                     /* (textured) */
        vec2 tc = ((f >> 2) & 1u) != 0u ? v_stq.xy : (v_stq.xy / max(v_stq.z, 1e-9)) * tsz;
        vec2 tc_frag = tc;
        uint ruv = v_round_uv & (RUV_SPRITE - 1u);
        if (point && ruv != 0u) {
            float rqA = gl_ViewIndex == 0 ? pc.rqx.x : pc.rqx.z, rqB = gl_ViewIndex == 0 ? pc.rqx.y : pc.rqx.w;
            vec2 e = frag_px() / pc.rqsz * 2.0 - 1.0;
            vec2 n = vec2((e.x - rqB) / rqA, (e.y - pc.rqy.y) / pc.rqy.x);
            ivec2 pix = ivec2(floor((n + 1.0) / pc.inv_size));
            if (((f >> 29) & 1u) == 0u)
                tc.x = sprite_round(tc.x, ruv & 3u, int((ruv >> 4) & 4095u) - 2048, pix.x);
            tc.y = sprite_round(tc.y, (ruv >> 2) & 3u, int((ruv >> 16) & 4095u) - 2048, pix.y);
        }
        if (!point && (f & (1u << 25)) != 0u) tc -= 0.5;
        vec2 unwrapped_tc = tc;
        vec2 gdx = dFdx(unwrapped_tc) / isz, gdy = dFdy(unwrapped_tc) / isz;
        bool own = any(notEqual(isz, tsz));
        tc.x = gs_wrap(tc.x, v_a.y & 3u, float(v_c.x), float(v_c.y), tsz.x, own);
        tc.y = gs_wrap(tc.y, (v_a.y >> 2) & 3u, float(v_c.z), float(v_c.w), tsz.y, own);
        vec4 t;
        if (rep && point && ((f >> 31) & 1u) != 0u) {
            vec2 p0 = floor(unwrapped_tc + (ruv == 0u ? 1.0 / 32.0 : 0.0));
            t = sample_region_repeat_up(p0, v_rs.x);
        } else if (rep && point) {
            float s = v_rs.x;
            vec2 p0 = floor(unwrapped_tc + (ruv == 0u ? 1.0 / 32.0 : 0.0));
            vec2 fc = (p0 + clamp(tc_frag - p0, 0.0, 1.0)) * s - 0.5;
            vec2 fw = vec2(1.0 / 64.0);                  /* (gs.frag's tdx / tdy are 0 at pc.scale 1) */
            vec2 lo = floor(fc);
            fc = clamp(lo + clamp((fc - lo - 0.5) / fw + 0.5, 0.0, 1.0), p0 * s, p0 * s + s - 1.0);
            t = sample_region_repeat_up((fc + 0.5) / s - 0.5, s);
        } else if (rep) {
            t = sample_region_repeat_up(unwrapped_tc, v_rs.x);
        } else if (point) {
            ivec2 size = textureSize(TEX, 0).xy;
            ivec2 p = ivec2(floor(tc + (ruv == 0u ? 1.0 / 32.0 : 0.0)));
            p = ((p % size) + size) % size;
            t = FETCH_T(p);
        } else if ((v_a.y & 3u) == 3u || ((v_a.y >> 2) & 3u) == 3u
                   || (own && ((v_a.y & 3u) == 0u || ((v_a.y >> 2) & 3u) == 0u))) {
            t = sample_region_repeat(unwrapped_tc);
        } else {
            bool whole = (v_a.y & 3u) < 2u && ((v_a.y >> 2) & 3u) < 2u;
            t = textureGrad(TEX, vec3((tc + 0.5) / isz, float(TL)), whole ? gdx : vec2(0.0), whole ? gdy : vec2(0.0));
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

    if (fge) c.rgb = mix(v_fc.rgb, c.rgb, clamp(v_fog, 0.0, 1.0));

    bool apass = true;
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

    /* the fixed-function blend clamps the source and the factors to [0, 1] first (UNORM target) */
    float as = clamp(c.a, 0.0, 1.0);
    vec3 cs = clamp(c.rgb, 0.0, 1.0);
    float sf = factor(v_a.z & 7u, as, v_fc.a);
    float df = factor((v_a.z >> 3) & 7u, as, v_fc.a);
    bool wa = ((v_a.z >> 6) & 1u) != 0u;
    if (((f >> 30) & 1u) != 0u) wa = wa && apass;        /* (alpha-test "RGB only" in one pass: the old alpha where it failed) */
    float a_out = clamp(fba ? max(c.a, 1.0) : c.a, 0.0, 1.0);
    out_color = vec4(cs * sf, wa ? a_out : 0.0);
    out_blend = vec4(vec3(df), wa ? 0.0 : 1.0);
}
