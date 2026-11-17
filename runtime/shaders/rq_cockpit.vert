#version 450
#extension GL_EXT_multiview : require
/* The Quest cockpit material's vertex stage (rq_cockpit.inc, ACZ_VR_COCKPIT_MATERIAL=quest): all of cockpit.frag's
   lighting that is a function of the interpolated normal moves here (the model has no normal map, no ORM and one UV set),
   so the fragment stage is texture(albedo) * (base + key * shadow) + spec * shadow with one shadow read. One merged
   vertex buffer per kind of pipeline; a per-vertex mesh id indexes the table of material rows and matrix rows. */
layout(location = 0) in uvec4 in_pid;   /* position (16 bits a coordinate, bbox of the mesh), w = mesh id */
layout(location = 1) in vec4 in_nao;    /* normal (snorm8), w = AO 0..1 */
layout(location = 2) in uvec2 in_uvq;   /* texture coordinates (16 bits, uv range of the mesh) */

layout(location = 0) out vec2 v_uv;
layout(location = 1) out mediump vec3 v_base;     /* ambient, fill lights, AO, flashes (x material colour) */
layout(location = 2) out mediump vec3 v_key;      /* key-light diffuse (the part the shadow removes) */
layout(location = 3) out mediump vec3 v_spec;     /* key-light specular (not multiplied by the texture) */
layout(location = 4) out vec3 v_sm;               /* shadow map coordinates: uv, depth */
layout(location = 5) flat out uint v_tex;         /* texture table index */
layout(location = 6) out mediump vec3 v_n1;       /* night glow: the faint flood, not multiplied by the mask */
layout(location = 7) out mediump vec3 v_n2;       /* night glow: the markings' term, x the mask */
layout(location = 8) out mediump vec4 v_rpw;      /* NIGHT 2 (R-85): xyz the position from the head, for the panel's area per fragment;
                                                     w a self-lit display page's light (not x the area) */

layout(constant_id = 1) const int NIGHT = 0;
layout(constant_id = 3) const int DBGM = 0;       /* ACZ_VR_RQ_COCKPIT_DBG: 4 = the light replaced by a constant */

struct Row { vec4 qp; vec4 qt; vec4 col; vec4 sp; };
layout(std430, set = 2, binding = 0) readonly buffer Rows { Row row[]; };
struct AnimRow { vec4 m[3]; vec4 uvv; };          /* uvv: uv offset (drum counters), z visible (0: collapse), w free */
layout(std430, set = 2, binding = 1) readonly buffer Anims { AnimRow anim[]; };
layout(std140, set = 2, binding = 2) uniform Fr {
    vec4 rot[3];      /* rows of the inverse head rotation */
    vec4 head;        /* head position, cockpit frame; w = half the eye separation */
    vec4 proj;        /* fx, fy, cx, cy (target pixels) */
    vec4 misc;        /* 2 / target size x, y; zA, zB (clip z = zA w + zB: the cockpit in the top of the depth range) */
    vec4 xfx;         /* the per-view crop: x' = A x + B w; A0, B0, A1, B1 */
    vec4 xfy;         /* A, B (y), head-frame up unused, eye-axis half separation sign unused */
    vec4 l[3];        /* the game's lights' directions (columns x, y, z of the rows: as cockpit.frag's block) */
    vec4 r[4];        /* the lights' colours, r[3] = the ambient */
    vec4 dome[4];     /* scene light: m[0] (up xyz, strength), m[1] (E_up, kfac), m[2] (E_side), m[3] (E_down) */
    vec4 cloud;       /* in-cloud light: rgb, w = kc */
    vec4 su, sv, sd;  /* sun shadow map rows (su.xyz = 0: none) */
    vec4 pack;        /* x key light index, y cloud-shadow factor, z material brightness, w 1 = shadow map used */
    vec4 tub;         /* flash lights: tub (m[0] of the light block), w = number of lights */
    vec4 flp[4];      /* flash positions (xyz, w = d2 eye + r0^2) */
    vec4 flc[4];      /* flash colours (rgb at the eye, w = r0^2) */
    vec4 night;       /* x level (nlv), yzw tint */
    vec4 mir[3];      /* rear-view mirror tone: mir[0] world up xyz, w refl; mir[1] zenith, w ground tone known; mir[2] horizon */
    vec4 mirg;        /* ground tone rgb */
    vec4 eyesize;     /* (rqc_frame) z: the self-lit display pages' strength at night (ACZ_VR_RQ_COCKPIT_SELFLIT, R-85) */
} F;

