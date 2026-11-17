#version 450
#extension GL_EXT_multiview : require
#extension GL_EXT_nonuniform_qualifier : require
/* The Quest cockpit's wet canopy (rq_cockpit.inc, ACZ_VR_COCKPIT_MATERIAL=quest): a separate pipeline drawn only while the
   canopy is wet, on the outer glass. The water itself is cockpit.frag's canopy_water (generated from it, tools note in
   rq_cockpit.inc), with the world read at most 4 times a pixel from the frame's quarter-size copy (the 1x feed of the
   eye pass: rq_feed): the mist, the film and the frost take the base tap (the small copy is already soft), the rime one
   displaced tap, the drops their lens taps. */
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec3 v_li;
layout(location = 2) in vec3 v_add;
layout(location = 3) in float v_fa;
layout(location = 4) in float v_ca;
layout(location = 5) flat in uint v_tex;
layout(location = 6) in vec3 v_p;
layout(location = 7) in vec3 v_n;
layout(location = 8) in vec3 v_amb;
layout(location = 9) in vec3 v_sc;
layout(location = 10) in float v_shade;
layout(location = 11) flat in vec3 v_ln;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform CP {
    vec4 rot[3];
    vec4 head;
    vec4 proj;
    vec4 misc;
    vec4 color;
    vec4 light;       /* w: -(1 + 256 x the glass's thickness in mm) */
    vec4 model[3];
    vec4 su, sv, sd;  /* the canopy water's inputs (cockpit.frag; ps2_video.c cpit_draw) */
    vec4 uvoff;
    vec4 wx;
} cp;
layout(set = 0, binding = 0) uniform sampler2DArray texs[1024];
layout(set = 1, binding = 0) uniform sampler2DArray world;     /* the frame's copy (rq_feed): both eyes, layers */
layout(std140, set = 2, binding = 2) uniform Fr {
    vec4 rot[3];
    vec4 head;
    vec4 proj;
    vec4 misc;
    vec4 xfx;         /* the per-view crop */
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
    vec4 eyesize;     /* xy: the eye pass's size */
} F;
#define WORLD(uv) texture(world, vec3(uv, float(gl_ViewIndex))).rgb

/* the feed uv of this fragment (eye pixel -> the symmetric frame the feed image holds), the inverse of the vertex crop */
vec2 rqw_uv() {
    vec2 e = gl_FragCoord.xy / F.eyesize.xy;
    float Ax = gl_ViewIndex == 0 ? F.xfx.x : F.xfx.z, Bx = gl_ViewIndex == 0 ? F.xfx.y : F.xfx.w;
    float Ay = F.xfy.x, By = F.xfy.y;
    /* clip x' = A x + B w -> ndc_eye = A ndc + B, so ndc_game = (ndc_eye - B) / A */
    vec2 ndc = vec2((2.0 * e.x - 1.0 - Bx) / Ax, (2.0 * e.y - 1.0 - By) / Ay);
    return ndc * 0.5 + 0.5;                                 /* (the feed holds the symmetric frame) */
}
/* ---- canopy water (glass parts only) ----
   Water on the outside of the canopy and windscreen, in the cockpit frame (metres; x right, y up, z back = the
   airflow), so it sits on the real glass in both eyes. Drops are small lenses: they show the world image behind the
   glass displaced (inverted towards their rims), a thin darker rim and a glint only where the sun or the sky lights
   them; nothing is painted opaque. Beads (mostly small, a few large, clumped, thinning in the middle of the windscreen
   at speed) and thin rivulets that run aft; a fine film in cloud (the world slightly diffused); grime from debris is
   a faint translucent smudge plus wet smears that slide off. One drop at most per cell, inside its inner part, so one
   hash per layer. */
uvec3 pcg3(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}
vec4 hash4(ivec3 c) {
    uvec3 h = pcg3(uvec3(c & 4095));
    uvec3 g = pcg3(h.zxy ^ 0x9E3779B9u);
    return vec4(h, g.x) * (1.0 / 4294967296.0);
}
float hash1(ivec3 c) { return float(pcg3(uvec3(c & 4095)).x) * (1.0 / 4294967296.0); }
/* a cheaper hash and value noise for the ice: the frost covers large parts of the glass, so one multiply-xorshift
   round instead of pcg3's two */
float hash_i(ivec3 c) {
    uvec3 u = uvec3(c & 1023);
    uint h = u.x * 0x8DA6B343u ^ u.y * 0xD8163841u ^ u.z * 0xCB1AB31Fu;
    h ^= h >> 15u; h *= 0x2C1B3C6Du; h ^= h >> 12u;
    return float(h) * (1.0 / 4294967296.0);
}
float vnoise_i(vec3 x) {
    ivec3 i = ivec3(floor(x));
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    float a = mix(hash_i(i), hash_i(i + ivec3(1, 0, 0)), f.x), b = mix(hash_i(i + ivec3(0, 1, 0)), hash_i(i + ivec3(1, 1, 0)), f.x);
    float c = mix(hash_i(i + ivec3(0, 0, 1)), hash_i(i + ivec3(1, 0, 1)), f.x), e = mix(hash_i(i + ivec3(0, 1, 1)), hash_i(i + ivec3(1, 1, 1)), f.x);
    return mix(mix(a, b, f.y), mix(c, e, f.y), f.z);
}
float vnoise(vec3 x) {                                   /* value noise 0..1 */
    ivec3 i = ivec3(floor(x));
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    float a = mix(hash1(i), hash1(i + ivec3(1, 0, 0)), f.x), b = mix(hash1(i + ivec3(0, 1, 0)), hash1(i + ivec3(1, 1, 0)), f.x);
    float c = mix(hash1(i + ivec3(0, 0, 1)), hash1(i + ivec3(1, 0, 1)), f.x), e = mix(hash1(i + ivec3(0, 1, 1)), hash1(i + ivec3(1, 1, 1)), f.x);
    return mix(mix(a, b, f.y), mix(c, e, f.y), f.z);
}
#ifdef GXV
/* (CANOPY QUALITY, GXQ_HASH) the water's noise on the ice's hash */
float vn_w(vec3 x) { return GXOFF(GXQ_HASH) ? vnoise_i(x) : vnoise(x); }
float hash1_w(ivec3 c) { return GXOFF(GXQ_HASH) ? hash_i(c) : hash1(c); }
/* (GXQ_CLUMP) clumps of ~6 cm, as vnoise(p * 16) around 0.5 */
float cq_clump(vec3 p) {
    return 0.5 + 0.17 * (sin(p.x * 52.0 + 1.7 * sin(p.z * 23.0)) + sin(p.y * 47.0 + p.z * 31.0) + sin((p.x + p.y) * 37.0 - p.z * 19.0));
}
#else
#define vn_w vnoise
#define hash1_w hash1
#endif
/* a bead layer: cell size s (m), offset along the flow (cells), stretch along the flow, presence probability, radius
   range (cells, cubed distribution: mostly small); the drop intensity grows the drops in place (radius x rk, at most
   to the cell's edge) and shifts the distribution (u^ex: ex < 3 more large ones); rk 1 / ex 3 = the drops as before.
   Returns the coverage in m, the distance from the centre in d (1 at the rim), the slope of the cap (cockpit frame,
   grows towards the rim) in g and the drop's size draw u (0..1, small drops low: they freeze first) */
