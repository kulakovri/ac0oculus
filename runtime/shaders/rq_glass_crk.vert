#version 450
#extension GL_EXT_multiview : require
/* Canopy cracks in the Quest eye pass (rq_cockpit.inc rqgx_crk_*, ACZ_VR_RQ_COCKPIT_CRACKS, R-58): the generator's segments
   (ps2_cracks.c, ps2_crk_rq_export: their two ends on the glass) as thin ribbons on the canopy, shaded per vertex for this
   eye as cockpit.frag crk_glass shades a crack's face -- the sky's silver where the face turns the ray up, the dark
   cockpit where down, brighter towards total internal reflection, the sun's glint along the crack -- at least 1.3 px wide
   with the energy kept (true width / drawn width), revealed as they run (t0 / t1); and per impact the crushed core and
   its halo of micro-cracks as a small polar patch on the glass, shaded in the fragment stage from the interpolated
   impact-plane coordinates. Push: x 0 = segments (6 vertices each), 1 = core vertices. No position read (R-45). */
struct Seg { vec4 a; vec4 b; vec4 n; vec4 f; };   /* a3 t0 | b3 t1 | the impact's n, true width | flags, brightness, impact, index */
layout(std430, set = 3, binding = 3) readonly buffer Segs { Seg sg[]; };
struct Core { vec4 p; vec4 q; vec4 h; };         /* xyz u | v rc rh core alpha | hole birth impact - */
layout(std430, set = 3, binding = 4) readonly buffer Cores { Core cr[]; };
layout(std140, set = 2, binding = 2) uniform Fr {
    vec4 rot[3]; vec4 head; vec4 proj; vec4 misc; vec4 xfx; vec4 xfy; vec4 l[3]; vec4 r[4]; vec4 dome[4]; vec4 cloud;
    vec4 su, sv, sd; vec4 pack; vec4 tub; vec4 flp[4]; vec4 flc[4]; vec4 night; vec4 mir[3]; vec4 mirg; vec4 eyesize;
    vec4 wa, wb, wc, wd, we, wf; vec4 uvk[2]; vec4 wg, wh;
    vec4 gx[8];
} F;
layout(push_constant) uniform Pc { vec4 k; } pc;
layout(location = 0) out mediump vec4 v_col;    /* segments: the colour, premultiplied; cores: the milk colour, the sun's share */
layout(location = 1) out highp vec2 v_uv;        /* cores: the impact-plane coordinates (m) */
layout(location = 2) flat out vec4 v_k;          /* x kind (0 segment, 1 core), y rc, z rh, w core alpha */
layout(location = 3) flat out vec4 v_h;          /* cores: x hole, y grow, z impact, w strength */
layout(location = 4) out highp float v_px;      /* cores: one pixel (m) */

vec4 clip_of(vec3 pa, float eye) {
    vec3 v = pa - F.head.xyz;
    vec3 c = vec3(dot(F.rot[0].xyz, v), dot(F.rot[1].xyz, v), dot(F.rot[2].xyz, v));
    c.x -= eye * F.head.w;
    float w = -c.z;
    float X = (F.proj.z * F.misc.x - 1.0) * w + F.proj.x * F.misc.x * c.x;
    float Y = (F.proj.w * F.misc.y - 1.0) * w - F.proj.y * F.misc.y * c.y;
    float rqA = gl_ViewIndex == 0 ? F.xfx.x : F.xfx.z, rqB = gl_ViewIndex == 0 ? F.xfx.y : F.xfx.w;
    return vec4(rqA * X + rqB * w, F.xfy.x * Y + F.xfy.y * w, F.misc.z * w + F.misc.w, w);
}

