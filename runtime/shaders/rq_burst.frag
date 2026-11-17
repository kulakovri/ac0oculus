#version 450
#extension GL_EXT_multiview : require
/* rq_burst.frag -- Pixy's MPBM burst as a volume in the Quest's eye pass (PS2_RQ_BURST, rq/rq_burst.inc; BUGS_RQ R-120; the user's M18
   flight of 2026-11-12: "it turns, it is not volumetric; copy how this explosion is made in the PC version"). The PC's burst.frag
   (ACZ_VR_FX_BURST, fx_burst 2026-11-02, bug #266) at the Quest's price: the same medium -- a thick shell whose outline is broken by
   turbulence riding the blast outwards, emitting through a black-body ramp (white-hot core, yellow, orange, deep red) and absorbing more
   as it cools into smoke, the game's halo colour on the thin outer rim -- marched along each eye's own ray (true stereo, the eye may be
   inside), but with the noise from a 256 x 256 texture (two value-noise slices per read, iq's packing) instead of 24 hashes a step, and
   rqy.z steps (8; 6, 5 or 4 when the burst covers more of the view) instead of 16 shaded once per 2 x 2 or 4 x 4 pixels. Each
   pixel is shaded once (the pass's 4x MSAA resolves coverage and depth only). The ray comes from v_pos, not from the pixel, and no
   attachment is read (the R-45 rule); gl_FragCoord seeds only the first step's jitter. Premultiplied over; the pass's alpha is left alone. */
layout(location = 0) in vec3 v_pos;
layout(location = 0) out vec4 o_col;

layout(push_constant) uniform BP {
    vec4 cx, cy, cz;
    vec4 proj;
    vec4 misc;
    vec4 eyes;          /* x left eye shift, y right eye shift (units along cx), z time (s), w units per metre */
    vec4 ctr;           /* xyz centre from the centre eye (units, world), w radius (units) */
    vec4 st;            /* x age / life, y strength, z seed, w quad half size */
    vec4 heat;          /* x core heat, y the game's glow strength (sqrt of its alpha: 1 full, 0 gone), z smoke share, w the eye's
                           distance to the centre / radius */
    vec4 tint;          /* rgb the game's halo colour, w its share */
    vec4 fogc;          /* rgb fog colour, w the fog's amount at the burst */
    vec4 rqx;
    vec4 rqy;           /* z the march's steps */
} bp;
layout(set = 0, binding = 0) uniform mediump sampler2D noiset;   /* 256 x 256 RGBA8, repeat, linear: r / g one noise's slices z / z + 1,
                                                                     b / a another's (g(x) = r(x + (37, 17)), a(x) = b(x + (37, 17))) */

/* two independent value noises at p, one read: the lattice's z slices lie (37, 17) apart in the texture */
mediump vec2 vnoise2(vec3 p) {
    vec3 i = floor(p);
    mediump vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    vec2 uv = i.xy + vec2(37.0, 17.0) * i.z + f.xy;
    mediump vec4 t = textureLod(noiset, (uv + 0.5) * (1.0 / 256.0), 0.0);
    return mix(t.xz, t.yw, f.z);
}
/* black-body-like ramp, 0 cold smoke .. 1 white-hot: piecewise linear between the PC's stops */
mediump vec3 heat_col(mediump float t) {
    const mediump vec3 c0 = vec3(0.075, 0.066, 0.062);
    const mediump vec3 c1 = vec3(0.30, 0.055, 0.025);
    const mediump vec3 c2 = vec3(0.78, 0.17, 0.035);
    const mediump vec3 c3 = vec3(1.00, 0.46, 0.10);
    const mediump vec3 c4 = vec3(1.00, 0.76, 0.36);
    const mediump vec3 c5 = vec3(1.00, 0.93, 0.74);
    const mediump vec3 c6 = vec3(1.00, 1.00, 0.98);
    t = clamp(t, 0.0, 1.0) * 6.0;
    if (t < 1.0) return mix(c0, c1, t);
    if (t < 2.0) return mix(c1, c2, t - 1.0);
    if (t < 3.0) return mix(c2, c3, t - 2.0);
    if (t < 4.0) return mix(c3, c4, t - 3.0);
    if (t < 5.0) return mix(c4, c5, t - 4.0);
    return mix(c5, c6, t - 5.0);
}