float bead_layer(vec3 p, float s, float off, float stretch, float prob, float rmin, float rmax, float px, float rk,
                 float ex, out float d, out vec3 g, out float u) {
    vec3 q = p / s;
    q.z = q.z / stretch - off;
    ivec3 c = ivec3(floor(q));
    vec3 f = fract(q) - 0.5;
    vec4 h = hash4(c);
    u = fract(h.x * 7.13 + h.w * 3.7);
    float r = mix(rmin, rmax, u * u * u);
    vec3 o = (h.yzw - 0.5) * (0.9 - 2.0 * r);              /* the centre keeps the drop inside its cell */
    if (rk != 1.0 || ex != 3.0)                            /* (the centre stays where it was: the drop grows in place) */
        r = min(mix(rmin, rmax, pow(u, ex)) * rk, 0.48 - max(abs(o.x), max(abs(o.y), abs(o.z))));
    vec3 v = (f - o) / r;
    d = length(v);
    float aa = max(px / (s * r), 1e-3);                    /* a pixel in units of the radius */
    float cover = clamp(1.6 / aa - 0.6, 0.0, 1.0);         /* drops below ~1.5 px fade out instead of flickering */
    aa = min(aa, 0.5);
    float m = (1.0 - smoothstep(1.0 - aa, 1.0, d)) * cover * step(h.x, prob);
    float hgt = sqrt(max(1.0 - d * d, 0.0));
    g = v / max(hgt, 0.25) * m;
    g.z /= stretch;
    return m;
}
/* a rivulet layer: thin water lines along the flow (cells s across, s * len along), sliding aft; wk widens them (drop
   intensity, 1 = as before); u = the line's own draw (freezing order) */
float rivulet_layer(vec3 p, float s, float len, float off, float prob, float px, float wk, out float d, out vec3 g,
                    out float u) {
    vec3 q = vec3(p.x / s, p.y / s, p.z / (s * len) - off);
    ivec3 c = ivec3(floor(q));
    vec3 f = fract(q) - 0.5;
    vec4 h = hash4(c + ivec3(1371, 2087, 593));
    vec2 o = (h.yz - 0.5) * 0.6;
    float w = mix(0.035, 0.09, h.w) * wk;                  /* half width (cells) */
    u = h.w;
    vec2 v = (f.xy - o) / w;
    d = length(v);
    float t = f.z + (h.w - 0.5) * 0.3;                     /* along the line: a head at the front, a fading tail */
    float along = smoothstep(-0.48, -0.25, t) * (1.0 - smoothstep(0.1, 0.45, t));
    float aa = max(px / (s * w), 1e-3);
    float cover = clamp(1.6 / aa - 0.6, 0.0, 1.0);
    aa = min(aa, 0.5);
    float m = (1.0 - smoothstep(1.0 - aa, 1.0, d)) * along * cover * step(h.x, prob);
    float hgt = sqrt(max(1.0 - d * d, 0.0));
    g = vec3(v / max(hgt, 0.3), 0.0) * m * 0.6;            /* a flat lens: bends only across the line */
    return m;
}
/* ---- the drops as real beads (cv_drops 2026-10-29, ACZ_VR_CANOPY_DROPS=lensdrops) ----
   What makes a modern game's canopy water read as volume (its M7 footage): every drop is a strong lens. A bead of water
   on the glass is a spherical cap (contact angle 90 deg, a rain-repellent canopy: ACZ_VR_CANOPY_DROP_ANGLE) that
   bends the eye's ray towards its far side by up to ~40 deg near its rim, so it shows the world turned over and
   shrunk -- the dark ground in the drops against the sky, the bright sky in the drops against the ground; beyond
   ~0.75 of its radius the ray meets the cap past the critical angle and is turned back into the cockpit (total
   internal reflection): a dark rim. At speed the airflow drags each bead aft and its receding (upstream) side leaves a
   thin trail of film, often broken into droplets. The same cells, hashes, centres and radii as bead_layer /
   rivulet_layer (the classic look): the same drops in the same places, plus the rain beads (cv_drops2) where the
   water comes hard. Each returns the coverage; rho = the distance from the drop's centre (or its trail's axis) along
   the glass over the half width of its slice there (1 at the rim), rv = the unit direction of that offset along the
   glass (cockpit frame), rpx = the half width in pixels; kc = the cap's steepness (1 the head, the trail's film
   flatter). */
/* (cv_drops2) the average transmission a bead under ~4 px fades to (with a 90 deg contact angle its turned-back rim is
   ~44 % of its area); the rain beads' amount at ACZ_VR_CANOPY_RAIN_BEADS 1 */
const float DROP_AVG = 0.6, RAIN_BEADS = 4.0;
float bead_lens(vec3 p, vec3 n, float s, float off, float stretch, float tail, float prob, float rmin, float rmax,
               float px, float rk, float ex, out float rho, out vec3 rv, out float kc, out float rpx, out float u) {
    vec3 q = p / s;
    q.z = q.z / stretch - off;
    ivec3 c = ivec3(floor(q));
    vec3 f = fract(q) - 0.5;
#ifdef GXV
    /* (GXV_EARLY: an empty cell returns before its second hash and its drop's draws -- the same test on the same first
       hash; u is read only where this drop covers the pixel) */
    if (GXOFF(GXV_EARLY) && float(pcg3(uvec3(c & 4095)).x) * (1.0 / 4294967296.0) >= prob) {
        rho = 1.0; rv = vec3(0.0); kc = 1.0; rpx = 0.0; u = 0.0;
        return 0.0;
    }
#endif
    vec4 h = hash4(c);
    u = fract(h.x * 7.13 + h.w * 3.7);
    float r = mix(rmin, rmax, u * u * u);
    vec3 o = (h.yzw - 0.5) * (0.9 - 2.0 * r);
    if (rk != 1.0 || ex != 3.0)
        r = min(mix(rmin, rmax, pow(u, ex)) * rk, 0.48 - max(abs(o.x), max(abs(o.y), abs(o.z))));
    rho = 1.0; rv = vec3(0.0); kc = 1.0; rpx = 0.0;
    if (h.x >= prob) return 0.0;
    /* the cell in true proportions (cell units, z aft x the cell's stretch): a round head and room behind it */
    vec3 w = vec3(f.xy - o.xy, (f.z - o.z) * stretch);
    float hv = fract(h.w * 13.7 + h.y * 5.1);              /* the drop's own second draw */
    /* the trail upstream (-z): longer at speed, each drop its own length, kept inside the cell */
    float L = min(tail * r * (0.35 + 0.95 * hv), max((o.z + 0.47) * stretch - 0.25 * r, 0.0));
    float t = L > 0.0 ? clamp(-w.z / L, 0.0, 1.0) : 0.0;
    vec3 v = w + vec3(0.0, 0.0, t * L);                    /* from the nearest point of the drop's axis */
    float rr = r * mix(1.0, 0.2, sqrt(t));                  /* a thick head, a thin trail */
    /* (exact early out: the droplets only narrow the drop and the lumps move it by at most 0.087 of its width, so
       nothing beyond 1.09 x the width reaches the drop -- most of a cell: the rain beads made this matter) */
    if (dot(v, v) > 1.19 * rr * rr) return 0.0;
    if (hv > 0.45) rr *= 1.0 - 0.6 * smoothstep(0.12, 0.3, t) * (0.5 - 0.5 * cos(t * L / r * 4.4));   /* droplets */
    v += rr * 0.05 * sin(v.yzx * (4.5 / rr) + hv * 6.2832);   /* merged drops are a little lumpy, not circles */
    float hd = dot(v, n);                                   /* across the glass: the glass cuts the cell's drop here */
    float rs2 = rr * rr - hd * hd;
    if (rs2 <= 0.0) return 0.0;
    float rs = sqrt(rs2);
    vec3 vp = v - n * hd;
    float lp = length(vp);
    rho = lp / rs;
    rv = lp > 1e-6 ? vp / lp : vec3(0.0);
    kc = mix(1.0, 0.55, t);
    float aa = max(px / (s * rs), 1e-3);                    /* a pixel in units of the half width */
    rpx = 1.0 / aa;
    float cover = clamp(1.6 / aa - 0.6, 0.0, 1.0);          /* below ~1.5 px they fade out instead of flickering */
    aa = min(aa, 0.5);
    return (1.0 - smoothstep(1.0 - aa, 1.0, rho)) * cover;
}
/* (lensdrops) a rivulet: the classic line, wandering across as it runs (it follows the wet path, not a ruler), with a bead
   at its head (aft, the way it runs) and its trail fading upstream; a cylinder of water across, so the same lens */
