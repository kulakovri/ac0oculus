#version 450
#extension GL_EXT_multiview : require
/* The Quest canopy's sunlit dust and water-spot rings as sprites (rq_cockpit.inc rqgx_specks, ACZ_VR_RQ_GLASS_WEAR bit 1, R-57):
   cockpit.frag glass_light's gx_specks -- dust specks and pits (0.05..0.3 mm) that scatter the sun that falls on the glass in
   every direction (bug #142's cue that tells two eyes there is a surface: faint points at the glass's depth), stronger
   within 23 deg of the sun's image, and dried water-spot rings -- not searched per pixel over the whole canopy but placed
   once at load (about 430 a square metre, as the PC's cells) and drawn as quads of at least 1.2 pixels with their energy
   kept (true area / drawn area), lit per vertex for this eye. 6 vertices a speck, no vertex buffer. */
struct Speck { vec4 p; vec4 n; vec4 k; };        /* p: xyz (model), mesh id; n: normal xyz, -; k: radius (m), ring width (m; 0: a speck), seed, - */
layout(std430, set = 3, binding = 2) readonly buffer Specks { Speck sp[]; };
struct AnimRow { vec4 m[3]; vec4 uvv; };
layout(std430, set = 2, binding = 1) readonly buffer Anims { AnimRow anim[]; };
layout(std140, set = 2, binding = 2) uniform Fr {
    vec4 rot[3]; vec4 head; vec4 proj; vec4 misc; vec4 xfx; vec4 xfy; vec4 l[3]; vec4 r[4]; vec4 dome[4]; vec4 cloud;
    vec4 su, sv, sd; vec4 pack; vec4 tub; vec4 flp[4]; vec4 flc[4]; vec4 night; vec4 mir[3]; vec4 mirg; vec4 eyesize;
    vec4 wa, wb, wc, wd, we, wf; vec4 uvk[2]; vec4 wg, wh;
    vec4 gx[8];
} F;
layout(location = 0) out mediump vec3 v_col;    /* the speck's light x its energy share */
layout(location = 1) out mediump vec2 v_c;       /* the quad's corner (speck: in units of the drawn radius; ring: of R) */
layout(location = 2) out mediump vec4 v_r;       /* ring: x drawn half-width / R, y true width / drawn width, zw Q along the ring's axes; speck: x 0 */

