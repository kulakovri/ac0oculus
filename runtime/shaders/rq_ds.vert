#version 450
/* rq_ds: R1 of the native Quest renderer. One triangle over the target: the 1x feed (slot 0 of the previous frame, read by
   the game's own reads) drawn from the eye images and their depth (rq_ds.frag). */
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
