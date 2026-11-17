#version 450
#extension GL_EXT_multiview : require
/* rq_clouds_near.vert (plan 4.2; rq_clouds.inc): one cell (8 x 8 near texels) of the near buffer as a quad for both views; a cell the
   mask pass (rq_cloud_nearmask.comp) found empty, or of the other pipeline's class, collapses. -DEDGE: the cells where the scene
   lies inside the near range (their fragment shader reads the depth and picks the near texel). */
layout(set = 1, binding = 2, std140) uniform V {
    vec4 cam_x, cam_y, cam_z, proj, tgt, xfx, xfy, eye[2], cam_now, depth, misc;
    vec4 ncell;                   /* cells per row, rows, target pixels per cell */
    vec4 nuv;                     /* the near buffer's uv of a target pixel */
} v;
layout(set = 1, binding = 4, std430) readonly buffer F { uint f[]; } fl;
layout(location = 0) out vec3 v_nuv;
#ifdef EDGE
layout(location = 1) out vec2 v_tgt;
#endif
void main() {
    const vec2 C[6] = vec2[6](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(1, 1), vec2(0, 1));
    int cols = int(v.ncell.x), rows = int(v.ncell.y), cx = gl_InstanceIndex % cols, cy = gl_InstanceIndex / cols;
    uint c = fl.f[cy * cols + cx] | fl.f[(rows + cy) * cols + cx];       /* the two eyes' classes */
#ifdef EDGE
    bool on = (c & 2u) != 0u;
#else
    bool on = (c & 3u) == 1u;
#endif
    if (!on) { gl_Position = vec4(2.0, 2.0, 0.0, 1.0); v_nuv = vec3(0.0); return; }
    vec2 p = min((vec2(cx, cy) + C[gl_VertexIndex]) * v.ncell.z, v.tgt.xy);
    vec2 n = p / v.tgt.xy * 2.0 - 1.0;
    int view = int(gl_ViewIndex);
    float A = view == 0 ? v.xfx.x : v.xfx.z, B = view == 0 ? v.xfx.y : v.xfx.w;
    gl_Position = vec4(A * n.x + B, v.xfy.x * n.y + v.xfy.y, 0.9955, 1.0);   /* (behind the Quest cockpit, in front of the world) */
    v_nuv = vec3(p * v.nuv.xy, float(view));
#ifdef EDGE
    v_tgt = p;
#endif
}
