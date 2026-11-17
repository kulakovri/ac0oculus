#version 450
#extension GL_EXT_multiview : require
/* rq_clouds.vert (plan 4.2; rq_clouds.inc): the quad of one far-map tile that holds cloud, for both views (multiview), by the
   instance of the draw list rq_cloud_far.comp made. The tile's corners are directions (the map's octahedral mapping); they go
   through the game's camera and projection (slot-0 target pixels) and the per-view crop, as the -DRQ vertex shaders place the
   game's draws (x' = A x + B w). The parallax is evaluated here, per corner (the map uv is linear inside a tile): the eye minus
   the tile's march origin, a first guess from the tile's nearest cloud, one step from the map's depth data (rq_cloud_far.comp:
   (1 / mean, 1 / first) of the four texels around the guess, the empty ones ignored). The fragment shader reads the far colour
   once at the interpolated uv. The quad's depth is the first cloud distance of the corner: GEQUAL against the frame's own depth
   cuts a mountain in front per sample, with no scene-depth read.
   PS2_RQ_CLOUDS_FARWARP (B-1, R-52; misc.w bit 0): the corner itself moves instead of its uv: its content (the map's own texels
   at the corner, uv unshifted) is put where it lies now, d x dist - off, with dist the mean distance of the texels around the
   corner (the same four texels for every tile that shares the corner; a wider ring where they are empty; a constant beyond).
   With one march origin for all tiles in view (PS2_RQ_CLOUDS_FARSWAP) the quads form one continuous displaced grid: no seam, no
   uv stretch inside a tile, no content cut at an undrawn neighbour.
   PS2_RQ_CLOUDS_SOFT (B-2, R-52; misc.w bit 1): the quad's depth is the tile's (and its neighbours') nearest cloud, a near bound:
   the hardware test only cuts where the scene lies in front of all of the tile's cloud; rq_clouds.frag fades the rest by the
   scene's distance inside the cloud's span (v_th: the target pixel, homogeneous; v_ke: eye / march-origin distance). */
struct ClTile { vec4 cam; uvec4 st; };
layout(set = 1, binding = 0, std430) readonly buffer T { ClTile tiles[]; };
layout(set = 1, binding = 1, std430) readonly buffer D { uvec2 draw[]; };
layout(set = 1, binding = 2, std140) uniform V {
    vec4 cam_x, cam_y, cam_z;     /* the view's axes (cloud space, world directions); cam_z = back */
    vec4 proj;                    /* fx, fy, cx, cy (slot-0 target pixels) */
    vec4 tgt;                     /* target w, target h, map N used, 8 / N */
    vec4 xfx;                     /* the per-view crop: A0, B0, A1, B1 */
    vec4 xfy;                     /* Ay, By, unused, unused */
    vec4 eye[2];                  /* the eyes in cloud space (m, relative to the centre eye) */
    vec4 cam_now;                 /* xyz: the centre eye in the far scheduler's anchor frame now */
    vec4 depth;                   /* a, c, mh, zmax: game view depth = mh / (a (z zmax + c)) */
    vec4 misc;                    /* x game units per metre, y [debug] PS2_RQ_CLOUDS_FARZ, z [debug] PS2_RQ_CLOUDS_VIEW,
                                     w the composite's flags (bit 0 FARWARP, bit 1 SOFT; bit 5 rq8_onelayer's PS2_RQ_CLOUDS_ONE) */
    vec4 ncell, nuv;              /* (rq_clouds_near.vert's); nuv.w the FARWARP fallback distance (m) */
} v;
layout(set = 1, binding = 3) uniform sampler2D prof;   /* the map's depth data, nearest (read with textureGather) */
layout(location = 0) out vec2 v_uv;
layout(location = 1) flat out vec4 v_id;   /* [debug] PS2_RQ_CLOUDS_VIEW=3: the tile's id as a flat colour (a > 0.5) */
layout(location = 2) out vec3 v_th;        /* (SOFT) the slot-0 target pixel, homogeneous: x w, y w, w */
layout(location = 3) flat out int v_eye;
layout(location = 4) out float v_ke;       /* (SOFT) the eye's distance over the march origin's at the corner's mean depth */
/* PS2_RQ_CLOUDS_FARRESOLVE (clouds6 2026-11-11, A.4 piece 4, R-80; misc.w bit 2): the quad stays at its tile's own directions (seen from
   the eye) and rq_clouds.frag resolves the parallax per pixel: the eye ray against the sphere of the map's distance about the tile's
   march origin. These carry the inputs: the world direction (interpolated: no fragment-position read), the eye's offset from the
   origin (flat), the corner's mean distance from the origin (the first guess) and the map's used-size scale */
