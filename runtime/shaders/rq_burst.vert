#version 450
#extension GL_EXT_multiview : require
/* rq_burst.vert -- Pixy's MPBM burst as a volume in the Quest's eye pass (PS2_RQ_BURST, rq/rq_burst.inc; BUGS_RQ R-120). The PC's
   burst.vert (bst_pass, ACZ_VR_FX_BURST) moved into the one eye-sized multiview pass: no vertex buffer; vertices 0..3 = a quad facing
   the frame's centre eye that covers the sphere's outline, at the sphere's front (the pass's depth test hides it behind the cockpit, the
   terrain and the aircraft in front; no depth write); vertices 4..7 (the eye in or near the sphere) = a quad across the whole view a few
   metres ahead of the eye (behind the cockpit, in front of the world). Each eye's own position comes from its shift along the camera's x
   axis, the game's symmetric frame is mapped to the eye by the per-view crop (x' = A x + B w, as gs.vert -DRQ). The fragment stage
   builds its ray from v_pos (a world point on the drawn plane): no gl_FragCoord, no attachment read (the R-45 rule, foveation-safe). */
layout(location = 0) out vec3 v_pos;     /* the drawn plane's point, from the frame's centre eye (game units, world axes) */

layout(push_constant) uniform BP {
    vec4 cx, cy, cz;    /* the camera's axes in world (x right, y up, z back) */
    vec4 proj;          /* fx, fy, cx, cy in the game frame's pixels */
    vec4 misc;          /* 2 / frame width, 2 / frame height, depth A, depth B */
    vec4 eyes;          /* x left eye shift, y right eye shift (game units along cx), z time (s), w game units per metre */
    vec4 ctr;           /* xyz the burst's centre from the centre eye (game units, world axes), w its radius (units) */
    vec4 st;            /* x age / life (0..1), y strength, z seed, w the quad's half size at its plane (units) */
    vec4 heat;          /* x core heat, y the game's glow strength, z smoke share, w the eye's distance to the centre / radius */
    vec4 tint;          /* rgb the game's halo colour, w its share */
    vec4 fogc;          /* rgb fog colour, w the fog's amount at the burst */
    vec4 rqx;           /* the per-view crop: A0, B0, A1, B1 (x) */
    vec4 rqy;           /* Ay, By (y); z the march's steps; w the inside quad's distance (units) */
} bp;

void main() {
    int i = gl_VertexIndex;
    float sh = gl_ViewIndex == 0 ? bp.eyes.x : bp.eyes.y;
    vec2 k = vec2((i & 1) != 0 ? 1.0 : -1.0, (i & 2) != 0 ? 1.0 : -1.0);
    vec3 p;
    if (i >= 4) {                                    /* the eye in or near the sphere: a quad across the view, rqy.w ahead */
        float dq = bp.rqy.w;
        p = -bp.cz.xyz * dq + (bp.cx.xyz * k.x + bp.cy.xyz * k.y) * (4.0 * dq);
    } else {
        vec3 c = bp.ctr.xyz;
        float d = max(length(c), 1e-3);
        vec3 f = c / d;                              /* towards the centre */
        vec3 u = normalize(abs(f.y) < 0.95 ? cross(f, vec3(0.0, 1.0, 0.0)) : cross(f, vec3(1.0, 0.0, 0.0)));
        vec3 v = cross(u, f);
        float R = bp.ctr.w;
        vec3 pc = f * max(d - R, 1.0);               /* the sphere's front */
        p = pc + (u * k.x + v * k.y) * bp.st.w;
    }
    vec3 cv = vec3(dot(p, bp.cx.xyz), dot(p, bp.cy.xyz), dot(p, bp.cz.xyz));
    cv.x -= sh;
    float w = -cv.z;
    float Xc = (bp.proj.z * bp.misc.x - 1.0) * w + bp.proj.x * bp.misc.x * cv.x;
    float Yc = (bp.proj.w * bp.misc.y - 1.0) * w - bp.proj.y * bp.misc.y * cv.y;
    float Zc = bp.misc.z + bp.misc.w * w;            /* z / w = A / depth + B */
    float rA = gl_ViewIndex == 0 ? bp.rqx.x : bp.rqx.z, rB = gl_ViewIndex == 0 ? bp.rqx.y : bp.rqx.w;
    gl_Position = vec4(rA * Xc + rB * w, bp.rqy.x * Yc + bp.rqy.y * w, Zc, w);
    v_pos = p;
}