void main() {
    float eye = gl_ViewIndex == 0 ? -1.0 : 1.0;
    vec3 E = F.head.xyz + eye * F.head.w * F.rot[0].xyz;
    vec3 amb = max(F.r[3].rgb, 0.0);
    vec3 S = F.gx[0].xyz, hc = F.gx[1].rgb;
    float now = F.gx[4].x, str = F.gx[4].z;
    bool sunok = hc.r + hc.g + hc.b > 0.0;
    /* (ACZ_VR_RQ_CRACK_NIGHT, R-86: the cockpit's own light at night -- the night instrument lighting's level x tint x the strength;
       0 by day and with the switch 0, so the rest is cockpit.frag's crk_glass as before) */
    vec3 nc = F.night.x > 0.0 ? F.night.yzw * (F.night.x * F.gx[5].y) : vec3(0.0);
    gl_Position = vec4(-4.0, -4.0, 0.5, 1.0); v_col = vec4(0.0); v_uv = vec2(0.0); v_k = vec4(0.0); v_h = vec4(0.0); v_px = 1e-3;
    if (pc.k.x < 0.5) {
        int i = gl_VertexIndex / 6, k = gl_VertexIndex - 6 * (gl_VertexIndex / 6);
        float side = k == 1 || k == 2 || k == 4 ? 1.0 : -1.0, endb = k == 2 || k == 4 || k == 5 ? 1.0 : 0.0;
        Seg g = sg[i];
        float hv = clamp((now - g.a.w) / max(g.b.w - g.a.w, 1e-4), 0.0, 1.0);   /* how far the crack has run along it */
        if (hv <= 0.0) return;
        vec3 a3 = g.a.xyz, b3 = mix(g.a.xyz, g.b.xyz, hv);
        vec3 ab = g.b.xyz - g.a.xyz;
        float al = length(ab);
        if (al < 1e-6) return;
        vec3 t3 = ab / al, ns = normalize(g.n.xyz);
        vec3 bn = normalize(cross(ns, t3));
        vec3 P = endb > 0.5 ? b3 : a3;
        vec3 D = normalize(P - E);
        vec4 cp = clip_of(P, eye);
        if (cp.w < 0.05) return;
        float px = cp.w / F.gx[4].w;
        float sa = max(length(bn - D * dot(bn, D)), 0.25);       /* (the ribbon's across direction as the eye sees it) */
        float wb = g.n.w, wd = max(wb, 1.3 * px / sa);
        float cov = wb / wd;
        uint fl = uint(g.f.x + 0.5);
        float tilt = float(fl >> 3) * (0.7 / 15.0);
        vec3 nfc = normalize(bn * cos(tilt) + ns * sin(tilt));    /* the crack's face */
        vec3 R = reflect(D, nfc);
        /* (night: a face turning the ray down mirrors the lit panel, one turning it up the dark sky through the glass; the crushed
           faces scatter some of the panel's light either way) */
        vec3 env = mix(amb * 0.3 + 0.02 + nc, amb * 1.6 + 0.12 + nc * 0.4, smoothstep(-0.45, 0.25, R.y));
        float ci = abs(dot(D, nfc)), Fr = 0.3 + 0.7 * (1.0 - ci) * (1.0 - ci) * (1.0 - ci);
        vec3 col = env * (0.7 + 0.5 * Fr);
        if (sunok) { vec3 Q = S - D; float gq = dot(Q, t3); col += hc * (exp(-gq * gq * (1.0 / 0.05)) * (0.35 + 0.65 * Fr) * 1.3); }
        float hk = ((fl >> 2) & 1u) != 0u ? 0.5 : 1.0;            /* (in the gunsight cone: half contrast) */
        float op = g.f.y;
        col *= op;
        float a = clamp(cov * (0.7 + 0.25 * Fr) * min(op, 1.0) * hk * str, 0.0, 1.0);
        vec3 Pq = P + bn * (side * 0.5 * wd);
        gl_Position = clip_of(Pq, eye);
        v_col = vec4(col * a, a);
    } else {
        Core c = cr[gl_VertexIndex];
        vec3 P = c.p.xyz;
        vec4 cp = clip_of(P, eye);
        gl_Position = cp;
        v_uv = vec2(c.p.w, c.q.x);
        v_px = max(cp.w, 0.05) / F.gx[4].w;
        vec3 D = normalize(P - E);
        float sunf = sunok ? exp(-length(S - D) * 3.0) : 0.0;     /* forward scatter near the sun's image */
        v_col = vec4(amb * 1.3 + 0.08 + nc * 0.8 + hc * (0.6 * sunf), 0.0);
        v_k = vec4(1.0, c.q.y, c.q.z, c.q.w);
        v_h = vec4(c.h.x, clamp((now - c.h.y) / 0.03, 0.0, 1.0), c.h.z, str);
    }
}