/* cockpit.frag: flash_light -- up to 4 point lights, per vertex here */
vec3 flash_light(vec3 p, vec3 n, float ao) {
    vec4 tub = F.tub;
    bool inside = abs(p.x) < tub.y && p.z > tub.z - 0.05 && p.z < 1.2;
    vec3 s = vec3(0.0), bn = vec3(0.0);
    for (int i = 0; i < 4; i++) {
        if (float(i) >= tub.w) break;
        vec4 lp = F.flp[i], lc = F.flc[i];
        vec3 L = lp.xyz - p;
        float d2 = dot(L, L);
        vec3 Ld = L * inversesqrt(max(d2, 1e-6));
        float fall = min(lp.w / (d2 + lc.w), 3.0);
        float ndl = dot(n, Ld);
        float lam = max(ndl, 0.0) * 0.85 + 0.15 * (0.5 + 0.5 * ndl);
        float open = 1.0;
        if (inside && p.y < tub.x) {
            if (Ld.y <= 0.02) open = 0.0;
            else {
                vec3 q = p + Ld * ((tub.x - p.y) / Ld.y);
                open = (1.0 - smoothstep(tub.y - 0.08, tub.y + 0.08, abs(q.x))) * smoothstep(tub.z - 0.08, tub.z + 0.08, q.z);
            }
        }
        s += lc.rgb * (fall * lam * open);
        bn += lc.rgb * fall;
    }
    return s * (0.55 + 0.45 * ao) + bn * (0.15 * ao);
}

