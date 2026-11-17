#version 450
/* PS2_XR_QUAD_TBLEND (rq19_sparkle 2026-11-16, BUGS_RQ R-260; ps2_video.c rq/rq_qtb.inc): the cinema screen's image for the quad
   layer = this game frame's display region blended with the LAST GAME FRAME's raw region (no feedback), in linear light (the render target holds the
   game's gamma-encoded values): the 3D menus' one-pixel lines (the briefing map's wireframe, the hangar) that pop on and off from
   one game frame to the next at 1x lose half of the step; still pictures are unchanged (a blend of two equal images).
   -DMV: the render target is a 2-layer array (multiview); the screen is its layer par.y. */
#ifdef MV
layout(set = 0, binding = 0) uniform sampler2DArray cur;
#else
layout(set = 0, binding = 0) uniform sampler2D cur;
#endif
layout(set = 1, binding = 0) uniform sampler2D prev;
layout(push_constant) uniform P { vec4 rect; vec4 par; } pc;   /* rect: the region in the target (0..1: x, y, w, h); par.x: the last
                                                                  frame's weight, par.y: the layer, par.zw: the history's corner (its
                                                                  image only grows) */
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o;
vec3 lin(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }
vec3 enc(vec3 c) { c = clamp(c, 0.0, 1.0); return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }
void main() {
    vec2 uc = pc.rect.xy + v_uv * pc.rect.zw;
#ifdef MV
    vec4 a = textureLod(cur, vec3(uc, pc.par.y), 0.0);
#else
    vec4 a = textureLod(cur, uc, 0.0);
#endif
    vec4 b = textureLod(prev, v_uv * pc.par.zw, 0.0);
    o = vec4(enc(mix(lin(a.rgb), lin(b.rgb), pc.par.x)), a.a);
}
