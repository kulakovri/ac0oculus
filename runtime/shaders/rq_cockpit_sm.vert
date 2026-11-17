#version 450
/* The Quest cockpit's merged casters into the sun's shadow map (rq_cockpit.inc rqc_shadow_draw): the same quantised
   vertices and table rows as the eye pass, one draw for all solid and cut parts; the map's rows u, v, d in the push. */
layout(location = 0) in uvec4 in_pid;   /* position q16, w = mesh id */

struct Row { vec4 qp; vec4 qt; vec4 col; vec4 sp; };
layout(std430, set = 2, binding = 0) readonly buffer Rows { Row row[]; };
struct AnimRow { vec4 m[3]; vec4 uvv; };
layout(std430, set = 2, binding = 1) readonly buffer Anims { AnimRow anim[]; };
layout(push_constant) uniform SP { vec4 u, v, d; } sp;

void main() {
    Row R = row[in_pid.w];
    AnimRow A = anim[in_pid.w];
    if (A.uvv.z == 0.0) { gl_Position = vec4(-4.0, -4.0, 0.5, 1.0); return; }
    vec3 p = vec3(in_pid.xyz) * (R.qp.w / 65535.0) + R.qp.xyz;
    vec3 pa = vec3(dot(A.m[0].xyz, p) + A.m[0].w, dot(A.m[1].xyz, p) + A.m[1].w, dot(A.m[2].xyz, p) + A.m[2].w);
    gl_Position = vec4(dot(sp.u.xyz, pa) + sp.u.w, dot(sp.v.xyz, pa) + sp.v.w, dot(sp.d.xyz, pa) + sp.d.w, 1.0);
}
