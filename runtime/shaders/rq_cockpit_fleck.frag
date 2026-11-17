#version 450
/* The Quest cockpit's debris flecks, ACZ_VR_RQ_COCKPIT_WET=lean2 (rq_cockpit.inc rqw_flecks): cockpit.frag's debris_flecks, drawn only on the few glass triangles round the
   active flecks and only on frames with flecks. The world behind is seen through the blend factor, not read (the soot's base term: the part of dst kept). */
layout(location = 6) in vec3 v_p;
layout(location = 7) in vec3 v_n;
layout(location = 8) in vec3 v_amb;
layout(location = 0) out vec4 o;

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

uvec3 pcg3(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}
float hash1(ivec3 c) { return float(pcg3(uvec3(c & 4095)).x) * (1.0 / 4294967296.0); }
float vnoise(vec3 x) {
    ivec3 i = ivec3(floor(x));
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    float a = mix(hash1(i), hash1(i + ivec3(1, 0, 0)), f.x), b = mix(hash1(i + ivec3(0, 1, 0)), hash1(i + ivec3(1, 1, 0)), f.x);
    float c = mix(hash1(i + ivec3(0, 0, 1)), hash1(i + ivec3(1, 0, 1)), f.x), e = mix(hash1(i + ivec3(0, 1, 1)), hash1(i + ivec3(1, 1, 1)), f.x);
    return mix(mix(a, b, f.y), mix(c, e, f.y), f.z);
}
vec3 fleck_dir(uint u) {
    vec2 e = vec2(float((u >> 15u) & 255u), float((u >> 7u) & 255u)) * (1.0 / 127.5) - 1.0;
    vec3 d = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    float t = max(-d.z, 0.0);
    d.xy += vec2(d.x >= 0.0 ? -t : t, d.y >= 0.0 ? -t : t);
    return normalize(d);
}
void main() {
    vec3 p = v_p, amb = v_amb;
    vec3 n = normalize(v_n);
    if (dot(n, cp.head.xyz - p) < 0.0) n = -n;
    float px = length(fwidth(p)) * 0.7;
    vec4 W = vec4(0.0);
    for (int i = 0; i < 3; i++) {
        float code = cp.su[i];
        if (code < 0.5) continue;
        uint u = uint(code + 0.25);
        float fq = float(u & 31u) / 31.0, s0 = 0.4 + 0.2 * float((u >> 5u) & 3u);
        vec3 m = fleck_dir(u);
        float t = dot(p, m);
        if (t <= 0.05) continue;
        vec3 q = p - m * t;
        bool big = (u >> 23u) != 0u;
        float gs = big ? 1.5 : 1.0;
        if (dot(q, q) > (big ? 0.0036 : 0.0016)) continue;
        vec3 ta = vec3(0.0, 0.0, 1.0) - n * n.z;
        ta = dot(ta, ta) > 1e-4 ? normalize(ta) : normalize(cross(n, vec3(1.0, 0.0, 0.0)));
        vec3 tc = cross(n, ta);
        vec2 oq = vec2(dot(q, tc), dot(q, ta));
        float h = fract(float((u >> 7u) & 0xFFFFu) * 0.6180339);
        float age = 1.0 - fq;
        float sm = 1.0 + 2.2 * age;
        vec2 os = vec2(oq.x, oq.y > 0.0 ? oq.y / sm : oq.y * 1.4);
        float R = (0.005 + 0.004 * h) * (1.0 + 0.25 * age) * gs;
        float rr = length(os);
        float edge = vnoise(vec3(oq * 1500.0, h * 31.0));
        float aa = max(px, 2e-4);
        float core = 1.0 - smoothstep(R * (0.72 + 0.25 * edge) - aa, R * (0.86 + 0.25 * edge) + aa, rr);
        core *= 0.7 + 0.3 * vnoise(vec3(oq * 4200.0, h * 7.0));
        float mid = 1.0 - smoothstep(0.0, R * 0.6, rr);
        vec2 gq = os * 330.0;
        float hs = hash1(ivec3(ivec2(floor(gq)), int(h * 997.0)));
        float sp = step(0.8, hs) * (1.0 - smoothstep(0.15, 0.35, length(fract(gq) - 0.5))) * (1.0 - smoothstep(R * 1.4, 0.022 * gs, rr)) * step(R, rr);
        sp *= clamp(0.003 / max(px, 1e-5) - 0.5, 0.0, 1.0);
        float ring = smoothstep(R * 0.9, R * 1.2, rr) * (1.0 - smoothstep(R * 1.2, R * 1.9, rr));
        float k = s0 * fq * (big ? 1.35 : 1.0);
        float ar = ring * 0.06 * k, a = min((core * 0.38 + mid * 0.17 + sp * 0.35) * k, 0.8);
        vec3 pale = amb * 1.3 + 0.06;
        W = vec4(pale * ar + W.rgb * (1.0 - ar), ar + W.a * (1.0 - ar));
        float af = a * 0.825;                                 /* (soot: mix(base x 0.5, K, 0.65): of the world behind stays 0.175 of a) */
        W = vec4(vec3(0.09, 0.08, 0.07) * 0.65 * a + W.rgb * (1.0 - af), af + W.a * (1.0 - af));
    }
    if (W.a <= 0.0) discard;
    o = W;
}
