#version 450
#extension GL_EXT_nonuniform_qualifier : require
/* rq_hud_p.frag: the plain path of the HUD batcher: one texture read or none, no table, no DATE, no fog, no region modes, no sprite
   rounding (rq_hud.inc rqh_plain says which draws qualify; the rest take rq_hud.frag). Same results as rq_hud.frag for those
   draws. mediump except the texture coordinates (the replacement atlases are large). */
#extension GL_EXT_multiview : require
precision mediump float;

layout(location = 0) in vec4 v_color;
layout(location = 1) in highp vec3 v_t;
layout(location = 2) flat in vec4 v_k;
layout(location = 3) flat in vec4 v_m;
layout(location = 4) flat in highp vec4 v_u;
layout(location = 5) flat in highp uvec2 v_o;
layout(location = 6) flat in highp vec4 v_cl;
layout(location = 7) flat in highp uint v_ruv;
layout(location = 8) flat in highp uvec2 v_dr;
layout(location = 9) flat in highp float v_dx;
layout(location = 10) noperspective in highp vec2 v_ndc;

layout(location = 0, index = 0) out vec4 out_color;
layout(location = 0, index = 1) out vec4 out_blend;

layout(set = 0, binding = 0) uniform sampler2DArray tab[256];
layout(set = 1, binding = 1, std430) readonly buffer W { highp vec4 w[]; };   /* DATE writers (rq_hud.frag) */

layout(push_constant) uniform PC {
    highp vec4  rqx;
    highp vec2  rqy;
    highp vec2  rqsz;
    highp vec2  inv_size;
    highp float halfw;
    highp uint  flags;
} pc;

highp float sprite_round(highp float t, uint mode, int origin, int pixel) {
    if (mode == 0u) return t + 1.0 / 1024.0;
    highp float halftexel = floor(t * 2.0 + 0.5) * 0.5;
    if (abs(t - halftexel) > 1.0 / 64.0) return t;
    return halftexel + ((mode == 1u || pixel == origin) ? 1.0 : -1.0) / 64.0;
}

highp vec2 frag_px() { return (pc.flags & 2u) != 0u ? (v_ndc * 0.5 + 0.5) * pc.rqsz : gl_FragCoord.xy; }   /* (PS2_RQ_DATE_FREE: as rq_hud.frag) */

