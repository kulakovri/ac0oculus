#version 450
#extension GL_EXT_multiview : require
/* The Quest cockpit (rq_cockpit.inc): cockpit.vert for the parts still drawn from the old 36-byte buffer in the eye
   pass -- the screens (cockpit_screen.frag) and the combiner mark's depth passes -- with the per-view crop and the depth
   of the top of the range. Same push constants as cockpit.vert; misc.z / misc.w = zA, zB of the depth (clip z = zA w + zB),
   the crop in sd (A0 B0 A1 B1) and wx.xy (A, B of y). */
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_nrm;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in float in_ao;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec3 v_n;
layout(location = 2) out vec3 v_p;
layout(location = 3) out float v_ao;

layout(push_constant) uniform CP {
    vec4 rot[3];
    vec4 head;
    vec4 proj;
    vec4 misc;
    vec4 color;
    vec4 light;
    vec4 model[3];
    vec4 su, sv, sd;
    vec4 uvoff;
    vec4 wx;
} cp;

void main() {
    vec3 pa = vec3(dot(cp.model[0].xyz, in_pos) + cp.model[0].w, dot(cp.model[1].xyz, in_pos) + cp.model[1].w,
                   dot(cp.model[2].xyz, in_pos) + cp.model[2].w);
    vec3 na = vec3(dot(cp.model[0].xyz, in_nrm), dot(cp.model[1].xyz, in_nrm), dot(cp.model[2].xyz, in_nrm));
    vec3 v = pa - cp.head.xyz;
    vec3 c = vec3(dot(cp.rot[0].xyz, v), dot(cp.rot[1].xyz, v), dot(cp.rot[2].xyz, v));
    float eye = gl_ViewIndex == 0 ? -1.0 : 1.0;
    c.x -= eye * cp.head.w;
    float w = -c.z;
    float X = (cp.proj.z * cp.misc.x - 1.0) * w + cp.proj.x * cp.misc.x * c.x;
    float Y = (cp.proj.w * cp.misc.y - 1.0) * w - cp.proj.y * cp.misc.y * c.y;
    float rqA = gl_ViewIndex == 0 ? cp.sd.x : cp.sd.z, rqB = gl_ViewIndex == 0 ? cp.sd.y : cp.sd.w;
    gl_Position = vec4(rqA * X + rqB * w, cp.wx.x * Y + cp.wx.y * w, cp.misc.z * w + cp.misc.w, w);
    v_uv = in_uv + cp.uvoff.xy;
    v_n = na;
    v_p = pa;
    v_ao = in_ao;
}