layout(location = 5) out vec3 v_dir;
layout(location = 6) flat out vec4 v_off;   /* xyz the eye's offset from the tile's march origin (m), w the uv scale (used / image size) */
layout(location = 7) out float v_dist;
/* (clouds7, PS2_RQ_CLOUDS_FARSHOWN, misc.w bit 4) the tile's texel-centre bounds in the shown map's uv (rq_clouds.frag clamps its read to
   them: a neighbour tile not resolved this frame is never mixed in) */
layout(location = 8) flat out vec4 v_tb;
/* (rq10_artefacts, PS2_RQ_CLOUDS_SOFT_CUT: rq_clouds.frag's constant CUT) the view depth (m) of the depth the corner is drawn at: interpolated
   perspective-correctly it is the quad's own depth at each pixel, the place where the pass's depth test cuts */
layout(location = 9) out float v_qm;
vec3 uv_dir(vec2 uv) {
    vec2 p = uv * 2.0 - 1.0;
    vec3 d = vec3(p.x, 1.0 - abs(p.x) - abs(p.y), p.y);
    if (d.y < 0.0) d.xz = (1.0 - abs(d.zx)) * vec2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
    return normalize(d);
}
vec2 dir_uv(vec3 d) {
    d /= abs(d.x) + abs(d.y) + abs(d.z);
    vec2 p = d.xz;
    if (d.y < 0.0) p = (1.0 - abs(p.yx)) * vec2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    return p * 0.5 + 0.5;
}
/* (rq10_artefacts round 2, PS2_RQ_CLOUDS_FOLD=2) dir_uv on the face of the octahedron that holds ref, continued linearly past its edges: for d
   on that face it is dir_uv(d); a d just across an edge -- the map's outer border, the lower hemisphere's meridians -- lands just outside
   the face's part of the map instead of on the mirrored side (the sampler clamps it to the border texel) */
