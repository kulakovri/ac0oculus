#version 450
/* Canopy cracks in the Quest eye pass (rq_glass_crk.vert): segments pass their colour through; a core patch is shaded as
   cockpit.frag crk_glass's core -- the milky crushed core with its grains, the halo of fine radial micro-cracks (to their
   mean where they get closer than about 3 px), the whitened rim of a hole -- from the interpolated impact-plane uv.
   Premultiplied. No position read (R-45). */
precision highp float;
layout(location = 0) in mediump vec4 v_col;
layout(location = 1) in vec2 v_uv;
layout(location = 2) flat in vec4 v_k;
layout(location = 3) flat in vec4 v_h;
layout(location = 4) in float v_px;
layout(location = 0) out vec4 o;

float hash_i(ivec3 c) {
    uvec3 u = uvec3(c & 1023);
    uint h = u.x * 0x8DA6B343u ^ u.y * 0xD8163841u ^ u.z * 0xCB1AB31Fu;
    h ^= h >> 15u; h *= 0x2C1B3C6Du; h ^= h >> 12u;
    return float(h) * (1.0 / 4294967296.0);
}
float vnoise(vec3 x) {
    ivec3 i = ivec3(floor(x));
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    float a = mix(hash_i(i), hash_i(i + ivec3(1, 0, 0)), f.x), b = mix(hash_i(i + ivec3(0, 1, 0)), hash_i(i + ivec3(1, 1, 0)), f.x);
    float c = mix(hash_i(i + ivec3(0, 0, 1)), hash_i(i + ivec3(1, 0, 1)), f.x), e = mix(hash_i(i + ivec3(0, 1, 1)), hash_i(i + ivec3(1, 1, 1)), f.x);
    return mix(mix(a, b, f.y), mix(c, e, f.y), f.z);
}
vec4 over(vec4 top, vec4 under) { return vec4(top.rgb + (1.0 - top.a) * under.rgb, top.a + (1.0 - top.a) * under.a); }

void main() {
    if (v_k.x < 0.5) { o = v_col; return; }
    vec2 uv = v_uv;
    float r = length(uv), rc = v_k.y, rh = v_k.z, ca = v_k.w, rho = v_h.x, grow = v_h.y, si = v_h.z;
    float px = v_px;
    vec3 milk = v_col.rgb;
    vec4 W = vec4(0.0);
    if (rho > 0.0) {                             /* a hole's whitened, ragged rim (the hole itself: the glass is clear there anyway) */
        float rag = rho * (0.85 + 0.3 * vnoise(vec3(uv * 1400.0, si * 3.7)));
        float rim = (1.0 - smoothstep(rag * 1.15, rag * 1.45 + px, r)) * grow * step(rag * grow, r);
        W = over(vec4(milk * (0.6 * rim), 0.6 * rim), W);
    }
    if (rh > 0.0 && r < rh) {
        float aa = max(px, 1e-4);
        float core = (1.0 - smoothstep(rc * 0.55, rc + aa, r)) * grow;
        float gr = vnoise(vec3(uv * 900.0, si * 7.1));            /* crushed grains, ~1 mm */
        gr = mix(0.5, gr, clamp(0.0011 / max(px, 1e-6) - 0.5, 0.0, 1.0));
        float ang = atan(uv.y, uv.x);
        float streak = vnoise(vec3(ang * 6.366, r * 160.0, si * 1.3));
        float sp = r * 0.157 / max(px, 1e-6);
        streak = mix(0.45, smoothstep(0.55, 0.85, streak), clamp(sp / 3.0 - 0.3, 0.0, 1.0));
        float halo = rh > rc ? (1.0 - smoothstep(rc, rh, r)) : 0.0;
        halo = halo * halo * streak * grow;
        float a = min(ca * core * (0.7 + 0.45 * gr) + 0.4 * halo * (1.0 - core), 0.75);
        W = over(vec4(milk * a, a), W);
    }
    o = W * v_h.w;
}