void main() {
    float sh = gl_ViewIndex == 0 ? bp.eyes.x : bp.eyes.y;
    vec3 eye = sh * bp.cx.xyz;
    vec3 D = normalize(v_pos - eye);
    float R = bp.ctr.w;
    vec3 oc = (eye - bp.ctr.xyz) / R;                    /* the eye in the sphere's units */
    float b = dot(oc, D), c = dot(oc, oc) - 1.0, disc = b * b - c;
    if (disc <= 0.0) discard;
    float s = sqrt(disc);
    float t0 = max(-b - s, 0.0), t1 = -b + s;
    if (t1 <= t0) discard;
    mediump float age = clamp(bp.st.x, 0.0, 1.0), seed = bp.st.z;
    mediump float core = bp.heat.x, dis = 1.0 - clamp(bp.heat.y, 0.0, 1.0), smoke = bp.heat.z;
    int N = int(bp.rqy.z + 0.5);
    float dt = (t1 - t0) / float(N);
    /* the first step's jitter: white noise over the pixel grid. gl_FragCoord only seeds noise here (under the density map its value in
       a reduced bin is off, which a dither does not mind; this stage has no specialization constant: the R-45 guard's rule). A hash of
       the ray's direction made moire rings; the PC's interleaved gradient noise drew diagonal hatching at 8 steps per pixel */
    float jit = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    float t = t0 + dt * jit;
    mediump vec3 acc = vec3(0.0);
    mediump float tr = 1.0;
    const mediump float sig = 6.0;                       /* extinction at full density, per radius */
    vec3 sd = vec3(seed, seed * 0.71, seed * 1.37);
    float roll = bp.eyes.z * 0.05;
    for (int k = 0; k < N; k++, t += dt) {
        vec3 p = oc + D * t;
        mediump float r = length(p);
        if (r > 1.0) continue;
        mediump vec3 u = p / max(r, 1e-3);
        vec3 q = p * 2.4 + sd - u * (0.8 * age) + vec3(0.0, roll, 0.0);
        mediump vec2 na = vnoise2(q);                     /* (the PC: fbm = 0.667 n(q) + 0.333 n(2.03 q); n2 = n(3.3 q)) */
        mediump vec2 nb = vnoise2(q * 2.03 + 17.1);
        mediump float bl = 0.78 * (0.667 * na.x + 0.333 * nb.x) + 0.22 * (0.5 * na.y + 0.5 * nb.y);
        mediump float edge = 1.0 - 0.34 * (1.0 - bl);
        mediump float dens = 1.0 - smoothstep(edge - 0.24, edge, r);
        dens *= smoothstep(0.18, 0.62, bl + 0.32 * (1.0 - r));
        dens *= smoothstep(0.55 * dis, 0.55 * dis + 0.30, bl);
        dens *= mix(1.0, 0.30 + 0.70 * smoothstep(0.0, 0.7, r), smoothstep(0.15, 0.75, age));
        if (dens <= 0.002) continue;
        mediump float T = clamp(core * (1.18 - 0.92 * r * r) + 0.50 * (bl - 0.5), 0.0, 1.0);
        mediump vec3 col = heat_col(T);
        mediump float em = 0.45 + 3.0 * T * T;
        col = max(col, heat_col(0.42) * (0.22 * core * (1.0 - T)));
        mediump float rim = smoothstep(0.62, 1.0, r) * (1.0 - smoothstep(0.0, 0.7, dens));
        col = mix(col, bp.tint.rgb, bp.tint.w * rim);
        mediump float ext = sig * dens * (0.32 + 1.2 * smoke * (1.0 - T));
        mediump float a = 1.0 - exp(-ext * dt);
        acc += tr * a * col * em;
        tr *= 1.0 - a;
        if (tr < 0.01) break;
    }
    mediump float alpha = (1.0 - tr) * bp.st.y;
    mediump vec3 rgb = acc * bp.st.y;
    rgb = mix(rgb, bp.fogc.rgb * alpha, bp.fogc.w);
    rgb += (jit - 0.5) / 255.0;                          /* (dither the 8-bit target) */
    o_col = vec4(max(rgb, vec3(0.0)), clamp(alpha, 0.0, 1.0));
}
