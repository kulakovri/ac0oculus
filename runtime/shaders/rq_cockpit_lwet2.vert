#version 450
#extension GL_EXT_multiview : require
#extension GL_EXT_nonuniform_qualifier : require
/* The Quest cockpit's wet canopy, the PC's look (rq_cockpit.inc, ACZ_VR_RQ_COCKPIT_WET=lean2): the lean vertex stage, the tiles' new coordinates: the glass's own light, the texture's tint (one
   low-mip read per vertex), the coordinates of the baked water textures and every spatial weight of cockpit.frag's canopy_water
   (where the water gathers, the film's edge, how cold the glass runs) are computed here; the fragment stage only combines them. */
layout(location = 0) in uvec4 in_pid;
layout(location = 1) in vec4 in_nao;
layout(location = 2) in uvec2 in_uvq;
layout(location = 3) in vec2 in_gl;              /* metres along the glass: across, along the flow (rqc_model_build) */
layout(location = 4) in vec3 in_gu;              /* the direction in which in_gl.x grows (cockpit frame) */

layout(location = 0) out mediump vec4 v_gc;      /* the glass over the water, premultiplied */
layout(location = 1) out vec4 v_tc;              /* xy the beads' tile, zw the rivulets' */
layout(location = 2) out vec2 v_nu;              /* the noise tile */
layout(location = 3) out mediump vec4 v_lens;    /* xy, zw: a unit of lens along the glass's two axes, in the frame image's uv */
layout(location = 4) out mediump vec4 v_amb;     /* rgb the ambient, a the sun's visibility */
layout(location = 5) out mediump vec4 v_sp;      /* x bead presence, y rivulet presence, z the film's threshold, w its edge weight */
layout(location = 6) out mediump vec4 v_fr;      /* x how cold the glass runs (rime), y n.H, zw H along the glass's axes */
layout(location = 8) out vec4 v_x;               /* xy the second rivulet layer, zw the rime map */
layout(location = 9) out mediump vec3 v_gl;      /* n.L, the ray in the water against the sun, the view axis along n (dot(rot[2], n)) */
layout(location = 10) out vec4 v_w;              /* xy the glass normal in the frame image, zw the view axis along U, V */
layout(location = 11) out mediump vec3 v_sh;     /* the sky glint's half vector along U, V, n */
#ifdef VUV
layout(location = 12) out highp vec3 v_fuv;     /* (ACZ_VR_RQ_COCKPIT_WET_VUV, R-45b) the frame image's uv x w, y w and w: the world
                                                  read's position without gl_FragCoord (rq_cockpit_lwet.vert) */
#endif
layout(location = 7) out mediump vec4 v_lh;      /* the eye's ray against the sun, x; yz the sun along the glass's axes; w how far a typical bead
                                                    is wide enough on screen for its lens to resolve (cockpit.frag's sm) */

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
    vec4 wa;          /* x mist alpha, y film alpha, z min(gain, 1), w rime gain */
    vec4 wb;          /* x the rime front, y the frozen share, z lens strength, w sin of the contact angle */
    vec4 wc;          /* x water, y cell scale, z bead stretch along the flow, w bead offset (cells) */
    vec4 wd;          /* x rivulet offset (cells), y rivulet length (cells), z speed 0..1, w bead density factor */
    vec4 we;          /* x rivulet density factor, y film, z grime alpha, w soot alpha */
    vec4 wf;          /* rgb the sun's colour on the water, w the frost */
    vec4 uvk[2];
    vec4 wg, wh;
} F;
layout(set = 0, binding = 0) uniform sampler2DArray texs[1024];

vec2 dir_uv(vec3 a) {                            /* a cockpit-frame direction to an offset in the frame image (cockpit.frag dir_uv) */
    return vec2(dot(F.rot[0].xyz, a) * F.proj.x * F.misc.x, -dot(F.rot[1].xyz, a) * F.proj.y * F.misc.y) * 0.5;
}