float rivulet_lens(vec3 p, vec3 n, float s, float len, float off, float prob, float px, float wk, out float rho,
                  out vec3 rv, out float rpx, out float u) {
    vec3 q = vec3(p.x / s, p.y / s, p.z / (s * len) - off);
    ivec3 c = ivec3(floor(q));
    vec3 f = fract(q) - 0.5;
#ifdef GXV
    if (GXOFF(GXV_EARLY) && float(pcg3(uvec3((c + ivec3(1371, 2087, 593)) & 4095)).x) * (1.0 / 4294967296.0) >= prob) {
        rho = 1.0; rv = vec3(0.0); rpx = 0.0; u = 0.0;                         /* (as bead_lens's GXV_EARLY) */
        return 0.0;
    }
#endif
    vec4 h = hash4(c + ivec3(1371, 2087, 593));
    vec2 o = (h.yz - 0.5) * 0.6;
    float w = mix(0.035, 0.09, h.w) * wk;                  /* half width (cells) */
    u = h.w;
    rho = 1.0; rv = vec3(0.0); rpx = 0.0;
    if (h.x >= prob) return 0.0;
    float t = f.z + (h.w - 0.5) * 0.3;                     /* along the line (aft +) */
    o += vec2(0.06, 0.04) * sin(vec2(8.0, 5.0) * f.z + h.yz * 6.2832);
    float along = smoothstep(-0.48, -0.2, t) * (1.0 - smoothstep(0.3, 0.42, t));
    float hb = (t - 0.3) * 20.0;
    w *= 0.7 + 0.3 * along + 1.1 * exp(-hb * hb);           /* the head's bead */
    vec3 v = vec3(f.xy - o, 0.0);
    float hd = dot(v, n);
    float rs2 = w * w - hd * hd;
    if (rs2 <= 0.0) return 0.0;
    float rs = sqrt(rs2);
    vec3 vp = v - n * hd;
    float lp = length(vp);
    rho = lp / rs;
    rv = lp > 1e-6 ? vp / lp : vec3(0.0);
    float aa = max(px / (s * rs), 1e-3);
    rpx = 1.0 / aa;
    float cover = clamp(1.6 / aa - 0.6, 0.0, 1.0);
    aa = min(aa, 0.5);
    return (1.0 - smoothstep(1.0 - aa, 1.0, rho)) * smoothstep(0.0, 0.15, along) * cover;
}
/* debris flecks (wish #47): where a bit of a destroyed aircraft hit the glass (ps2_video.c canopy_marks; su.xyz on the
   glass, one fleck each: its direction from the eye point = the cockpit frame's origin octahedral-encoded in 2 x 8 bits
   from bit 7, strength class 2 bits, the share left 5 bits; 0 = none). A small soot fleck, 5-9 mm in radius with a
   ragged edge and a darker middle, a few 2 mm specks thrown around it and a faint pale scuff ring (the coating
   scattering light); as it fades it smears aft with the airflow (along the glass towards +z). Never a crack, never
   opaque: at most ~0.55 alpha in its middle. Premultiplied colour + alpha over the water's lower layers. */
vec3 fleck_dir(uint u) {
    vec2 e = vec2(float((u >> 15u) & 255u), float((u >> 7u) & 255u)) * (1.0 / 127.5) - 1.0;
    vec3 d = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    float t = max(-d.z, 0.0);
    d.xy += vec2(d.x >= 0.0 ? -t : t, d.y >= 0.0 ? -t : t);
    return normalize(d);
}
/* a fleck within 4 cm of this point of the glass (the rest of the glass returns at once) */
bool fleck_near(vec3 p) {
    for (int i = 0; i < 3; i++) {
        if (cp.su[i] < 0.5) continue;
        uint u = uint(cp.su[i] + 0.25);              /* (+ 0.25: codes past 2^23 have no halves in float) */
        vec3 m = fleck_dir(u);
        float t = dot(p, m);
        vec3 q = p - m * t;
        if (t > 0.05 && dot(q, q) < ((u >> 23u) != 0u ? 0.0036 : 0.0016)) return true;   /* (bug #260: 6 cm, a big one) */
    }
    return false;
}
vec4 debris_flecks(vec3 p, vec3 n, vec3 base, vec3 amb, float px) {
    vec4 W = vec4(0.0);
    for (int i = 0; i < 3; i++) {
        float code = cp.su[i];
        if (code < 0.5) continue;
        uint u = uint(code + 0.25);                    /* (bit 23: + 0.5 would round up to the next code) */
        float fq = float(u & 31u) / 31.0, s0 = 0.4 + 0.2 * float((u >> 5u) & 3u);
        vec3 m = fleck_dir(u);
        float t = dot(p, m);
        if (t <= 0.05) continue;
        vec3 q = p - m * t;                                    /* off the fleck's line of sight (m) */
        /* bit 23 (ACZ_IMM_DEBRIS_LOOK, bug #260): 1.5 x across (7.5-13.5 mm), its specks as far out, x 1.35 darker */
        bool big = (u >> 23u) != 0u;
        float gs = big ? 1.5 : 1.0;
        if (dot(q, q) > (big ? 0.0036 : 0.0016)) continue;    /* beyond 4 cm (a big one 6 cm): none */
        vec3 ta = vec3(0.0, 0.0, 1.0) - n * n.z;               /* aft along the glass */
        ta = dot(ta, ta) > 1e-4 ? normalize(ta) : normalize(cross(n, vec3(1.0, 0.0, 0.0)));
        vec3 tc = cross(n, ta);
        vec2 o = vec2(dot(q, tc), dot(q, ta));                 /* across, aft (m) */
        float h = fract(float((u >> 7u) & 0xFFFFu) * 0.6180339);   /* the fleck's own draw, from its place */
        float age = 1.0 - fq;
        float sm = 1.0 + 2.2 * age;                            /* smeared aft as it fades */
        vec2 os = vec2(o.x, o.y > 0.0 ? o.y / sm : o.y * 1.4);
        float R = (0.005 + 0.004 * h) * (1.0 + 0.25 * age) * gs;
        float rr = length(os);
        float edge = vnoise(vec3(o * 1500.0, h * 31.0));
        float aa = max(px, 2e-4);
        float core = 1.0 - smoothstep(R * (0.72 + 0.25 * edge) - aa, R * (0.86 + 0.25 * edge) + aa, rr);
        core *= 0.7 + 0.3 * vnoise(vec3(o * 4200.0, h * 7.0));    /* soot is grainy, not a disc */
        float mid = 1.0 - smoothstep(0.0, R * 0.6, rr);        /* darker where it struck */
        vec2 gq = os * 330.0;                                  /* specks: ~3 mm cells, one in five holds one */
        float hs = hash1(ivec3(ivec2(floor(gq)), int(h * 997.0)));
        float sp = step(0.8, hs) * (1.0 - smoothstep(0.15, 0.35, length(fract(gq) - 0.5)))
                   * (1.0 - smoothstep(R * 1.4, 0.022 * gs, rr)) * step(R, rr);
        sp *= clamp(0.003 / max(px, 1e-5) - 0.5, 0.0, 1.0);    /* below a pixel they fade instead of flickering */
        float ring = smoothstep(R * 0.9, R * 1.2, rr) * (1.0 - smoothstep(R * 1.2, R * 1.9, rr));
        float k = s0 * fq * (big ? 1.35 : 1.0);
        float ar = ring * 0.06 * k, a = min((core * 0.38 + mid * 0.17 + sp * 0.35) * k, 0.8);
        vec3 soot = mix(base * 0.5, vec3(0.09, 0.08, 0.07), 0.65), pale = amb * 1.3 + 0.06;
        W = vec4(pale * ar + W.rgb * (1.0 - ar), ar + W.a * (1.0 - ar));
        W = vec4(soot * a + W.rgb * (1.0 - a), a + W.a * (1.0 - a));
    }
    return W;
}
/* a cockpit-frame direction (an angle in radians, small) to a texture-coordinate offset in the frame image */
vec2 dir_uv(vec3 a) {
    return vec2(dot(cp.rot[0].xyz, a) * cp.proj.x * cp.misc.x, -dot(cp.rot[1].xyz, a) * cp.proj.y * cp.misc.y) * 0.5;
}
/* a cockpit-frame direction (any angle in front of the eye) to its place in the frame image, less the image's centre
   (cockpit.vert's projection; the difference of two is an exact offset) */