vec2 dir_uv_face(vec3 d, vec3 ref) {
    vec3 sg = vec3(ref.x >= 0.0 ? 1.0 : -1.0, ref.y >= 0.0 ? 1.0 : -1.0, ref.z >= 0.0 ? 1.0 : -1.0);
    d /= dot(d, sg);
    vec2 p = d.xz;
    if (ref.y < 0.0) p = (1.0 - vec2(p.y * sg.z, p.x * sg.x)) * sg.xz;
    return p * 0.5 + 0.5;
}
/* the four texels around u: sum of the valid 1 / mean (x), their count (y), the largest valid 1 / first (z) */
vec3 gather3(vec2 u) {
    vec4 g0 = textureGather(prof, u, 0), g1 = textureGather(prof, u, 1);
    vec4 ok = vec4(greaterThan(g0, vec4(1.0e-6)));
    return vec3(dot(g0, ok), ok.x + ok.y + ok.z + ok.w, max(max(g1.x * ok.x, g1.y * ok.y), max(g1.z * ok.z, g1.w * ok.w)));
}
void main() {
    const vec2 C[6] = vec2[6](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(1, 1), vec2(0, 1));
    uvec2 e = draw[gl_InstanceIndex];
    uint id = e.x, tpr = uint(v.tgt.z / 8.0 + 0.5);
    ivec2 tc = ivec2(id % tpr, id / tpr);
    vec2 uv = (vec2(tc) + C[gl_VertexIndex]) * v.tgt.w;
    v_id = vec4(vec2(tc % 16) * (16.0 / 255.0), 1.0, v.misc.z > 2.5 ? 1.0 : 0.0);
    vec3 d = uv_dir(uv);
    int view = int(gl_ViewIndex);
    int fl = int(v.misc.w + 0.5);
    /* the parallax at this corner */
    vec3 off = v.eye[view].xyz + v.cam_now.xyz - tiles[id].cam.xyz;
    /* PS2_RQ_CLOUDS_FARWARP_ONE (clouds6 2026-11-11, the light variant; misc.w bit 3): every tile's parallax from the SHOWN sweep's
       origin (PASS_TILES' draw[NT]), not its own. FARWARP's corners are a function of the corner's texels and the offset; with one
       offset for every tile two neighbours move their shared corner identically, so the quads stay watertight whatever origins the
       tiles were marched from (the tearing of R-80 was neighbours moving a shared corner by different offsets). A tile marched
       elsewhere is corrected with the shown origin's offset: its content off by (its origin - the shown) across the ray over its
       depth -- with FARONE and 256 tiles a frame, a sweep or two (tens to a few hundred metres at kilometres) */
    if ((fl & 8) != 0) {
        uint nt1 = tpr * tpr;
        if (draw[nt1 + 1u].y == 0x5EEDu) off = v.eye[view].xyz + v.cam_now.xyz - vec3(uintBitsToFloat(draw[nt1]), uintBitsToFloat(draw[nt1 + 1u].x));
    }
    float dp = uintBitsToFloat(e.y), scale = v.tgt.z / float(textureSize(prof, 0).x);
    vec3 dw = d;                                                          /* the direction the corner is drawn at */
    float first, ke = 1.0;
    v_dir = d; v_off = vec4(off, scale); v_dist = 4000.0;
    {   vec2 t0 = vec2(tc) * v.tgt.w * scale, hp = vec2(0.5) / vec2(textureSize(prof, 0));
        v_tb = vec4(t0 + hp, t0 + v.tgt.w * scale - hp); }
    if ((fl & 16) != 0) {
        /* PS2_RQ_CLOUDS_FARSHOWN: the shown map is already seen from the eye -- the corner unmoved, the quad at the tile's nearest cloud
           seen from the eye (PASS_RESOLVE's draw list), no parallax term */
        off = vec3(0.0);
        first = 0.9 * max(dp, 1.0);
        v_uv = uv * scale;
    } else
    if ((fl & 4) != 0) {
        /* (an empty tile: the shown sweep's origin, rq_cloud_far.comp PASS_TILES' draw[NT]; its own may be kilometres old) */
        uint nt = tpr * tpr;
        if (tiles[id].cam.w >= 60000.0 && draw[nt + 1u].y == 0x5EEDu) {
            off = v.eye[view].xyz + v.cam_now.xyz - vec3(uintBitsToFloat(draw[nt]), uintBitsToFloat(draw[nt + 1u].x));
            v_off.xyz = off;
        }
        /* FARRESOLVE: the corner unmoved; the first guess of the distance from the origin = the mean of the four texels round the corner
           (a ring 2 texels out where they are empty, else the tile's proxy) */
        vec2 u0 = uv * scale, px = vec2(2.0) / vec2(textureSize(prof, 0));
        vec3 g = gather3(u0);
        if (g.y < 0.5) g = gather3(u0 + vec2(px.x, 0.0)) + gather3(u0 - vec2(px.x, 0.0)) + gather3(u0 + vec2(0.0, px.y)) + gather3(u0 - vec2(0.0, px.y));
        v_dist = g.y > 0.5 ? g.y / g.x : max(dp, 1.0);
        first = 0.9 * max(length(d * dp - off), 1.0);                     /* (the tile's nearest cloud seen from the eye: a near bound) */
        v_uv = u0;
    } else if ((fl & 1) != 0) {
        /* FARWARP: the corner's own texels; where they are empty a ring 2 texels out; beyond that a constant (only where the content
           is transparent anyway). Every input is a function of the corner (and the one origin), so neighbours agree exactly. */
        vec2 u0 = uv * scale, px = vec2(2.0) / vec2(textureSize(prof, 0));
        vec3 g = gather3(u0);
        if (g.y < 0.5) g = gather3(u0 + vec2(px.x, 0.0)) + gather3(u0 - vec2(px.x, 0.0)) + gather3(u0 + vec2(0.0, px.y)) + gather3(u0 - vec2(0.0, px.y));
        float dist = g.y > 0.5 ? g.y / g.x : v.nuv.w;
        first = g.z > 1.0e-9 ? 1.0 / g.z : dist;
        vec3 P = d * dist - off;
        dw = normalize(P);
        ke = length(P) / dist;
        first = length(d * first - off);
        v_uv = u0;
    } else {
        vec2 u1 = dir_uv(normalize(d * dp + off)) * scale;
        vec4 g0 = textureGather(prof, u1, 0), g1 = textureGather(prof, u1, 1);   /* 1 / mean, 1 / first (0 = no cloud) */
        vec4 ok = vec4(greaterThan(g0, vec4(1.0e-6)));
        float n = ok.x + ok.y + ok.z + ok.w, dist = dp;
        first = dp;
        if (n > 0.0) {
            dist = n / dot(g0, ok);
            first = 1.0 / max(max(g1.x * ok.x, g1.y * ok.y), max(g1.z * ok.z, g1.w * ok.w));
        }
        vec2 du = (int(v.misc.w + 0.5) & 128) != 0 ? dir_uv_face(normalize(d * dist + off), d) - dir_uv(d)   /* (FOLD=2: exact, no cap) */
                                                    : dir_uv(normalize(d * dist + off)) - dir_uv(d);
        /* (rq10_artefacts, PS2_RQ_CLOUDS_FOLD, misc.w bit 6: with ONE layer every tile is marched from this frame's eye, so the shift is only
           each eye's own 3 cm -- a few texels at most even for a cloud a few metres away. A corner ON the octahedral map's outer border (the
           lower hemisphere's meridians x = 0 / z = 0) whose shifted direction crossed the meridian was read on the mirrored side of the
           border, up to 0.2 of the map away, and that tile's triangles stretched content from there: straight-edged pale and dark wedges
           along the meridian below the horizon (t13a's "square round the aircraft"). The limit: 4 texels) */
        float dlim = (int(v.misc.w + 0.5) & 64) != 0 ? 4.0 / v.tgt.z : (int(v.misc.w + 0.5) & 128) != 0 ? 1.0e9 : 0.2;
        if (abs(du.x) > dlim || abs(du.y) > dlim) du = vec2(0.0);           /* (across a fold of the mapping: no shift at this corner) */
        v_uv = (uv + du) * scale;
        ke = dist / max(length(d * dist + off), 1.0);                     /* (the content seen along d from the eye, at dist from it) */
    }
    /* SOFT: the near bound of the tile's cloud (its and its neighbours' nearest first cloud), seen from the eye, 3 % nearer */
    if ((fl & 2) != 0) first = 0.97 * length(d * dp - off);   /* (FARSHOWN: off 0, dp from the eye) */
    /* (rq8_onelayer, PS2_RQ_CLOUDS_ONE, misc.w bit 5: the one layer starts at the eye and every scene relation is the fragment's (SOFT); the
       quad sits at the near plane, behind the cockpit's depth band only. A quad at the tile's nearest cloud let the hardware test cut, along
       the quad's two triangles, what the per-pixel fade keeps: a row of teeth along the horizon, M10 f5936 (dp is also the last frame's)) */
    if ((fl & 34) == 34) first = 0.05;
    /* the corner's place on the screen */
    float W = -dot(dw, v.cam_z.xyz);                                      /* the forward distance along the view axis */
    float xc = (2.0 * v.proj.z / v.tgt.x - 1.0) * W + 2.0 * v.proj.x * dot(dw, v.cam_x.xyz) / v.tgt.x;
    float yc = (2.0 * v.proj.w / v.tgt.y - 1.0) * W - 2.0 * v.proj.y * dot(dw, v.cam_y.xyz) / v.tgt.y;
    float A = view == 0 ? v.xfx.x : v.xfx.z, B = view == 0 ? v.xfx.y : v.xfx.w;
    float vm = max(first * max(W, 0.02), 0.05);                           /* m along the view axis */
    float zq = clamp((v.depth.z / (vm * v.misc.x) / v.depth.x - v.depth.y) / v.depth.w, 0.0, 0.9955);   /* (never in the Quest cockpit's depth range, (0.9956, 1]) */
    if (v.misc.y > 0.5) zq = v.misc.y > 1.5 ? 0.0 : 0.9955;              /* [debug] PS2_RQ_CLOUDS_FARZ: 1 in front of the whole scene, 2 behind it */
    if ((fl & 256) != 0) zq = 0.0;   /* (rq13_cut, PS2_RQ_CLOUDS_CUT, misc.w bit 8: at the far plane -- GEQUAL passes only where nothing stands; every
                                        object applies the cloud in front of it itself, rq_cut.glsl) */
    gl_Position = vec4(A * xc + B * W, v.xfy.x * yc + v.xfy.y * W, zq * W, W);
    {   float dq = v.depth.x * (zq * v.depth.w + v.depth.y);
        v_qm = zq > 0.0 && dq > 0.0 ? v.depth.z / dq / v.misc.x : 1.0e9; }
    v_th = vec3((xc + W) * 0.5 * v.tgt.x, (yc + W) * 0.5 * v.tgt.y, W);
    v_eye = view;
    v_ke = ke;
}
