#version 450
#extension GL_EXT_multiview : require
/* rq_ds: the 1x feed from the eye images. Each eye image is the crop [cx0, cx1] x [cy0, cy1] of the game's symmetric frame;
   a feed pixel outside this eye's crop stays black. Depth is written as well (the sun probe reads it).
   -DNOZ (PS2_RQ_ZPRE, rq_zpre.inc): colour only (the depth comes from this frame's pre-pass); where the fade quad was
   skipped (sc.x < 0: its write of alpha 0 over the frame) the cockpit's coverage mark, alpha 16/255, stays in the alpha for the next pre-pass. */
layout(set = 0, binding = 0) uniform sampler2DArray col;
layout(set = 0, binding = 1) uniform sampler2DArray dep;
layout(push_constant) uniform PC { vec4 cx; vec2 cy; vec2 sc; } pc;   /* (sc: PS2_RQ_DYNRES: the frame's centred rectangle of the image) */
layout(location = 0) out vec4 o;
/* (PS2_RQ_ZPRE_VIEW, a debug view in the feed: 6 decades of the depth value as grey, the cockpit's band red) */
vec3 zview(float z) { return z >= 0.99 ? vec3(1.0, 0.2, 0.2) : vec3(z <= 0.0 ? 0.0 : clamp(1.0 + log(z) / log(10.0) / 6.0, 0.0, 1.0)); }
void main() {
    float x0 = gl_ViewIndex == 0 ? pc.cx.x : pc.cx.z, x1 = gl_ViewIndex == 0 ? pc.cx.y : pc.cx.w;
    vec2 uv = vec2((gl_FragCoord.x - x0) / (x1 - x0), (gl_FragCoord.y - pc.cy.x) / (pc.cy.y - pc.cy.x));
#ifdef NOZ
    if (pc.sc.y < 0.0) {                                               /* (the pre-pass's depth; the mark goes on, so the next pre-pass's coverage fill still has it) */
        o = vec4(zview(texture(dep, vec3(gl_FragCoord.xy / vec2(textureSize(dep, 0).xy), gl_ViewIndex)).r), 0.0);
        if (uv.x >= 0.0 && uv.y >= 0.0 && uv.x <= 1.0 && uv.y <= 1.0) {
            float a = textureLod(col, vec3(0.5 + (uv - 0.5) * abs(pc.sc), gl_ViewIndex), 0.0).a;
            o.a = abs(a - 16.0 / 255.0) < 5.5 / 255.0 ? 16.0 / 255.0 : 0.0;
        }
        return;
    }
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) { o = vec4(0.0); return; }
#else
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) { o = vec4(0.0); gl_FragDepth = 0.0; return; }
#endif
    uv = 0.5 + (uv - 0.5) * abs(pc.sc);
    o = textureLod(col, vec3(uv, gl_ViewIndex), 0.0);
#ifdef NOZ
    if (pc.sc.x < 0.0) o.a = abs(o.a - 16.0 / 255.0) < 5.5 / 255.0 ? 16.0 / 255.0 : 0.0;   /* (the skipped fade quad's alpha 0, but the mark) */
#else
    gl_FragDepth = texture(dep, vec3(uv, gl_ViewIndex)).r;
    if (pc.sc.y < 0.0) { o = vec4(zview(gl_FragDepth), 1.0); return; }   /* (the eye pass's resolved depth) */
    if (pc.sc.x < 0.0) o.a = 0.0;    /* (PS2_RQ_HUD=batch: the skipped fade quad's write of alpha 0 over the frame) */
#endif
}
