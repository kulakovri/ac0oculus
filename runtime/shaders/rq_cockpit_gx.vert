#version 450
#extension GL_EXT_multiview : require
/* The Quest cockpit's readable glass (rq_cockpit.inc rqgx_*, R-57): rq_cockpit_glass.vert's tint, light and highlight, and on
   the canopy and windscreen (the PC's fx_glass meshes: row flag 4) cockpit.frag glass_light's terms evaluated PER VERTEX for
   this eye -- the Fresnel rim x the sky's tone along the reflected ray (ACZ_VR_RQ_GLASS_FRESNEL), the interior's reflection read
   from the baked interior cube along the reflected ray with acrylic's Fresnel (ACZ_VR_RQ_GLASS_REFLECT), and the sun's lobes
   near its image for the fragment stage's one wear read (ACZ_VR_RQ_GLASS_WEAR) and its glare core (ACZ_VR_RQ_GLASS_GLARE).
   -DWEAR: the near-sun variant (the triangles within 25 deg of the sun's image). No per-fragment position read: the fragment
   stage gets the eye ray as an interpolated vector (v_pe), never gl_FragCoord (the R-45 rule). */
layout(location = 0) in uvec4 in_pid;
layout(location = 1) in vec4 in_nao;
layout(location = 2) in uvec2 in_uvq;
layout(location = 3) in vec2 in_gl;              /* metres along the glass (rqc_model_build: across, along the flow) */
layout(location = 4) in vec3 in_gu;              /* the direction in which in_gl.x grows (cockpit frame) */

layout(location = 0) out vec2 v_uv;
layout(location = 1) out mediump vec3 v_li;
layout(location = 2) out mediump vec3 v_add;
layout(location = 3) out mediump float v_fa;
layout(location = 4) out mediump float v_ca;
layout(location = 5) flat out uint v_tex;
#ifdef WEAR
layout(location = 6) out highp vec2 v_wt;        /* the wear tile's coordinates */
layout(location = 7) out mediump vec2 v_q;       /* Q = S - D along the glass's two axes (the scratches' glint rule) */
layout(location = 8) out mediump vec3 v_sl;      /* the scratches' light: sun colour x strength x the forward-scatter lobe */
layout(location = 9) out highp vec3 v_pe;        /* this eye's ray to the glass, unnormalised (the glare core per fragment) */
layout(location = 10) out mediump vec3 v_gl;     /* the glare's light: sun colour x strength x lit */
#endif
#ifdef RFS                                       /* (ACZ_VR_RQ_GLASS_REFLECT_FS: the interior read per fragment) */
#ifndef WEAR
layout(location = 9) out highp vec3 v_pe;
#endif
layout(location = 11) out mediump vec3 v_n;      /* the normal facing the eye */
layout(location = 12) out mediump vec3 v_rim;    /* the rim before the interior takes its place */
layout(location = 13) out mediump float v_lv;    /* the interior's light level x the reflection's strength (0: none here) */
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
    vec4 tub;
    vec4 flp[4];
    vec4 flc[4];
    vec4 night;
    vec4 mir[3];
    vec4 mirg;
    vec4 eyesize;
    vec4 wa, wb, wc, wd, we, wf;
    vec4 uvk[2];
    vec4 wg, wh;
    vec4 gx[8];       /* rq_cockpit.inc rqgx_frame: [0] sun xyz, scratch strength; [1] sun colour, glare strength; [2] reflection,
                         Fresnel x sky, dust, the interior's parallax sphere; [3] one geometry for both eyes, 1 / sigma^2 of the
                         scratches' glint, the wear tile (1/m), -; [4] the cracks' clock, -, strength, the eye's focal length (px);
                         [5..7] spare / debug */
} F;
layout(set = 3, binding = 0) uniform samplerCube interior;
layout(push_constant) uniform Pc { vec4 k; } pc;  /* x 1: the new terms alone, additive (the triangles the lean wet canopy draws) */

/* the outside's tone along a cockpit-frame direction (as rq_cockpit.vert's mirror stand-in: the scene light's bands) */
vec3 sky_tone(vec3 rr) {
    float wy = dot(rr, F.mir[0].xyz);
    float t = 1.0 - min(max(wy, 0.0) * 1.25, 1.0);
    vec3 up = mix(F.mir[1].rgb, F.mir[2].rgb, t * sqrt(t));
    float d = max(-wy, 0.0);
    vec3 dn = mix(F.mir[2].rgb, F.mirg.rgb, smoothstep(0.0, 0.06, d));
    return mix(dn, up, smoothstep(-0.15, 0.15, wy));
}

