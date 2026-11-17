#version 450
#extension GL_EXT_multiview : require
/* rq_layer: R-15 of the native Quest renderer (rq_video.inc rql_*): one full-screen draw inside the eye-sized pass.
   -DCL: our clouds: the 1x canvases (a: over 0 = L, b: over 1 = L + T) as dst = L + dst T (premultiplied, the blend of
   cloud_comp.frag). -DCP: the 3D cockpit's resolved image, premultiplied, as cpit_composite puts it over the frame.
   Each eye pixel goes back to the game's frame through the inverse of the per-view crop (the vertex shaders' x' = A x + B w). */
#ifdef MP
precision mediump float;   /* (PS2_RQ_MP: relaxed precision; coordinates, depth and flags stay highp, named below) */
#endif
layout(set = 0, binding = 0) uniform mediump sampler2DArray a;
#ifdef CL
layout(set = 0, binding = 1) uniform mediump sampler2DArray b;
#endif
layout(push_constant) uniform PC { highp vec4 rqx; highp vec2 rqy; highp vec2 rqsz; } pc;
layout(location = 0) out vec4 o;
void main() {
    highp float A = gl_ViewIndex == 0 ? pc.rqx.x : pc.rqx.z, B = gl_ViewIndex == 0 ? pc.rqx.y : pc.rqx.w;
    highp vec2 e = gl_FragCoord.xy / pc.rqsz * 2.0 - 1.0;
    highp vec2 n = vec2((e.x - B) / A, (e.y - pc.rqy.y) / pc.rqy.x);
    highp vec3 uvw = vec3((n + 1.0) * 0.5, float(gl_ViewIndex));
#ifdef CL
    vec3 l = texture(a, uvw).rgb, w = texture(b, uvw).rgb;
    o = vec4(l, clamp(dot(w - l, vec3(1.0 / 3.0)), 0.0, 1.0));
#else
    o = texture(a, uvw);
#endif
}
