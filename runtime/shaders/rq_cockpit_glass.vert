#version 450
#extension GL_EXT_multiview : require
/* The Quest cockpit's dry glass (rq_cockpit.inc, ACZ_VR_COCKPIT_MATERIAL=quest): per-vertex tint and Fresnel. The CPU
   builds the index list of the triangles that show anything at this eye and sun; everything cockpit.frag computes for a
   dry canopy that survives is a function of the interpolated normal. */
layout(location = 0) in uvec4 in_pid;
layout(location = 1) in vec4 in_nao;
layout(location = 2) in uvec2 in_uvq;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out mediump vec3 v_li;
layout(location = 2) out mediump vec3 v_add;
layout(location = 3) out mediump float v_fa;
layout(location = 4) out mediump float v_ca;
layout(location = 5) flat out uint v_tex;
#ifdef WET
layout(location = 6) out vec3 v_p;
layout(location = 7) out vec3 v_n;
layout(location = 8) out mediump vec3 v_amb;
layout(location = 9) out mediump vec3 v_sc;
layout(location = 10) out float v_shade;
layout(location = 11) flat out vec3 v_ln;
#endif

struct Row { vec4 qp; vec4 qt; vec4 col; vec4 sp; };
layout(std430, set = 2, binding = 0) readonly buffer Rows { Row row[]; };
struct AnimRow { vec4 m[3]; vec4 uvv; };
layout(std430, set = 2, binding = 1) readonly buffer Anims { AnimRow anim[]; };
layout(std140, set = 2, binding = 2) uniform Fr {
    vec4 rot[3];
    vec4 head;
    vec4 proj;
    vec4 misc;
    vec4 xfx;
    vec4 xfy;
    vec4 l[3];
    vec4 r[4];
    vec4 dome[4];
    vec4 cloud;
    vec4 su, sv, sd;
    vec4 pack;
} F;

void main() {
    Row R = row[in_pid.w];
    AnimRow A = anim[in_pid.w];
    if (A.uvv.z == 0.0) { gl_Position = vec4(-4.0, -4.0, 0.5, 1.0); v_uv = vec2(0.0); v_li = v_add = vec3(0.0); v_fa = v_ca = 0.0; v_tex = 0u; return; }
    vec3 p = vec3(in_pid.xyz) * (R.qp.w / 65535.0) + R.qp.xyz;
    vec3 pa = vec3(dot(A.m[0].xyz, p) + A.m[0].w, dot(A.m[1].xyz, p) + A.m[1].w, dot(A.m[2].xyz, p) + A.m[2].w);
    vec3 v = pa - F.head.xyz;
    vec3 c = vec3(dot(F.rot[0].xyz, v), dot(F.rot[1].xyz, v), dot(F.rot[2].xyz, v));
    float eye = gl_ViewIndex == 0 ? -1.0 : 1.0;
    c.x -= eye * F.head.w;
    float w = -c.z;
    float X = (F.proj.z * F.misc.x - 1.0) * w + F.proj.x * F.misc.x * c.x;
    float Y = (F.proj.w * F.misc.y - 1.0) * w - F.proj.y * F.misc.y * c.y;
    float rqA = gl_ViewIndex == 0 ? F.xfx.x : F.xfx.z, rqB = gl_ViewIndex == 0 ? F.xfx.y : F.xfx.w;
    gl_Position = vec4(rqA * X + rqB * w, F.xfy.x * Y + F.xfy.y * w, F.misc.z * w + F.misc.w, w);
    v_uv = vec2(in_uvq) * vec2(R.qt.z, R.qt.w) * (1.0 / 65535.0) + R.qt.xy + A.uvv.xy;
    v_tex = uint(R.sp.w);

    vec3 na = vec3(dot(A.m[0].xyz, in_nao.xyz), dot(A.m[1].xyz, in_nao.xyz), dot(A.m[2].xyz, in_nao.xyz));
    vec3 V = normalize(F.head.xyz - pa);
    vec3 n = normalize(na);
    if (dot(n, V) < 0.0) n = -n;
    float ao = clamp(in_nao.w, 0.0, 1.0);

    /* cockpit.frag, the glass's branch */
    vec3 d0 = vec3(F.l[0].x, F.l[1].x, F.l[2].x), d1 = vec3(F.l[0].y, F.l[1].y, F.l[2].y), d2 = vec3(F.l[0].z, F.l[1].z, F.l[2].z);
    vec3 l = vec3(dot(n, d0), dot(n, d1), dot(n, d2));
    l = max(2.0 * l * l * l, 0.0);
    int sun = int(F.pack.x + 0.5);
    vec3 r0 = F.r[0].rgb, r1 = F.r[1].rgb, r2 = F.r[2].rgb;
    vec3 dir = max(r0 * l.x + r1 * l.y + r2 * l.z, 0.0);
    vec3 amb = max(F.r[3].rgb, 0.0);
    float ssl = F.dome[0].w;
    if (ssl > 0.0) {
        float kfac = F.dome[1].w;
        vec3 key = (sun == 0 ? r0 * l.x : (sun == 1 ? r1 * l.y : r2 * l.z)) * kfac;
        float nu = clamp(dot(n, F.dome[0].xyz), -1.0, 1.0);
        vec3 es = F.dome[2].rgb, env = nu >= 0.0 ? mix(es, F.dome[1].rgb, nu) : mix(es, F.dome[3].rgb, -nu);
        amb = mix(amb, max(env, 0.0), ssl);
        dir = mix(dir, max(key, 0.0), ssl);
    }
    float shade = 1.0;
    float kc = F.cloud.w;
    if (kc > 0.0) {
        vec3 cc = vec3(dot(amb, vec3(0.333)) * 1.35 + 0.06);
        vec3 diffuse = cc * (0.62 + 0.38 * (0.5 + 0.5 * n.y));
        amb = mix(amb, diffuse, kc);
        dir *= 1.0 - kc;
        shade *= 1.0 - 0.8 * kc;
    }
    float ghid = clamp(F.dome[2].w, 0.0, 1.0);
    if (ghid > 0.0) { dir *= 1.0 - ghid; shade *= 1.0 - ghid; }
    float smv = F.pack.w;
    float aoa = ssl > 0.0 ? mix(ao, pow(ao, 0.4545), ssl) : ao;
    vec3 li = amb * aoa + dir * (smv > 0.5 ? 0.75 + 0.25 * ao : 0.4 + 0.6 * ao);
    v_li = R.col.rgb * li * F.pack.z;
    v_ca = R.col.a;
    vec3 L = sun == 0 ? d0 : (sun == 1 ? d1 : d2);
    vec3 Ln = normalize(L);
    float s = 0.0;
    if (R.sp.x > 0.0 && dot(n, Ln) > 0.0)
        s = R.sp.x * pow(max(dot(n, normalize(Ln + V)), 0.0), max(R.sp.y, 1.0)) * ao * shade;
    vec3 sc = min(r0 + r1 + r2, vec3(1.5));
    if (ssl > 0.0) sc = mix(sc, min(2.0 * (sun == 0 ? r0 : (sun == 1 ? r1 : r2)) * F.dome[1].w, vec3(1.5)), ssl);
    sc *= 1.0 - ghid;
    float f = pow(1.0 - max(dot(n, V), 0.0), 5.0);
    v_fa = f * 0.35;
    v_add = amb * 0.35 * f + s * sc;
#ifdef WET
    v_p = pa; v_n = n; v_amb = amb; v_sc = sc; v_shade = shade; v_ln = Ln;
#endif
}
