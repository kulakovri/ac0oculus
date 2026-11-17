#version 450
#extension GL_EXT_multiview : require
/* rq_testpat: PS2_RQ_TESTPAT (outside note 5, step A, 2026-11-10): the eye pass's last draw over its render area, an
   analytic picture of gl_FragCoord, the ground truth for every reader of the eye image (the raw copy, the decode passes,
   the feed, the compositor). R: x / W (view 0) or 1 - x / W (view 1: the layer is known from the ramp's direction);
   G: y / H; B: a checker of `cell` px, high cells 0.60 + 0.10 phase, low cells 0.05 + 0.10 phase, phase = the frame's
   sequence number mod 4 (a stale texel names the frame it came from). Values are linear; the attachment's format
   encodes them (sRGB chain 6: the raw copy holds sRGB bytes, a sampled read through an sRGB view gives them back). */
layout(push_constant) uniform P { vec4 a; } pc;   /* a.x cell px (0: no checker), a.y phase 0..3, a.z W, a.w H */
layout(location = 0) out vec4 o;
void main() {
    vec2 p = gl_FragCoord.xy;
    float cell = max(pc.a.x, 1.0);
    int cx = int(floor(p.x / cell)), cy = int(floor(p.y / cell));
    float hi = float((cx + cy) & 1);
    float r = p.x / pc.a.z;
    if (gl_ViewIndex == 1) r = 1.0 - r;
    /* cell 0 (PS2_RQ_TESTPAT=1): B carries the phase only (0.125 + 0.25 phase), robust to a filtered read; else the checker */
    float b = pc.a.x < 0.5 ? 0.125 + 0.25 * pc.a.y : mix(0.05, 0.60, hi) + 0.10 * pc.a.y;
    o = vec4(r, p.y / pc.a.w, b, 1.0);
}