vec2 view_uv(vec3 D) {
    float w = max(-dot(cp.rot[2].xyz, D), 0.05);
    return vec2(dot(cp.rot[0].xyz, D) * cp.proj.x * cp.misc.x, -dot(cp.rot[1].xyz, D) * cp.proj.y * cp.misc.y) * (0.5 / w);
}
/* (rain_layers 2026-10-30, ACZ_VR_CANOPY_LAYERS: a modern game's canopy is a reflection mesh and a weather mesh "separated by the
   thickness of the glass") the point of the glass's outer face this eye sees through the inner face's point p: the
   eye's own ray refracted into the acrylic (n 1.49) and carried through the glass's thickness thk (m) along it -- a
   parallel slab, exact for glass that is flat over a few millimetres. Each eye reaches the outer face at its own place,
   so the weather evaluated there sits at the outer face's depth in stereo and with head motion: behind the inner
   face's reflections, wear and cracks by the glass's apparent thickness (~thk / 1.49 square on). n faces the eye. */
vec3 glass_outer(vec3 p, vec3 n, float thk) {
    float eye = cp.misc.w;
#ifdef MV
    eye = gl_ViewIndex == 0 ? -1.0 : 1.0;
#endif
    vec3 I = normalize(p - cp.head.xyz - eye * cp.head.w * cp.rot[0].xyz);
    vec3 t = refract(I, n, 0.6711);
    return p + t * (thk / max(-dot(t, n), 0.25));
}
/* the water layer over the glass colour gc (premultiplied; the glass is nearer than the water on its outside); wash =
   how much water, film, frost or grime covers this spot (0..1: the glass's own scratches fill with water and hide) */
