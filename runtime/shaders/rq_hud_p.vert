#version 450
#extension GL_EXT_multiview : require
/* rq_hud_p.vert: the plain path of the HUD batcher (PS2_RQ_HUD=batch, rq_hud.inc rqh_plain): the draws that need nothing but one
   texture read or none. Position as rq_hud.vert; the row's state is folded into a few flat values for rq_hud_p.frag. */
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec3 in_stq;
layout(location = 4) in uint in_round_uv;
layout(location = 5) in float in_dx;
layout(location = 6) in uint in_id;
layout(location = 7) in vec4 in_other;

layout(location = 0) out mediump vec4 v_color;
layout(location = 1) out highp vec3 v_t;        /* the texel coordinate before the divide: xy / z */
layout(location = 2) flat out mediump vec4 v_k; /* colour factors: src = k.x + k.y As, dst = k.z + k.w As */
layout(location = 3) flat out mediump vec4 v_m; /* x: writes alpha, y: the texture's alpha counts (TCC), z: 1 = FBA, w: alpha-test reference for the alpha write, < 0 none */
layout(location = 4) flat out highp vec4 v_u;   /* x: replacement scale, yz: 1 / image size, w: bilinear texel offset */
layout(location = 5) flat out uvec2 v_o;        /* texture slot; bits: 0 textured, 1 point sampled, 2 sprite rounding, 3 no rounding of x, 4 a target box's flat texel, 5/6 region repeat in u/v */
layout(location = 6) flat out highp vec4 v_cl;  /* the texel coordinate's clamp: lo.xy, hi.xy */
layout(location = 7) flat out highp uint v_ruv; /* the rounding of sprite edges (gs.frag's v_round_uv) */
layout(location = 8) flat out highp uvec2 v_dr;  /* DATE by rectangles: first, count (PS2_RQ_HUD_ONE) */
layout(location = 9) flat out highp float v_dx;  /* the vertex's eye shift (the DATE rectangles are the game's) */
layout(location = 10) noperspective out highp vec2 v_ndc;   /* PS2_RQ_DATE_FREE: as rq_hud.vert */

struct Draw { uint flags, wrap, mode, tex; uvec4 clamp_uv; vec2 tex_size; vec2 img_size; vec4 fog; uint dfirst, dcount; float rep, pad; vec4 k; vec4 m; vec4 cl; vec4 sc; };
layout(set = 1, binding = 0, std430) readonly buffer B { Draw d[]; };

layout(push_constant) uniform PC {
    vec4  rqx;
    vec2  rqy;
    vec2  rqsz;
    vec2  inv_size;
    float halfw;
    uint  flags;
} pc;

void main() {
    Draw D = d[in_id];
    uint f = D.flags;
    bool textured = ((D.mode >> 8) & 1u) != 0u;
    v_color = in_color / 128.0;
    if (!textured) v_color.rgb *= 128.0 / 255.0;
    /* (fst: the coordinates are texels; else s/q, t/q are normalised and scale with the texture's size, as gs.frag) */
    v_t = ((f >> 2) & 1u) != 0u ? vec3(in_stq.xy, 1.0) : vec3(in_stq.xy * D.tex_size, in_stq.z);
    v_k = D.k;
    v_m = D.m;
    v_u = vec4(D.rep, 1.0 / D.img_size.x, 1.0 / D.img_size.y, ((f >> 25) & 1u) != 0u ? -0.5 : 0.0);
    bool point = ((f >> 19) & 1u) != 0u;
    uint ruv = in_round_uv & ((1u << 29) - 1u);
    v_o = uvec2(D.tex, (textured ? 1u : 0u) | (point ? 2u : 0u) | (point && ruv != 0u ? 4u : 0u) | (((f >> 29) & 1u) != 0u ? 8u : 0u)
                       | (point && ((f >> 31) & 1u) != 0u && D.rep > 1.0 ? 16u : 0u)
                       | ((D.wrap & 3u) == 3u ? 32u : 0u) | (((D.wrap >> 2) & 3u) == 3u ? 64u : 0u)
                       | (((f >> 23) & 1u) != 0u ? 128u : 0u));
    v_dr = uvec2(D.dfirst, D.dcount);
    v_dx = in_dx;
    v_cl = D.cl;
    v_ruv = ruv;
    float sgn = (pc.flags & 1u) != 0u ? (gl_ViewIndex == 0 ? -1.0 : 1.0) : 0.0;
    float A = gl_ViewIndex == 0 ? pc.rqx.x : pc.rqx.z, B = gl_ViewIndex == 0 ? pc.rqx.y : pc.rqx.w;
    vec2 p = vec2(A * ((in_pos.x + sgn * in_dx) * pc.inv_size.x - 1.0) + B, pc.rqy.x * (in_pos.y * pc.inv_size.y - 1.0) + pc.rqy.y);
    if (in_other.w != 0.0) {
        vec2 q = vec2(A * ((in_other.x + sgn * in_other.z) * pc.inv_size.x - 1.0) + B, pc.rqy.x * (in_other.y * pc.inv_size.y - 1.0) + pc.rqy.y);
        vec2 d_px = (q - p) * 0.5 * pc.rqsz;
        float l = length(d_px);
        if (l > 1e-6) {
            vec2 n = vec2(-d_px.y, d_px.x) / l;
            p += n * in_other.w * pc.halfw * 2.0 / pc.rqsz;
        }
    }
    gl_Position = vec4(p, in_pos.z, 1.0);
    v_ndc = p;
}