void main() {
    Row R = row[in_pid.w];
    AnimRow A = anim[in_pid.w];
    if (A.uvv.z == 0.0) {                          /* hidden this frame (select animation, a screen shown as itself) */
        gl_Position = vec4(-4.0, -4.0, 0.5, 1.0);
        v_uv = vec2(0.0); v_base = v_key = v_spec = v_n1 = v_n2 = vec3(0.0); v_sm = vec3(0.0); v_tex = 0u; v_rpw = vec4(0.0);
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
    v_uv = (vec2(in_uvq) * vec2(R.qt.z, R.qt.w) * (1.0 / 65535.0) + R.qt.xy) + A.uvv.xy;
    v_tex = uint(R.sp.w);
    vec3 C = R.col.rgb;
    v_rpw = vec4(0.0);
    if ((DBGM & 4) != 0) { v_base = C * 0.5; v_key = vec3(0.0); v_spec = vec3(0.0); v_n1 = vec3(0.0); v_n2 = vec3(0.0); v_sm = vec3(0.5, 0.5, 0.0); return; }

    vec3 na = vec3(dot(A.m[0].xyz, in_nao.xyz), dot(A.m[1].xyz, in_nao.xyz), dot(A.m[2].xyz, in_nao.xyz));
    vec3 V = normalize(F.head.xyz - pa);
    vec3 n = normalize(na);
    if (dot(n, V) < 0.0) n = -n;                 /* the side facing the eye (the model's normals are not consistent) */
    float ao = clamp(in_nao.w, 0.0, 1.0);

    uint fl = uint(R.sp.z + 0.5);
    if ((fl & 2u) != 0u) {                       /* a rear-view mirror: a tonal stand-in by the reflected ray (TONE) */
        vec3 D = normalize(pa - (F.head.xyz + eye * F.head.w * F.rot[0].xyz));
        vec3 nf = dot(n, D) > 0.0 ? -n : n;
        vec3 rr = reflect(D, nf);
        float wy = dot(rr, F.mir[0].xyz);
        float t = 1.0 - min(max(wy, 0.0) * 1.25, 1.0);
        vec3 up = mix(F.mir[1].rgb, F.mir[2].rgb, t * sqrt(t));
        float d = max(-wy, 0.0);
        vec3 dn = mix(F.mir[2].rgb, F.mirg.rgb, smoothstep(0.0, 0.06, d));
        vec3 env = mix(dn, up, smoothstep(-0.15, 0.15, wy));
        float f5 = 1.0 - max(dot(nf, -D), 0.0);
        f5 = f5 * f5 * f5 * f5 * f5;
        v_base = env * ((0.82 + 0.14 * f5) * F.mir[0].w) * vec3(0.95, 0.98, 1.0);
        v_key = vec3(0.0); v_spec = vec3(0.0); v_n1 = vec3(0.0); v_n2 = vec3(0.0);
        v_sm = vec3(0.5, 0.5, 0.0);
        v_tex = 0u;                              /* the white texture */
        return;
    }

    vec3 d0 = vec3(F.l[0].x, F.l[1].x, F.l[2].x), d1 = vec3(F.l[0].y, F.l[1].y, F.l[2].y), d2 = vec3(F.l[0].z, F.l[1].z, F.l[2].z);
    vec3 l = vec3(dot(n, d0), dot(n, d1), dot(n, d2));
    l = max(2.0 * l * l * l, 0.0);
    int sun = int(F.pack.x + 0.5);
    vec3 r0 = F.r[0].rgb, r1 = F.r[1].rgb, r2 = F.r[2].rgb;
    vec3 keyc = sun == 0 ? r0 * l.x : (sun == 1 ? r1 * l.y : r2 * l.z);
    vec3 dirOther = r0 * l.x + r1 * l.y + r2 * l.z - keyc;
    vec3 L = sun == 0 ? d0 : (sun == 1 ? d1 : d2);
    vec3 Ln = normalize(L);
    vec3 amb = max(F.r[3].rgb, 0.0);
    float shade = 1.0;
    float ssl = F.dome[0].w;
    if (ssl > 0.0) {
        float kfac = F.dome[1].w;
        float nu = clamp(dot(n, F.dome[0].xyz), -1.0, 1.0);
        vec3 es = F.dome[2].rgb;
        vec3 env = nu >= 0.0 ? mix(es, F.dome[1].rgb, nu) : mix(es, F.dome[3].rgb, -nu);
        amb = mix(amb, max(env, 0.0), ssl);
        dirOther *= 1.0 - ssl;
        keyc *= (1.0 - ssl) + ssl * kfac;
    }
    float kc = F.cloud.w;
    if (kc > 0.0) {
        vec3 diffuse = F.cloud.rgb * (0.62 + 0.38 * (0.5 + 0.5 * n.y));
        amb = mix(amb, diffuse, kc);
        dirOther *= 1.0 - kc; keyc *= 1.0 - kc;
        shade *= 1.0 - 0.8 * kc;
    }
    float ks = F.pack.y;
    dirOther *= ks; keyc *= ks; shade *= ks;
    float smv = F.pack.w;
    float aoa = ssl > 0.0 ? mix(ao, pow(ao, 0.4545), ssl) : ao;
    float kao = smv > 0.5 ? 0.75 + 0.25 * ao : 0.4 + 0.6 * ao;
    vec3 flash = F.tub.w > 0.5 ? flash_light(pa, n, ao) : vec3(0.0);
    float bright = F.pack.z;
    v_base = C * (amb * aoa + dirOther * kao + flash) * bright;
    v_key = C * keyc * kao * bright;
    /* the highlight: key light only (Blinn-Phong as cockpit.frag) */
    vec3 sc = min(r0 + r1 + r2, vec3(1.5));
    if (ssl > 0.0) sc = mix(sc, min(2.0 * (sun == 0 ? r0 : (sun == 1 ? r1 : r2)) * F.dome[1].w, vec3(1.5)), ssl);
    float s = 0.0;
    if (R.sp.x > 0.0 && dot(n, Ln) > 0.0)
        s = R.sp.x * pow(max(dot(n, normalize(Ln + V)), 0.0), max(R.sp.y, 1.0)) * ao * shade;
    v_spec = s * sc;
    /* the shadow map coordinates at the vertex (ortho: exact when interpolated), the same offsets as cockpit.frag */
    vec3 q = pa + n * 0.004 + Ln * 0.002;
    vec3 sv = vec3(dot(F.su.xyz, q) + F.su.w, dot(F.sv.xyz, q) + F.sv.w, dot(F.sd.xyz, q) + F.sd.w);
    v_sm = vec3(sv.xy * 0.5 + 0.5, sv.z - 0.0005);
    /* the night panel (cockpit.frag): flood and markings are texture terms, the area is a function of the position */
    if (NIGHT != 0 && F.night.x > 0.0) {
        vec3 rp = pa - F.head.xyz;
        /* (NIGHT 2, ACZ_VR_RQ_COCKPIT_NIGHT_FS, R-85: the area per fragment, rq_cockpit.frag -- per vertex a big floor or sill triangle
           whose corners leave the area took almost no flood in its middle, where cockpit.frag lights it per pixel) */
        float area = NIGHT == 2 ? 1.0 : (1.0 - smoothstep(-0.12, -0.06, rp.y)) * (1.0 - smoothstep(1.1, 1.4, length(rp)))
                   * max(1.0 - smoothstep(-0.38, -0.28, rp.z), smoothstep(0.18, 0.26, abs(rp.x)))
                   * (1.0 - smoothstep(0.15, 0.35, rp.z));
        vec3 base = C * F.night.yzw * (F.night.x * area);
        v_n1 = base * 0.10 * ao;
        v_n2 = base * 0.50;
        /* a display page lit from within (row flag 8, R-85): its own picture, dimmed for the night -- not the panel's area, the
           material's colour or the occlusion (the source model lights it by emission) */
        float sl = (fl & 8u) != 0u ? F.eyesize.z * F.night.x : 0.0;
        if (NIGHT == 2) v_rpw = vec4(rp, sl); else v_n1 += vec3(sl);
    } else { v_n1 = vec3(0.0); v_n2 = vec3(0.0); }
}