void main() {
    Row R = row[in_pid.w];
    AnimRow A = anim[in_pid.w];
    if (A.uvv.z == 0.0) {
        gl_Position = vec4(-4.0, -4.0, 0.5, 1.0); v_uv = vec2(0.0); v_li = v_add = vec3(0.0); v_fa = v_ca = 0.0; v_tex = 0u;
#ifdef WEAR
        v_wt = vec2(0.0); v_q = vec2(0.0); v_sl = vec3(0.0); v_pe = vec3(0.0, 0.0, -1.0); v_gl = vec3(0.0);
#endif
#ifdef RFS
        v_pe = vec3(0.0, 0.0, -1.0); v_n = vec3(0.0, 0.0, 1.0); v_rim = vec3(0.0); v_lv = 0.0;
#endif
        return;
    }
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

    /* rq_cockpit_glass.vert's light, unchanged */
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
    vec3 rim = amb * 0.35 * f;
    vec3 add = s * sc;
    bool only = pc.k.x > 0.5;
    if (only) { v_ca = 0.0; v_fa = 0.0; rim = vec3(0.0); add = vec3(0.0); }

    uint fl = uint(R.sp.z + 0.5);
#ifdef WEAR
    v_wt = vec2(0.0); v_q = vec2(0.0); v_sl = vec3(0.0); v_pe = vec3(0.0, 0.0, -1.0); v_gl = vec3(0.0);
#endif
#ifdef RFS
    v_n = n; v_lv = 0.0;
    {   float ek0 = F.gx[3].x > 0.5 ? 0.0 : eye; v_pe = pa - (F.head.xyz + ek0 * F.head.w * F.rot[0].xyz); }
#endif
    if ((fl & 4u) != 0u) {                       /* the canopy / windscreen (fx_glass): glass_light's terms for this eye */
        float ek = F.gx[3].x > 0.5 ? 0.0 : eye;
        vec3 E = F.head.xyz + ek * F.head.w * F.rot[0].xyz;
        vec3 D = normalize(pa - E);
        float cv = max(dot(n, -D), 0.0), c5 = 1.0 - cv;
        c5 = c5 * c5 * c5 * c5 * c5;
        vec3 Rr = reflect(D, n);                 /* (n faces the eye: back into the cockpit, or out through the far side) */
        float kf = F.gx[2].y;
        if (kf > 0.0 && !only) rim = mix(rim, sky_tone(Rr) * (0.35 * c5), kf);   /* the rim takes the tone of what the ray meets beyond */
        float rw = F.gx[2].x;
#ifdef RFS
        if (rw > 0.0) v_lv = clamp(dot(amb, vec3(0.333)) + 0.2 * dot(sc, vec3(0.333)), 0.03, 1.2) * rw;
        if (false) {
#else
        if (rw > 0.0) {
#endif
            float bq = dot(pa, Rr), cq = dot(pa, pa) - F.gx[2].w * F.gx[2].w, disc = bq * bq - cq;
            float t = disc > 0.0 ? (cq > 0.0 ? -bq - sqrt(disc) : -bq + sqrt(disc)) : -1.0;
            vec4 ic = textureLod(interior, t > 0.0 ? pa + t * Rr : Rr, 0.0);
            float fr = min(0.04 + 0.96 * c5, 0.15);
            float lv = clamp(dot(amb, vec3(0.333)) + 0.2 * dot(sc, vec3(0.333)), 0.03, 1.2);
            add += ic.rgb * (fr * lv * rw);
            rim *= 1.0 - ic.a * min(rw, 1.0);    /* (where the interior shows it takes the uniform rim's place, as glass_light) */
        }
#ifdef WEAR
        vec3 S = F.gx[0].xyz, g1 = F.gx[1].rgb;
        vec3 Q = S - D;
        float th = 2.0 * asin(min(length(Q) * 0.5, 1.0));
        float behind = -dot(S, n);
        float lit = smoothstep(0.0, 0.1, behind);
        float fade = 1.0 - smoothstep(0.3, 0.4, th);
        vec3 e1 = in_gu - n * dot(in_gu, n);
        float el = dot(e1, e1);
        e1 = el > 1e-6 ? e1 * inversesqrt(el) : normalize(cross(n, abs(n.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
        vec3 e2 = cross(n, e1);
        v_q = vec2(dot(Q, e1), dot(Q, e2));
        v_wt = in_gl * F.gx[3].z;
        v_sl = g1 * (F.gx[0].w * lit * 50.0 * exp(-th * 5.0) * fade);
        v_pe = pa - E;
        v_gl = g1 * (F.gx[1].w * lit * (0.35 + 0.65 * min(F.gx[0].w, 2.0)));
#endif
    }
#ifdef RFS
    v_rim = rim; v_add = add;
#else
    v_add = rim + add;
#endif
}
