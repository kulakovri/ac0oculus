#version 450
#extension GL_EXT_multiview : require
/* rq_zpre.vert -- PS2_RQ_ZPRE (rq_zpre.inc; plan 2 section 4.6): the depth-only pre-pass of the world's native meshes. mesh.vert -DBATCH reduced to the
   position: the same vertex fetch from the frame's mesh buffer (binding 7), the draw's record (binding 8), the block's matrices (binding 0), the same
   depth, stereo eye shift and reprojection as the eye pass's mesh.vert; no per-view crop (RQ_XF): the target is the game's symmetric frame at the feed's
   size, one layer per eye, the same mapping as every consumer of rq_feed.depth. Keep in step with mesh.vert's position code. */
struct MeshHead {
    vec2  inv_size;
    vec2  tex_size;
    uvec4 flags;
    vec4  fogcol;
    uvec4 clamp_uv;
    vec2  img_size;
    float scale;
    float point_size;
    float tex_scale;
    float pad0;
    uint  mesh_block;
    uint  mesh_pad;
    uvec4 tslot;
};
layout(std430, set = 2, binding = 7) readonly buffer MeshVerts { uvec4 mvx[]; };
layout(std430, set = 2, binding = 8) readonly buffer MeshRecs { MeshHead rec[]; };
struct Block {
    vec4  m[4];
    vec4  c[4];
    vec4  l[3];
    vec4  r[4];
    vec4  e[3];
    float fog0, fog1, zscale, zmax;
    float xoff, yoff, spec_alpha, xr_k;
    uint  mode, pass, clip, pad1;
    uint  prog;
    float xr_wmin;
    uint  xr_reproj, pad4;
    vec4  p[4];
    vec4  mp[4];
};
layout(std430, set = 2, binding = 0) readonly buffer Blocks { Block b[]; };
layout(push_constant) uniform PC {               /* (gs_push; the head comes from the record) */
    vec2  inv_size0; vec2 tex_size0; uvec4 flags0; vec4 fogcol0; uvec4 clamp_uv0; vec2 img_size0;
    float scale0, point_size0, tex_scale0, pad00;
    uint  mesh_block0, mesh_pad0;
    vec4  rp[4];
    vec4  rp2;
} pc;
out gl_PerVertex {
    vec4 gl_Position;
    float gl_ClipDistance[1];
};
/* mesh.vert's reproject */
vec3 reproject(vec2 inv, float x, float y, float iw) {
    vec3 d = vec3((x - pc.rp[0].w) / pc.rp[2].w, -(y - pc.rp[1].w) / pc.rp[3].w, -1.0);
    vec3 p = vec3(dot(pc.rp[0].xyz, d), dot(pc.rp[1].xyz, d), dot(pc.rp[2].xyz, d)) + pc.rp[3].xyz * iw;
    float pz = -p.z;
    if (pz < 1e-4) return vec3(x, y, 1.0);
    return vec3(pc.rp[0].w + pc.rp[2].w * p.x / pz, pc.rp[1].w - pc.rp[3].w * p.y / pz, 1.0 / pz);
}
void main() {
    uint rv = uint(gl_VertexIndex), vi = (rv & 0x1FFFFu) * 4u;
    uint rc = rv >> 17;
    vec2 inv = rec[rc].inv_size;
    uint block = rec[rc].mesh_block, pad = rec[rc].mesh_pad;
    Block k = b[block];
    vec4 in_pos = uintBitsToFloat(mvx[vi]);
    vec3 p = in_pos.xyz;
    vec4 s = k.m[0] * p.x + k.m[1] * p.y + k.m[2] * p.z + k.m[3];
    uint rk = k.xr_reproj == 3u ? (pc.rp2.z > 0.5 ? 2u : 1u) : k.xr_reproj;
    if ((pad & 256u) != 0u && rk == 2u) {
        vec4 s0 = k.mp[0] * p.x + k.mp[1] * p.y + k.mp[2] * p.z + k.mp[3];
        s += pc.rp2.x * (s - s0);
    }
    float w = s.w;
    float X = (s.x + (0.5 - k.xoff) * w) * inv.x - w;
    float Y = (s.y + (0.5 - k.yoff) * w) * inv.y - w;
    float zlim = min(16777215.0, k.zmax);
    float Z = min(max(s.z * k.zscale, 0.0), zlim * max(w, 0.0)) / k.zmax;
    if ((pad & 16384u) != 0u && w <= 0.0) Z = s.z * k.zscale / k.zmax;
    float eye = (pad & 4u) != 0u ? (gl_ViewIndex == 0 ? -1.0 : 1.0) : 0.0;
    if ((pad & 256u) != 0u && rk == 1u && w > 1e-3) {
        vec3 r = reproject(inv, X / w * (1.0 / inv.x) + 1.0 / inv.x, Y / w * (1.0 / inv.y) + 1.0 / inv.y, 1.0 / w);
        float w2 = w / r.z;
        X = (r.x * inv.x - 1.0) * w2;
        Y = (r.y * inv.y - 1.0) * w2;
        Z = Z / w * w2;
        w = w2;
    }
    if ((pad & 1048576u) != 0u && k.xr_k > 0.0 && w > 1e-3) {   /* (PS2_XR_LATE_POSE: as mesh.vert -DRQ; the reserved block RQ_LATE_BLOCK) */
        vec4 r0 = b[16375u].m[0], r1 = b[16375u].m[1], r2 = b[16375u].m[2], r3 = b[16375u].m[3];
        float x = X / w * (1.0 / inv.x) + 1.0 / inv.x, y = Y / w * (1.0 / inv.y) + 1.0 / inv.y;
        vec3 d = vec3((x - r0.w) / r2.w, -(y - r1.w) / r3.w, -1.0);
        vec3 q = vec3(dot(r0.xyz, d), dot(r1.xyz, d), dot(r2.xyz, d)) + r3.xyz / w;
        float pz = -q.z;
        if (pz >= 1e-4) {
            float w2 = w * pz;
            X = ((r0.w + r2.w * q.x / pz) * inv.x - 1.0) * w2;
            Y = ((r1.w - r3.w * q.y / pz) * inv.y - 1.0) * w2;
            Z = Z / w * w2;
            w = w2;
        }
    }
    if (eye != 0.0 && k.xr_k > 0.0)
        X -= eye * k.xr_k * clamp(w / k.xr_wmin, 0.0, 1.0) * inv.x;
    gl_Position = vec4(X, Y, Z, w);
    vec4 cc = k.c[0] * p.x + k.c[1] * p.y + k.c[2] * p.z + k.c[3];
    gl_ClipDistance[0] = k.clip != 0u ? cc.z + cc.w : 1.0;
}