void main() {
    if (v_dr.y != 0u) {                                  /* DATE by rectangles (rq_hud.frag): the alpha the writers left at this game pixel */
        highp float rqA = gl_ViewIndex == 0 ? pc.rqx.x : pc.rqx.z, rqB = gl_ViewIndex == 0 ? pc.rqx.y : pc.rqx.w;
        highp vec2 e = frag_px() / pc.rqsz * 2.0 - 1.0;
        highp vec2 g0 = (vec2((e.x - rqB) / rqA, (e.y - pc.rqy.y) / pc.rqy.x) + 1.0) / pc.inv_size;
        highp vec2 es = (frag_px() + vec2(-0.125, -0.375)) / pc.rqsz * 2.0 - 1.0;   /* (PS2_RQ_DATE_FREE: at sample 0, as rq_hud.frag) */
        highp vec2 gs = (vec2((es.x - rqB) / rqA, (es.y - pc.rqy.y) / pc.rqy.x) + 1.0) / pc.inv_size;
        highp vec2 g = g0;
        g.x -= ((pc.flags & 1u) != 0u ? (gl_ViewIndex == 0 ? -1.0 : 1.0) : 0.0) * v_dx;
        bool a1 = false, hit = false;
        uint np = 0u;
        uint pend[4];                                    /* (PS2_RQ_DATE_FREE: as rq_hud.frag) */
        for (uint i = 0u; i < v_dr.y; i++) {
            highp vec4 r = w[2u * (v_dr.x + i)], s = w[2u * (v_dr.x + i) + 1u];
            uint kind = uint(s.w + 0.5);
            highp vec2 p = g;
            if (kind >= 2u) { if ((kind & 1u) != uint(gl_ViewIndex)) continue; p = gs; }
            bool in_w;
            if (kind == 1u || kind == 2u || kind == 3u) {
                highp vec2 a = r.xy, b = r.zw, t = s.xy;
                highp float s0 = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
                highp float s1 = (t.x - b.x) * (p.y - b.y) - (t.y - b.y) * (p.x - b.x);
                highp float s2 = (a.x - t.x) * (p.y - t.y) - (a.y - t.y) * (p.x - t.x);
                in_w = (s0 >= 0.0 && s1 >= 0.0 && s2 >= 0.0) || (s0 <= 0.0 && s1 <= 0.0 && s2 <= 0.0);
            } else in_w = p.x >= r.x && p.x < r.z && p.y >= r.y && p.y < r.w;
            if (!in_w) continue;
            uint code = uint(s.z + 0.5);
            if ((code >> 1) != 0u) { if (np < 4u) pend[np++] = code; continue; }
            a1 = (code & 1u) != 0u; hit = true; break;
        }
        bool datm = (v_o.y & 128u) != 0u;
        bool cur = hit ? a1 : datm;
        for (int k = int(np) - 1; k >= 0; k--) {
            uint code = pend[k];
            if (cur == ((code >> 1) == 2u)) cur = (code & 1u) != 0u;
        }
        if (cur != datm) discard;
    }
    vec4 c = v_color;
    if ((v_o.y & 1u) != 0u) {
        highp vec2 tc = v_t.xy / max(v_t.z, 1e-9);
        highp vec2 uv;
        highp float s = v_u.x;
        if ((v_o.y & 2u) != 0u) {
            /* point: the game's texel, clamped as its wrap modes clamp it; a replacement image: inside it this fragment's own position,
               nearest in its s x s block (gs.frag's "sharp bilinear", one filtered read at the snapped coordinate). s = 1: the texel itself */
            highp vec2 tcf = tc;
            highp float o = 1.0 / 32.0;
            if ((v_o.y & 4u) != 0u) {                    /* the rounding of sprite edges to the game's own pixel */
                uint ruv = v_ruv;
                highp float rqA = gl_ViewIndex == 0 ? pc.rqx.x : pc.rqx.z, rqB = gl_ViewIndex == 0 ? pc.rqx.y : pc.rqx.w;
                highp vec2 e = frag_px() / pc.rqsz * 2.0 - 1.0;
                highp vec2 n = vec2((e.x - rqB) / rqA, (e.y - pc.rqy.y) / pc.rqy.x);
                ivec2 pix = ivec2(floor((n + 1.0) / pc.inv_size));
                if ((v_o.y & 8u) == 0u) tc.x = sprite_round(tc.x, ruv & 3u, int((ruv >> 4) & 4095u) - 2048, pix.x);
                tc.y = sprite_round(tc.y, (ruv >> 2) & 3u, int((ruv >> 16) & 4095u) - 2048, pix.y);
                o = 0.0;
            }
            highp vec2 p0 = floor(tc + o);              /* (the wrap modes: x < 0 in v_cl = repeat over the size in zw, else clamp to [xy, zw]) */
            highp vec2 fr = clamp(tcf - p0, 0.0, 1.0);  /* (where in its texel, before the wrap moves the texel) */
            p0.x = (v_o.y & 32u) != 0u ? float((int(p0.x) & int(v_cl.x)) | int(v_cl.z)) : v_cl.x < 0.0 ? mod(p0.x, v_cl.z) : clamp(p0.x, v_cl.x, v_cl.z);
            p0.y = (v_o.y & 64u) != 0u ? float((int(p0.y) & int(v_cl.y)) | int(v_cl.w)) : v_cl.y < 0.0 ? mod(p0.y, v_cl.w) : clamp(p0.y, v_cl.y, v_cl.w);
            highp vec2 k;
            if ((v_o.y & 16u) != 0u) k = (p0 + 0.5) * s - 0.5;   /* (a target box: the texel's flat colour) */
            else {
                k = (p0 + fr) * s - 0.5;
                highp vec2 lo = floor(k);
                k = clamp(lo + clamp((k - lo - 0.5) * 64.0 + 0.5, 0.0, 1.0), p0 * s, p0 * s + s - 1.0);
            }
            uv = (k + 0.5) * v_u.yz;
        } else {
            tc += v_u.w;                                 /* (the clamp modes; repeat is the sampler's, as gs.frag's unwrapped coordinate) */
            if (v_cl.x >= 0.0) tc.x = clamp(tc.x, v_cl.x, v_cl.z);
            if (v_cl.y >= 0.0) tc.y = clamp(tc.y, v_cl.y, v_cl.w);
            uv = (tc + 0.5) * s * v_u.yz;                /* (bilinear; a replacement image scales the texel grid) */
        }
        vec4 t = textureLod(tab[nonuniformEXT(v_o.x)], vec3(uv, 0.0), 0.0);
        c.rgb *= t.rgb;
        c.a *= 1.0 + v_m.y * (t.a - 1.0);
    }
    float as = clamp(c.a, 0.0, 1.0);
    float wa = v_m.w < 0.0 || c.a >= v_m.w ? v_m.x : 0.0;
    out_color = vec4(clamp(c.rgb, 0.0, 1.0) * (v_k.x + v_k.y * as), wa * clamp(max(c.a, v_m.z), 0.0, 1.0));
    out_blend = vec4(vec3(v_k.z + v_k.w * as), 1.0 - wa);
}
