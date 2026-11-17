/* rq_cut.glsl -- PS2_RQ_CLOUDS_CUT (rq13_cut 2026-11-15, prompt 12 Part A, BUGS_RQ R-190): the cloud in front of an object is applied by
   the object at its own exact distance, not cut out of the cloud layer by a coarse scene depth. The composite cut the one cloud layer at
   the scene by the 1x feed depth (512 x 448, one texel ~3.4 x 4.3 eye pixels): every edge of a ridge or an aircraft against cloud came
   out in that texel grid ("the stairs"). Now the far map (the one layer, marched from this frame's eye: per direction the whole ray's
   light L, premultiplied, and transmittance T; its profile w / mean, w / first, w and the front part's opacity to the split) is read by
   the object itself: a point at distance d takes the part of the ray in front of it, spread as the composite's SOFT spread it -- the
   front part (to the split) over [first, min(2 mean - first, split)], the rest over [max(first, split), 2 mean - that] -- and inside
   each span as Beer-Lambert: T_d = T_f^s_f x T_b^s_b, L_d = L (1 - T_d) / (1 - T); colour = colour x T_d + L_d. The composite then draws
   only where nothing stands (its quads at the far plane, depth GEQUAL: the cleared depth), with no scene-depth read: every silhouette
   is the object's own, resolved per sample by the 4x pass, and nothing lags a frame.
   Included by mesh.vert and gs.vert (-DRQCUT: per vertex, the -DRQ eye-pass twins cut_*.vert) and rq_terrain.frag -DRQO (-DRQCUT: per
   fragment). Inputs: set 2 bindings 9 (the far map, linear, clamp) and 10 (its profile, linear, clamp), written by rq_clouds.inc
   rqcut_mset; the reserved mesh block RQC_BLOCK (rq_clouds.inc rqcut_block): q0 the game camera's x axis in cloud space + game units a
   metre, q1 its y axis + the map's used size N, q2 its z axis (back) + the split (m), q3 fx, fy, cx, cy (slot-0 target
   pixels), q4 the GS depth's a, c, mh, zmax (view depth = mh / (a (z zmax + c)) game units), q5.x 1 on, 2 [test] the share of cloud
   in front as red, the object's own colour dropped (PS2_RQ_CLOUDS_CUT=2), q5.y PS2_RQ_CLOUDS_CUT_PARTS (bit 16: the merged terrain per vertex), q5.z [test] PS2_RQ_CLOUDS_CUT_MODEL, q6 (round 2) the eye's altitude, the band's base and top (m), 1 = the band test on */
#define RQC_BLOCK 16373u                    /* = rq_clouds.inc RQC_BLOCK (MESH_MAX_BLOCKS - 11) */
#define RQC_TL_BIT 536870912u               /* mesh_pad / mv_flags bit 29: this draw takes the cloud in front of it (T and L) */
#define RQC_T_BIT 268435456u                /* bit 28: only its transmittance (an additive or subtractive draw: the light is the base's) */
#define RQC_FLAT_BIT 67108864u              /* (round 2) bit 26, GS draws: an alpha-tested card -- one evaluation, its provoking vertex's, flat */
layout(std430, set = 2, binding = 0) readonly buffer RqcBlocks { vec4 rqcq[]; };
layout(set = 2, binding = 9) uniform highp sampler2D rqc_col;
layout(set = 2, binding = 10) uniform highp sampler2D rqc_prof;
#define RQC_Q(i) rqcq[RQC_BLOCK * 30u + (i)]
highp vec2 rqc_dir_uv(highp vec3 d) {
    d /= abs(d.x) + abs(d.y) + abs(d.z);
    highp vec2 p = d.xz;
    if (d.y < 0.0) p = (1.0 - abs(p.yx)) * vec2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    return p * 0.5 + 0.5;
}
/* a slot-0 target pixel at view depth vm (m) -> cloud space (m, from the centre eye) */
highp vec3 rqc_px_cloud(highp vec2 px, highp float vm) {
    highp vec4 pj = RQC_Q(3);
    highp vec3 v = vec3((px.x - pj.z) / pj.x, -(px.y - pj.w) / pj.y, -1.0) * vm;
    return RQC_Q(0).xyz * v.x + RQC_Q(1).xyz * v.y + RQC_Q(2).xyz * v.z;
}
/* the homogeneous slot-0 target position (x w, y w, w; w the view depth in game units, of any sign: a vertex behind the eye too) -> cloud
   space (m, from the centre eye): linear in the position, so it interpolates exactly across a triangle the clipper cut at the eye */