vec4 canopy_water(vec3 p, vec3 n, vec3 V, vec3 Ln, vec3 amb, vec3 sun, float shade, vec4 gc, float thk, out float wash) {
    wash = 0.0;
#ifdef GXV
    if (GXOFF(GXV_WATER)) return gc;            /* (a dry glass without flecks: the test below returns gc for it too) */
#endif
    float wet = cp.wx.x, mist = cp.wx.w, grime = cp.uvoff.z, st = max(cp.uvoff.w, 1.0), soot = 0.0;
    if (grime < 0.0) {                                     /* (soot packed with the grime: ps2_video.c, bug #139) */
        float q = -grime - 1.0, gq = floor(q * (1.0 / 256.0) + 1e-3);
        soot = (q - gq * 256.0) * (1.0 / 255.0); grime = gq * (1.0 / 255.0);
    }
#ifdef GXV
    if (GXOFF(GXV_GRIME)) grime = 0.0;          /* (each bit: the CPU found the same value in this draw's push constants) */
    if (GXOFF(GXV_SOOT)) soot = 0.0;
    if (GXOFF(GXV_MIST)) mist = 0.0;
    if (GXOFF(GXV_DROPS)) wet = 0.0;            /* (wet <= 0: no drop layers; wet is only compared with 0) */
#endif
    /* (ACZ_VR_CANOPY_DROPS=lensdrops: sv.z = the size + 4 x the refraction's strength in quarters (1..16) + 128 x the beads'
       contact angle in 5 deg steps above 45 deg) */
    float aq = floor(cp.sv.z * 0.25), ai = floor(aq * (1.0 / 32.0)), sz = cp.sv.z - 4.0 * aq;
    aq -= 32.0 * ai;
    float ak = aq * 0.25;
    bool lensdrops = aq >= 1.0;
#ifdef GXV
    if (GXOFF(GXV_CLASSIC)) lensdrops = true;         /* (one drop look in the binary) */
    if (GXOFF(GXV_LENS)) lensdrops = false;
#endif
    float dsin = sin(radians(45.0 + 5.0 * ai));             /* the beads' contact angle (ACZ_VR_CANOPY_DROP_ANGLE) */
    /* (lensdrops: su.w = the drop intensity + 2 x the rain beads' amount in quarters, ACZ_VR_CANOPY_RAIN_BEADS) */
    float rq = lensdrops ? floor(max(cp.su.w, 0.0) * 0.5) : 0.0, dint = cp.su.w - 2.0 * rq, rbk = rq * 0.25;
    float smear = cp.sv.y, size = lensdrops ? sz : (cp.sv.z > 0.0 ? cp.sv.z : 1.0), gain = cp.sv.w > 0.0 ? cp.sv.w : 1.0;
#ifdef GXV
    if (GXOFF(GXV_GRIME)) smear = 0.0;
#endif
    bool flecks = max(cp.su.x, max(cp.su.y, cp.su.z)) > 0.5 && fleck_near(p);
#ifdef GXV
    if (GXOFF(GXV_FLECK)) flecks = false;
#endif
    if (wet <= 0.0 && mist <= 0.0 && grime <= 0.0 && smear <= 0.0 && cp.sd.w <= 0.0 && cp.sd.y <= 0.0 && !flecks
        && soot <= 0.0) return gc;
    /* (rain_layers, ACZ_VR_CANOPY_LAYERS: thk > 0) from here on the outside weather lies on the glass's outer face (a dry
       glass returned above: no cost there) */
    if (thk > 0.0) p = glass_outer(p, n, thk);
    bool have = mod(cp.sv.x, 2.0) > 0.5;
    vec2 tsz = have ? vec2(textureSize(world, 0).xy) : vec2(1.0);
    vec2 uv0 = rqw_uv();
    float px = length(fwidth(p)) * 0.7;
    vec3 base = have ? WORLD(uv0) : amb;
    /* canopy ice (ps2_video.c canopy_update): frz = the share of the water frozen where it sits, frost = how far rime
       and frost have grown in from the sides */
    float ipk = max(cp.sd.y, 0.0);
#ifdef GXV
    if (GXOFF(GXV_ICE)) ipk = 0.0;
#endif
    bool rime = ipk >= 1048576.0;                          /* (rain_layers, ACZ_VR_CANOPY_RIME: + 2^20) */
    if (rime) ipk -= 1048576.0;
    float ifz = floor(ipk * (1.0 / 1024.0));
    float frz = ifz * (1.0 / 1023.0), frost = (ipk - ifz * 1024.0) * (1.0 / 1023.0);
    /* drop intensity: how hard the water comes (a dense cloud core or a downpour: larger drops, more of the large
       ones, streaming rivulets; a thin veil or drizzle: fine sparse droplets); 0.45 = exactly the drops as before */
    float di = clamp(dint, 0.0, 1.0) - 0.45;
    float rk = 1.0 + 0.6 * di, ex = 3.0 - 2.4 * di, pf1 = 1.0 + 1.6 * di, pf2 = 1.0 - 0.55 * di;
    float pv = 1.0 + di * (di > 0.0 ? 3.6 : 1.65), wk = 1.0 + 0.45 * di;

    /* where the water gathers: clumps (~6 cm), more towards the sides, the airflow clears the middle of the screen */
    float side = smoothstep(0.06, 0.35, abs(p.x));
    /* (the clump noise also shapes the frost's front; on a glass with frost but no water it is only looked up where
       the frost can reach) */
#ifdef GXV
    float vn0 = wet > 0.0 ? (GXOFF(GXQ_CLUMP) ? cq_clump(p) : vn_w(p * 16.0 + vec3(3.1, 7.7, 1.3))) : 0.5;
#else
    float vn0 = wet > 0.0 ? vnoise(p * 16.0 + vec3(3.1, 7.7, 1.3)) : 0.5;
#endif
    float dens = 0.3 + 0.7 * smoothstep(0.3, 0.75, vn0);
    dens *= mix(1.0 - 0.55 * cp.sd.x, 1.0, side);

    /* drops: big and fine beads, rivulets running aft (none to look up on a dry glass that only has frost) */
    float d1 = 0.0, d2 = 0.0, d3 = 0.0, u1 = 0.0, u2 = 0.0, u3 = 0.0, m1 = 0.0, m2 = 0.0, m3 = 0.0;
    vec3 g1 = vec3(0.0), g2 = vec3(0.0), g3 = vec3(0.0);
    float k1 = 1.0, k2 = 1.0, x1 = 0.0, x2 = 0.0, x3 = 0.0;    /* (lensdrops: steepness, half width in pixels) */
    float m4 = 0.0, d4 = 0.0, u4 = 0.0, k4 = 1.0, x4 = 0.0;   /* (lensdrops: the rain beads) */
    vec3 g4 = vec3(0.0);
    if (wet > 0.0 && lensdrops) {     /* g1..g4 carry the unit direction along the glass, d1..d4 rho */
        float tl = 1.1 * (st - 1.0);                       /* the trail: ~1.8 radii at 250 kt, ~3.3 at 450 kt */
        m1 = bead_lens(p, n, 0.014 * size, cp.wx.y, 1.0 + 0.3 * (st - 1.0), tl, 0.7 * wet * dens * pf1, 0.08, 0.4, px,
                      rk, ex, d1, g1, k1, x1, u1);
        m2 = bead_lens(p + 0.37, n, 0.007 * size, cp.wx.y * 1.7, 1.0 + 0.5 * (st - 1.0), tl, 0.6 * wet * dens * pf2, 0.1,
                      0.34, px, rk, ex, d2, g2, k2, x2, u2);
        m3 = rivulet_lens(p + 1.13, n, 0.016 * size, 2.0 + 2.5 * st, cp.wx.z,
                         0.35 * wet * mix(0.4, 1.0, side) * cp.sd.x * pv, px, wk, d3, g3, x3, u3);
        /* rain beads (cv_drops2): where the water comes hard -- real rain, a dense cloud core: drop intensity above
           ~0.3 -- a third layer of small beads (~1.8-5.5 mm, sizes evenly spread rather than mostly fine), as many as
           the water and its intensity give: a modern game's downpour covers up to a quarter of the glass. Thin water (a veil, a
           cloud's edge, the fine droplets left after a cloud: intensity ~0.3) has none, so the clearing stays */
        float pr = RAIN_BEADS * rbk * wet * dens * smoothstep(0.3, 0.8, clamp(dint, 0.0, 1.0));
        if (pr > 0.0)
            m4 = bead_lens(p + 0.71, n, 0.0065 * size, cp.wx.y * 1.3, 1.0 + 0.4 * (st - 1.0), tl, pr, 0.14, 0.42, px, 1.0,
                          1.5, d4, g4, k4, x4, u4);
    } else if (wet > 0.0) {
        m1 = bead_layer(p, 0.014 * size, cp.wx.y, 1.0 + 0.3 * (st - 1.0), 0.7 * wet * dens * pf1, 0.08, 0.4, px, rk, ex,
                        d1, g1, u1);
        m2 = bead_layer(p + 0.37, 0.007 * size, cp.wx.y * 1.7, 1.0 + 0.5 * (st - 1.0), 0.6 * wet * dens * pf2, 0.1, 0.34,
                        px, rk, ex, d2, g2, u2);
        m3 = rivulet_layer(p + 1.13, 0.016 * size, 2.0 + 2.5 * st, cp.wx.z,
                           0.35 * wet * mix(0.4, 1.0, side) * cp.sd.x * pv, px, wk, d3, g3, u3);
    }
    float m = max(m1, max(m2, m3));
    vec3 g = g1 + g2 * (1.0 - m1) + g3 * (1.0 - max(m1, m2));
    float dr = m1 >= max(m2, m3) ? d1 : (m2 >= m3 ? d2 : d3);
    float du = m1 >= max(m2, m3) ? u1 : (m2 >= m3 ? u2 : u3);
    vec3 gt = g - n * dot(g, n);                           /* the cap's slope along the glass */
    float tl = length(gt);
    vec3 tilt = tl > 0.0 ? gt / tl * (tl / sqrt(1.0 + tl * tl)) : vec3(0.0);    /* sine of the cap's tilt */
    /* (lensdrops: the top drop's direction along the glass, steepness, half width in pixels) */
    vec3 rva = m1 >= max(m2, m3) ? g1 : (m2 >= m3 ? g2 : g3);
    float kca = m1 >= max(m2, m3) ? k1 : (m2 >= m3 ? k2 : 0.6), rpa = m1 >= max(m2, m3) ? x1 : (m2 >= m3 ? x2 : x3);
    if (m4 > m) { m = m4; dr = d4; du = u4; rva = g4; kca = k4; rpa = x4; }

    /* mist: the world a little diffused and paler behind a fine film (cloud) */
    vec4 W = vec4(0.0);
    if (mist > 0.0) {
        float am = mist * 0.35 * gain;
        vec3 mc = amb * 1.4 + 0.08;
        if (have) {
            vec2 r = 3.0 / tsz;
            mc = mix(base, mc, 0.25);
        } else am *= 0.3;
        W = vec4(mc * am, am);
    }
    /* condensation: humid air from the cloud fogs the glass in patches that grow over seconds from the frame and the
       sides inwards (the airflow keeps the middle of the windscreen clearest) and clear slowly after the cloud; behind a
       fogged patch the world is frosted (a wide blur, rotated per spot of glass so it reads as a texture of the glass,
       not of the screen) and lifted towards the light it scatters */
    float film = cp.sd.w;
#ifdef GXV
    if (GXOFF(GXV_FILM)) film = 0.0;
#endif
    if (film > 0.0) {
        float edge = smoothstep(0.08, 0.42, abs(p.x)) * 0.6 + smoothstep(0.1, 0.6, p.z + 0.25) * 0.4;
#ifdef GXV
        /* (GXQ_FILM: the second octave as two sines that wander with the first octave: ragged patch edges without a lookup) */
        float fn = vn_w(p * 34.0 + vec3(1.7, 4.1, 9.3));
        fn = GXOFF(GXQ_FILM) ? fn * 0.62 + 0.38 * (0.5 + 0.11 * (sin((p.x + p.z) * 280.0 + fn * 9.0) + sin((p.y - p.z) * 255.0 - fn * 7.0)))
                             : fn * 0.62 + vn_w(p * 97.0 + 3.3) * 0.38;
#else
        float fn = vnoise(p * 34.0 + vec3(1.7, 4.1, 9.3)) * 0.62 + vnoise(p * 97.0 + 3.3) * 0.38;
#endif
        float th = 1.02 - film * (0.75 + 0.55 * edge);
        float fm = smoothstep(th - 0.1, th + 0.14, fn) * mix(0.55, 1.0, edge);
        if (fm > 0.003) {
            float af = fm * 0.5 * min(gain, 1.3);
            vec3 fc = amb * 1.5 + 0.1;
            if (have) {
                ivec3 gp = ivec3(floor(p * 900.0));
                float rot = hash1_w(gp) * 6.2831853, cs = cos(rot), sn = sin(rot);
                vec2 r = (5.0 + 9.0 * fm) / tsz;
                fc = mix(base, fc, 0.3);          /* (the quarter-size copy's one tap is already this blur: RQ, at most 4 taps) */
            } else af *= 0.4;
            W = vec4(fc * af + W.rgb * (1.0 - af), af + W.a * (1.0 - af));
        }
    }
    /* grime from debris: faint translucent smudges, streaked aft; wet smears slide off with the airflow */
    if (grime > 0.0 || smear > 0.0) {
        vec3 gp = vec3(p.x * 11.0, p.y * 11.0, p.z * 7.0 - cp.wx.z * 0.02);
#ifdef GXV
        float gn = GXOFF(GXQ_GRAIN) ? 0.5 + 0.72 * (vn_w(gp * 1.5) - 0.5) : vn_w(gp) * 0.6 + vn_w(gp * 3.7 + 5.0) * 0.4;
#else
        float gn = vnoise(gp) * 0.6 + vnoise(gp * 3.7 + 5.0) * 0.4;
#endif
        float gs = smoothstep(0.55, 0.9, gn);
        float ag = gs * grime * 0.14 * gain;                   /* a dusty film, never a blot */
        vec3 gcol = mix(base * 0.75, vec3(0.32, 0.29, 0.25), 0.5);
        W = vec4(gcol * ag + W.rgb * (1.0 - ag), ag + W.a * (1.0 - ag));
        if (smear > 0.0 && have) {
            float sm = smoothstep(0.45, 0.8, vn_w(gp * 0.7 + 11.0)) * smear;
            vec2 off = dir_uv(vec3(0.0, 0.0, 0.025));             /* the water film shifts the view aft a little */
            vec3 sc = WORLD(uv0 + off * sm) * 0.97 + amb * 0.06 * sm;
            float as = sm * 0.85;
            W = vec4(sc * as + W.rgb * (1.0 - as), as + W.a * (1.0 - as));
        }
    }
    /* soot from smoke (bug #139, ACZ_IMM_SOOT): fine particles impacted on the faces the airflow meets (the windscreen and
       the canopy's front: the glass's inward normal pointing aft), streaked aft by the flow, a faint brownish grey that
       dims and veils the world behind a little; never opaque (at most ~0.22) */
    if (soot > 0.0) {
        float front = 0.3 + 0.7 * smoothstep(-0.2, 0.6, n.z);
        vec3 sq = vec3(p.x * 55.0, p.y * 55.0, p.z * 7.0 - cp.wx.z * 0.01);
#ifdef GXV
        float sn = GXOFF(GXQ_GRAIN) ? 0.5 + 0.74 * (vn_w(sq * 1.5) - 0.5) : vn_w(sq) * 0.65 + vn_w(sq * 3.1 + 7.0) * 0.35;
        float sg = GXOFF(GXQ_GRAIN) ? 0.5 : mix(0.5, vn_w(p * 420.0 + 2.0), clamp(0.0024 / max(px, 1e-5) - 0.5, 0.0, 1.0));
#else
        float sn = vnoise(sq) * 0.65 + vnoise(sq * 3.1 + 7.0) * 0.35;           /* streaks along the flow */
        float sg = vnoise(p * 420.0 + 2.0);                                     /* fine grain */
        sg = mix(0.5, sg, clamp(0.0024 / max(px, 1e-5) - 0.5, 0.0, 1.0));
#endif
        float as = soot * front * (0.08 + 0.14 * smoothstep(0.35, 0.8, sn)) * (0.75 + 0.5 * sg) * min(gain, 1.3);
        vec3 sc = mix(base * 0.55, vec3(0.11, 0.10, 0.09) + amb * 0.12, 0.6);
        W = vec4(sc * as + W.rgb * (1.0 - as), as + W.a * (1.0 - as));
    }
    /* debris flecks (under the water) */
    if (flecks) {
        vec4 F = debris_flecks(p, n, base, amb, px);
        W = vec4(F.rgb + W.rgb * (1.0 - F.a), F.a + W.a * (1.0 - F.a));
    }
    /* the drops over all of it */
    if (m > 0.0) {
        vec3 N, H = normalize(Ln + V), Hs = normalize(vec3(0.0, 1.0, 0.0) + V);
        float rim = smoothstep(0.72, 1.0, dr), glint, sky;
        vec3 dc;
        float ad;
        if (lensdrops) {
            /* (lensdrops) the drop as a lens of water on the glass, ray by ray for this eye: into the water through the flat
               glass (air -> water: the acrylic between them is a parallel plate), out through the cap -- Snell's law at
               its slope (sine = rho x dsin x steepness), or turned back past the critical angle (the dark rim) */
            float eye = cp.misc.w;
#ifdef MV
            eye = gl_ViewIndex == 0 ? -1.0 : 1.0;
#endif
            vec3 I = normalize(p - cp.head.xyz - eye * cp.head.w * cp.rot[0].xyz);
            float sa = min(dr, 1.0) * dsin * kca;
            vec3 Nc = rva * sa - n * sqrt(1.0 - sa * sa);           /* the cap's outward normal (n faces the eye) */
            vec3 t1 = refract(I, n, 0.75);
            float ci = dot(t1, Nc);
            float s2 = 1.333 * sqrt(max(1.0 - ci * ci, 0.0));       /* sine of the way out (> 1: turned back) */
            float tr = 1.0 - smoothstep(0.93, 1.0, s2);             /* (the transmission falls towards the critical angle) */
            vec3 t2 = s2 < 1.0 ? refract(t1, -Nc, 1.333) : I;
            /* below ~4 px of half width the turned-over image and the rim would sparkle: towards their average (a bead
               with its rim; a rivulet's flat film turns nothing back) */
            float sm = clamp((rpa - 1.5) * 0.4, 0.0, 1.0);
            tr = mix(mix(0.97, DROP_AVG, smoothstep(0.6, 1.0, kca)), tr, sm);
            N = -Nc;
            sky = pow(max(dot(N, Hs), 0.0), 60.0) * 0.35;
            /* the sun through the lens (a small image of it in each drop near the sun's direction) and, behind the pilot,
               off the turned-back rim */
            glint = (pow(max(dot(t2, Ln), 0.0), 300.0) * 1.5 * tr
                     + pow(max(dot(reflect(t1, Nc), Ln), 0.0), 200.0) * (1.0 - tr)) * shade * sm;
            if (have) {
                vec3 wv = WORLD(clamp(uv0 + (view_uv(t2) - view_uv(I)) * (ak * mix(0.4, 1.0, sm)), vec2(0.0), vec2(1.0)));
                vec3 back = base * 0.3 + amb * 0.12;                /* turned back into the cockpit: its dim interior */
                dc = mix(back, wv * 0.97, tr) + amb * (sky * 0.4) + sun * glint;
                ad = m;
            } else {
                dc = amb * (0.4 * (1.0 - tr) + sky) / max(0.05, 0.1 + 0.3 * (1.0 - tr)) + sun * glint;
                ad = m * (0.1 + 0.3 * (1.0 - tr));
            }
        } else {
            N = normalize(n + tilt * 1.2);
            float fr = pow(1.0 - max(dot(N, V), 0.0), 4.0);
            glint = pow(max(dot(N, H), 0.0), 260.0) * shade * 2.5;
            sky = pow(max(dot(N, Hs), 0.0), 60.0) * 0.35;
            if (have) {
                /* a lens: towards the rim it shows the world from further away, inverted */
                vec2 off = -dir_uv(tilt * 0.12);
                /* a lens turns the world over: the sky shows in its lower half, the ground in its upper half (kept even
                   against a flat overcast); a thin darker rim; the light it gathers brightens the far side a little */
                float up = dot(tilt, vec3(0.0, 1.0, 0.0));
                float turn = 1.03 + 0.28 * clamp(-up, 0.0, 1.0) - 0.2 * clamp(up, 0.0, 1.0);
                float caustic = clamp(dot(tilt, normalize(Ln)), 0.0, 1.0) * (1.0 - rim) * 0.12;
                dc = WORLD(uv0 + off) * (turn - 0.18 * rim + caustic) + amb * (0.2 * fr + sky) + sun * glint;
                ad = m;
            } else {
                dc = amb * (0.4 * fr + sky) / max(0.05, 0.1 + 0.3 * rim) + sun * glint;
                ad = m * (0.1 + 0.3 * rim);
            }
        }
        /* frozen where it sat (small drops first): milky ice that scatters instead of a lens -- the world straight
           behind shows only as a pale glow, the frozen-in air whitens the middle and the rim, crystal facets catch the
           sun in small glints; frozen rivulets are streaks of the same ice */
        float fz = frz > 0.0 ? smoothstep(0.0, 0.18, frz * 1.18 - du) : 0.0;
        if (fz > 0.0) {
            float gr = vnoise_i(p * 1500.0 + 2.7);             /* crystal mottling, ~0.7 mm */
            float gr2 = hash_i(ivec3(floor(p * 900.0)));        /* facets, ~1.1 mm */
            float core = 1.0 - smoothstep(0.05, 0.5, dr);      /* the air the freezing pushed to the middle */
            float milk = min(0.55 + 0.2 * core + 0.35 * rim + 0.25 * (gr - 0.5), 1.0);
            vec3 ice = amb * 1.55 + vec3(0.15, 0.16, 0.18);
            vec3 Nf = normalize(N + (vec3(gr2, gr, fract(gr * 7.3 + gr2)) - 0.5) * 0.9);
            float spk = pow(max(dot(Nf, H), 0.0), 160.0) * shade * 2.2 * step(0.72, gr2);
            /* clear ice still bends the light a little: a third of the lens image under the milk */
            vec3 ic = mix(mix(base, dc, 0.35), ice, max(milk, 0.0)) + amb * sky * 0.6 + sun * (spk + glint * 0.3);
            float ai = m * min(0.55 + 0.15 * core + 0.3 * rim + 0.1 * gr, 1.0);
            dc = mix(dc, ic, fz);
            ad = mix(ad, ai, fz);
        }
        ad *= min(gain, 1.0);
        W = vec4(dc * ad + W.rgb * (1.0 - ad), ad + W.a * (1.0 - ad));
    }
    /* rime and frost: in cloud above the freezing level it grows from the sides and the canopy's edges inwards (frost =
       how far). The cold side of the glass is judged by the angle off the nose seen from the cockpit frame's origin
       (the pilot's eye point; the same spot of glass for both eyes): the windscreen ahead is heated and swept by the
       airflow, so it stays clearest and only ever gets a few faint patches. The front is ragged (the clump noise above
       plus a fine one), the frost itself feathery (thin bright veins of ridged noise stretched along the growth
       direction), the world behind it frosted, a few crystals glinting in the sun. */
    if (frost > 0.0) {
        vec3 dir = p * inversesqrt(max(dot(p, p), 1e-4));
        float ax = 1.0 + dir.z;                                /* 0 ahead, 1 abeam, 2 behind */
        /* how cold this spot of glass runs: nothing within ~35 deg of the nose, most at the sides, the canopy top and
           the rails */
        float e = smoothstep(0.2, 1.05, ax + 0.3 * max(dir.y - 0.35, 0.0) + 0.25 * max(-dir.y - 0.2, 0.0));
        float th = 1.0 - 0.72 * frost;                         /* the front moves in as the frost grows */
        if (e + 0.24 > th - 0.02) {
#ifdef GXV
            if (wet <= 0.0) vn0 = GXOFF(GXQ_CLUMP) ? cq_clump(p) : vn_w(p * 16.0 + vec3(3.1, 7.7, 1.3));
#else
            if (wet <= 0.0) vn0 = vnoise(p * 16.0 + vec3(3.1, 7.7, 1.3));
#endif
            /* growth direction: along the glass towards the nose; feathers grow along it, barbs at 45 deg */
            vec3 tin = vec3(0.0, 0.0, -1.0) + dir * dir.z;
            tin -= n * dot(tin, n);
            tin *= inversesqrt(max(dot(tin, tin), 1e-6));
            vec3 q = p * 130.0;
            q -= tin * dot(q, tin) * 0.6;                      /* fronds ~2 cm long, ~8 mm apart */
            float fn = vnoise_i(q);
            float fe = e + 0.32 * (vn0 - 0.5) + 0.16 * (fn - 0.5);  /* a ragged front with fingers along the growth */
            float cov = smoothstep(th - 0.02, th + 0.05, fe);
            if (cov > 0.003 && rime) {
                /* (rain_layers 2026-10-30, ACZ_VR_CANOPY_RIME) in-flight icing is rime, not a window's frost ferns (those
                   grow by deposition in still air): the supercooled cloud droplets freeze where they strike, a granular
                   white crust mottled by the way the drops piled up, its feathers grown into the airflow (streaks along
                   the flow on the glass, ~2 mm across, ~2 cm along), thicker behind its front; as a modern game's mottled grey ice
                   layer (ice11 / ice12 and their pre-blurred copies). The world behind it is seen through a rough ice
                   surface: displaced by its facets (glass space: the same for both eyes) and blurred, more where it is
                   thick; never opaque (alpha <= ~0.55) */
                vec3 ta = vec3(0.0, 0.0, 1.0) - n * n.z;            /* aft along the glass (the flow) */
                ta = dot(ta, ta) > 1e-4 ? normalize(ta) : normalize(cross(n, vec3(1.0, 0.0, 0.0)));
                vec3 tc = cross(n, ta);
                /* (solid noise in the cockpit frame, as the rest of the weather: no parametrisation of the glass; the
                   feathers stretched along z, the flow, show along the flow's line on any pane) */
                float kd = clamp(0.0014 / max(px, 1e-5) - 0.6, 0.0, 1.0);   /* (fine grain faded where a pixel is coarser) */
                float gr1 = kd > 0.0 ? mix(0.5, vnoise_i(p * 700.0 + 1.3), kd) : 0.5;       /* grains ~1.4 mm */
                float gr2 = fn;               /* mottling: the front's own noise (~8 mm, drawn out along the flow) */
                float fe1 = mix(0.5, vnoise_i(vec3(p.x * 520.0, p.y * 520.0, p.z * 55.0) + 3.7),
                                clamp(0.002 / max(px, 1e-5) - 0.6, 0.0, 1.0));                /* feathers ~2 x 18 mm */
                float deep = smoothstep(th + 0.04, th + 0.4, fe);   /* behind the front the crust thickens */
                float h = 0.5 * gr2 + 0.25 * fe1 + 0.25 * gr1;
                float thick = clamp(mix(0.55 * h, 0.55 + 0.9 * (h - 0.5), deep), 0.0, 1.0);
                float af = cov * (0.07 + 0.45 * thick) * mix(0.3, 1.0, e) * min(gain, 1.3);
                vec3 ic = amb * 1.45 + vec3(0.13, 0.14, 0.16);
                vec3 fc = ic;
                if (have) {        /* the world through the rough ice: displaced by its facets, blurred by its grain */
                    vec2 o = dir_uv(tc * ((gr2 - 0.5) * 0.06) + ta * ((fe1 - 0.5) * 0.04));
                    float hg = hash_i(ivec3(floor(p * 900.0))) * 6.2831853;
                    vec2 r = (2.0 + 9.0 * thick) / tsz, cs = vec2(cos(hg), sin(hg));
                    /* (4 taps: two radii on a cross turned per crystal grain) */
                    vec3 bl = WORLD(uv0 + o);
                    fc = mix(bl, ic, 0.25 + 0.5 * thick);
                } else af *= 0.6;
                vec3 cq = p * 650.0;                                /* glints: 1.5 mm crystals, a third of them */
                ivec3 ci = ivec3(floor(cq));
                float hgl = hash_i(ci);
                if (hgl > 0.66) {
                    vec3 Nf = normalize(n + (vec3(hash_i(ci + ivec3(17, 0, 0)), hgl, hash_i(ci + ivec3(0, 31, 0))) - 0.5) * 0.9);
                    float spot = 1.0 - smoothstep(0.15, 0.45, length(fract(cq) - 0.5));
                    fc += sun * pow(max(dot(Nf, normalize(Ln + V)), 0.0), 220.0) * shade * 2.0 * spot * cov;
                }
                W = vec4(fc * af + W.rgb * (1.0 - af), af + W.a * (1.0 - af));
            } else if (cov > 0.003) {
                /* fern-like fronds: a spine where the feather noise crosses its middle, barbs leaning towards the
                   growth on either side (chevrons; thin lines ~2 mm apart that fade to their average where a pixel
                   is too coarse for them -- px comes from outside this branch) */
                float sg = 2.0 * fn - 1.0;                          /* across a frond: 0 on its spine */
                vec3 bd = normalize(tin + cross(n, tin) * (sg > 0.0 ? 1.0 : -1.0));
                float ac = dot(p, cross(n, bd)) * 500.0 + 3.0 * vn0 + 1.5 * fn;   /* (curling a little) */
                float hw = 1.0 - abs(2.0 * fract(ac) - 1.0);
                hw *= hw; hw *= hw; hw *= hw;
                hw = mix(hw, 0.12, smoothstep(0.2, 0.5, 700.0 * px));
                float spine = 1.0 - abs(sg); spine *= spine; spine *= spine;
                /* each barb (per frond patch) its own length and strength, so the fronds do not read as a weave */
                float bi = hash_i(ivec3(floor(q)) + ivec3(int(floor(ac)) * 7, sg > 0.0 ? 3 : 0, 0));
                float barb = hw * (1.0 - smoothstep(0.08, 0.2 + 0.55 * bi, abs(sg))) * (0.45 + 0.55 * bi);
                float gn = hash_i(ivec3(floor(p * 1100.0)));        /* crystal grain ~0.9 mm */
                float deep = smoothstep(th + 0.04, th + 0.4, fe);   /* behind the front the crystals close up into rime */
                float fth = min(spine * 0.7 + barb * 0.9, 1.0) * (1.0 - 0.35 * deep);
                float af = cov * (0.05 + 0.2 * fth + (0.04 + 0.1 * deep) * gn + 0.2 * deep) * mix(0.3, 1.0, e) * min(gain, 1.3);
                vec3 ic = amb * 1.45 + vec3(0.13, 0.14, 0.16);
                vec3 fc = ic;
                if (have) {        /* frosted: the world behind blurred (one diagonal per crystal grain, 3 taps) */
                    vec2 r = (3.0 + 8.0 * deep) / tsz * vec2(1.0, gn > 0.5 ? 1.0 : -1.0);
                    vec3 bl = base;
                    fc = mix(bl, ic, 0.3 + 0.4 * fth + 0.25 * deep);
                } else af *= 0.6;
                vec3 cq = p * 650.0;                                /* glints: 1.5 mm crystals, a third of them */
                ivec3 ci = ivec3(floor(cq));
                float hg = hash_i(ci);
                if (hg > 0.66) {
                    vec3 Nf = normalize(n + (vec3(hash_i(ci + ivec3(17, 0, 0)), hg, hash_i(ci + ivec3(0, 31, 0))) - 0.5) * 0.9);
                    float spot = 1.0 - smoothstep(0.15, 0.45, length(fract(cq) - 0.5));
                    fc += sun * pow(max(dot(Nf, normalize(Ln + V)), 0.0), 220.0) * shade * 2.0 * spot * cov;
                }
                W = vec4(fc * af + W.rgb * (1.0 - af), af + W.a * (1.0 - af));
            }
        }
    }
    /* the glass (inside) over the water */
    wash = clamp(max(m, W.a * 2.0), 0.0, 1.0);
    return vec4(gc.rgb + (1.0 - gc.a) * W.rgb, gc.a + (1.0 - gc.a) * W.a);
}

void main() {
    vec4 t = texture(texs[nonuniformEXT(v_tex)], vec3(v_uv, 0.0));
    float ca = v_ca * t.a;
    float a = clamp(ca + (1.0 - ca) * v_fa, 0.0, 1.0);
    vec4 gc = vec4(t.rgb * v_li * ca + v_add, a);
    vec3 V = normalize(cp.head.xyz - v_p);
    vec3 n = normalize(v_n);
    if (dot(n, V) < 0.0) n = -n;
    int gcode = int(-cp.light.w + 0.5);
    float thk = float(gcode >> 8) * 0.001;
    float wash;
    out_color = canopy_water(v_p, n, V, v_ln, v_amb, v_sc, v_shade, gc, thk, wash);
}
