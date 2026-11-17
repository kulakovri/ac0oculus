#version 450
/* PS2_XR_QUAD_TBLEND (rq19_sparkle, ps2_video.c rq/rq_qtb.inc): one triangle over the blend image, its 0..1 coordinates */
layout(location = 0) out vec2 v_uv;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