void main() {
    Row R = row[in_pid.w];
    AnimRow A = anim[in_pid.w];
    if (A.uvv.z == 0.0) {
        gl_Position = vec4(-4.0, -4.0, 0.5, 1.0);
#ifdef VUV
        v_fuv = vec3(0.0, 0.0, 1.0);
#endif
        v_x = vec4(0.0); v_gl = vec3(0.0); v_w = vec4(0.0); v_sh = vec3(0.0); v_gc = vec4(0.0); v_tc = vec4(0.0); v_nu = vec2(0.0); v_lens = vec4(0.0); v_amb = vec4(0.0); v_sp = vec4(0.0); v_fr = vec4(0.0); v_lh = vec4(0.0);
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
#ifdef VUV
    v_fuv = vec3(0.5 * (X + w), 0.5 * (Y + w), w);  /* (uvk's mapping: uv = 0.5 X / w + 0.5, the same for y) */
#endif

    vec3 na = vec3(dot(A.m[0].xyz, in_nao.xyz), dot(A.m[1].xyz, in_nao.xyz), dot(A.m[2].xyz, in_nao.xyz));
    vec3 V = normalize(F.head.xyz - pa);
    vec3 n = normalize(na);
    if (dot(n, V) < 0.0) n = -n;
    float ao = clamp(in_nao.w, 0.0, 1.0);

    /* the glass's own light: rq_cockpit_glass.vert */
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
        amb = mix(amb, cc * (0.62 + 0.38 * (0.5 + 0.5 * n.y)), kc);
        dir *= 1.0 - kc;
        shade *= 1.0 - 0.8 * kc;
    }
    float ghid = clamp(F.dome[2].w, 0.0, 1.0);
    if (ghid > 0.0) { dir *= 1.0 - ghid; shade *= 1.0 - ghid; }
    float aoa = ssl > 0.0 ? mix(ao, pow(ao, 0.4545), ssl) : ao;
    vec3 li = amb * aoa + dir * (F.pack.w > 0.5 ? 0.75 + 0.25 * ao : 0.4 + 0.6 * ao);
    vec3 Ln = normalize(sun == 0 ? d0 : (sun == 1 ? d1 : d2));
    vec3 H = normalize(Ln + V);
    float s = 0.0;
    if (R.sp.x > 0.0 && dot(n, Ln) > 0.0) s = R.sp.x * pow(max(dot(n, H), 0.0), max(R.sp.y, 1.0)) * ao * shade;
    float f = pow(1.0 - max(dot(n, V), 0.0), 5.0);
    /* the glass's texture: its tint at a low mip, per vertex (no fragment read) */
    vec2 uv = vec2(in_uvq) * vec2(R.qt.z, R.qt.w) * (1.0 / 65535.0) + R.qt.xy + A.uvv.xy;
    uint ti = uint(R.sp.w);
    vec4 t = textureLod(texs[nonuniformEXT(ti)], vec3(uv, 0.0), 3.0);
    float ca = R.col.a * t.a;
    v_gc = vec4(t.rgb * (R.col.rgb * li * F.pack.z) * ca + amb * 0.35 * f + s * F.wf.rgb, clamp(ca + (1.0 - ca) * f * 0.35, 0.0, 1.0));

    /* the water textures' coordinates: the bake's tiles are 32 cells of beads (in true proportions at the stretch 1.45: rqw_bake2), 16 of rivulets, 0.48 m of noise */
    float sc = 0.014 * F.wc.y;
    v_tc.xy = vec2(in_gl.x / sc, in_gl.y / (sc * 1.45) - F.wc.w) * (1.0 / 32.0);
    float sr = 0.016 * F.wc.y;
    v_tc.zw = vec2(in_gl.x / sr, in_gl.y / (sr * F.wd.y) - F.wd.x) * (1.0 / 16.0);
    v_nu = in_gl * (1.0 / 0.48);
    /* a second rivulet layer, slower and elsewhere (a modern game's streak map runs at several rates); the rime map: a 0.24 m tile */
    v_x.xy = vec2(in_gl.x / sr + 7.37, in_gl.y / (sr * F.wd.y) - F.wd.x * 0.29) * (1.0 / 16.0);
    v_x.zw = in_gl * (1.0 / 0.24);
    /* the glass's axes: V along the flow (aft), U across; a unit of lens in the frame image */
    vec3 Vg = vec3(0.0, 0.0, 1.0) - n * n.z;
    Vg = dot(Vg, Vg) > 1e-4 ? normalize(Vg) : normalize(cross(n, vec3(1.0, 0.0, 0.0)));
    vec3 Ug = normalize(in_gu - n * dot(in_gu, n));
    v_lens = vec4(dir_uv(Ug), dir_uv(Vg));
    v_amb = vec4(amb, shade);
    /* where the water gathers (cockpit.frag): more towards the sides, the airflow clears the middle of the screen */
    float side = smoothstep(0.06, 0.35, abs(pa.x));
    v_sp.x = F.wc.x * mix(1.0 - 0.55 * F.wd.z, 1.0, side) * F.wd.w;
    v_sp.y = F.wc.x * mix(0.4, 1.0, side) * F.wd.z * F.we.x;
    /* the film: grows from the frame and the sides inwards */
    float edge = smoothstep(0.08, 0.42, abs(pa.x)) * 0.6 + smoothstep(0.1, 0.6, pa.z + 0.25) * 0.4;
    v_sp.z = 1.02 - F.we.y * (0.75 + 0.55 * edge);
    v_sp.w = mix(0.55, 1.0, edge);
    /* rime: nothing within ~35 deg of the nose, most at the sides, the canopy top and the rails */
    vec3 dirp = pa * inversesqrt(max(dot(pa, pa), 1e-4));
    float ax = 1.0 + dirp.z;
    v_fr.x = smoothstep(0.2, 1.05, ax + 0.3 * max(dirp.y - 0.35, 0.0) + 0.25 * max(-dirp.y - 0.2, 0.0));
    /* the rim's glint: the ray in the water (cockpit.frag: refract(I, n, 0.75)), reflected off the cap Nc = U lens.x + V lens.y - n sqrt(..) */
    vec3 I = normalize(pa - (F.head.xyz + eye * F.head.w * F.rot[0].xyz));
    vec3 t1 = refract(I, n, 0.75);
    v_fr.y = dot(t1, Ug); v_fr.z = dot(t1, Vg); v_fr.w = dot(t1, n);
    v_gl = vec3(dot(n, Ln), dot(t1, Ln), dot(F.rot[2].xyz, n));
    v_w = vec4(dir_uv(n), dot(F.rot[2].xyz, Ug), dot(F.rot[2].xyz, Vg));
    vec3 Hs = normalize(vec3(0.0, 1.0, 0.0) + V);
    v_sh = vec3(dot(Ug, Hs), dot(Vg, Hs), dot(n, Hs));
    /* eye pixels a metre at this distance x a typical bead's half width (0.2 of its 14 mm cell): under ~4 px the lens fades to its average */
    float pxm = abs(rqA) * F.proj.x * F.eyesize.x * F.misc.x * 0.5 / max(w, 0.05);
    v_lh = vec4(dot(-V, Ln), dot(Ug, Ln), dot(Vg, Ln), clamp((0.2 * sc * pxm - 1.5) * 0.4, 0.0, 1.0));
}