highp vec3 rqc_hom_cloud(highp vec3 h) {
    highp vec4 pj = RQC_Q(3);
    highp vec3 v = vec3((h.x - pj.z * h.z) / pj.x, -(h.y - pj.w * h.z) / pj.y, -h.z) / RQC_Q(0).w;
    return RQC_Q(0).xyz * v.x + RQC_Q(1).xyz * v.y + RQC_Q(2).xyz * v.z;
}
/* the GS depth z (0..1, reversed) -> view depth (m); 1e9 = at infinity */
highp float rqc_gs_depth_m(highp float z) {
    highp vec4 dq = RQC_Q(4);
    highp float d = dq.x * (z * dq.w + dq.y);
    return (z > 0.0 && d > 0.0) ? dq.z / d / RQC_Q(0).w : 1.0e9;
}
/* the cloud in front of P (cloud space, m, from the centre eye): rgb L_d, a T_d; full = the whole ray's (L, T) */
highp vec4 rqc_front(highp vec3 P, out highp vec4 full) {
    highp float d = length(P);
    highp vec2 uv = rqc_dir_uv(P / max(d, 1.0e-3)) * (RQC_Q(1).w / float(textureSize(rqc_col, 0).x));   /* (the map's used N of its image) */
    highp vec4 c = textureLod(rqc_col, uv, 0.0);
    full = c;
    if (!(c.a < 0.999)) return vec4(0.0, 0.0, 0.0, 1.0);
    highp vec4 pr = textureLod(rqc_prof, uv, 0.0);
    if (!(pr.z > 1.0e-3 && pr.x > 1.0e-9)) return c;                     /* (no distance: as the composite, the whole ray) */
    highp float mean = pr.z / pr.x, first = min(pr.z / max(pr.y, 1.0e-9), mean), ds = RQC_Q(2).w;
    highp float A = 1.0 - c.a, as = clamp(pr.w, 0.0, A);
    highp float lastf = clamp(2.0 * mean - first, first + 30.0, max(ds, first + 30.0));
    highp float fb = max(first, ds), lastb = max(2.0 * mean - fb, fb + 30.0);
    highp float sf = smoothstep(first, lastf, d), sb = smoothstep(fb, lastb, d);
    highp float Tf = max(1.0 - as, 1.0e-4), Tb = clamp(c.a / Tf, 1.0e-4, 1.0);
    highp float Td = exp2(sf * log2(Tf) + sb * log2(Tb));
    if (RQC_Q(5).z > 0.5 && RQC_Q(5).z < 1.5) Td = 1.0 - (as * sf + (A - as) * sb);         /* ([test] PS2_RQ_CLOUDS_CUT_MODEL=1: SOFT's own share, linear in the opacity) */
    if (RQC_Q(5).x > 1.5) return vec4(1.0 - Td, 0.0, 0.0, 0.0);          /* ([test] =2: the share in front as red) */
    return vec4(c.rgb * ((1.0 - Td) / max(A, 1.0e-4)), Td);
}
/* (round 2, R-195) the band test, per vertex (taken flat from the provoking vertex): false when no cloud can lie between the eye and P -- the
   eye more than 100 m and P more than vmargin under the cloud band's base, or both over its top (the march holds density only inside the
   band, ps2_clouds C1 base_m / top_m). vmargin covers the triangle's own extent (the terrain's: 400 m). q6 = the eye's altitude (m), the
   band's base, top, 1 on (PS2_RQ_CLOUDS_CUT_SKIP) */
bool rqc_need(highp vec3 P, highp float vmargin) {
    highp vec4 b = RQC_Q(6);
    if (b.w < 0.5) return true;
    highp float yp = b.x + P.y;
    if (b.x < b.y - 100.0 && yp < b.y - vmargin) return false;
    if (b.x > b.z + 100.0 && yp > b.z + vmargin) return false;
    return true;
}
/* (round 2) the natives' and the unmerged terrain's v_cut (mesh.vert -DRQCUT, mesh.frag / rq_terrain.frag without RQO): w < -0.5 nothing;
   [-0.5, 1.5) T and L at the place xyz, per fragment; [1.5, 3.5) T only there; >= 3.5 [test] per vertex (L_d, T_d + 4), _CUT_PARTS bit 32.
   k: the band test, flat. Returns (L, T) for colour x T + L, every per-vertex part clamped */
highp vec4 rqc_native(highp vec4 vc, highp float k) {
    if (k < 0.5 || vc.w < -0.5) return vec4(0.0, 0.0, 0.0, 1.0);
    if (vc.w > 3.5) return vec4(max(vc.rgb, vec3(0.0)), clamp(vc.w - 4.0, 0.0, 1.0));
    highp vec4 full, f = rqc_front(vc.xyz, full);
    if (vc.w > 1.5 && RQC_Q(5).x < 1.5) f.rgb = vec3(0.0);
    return f;
}
/* a draw's mode (mesh_pad bits 28 / 29) applied to P: (L_d, T_d), (0, T_d) or the identity */
highp vec4 rqc_vertex(uint bits, highp vec3 P) {
    if ((bits & (RQC_TL_BIT | RQC_T_BIT)) == 0u) return vec4(0.0, 0.0, 0.0, 1.0);
    highp vec4 full, f = rqc_front(P, full);
    if ((bits & RQC_TL_BIT) == 0u && RQC_Q(5).x < 1.5) f.rgb = vec3(0.0);
    return f;
}
