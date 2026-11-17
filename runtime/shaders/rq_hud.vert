#version 450
#extension GL_EXT_multiview : require
/* rq_hud.vert: the HUD batcher of the eye-sized pass (PS2_RQ_HUD=batch, rq_hud.inc). One vertex stream built on the CPU per
   list: the game's class-2 draws, lines already expanded to quads. Per draw: the fragment state from a table (the storage
   buffer), read once per vertex here and handed on flat. Positions as gs.vert -DMV -DRQ: game pixels -> the frame's NDC ->
   the per-view crop. A line vertex carries its other end and a side: the quad is widened in eye pixels, width as the
   old path's vkCmdSetLineWidth (rqe.pxs eye pixels = one game pixel across), the rectangle of strict wide lines. */
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec3 in_stq;
layout(location = 3) in float in_fog;
layout(location = 4) in uint in_round_uv;
layout(location = 5) in float in_dx;          /* eye shift (xr_dx: game pixels, view 0 negative) */
layout(location = 6) in uint in_id;           /* the draw's row in the table */
layout(location = 7) in vec4 in_other;        /* lines: the other end x, y, its eye shift; w: side, 0 = not a line vertex */

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec3 v_stq;
layout(location = 2) out float v_fog;
layout(location = 3) flat out uint v_round_uv;
layout(location = 4) flat out uvec4 v_a;      /* flags, wrap, mode, texture slot */
layout(location = 5) flat out uvec4 v_c;      /* clamp_uv */
layout(location = 6) flat out vec4 v_s;       /* tex_size, img_size */
layout(location = 7) flat out vec4 v_fc;      /* fog colour, FIX */
layout(location = 8) flat out uvec2 v_dr;     /* DATE by rectangles: first, count (0: none) */
layout(location = 9) flat out vec2 v_rs;      /* replacement image's scale (> 1), this draw's eye shift */
layout(location = 10) flat out vec4 v_sc;     /* PS2_RQ_DATE_FREE: the draw's scissor in eye pixels (z <= x: none) */
layout(location = 11) noperspective out vec2 v_ndc;   /* PS2_RQ_DATE_FREE: the position as NDC from the vertices (linear on screen: exact), not gl_FragCoord */

struct Draw { uint flags, wrap, mode, tex; uvec4 clamp_uv; vec2 tex_size; vec2 img_size; vec4 fog; uint dfirst, dcount; float rep, pad; vec4 k; vec4 m; vec4 cl; vec4 sc; };
layout(set = 1, binding = 0, std430) readonly buffer B { Draw d[]; };

layout(push_constant) uniform PC {
    vec4  rqx;        /* per-view crop: x' = A x + B w (A0, B0, A1, B1) */
    vec2  rqy;        /* y' = A y + B w */
    vec2  rqsz;       /* the eye image's size */
    vec2  inv_size;   /* 2 / game frame */
    float halfw;      /* half the line width in eye pixels */
    uint  flags;      /* 1: shift the eyes (mv_shift) */
} pc;

void main() {
    v_color = in_color / 128.0;
    v_stq = in_stq;
    v_fog = in_fog;
    v_round_uv = in_round_uv;
    Draw D = d[in_id];
    v_a = uvec4(D.flags, D.wrap, D.mode, D.tex);
    v_c = D.clamp_uv;
    v_s = vec4(D.tex_size, D.img_size);
    v_fc = D.fog;
    v_dr = uvec2(D.dfirst, D.dcount);
    v_rs = vec2(D.rep, in_dx);
    v_sc = D.sc;
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