void main() {
    int i = gl_VertexIndex / 6, k = gl_VertexIndex - 6 * (gl_VertexIndex / 6);
    vec2 cn = vec2(k == 1 || k == 2 || k == 4 ? 1.0 : -1.0, k == 2 || k == 4 || k == 5 ? 1.0 : -1.0);
    Speck s = sp[i];
    AnimRow A = anim[int(s.p.w + 0.5)];
    vec3 S = F.gx[0].xyz, g1 = F.gx[1].rgb;
    float dk = F.gx[2].z, sstr = F.gx[0].w;
    gl_Position = vec4(-4.0, -4.0, 0.5, 1.0); v_col = vec3(0.0); v_c = vec2(0.0); v_r = vec4(0.0);
    if (A.uvv.z == 0.0 || g1.r + g1.g + g1.b <= 0.0) return;
    vec3 pa = vec3(dot(A.m[0].xyz, s.p.xyz) + A.m[0].w, dot(A.m[1].xyz, s.p.xyz) + A.m[1].w, dot(A.m[2].xyz, s.p.xyz) + A.m[2].w);
    vec3 n = normalize(vec3(dot(A.m[0].xyz, s.n.xyz), dot(A.m[1].xyz, s.n.xyz), dot(A.m[2].xyz, s.n.xyz)));
    float eye = gl_ViewIndex == 0 ? -1.0 : 1.0, ek = F.gx[3].x > 0.5 ? 0.0 : eye;
    vec3 E = F.head.xyz + ek * F.head.w * F.rot[0].xyz;
    vec3 D = normalize(pa - E);
    if (dot(n, D) > 0.0) n = -n;                 /* (n faces the eye) */
    float behind = -dot(S, n);
    if (abs(behind) <= 0.02) return;
    vec3 Q = S - D;
    float th = 2.0 * asin(min(length(Q) * 0.5, 1.0));
    float fade = 1.0 - smoothstep(0.3, 0.4, th);
    float ld = smoothstep(0.02, 0.12, abs(behind)) * (behind > 0.0 ? 1.0 : 0.6);
    float lit = smoothstep(0.0, 0.1, behind);
    /* glass_light: the dust at every angle (dk) + the near-sun specks (25 x the forward lobe) */
    float kd = dk * ld, kn = sstr * lit * 25.0 * exp(-th * 3.5) * fade;
    vec3 v = pa - F.head.xyz;
    vec3 c = vec3(dot(F.rot[0].xyz, v), dot(F.rot[1].xyz, v), dot(F.rot[2].xyz, v));
    c.x -= eye * F.head.w;
    float wz = -c.z;
    if (wz < 0.05) return;
    float px = wz / F.gx[4].w;                   /* one eye pixel at the speck (m) */
    float rt = s.k.x, wr = s.k.y;
    vec3 off;
    if (wr <= 0.0) {                             /* a speck: a disc facing the eye, drawn at least 1.2 px across */
        float a = max(1.2 * px, rt);
        float en = (rt * rt) / (a * a) * 2.0;
        v_col = g1 * (kd * en);                  /* (the PC's min(dk ld sd, 0.25): per pixel in the fragment stage) */
        v_col += g1 * (kn * en);
        float pk = max(v_col.r, max(v_col.g, v_col.b));
        if (pk < 0.5 / 255.0) return;
        if (pk > 0.25) {                         /* (brighter than the PC's firefly cap: spread wider with its energy kept, as the
                                                    PC's coarser cockpit pass does -- the halo of lit dust near the sun) */
            float sp = min(sqrt(pk / 0.25), 3.0);
            a *= sp; v_col /= sp * sp;
        }
        vec3 ux = normalize(cross(D, abs(D.y) < 0.95 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0))), uy = cross(ux, D);
        off = (ux * cn.x + uy * cn.y) * a;
        v_c = cn;
    } else {                                     /* a dried water-spot ring on the glass's plane (lit more where its tangent is square to Q) */
        float a = max(1.3 * px, wr);
        float hw = (a + 0.5 * px) / rt;
        v_col = g1 * ((kd + kn) * 0.35 * (wr / a));
        if (max(v_col.r, max(v_col.g, v_col.b)) < 0.5 / 255.0) return;
        vec3 e1 = normalize(cross(n, abs(n.y) < 0.95 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0))), e2 = cross(n, e1);
        float ext = 1.0 + hw;
        off = (e1 * cn.x + e2 * cn.y) * (rt * ext);
        v_c = cn * ext;
        v_r = vec4(hw, 1.0, dot(Q, e1), dot(Q, e2));
    }
    vec3 pq = pa + off;
    v = pq - F.head.xyz;
    c = vec3(dot(F.rot[0].xyz, v), dot(F.rot[1].xyz, v), dot(F.rot[2].xyz, v));
    c.x -= eye * F.head.w;
    float w = -c.z;
    float X = (F.proj.z * F.misc.x - 1.0) * w + F.proj.x * F.misc.x * c.x;
    float Y = (F.proj.w * F.misc.y - 1.0) * w - F.proj.y * F.misc.y * c.y;
    float rqA = gl_ViewIndex == 0 ? F.xfx.x : F.xfx.z, rqB = gl_ViewIndex == 0 ? F.xfx.y : F.xfx.w;
    gl_Position = vec4(rqA * X + rqB * w, F.xfy.x * Y + F.xfy.y * w, F.misc.z * w + F.misc.w, w);
}
