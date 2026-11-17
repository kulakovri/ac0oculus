#version 450
#extension GL_EXT_multiview : require
/* rq_gs_rt: PS2_RQ_X3 (rq_video.inc): the game's full-screen glow overlay (xcls 3, blend 0x164 with FIX, frag flags 0x0202000C: st in texels, modulate, TCC, no fog, an alpha test that never
   discards, bilinear, region clamp) with rq_gs.frag's result and nothing else: one lod-0 read of the render target (a single level, so textureGrad's gradients select nothing). */
layout(location = 0) in mediump vec4 v_color;
layout(location = 1) in vec3 v_stq;

layout(location = 0, index = 0) out mediump vec4 out_color;
layout(location = 0, index = 1) out mediump vec4 out_blend;

layout(set = 0, binding = 0) uniform mediump sampler2DArray tex;

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
    float rep_scale;
    uint  mesh_block;
    uint  mv_flags;
} pc;

void main() {
    vec2 tc = clamp(v_stq.xy - 0.5, vec2(pc.clamp_uv.xz), max(vec2(pc.clamp_uv.yw), vec2(pc.clamp_uv.xz)));   /* (GS region clamp) */
    mediump vec4 c = textureLod(tex, vec3((tc + 0.5) / pc.img_size, (pc.mv_flags & 1u) != 0u ? float(gl_ViewIndex) : 0.0), 0.0) * v_color;
    out_blend = vec4(0.0, 0.0, 0.0, clamp(c.a, 0.0, 1.0));
    out_color = clamp(c, vec4(0.0), vec4(2.0));
}
