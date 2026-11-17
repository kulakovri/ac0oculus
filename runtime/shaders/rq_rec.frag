#version 450
/* rq_rec: ACZ_QUEST_RECORD's only GPU work (rq_rec.inc, runtime/src/android/ps2_qrec_android.c): a rectangle of a source image
   (the left eye's swapchain image after the eye pass, or the game's render target for screen frames) converted to NV12 for the
   encoder, written straight into a host-visible buffer. The pass has no attachments: one fragment = 4 x 2 output pixels (as
   rx_nv12.comp's invocation), so the pass is (W / 4) x (H / 2) fragments; each fragment samples its 8 pixels (bilinear) and
   writes two words of Y (rows y, y + 1) and one word of interleaved Cb Cr (the two 2 x 2 blocks' means). BT.709 limited range,
   the gamma-encoded values taken as they are (rx_nv12.comp's rule). A fragment shader, not a compute shader: the eye image is a
   subsampled image under the density map (PS2_RQ_FDM) and is read here exactly as rq_ds.frag reads it (an immutable subsampled
   sampler, in a fragment shader). Needs fragmentStoresAndAtomics (enabled with the recorder only). */
layout(set = 0, binding = 0) uniform sampler2DArray src;
layout(set = 1, binding = 0, std430) writeonly buffer Out { uint d[]; };
/* m: u0, du, v0, dv: the source rectangle (normalised, before sc); sc: PS2_RQ_DYNRES's centred rectangle of the eye image
   (1, 1 for a render target); w, h: the output size (w a multiple of 4, h of 2) */
layout(push_constant) uniform PC { vec4 m; vec2 sc; uint w, h; } pc;

const vec3 KY = vec3(0.2126, 0.7152, 0.0722);

uint y8(vec3 c) { return uint(clamp(16.0 + 219.0 * dot(c, KY) + 0.5, 0.0, 255.0)); }

vec3 px(float x, float y) {
    vec2 uv = vec2(pc.m.x + x / float(pc.w) * pc.m.y, pc.m.z + y / float(pc.h) * pc.m.w);
    uv = 0.5 + (uv - 0.5) * pc.sc;
    return textureLod(src, vec3(uv, 0.0), 0.0).rgb;
}

void main() {
    uint bx = uint(gl_FragCoord.x), by = uint(gl_FragCoord.y);
    uint wq = pc.w >> 2u;
    if (bx >= wq || by >= (pc.h >> 1u)) return;
    float x0 = float(bx * 4u) + 0.5, y0 = float(by * 2u) + 0.5;
    vec3 a[4], b[4];
    for (int i = 0; i < 4; i++) {
        a[i] = px(x0 + float(i), y0);
        b[i] = px(x0 + float(i), y0 + 1.0);
    }
    uint r0 = 0u, r1 = 0u;
    for (int i = 0; i < 4; i++) {
        r0 |= y8(a[i]) << (8u * uint(i));
        r1 |= y8(b[i]) << (8u * uint(i));
    }
    d[(by * 2u) * wq + bx] = r0;
    d[(by * 2u + 1u) * wq + bx] = r1;
    uint uv = 0u;
    for (int k = 0; k < 2; k++) {
        vec3 c = 0.25 * (a[2 * k] + a[2 * k + 1] + b[2 * k] + b[2 * k + 1]);
        float yy = dot(c, KY);
        uint u = uint(clamp(128.0 + 224.0 * (c.b - yy) / 1.8556 + 0.5, 0.0, 255.0));
        uint v = uint(clamp(128.0 + 224.0 * (c.r - yy) / 1.5748 + 0.5, 0.0, 255.0));
        uv |= (u | (v << 8u)) << (16u * uint(k));
    }
    d[pc.h * wq + by * wq + bx] = uv;
}
