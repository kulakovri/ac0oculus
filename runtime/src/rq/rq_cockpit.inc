/* rq_cockpit.inc -- the Quest cockpit material (PLAN 4.1, agent cockpit of the frame recipe, ACZ_VR_COCKPIT_MATERIAL=quest).
   Included by ps2_video.c after rq_video.inc (PS2_RQ_SUPPORT only). The 3D cockpit is drawn inside the eye-sized pass, at the
   eye's size and 4x MSAA, with a material whose whole lighting is per vertex (rq_cockpit.vert: cockpit.frag has no normal map,
   no ORM, no emissive and one UV set, so every term but the shadow is a function of the interpolated normal) and whose
   fragment stage is texture(albedo) * (base + key * shadow) + spec * shadow with one hardware-PCF shadow read.
   - One vertex / index range per pipeline kind, built at load: the static meshes merged (rqc_vtx: 16 bytes, positions and
     UVs quantised per mesh), every vertex carries its mesh id; the id indexes a table of material rows (quantisation scale,
     colour, specular, texture slot) and a per-frame table of matrix rows (animated parts: needles, stick, throttle, drums,
     the hidden parts collapse). Kinds: solid, cut (alpha tested), the canopy and instrument glass; the screens draw from the
     old 36-byte buffer.
   - Depth: the cockpit in the top of the depth range (R-5): z / w = zA + zB / w, 1 at near = RQC_NEAR, the far end above
     0.9956: above the world (below 0.99) and the HUD glass mark (HG_MARK 0.995), so the HUD's depth-bounds keys work.
   - Shadow map on change only: the merged casters into the existing 1024 map when the sun has moved over 0.5 deg in the
     cockpit frame (or a stick / throttle part moved), else the last map with its own matrices.
   ACZ_VR_COCKPIT_MATERIAL=quest|pc (pc, the default, keeps the old shader in its own pass: the comparison). */

#define RQC_NEAR 0.06f              /* the near plane of the cockpit's depth (m) */
#define RQC_S 0.0044f               /* the depth range of the model: (1 - RQC_S, 1] */
#define RQC_TEX_MAX 1024u           /* the texture table (descriptors; unused ones are never read) */

typedef struct { u16 q[3]; u16 id; s8 n[3]; s8 ao; u16 uv[2]; } rqc_vtx;    /* 16 bytes */
typedef struct { float qp[4], qt[4], col[4], sp[4]; } rqc_row;              /* (rq_cockpit.vert Row) */
typedef struct { float m[12]; float uv[2]; float vis; float pad; } rqc_anim;
typedef struct { float rot[12], head[4], proj[4], misc[4], xfx[4], xfy[4], l[12], r[16], dome[16], cloud[4], su[4], sv[4], sd[4], pack[4], tub[4], flp[16], flc[16],
                 night[4], mir[12], mirg[4], eyesize[4], wa[4], wb[4], wc[4], wd[4], we[4], wf[4], uvk[8], wg[4], wh[4],
                 gx[32]; } rqc_frame;   /* std140: rq_cockpit.vert Fr (gx: the readable glass's inputs, rq_glass.inc rqgx_frame) */

#define RQC_CHUNKS 12u              /* the solid triangles by azimuth around the design eye: twelve 30-degree sectors */
typedef struct rqc_gmesh { u32 mesh, ifirst, icount; } rqc_gmesh;
typedef struct rqc_model {
    int ok;
    u32 nvtx, n_solid, n_cut, nmesh;              /* indices: solid [0, n_solid), cut [n_solid, n_solid + n_cut) */
    VkBuffer vb, ib; VkDeviceMemory vbm, ibm;
    VkBuffer rows; VkDeviceMemory rowsm; void *rowsp;
    rqc_row *rowsh;                               /* PS2_RQ_MAP_HOST: the rows' host copy (the CPU's readers) */
    VkBuffer animb[FRAMES_IN_FLIGHT], fb[FRAMES_IN_FLIGHT], gib[FRAMES_IN_FLIGHT];
    VkDeviceMemory animm[FRAMES_IN_FLIGHT], fm[FRAMES_IN_FLIGHT], gim[FRAMES_IN_FLIGHT];
    void *animp[FRAMES_IN_FLIGHT], *fp[FRAMES_IN_FLIGHT], *gip[FRAMES_IN_FLIGHT];
    VkDescriptorPool pool;
    VkDescriptorSet texset, fset[FRAMES_IN_FLIGHT];
    u32 *anm; u32 nan;                            /* meshes whose anim row changes (animated or screens) */
    /* glass: merged vertices (first glass vertex: gvx0) */
    rqc_gmesh *gm; u32 ng, ngtri;
    u32 *gix; float *gv;                          /* the glass's triangles (merged vertex ids) and their positions + normals: the CPU's by-need test */
    VkBuffer gvb; VkDeviceMemory gvbm;            /* per glass vertex: metres along the glass (across, along the flow): the lean wet canopy */
    u32 gvx0, ngv;
    u32 tris_solid, tris_cut;
    /* the solid range, split: list 0 the triangles back-face culling may drop (closed components the head cannot be inside), list 1 the rest; each
       list RQC_CHUNKS sectors of azimuth round the design eye (the view is a cone from the head, so sectors are what a turn of the head rules out; straight
       ahead is the middle of the list: looking ahead the sectors in view are one run, one draw); then the animated parts (always drawn, no culling) */
    struct { u32 first, count; float c[3], h[3]; } ch[3][RQC_CHUNKS + 1u];   /* (the last: triangles within 30 cm of the design eye, longer than 40 cm, or over / under the head: a box round them says nothing, always drawn) */
    u32 anim_first, anim_count, tris_cull;
    u32 near_first, near_count;                   /* the solid parts round the HUD's combiner, once more at the end of the index buffer: their depth back before the glass (rqc_depth_back) */
    VkBuffer gxsb; VkDeviceMemory gxsm; void *gxsp; u32 ngxs;   /* the readable glass (rq_glass.inc): the dust specks and rings */
    u8 *gxst;                                     /* ... per glass triangle: 1 drawn last frame, 2 in the near-sun list (the need test's hysteresis) */
    float *gxtc;                                  /* ... per glass triangle: centre, unit normal (the need test, made once) */
    u8 *gxrl;                                     /* ... per glass triangle: the brightest interior its reflection can show from the head's range (x 255; rqgx_rl_make) */
    u32 sm_first, sm_count; int sm_lod;           /* the sun shadow's casters from another LOD level, after those (ACZ_VR_RQ_COCKPIT_SM_LOD; count 0 = the drawn level's solid + cut) */
} rqc_model;
/* ACZ_VR_RQ_COCKPIT_SM_LOD (rq3_cockpit 2026-11-10, R-61): the mesh LOD level the sun shadow map's casters are drawn at. 0 (default) = the
   level the model draws, as before; N = level N of the side file when it is coarser than the drawn one; -1 = the side file's last
   level. The map is redrawn in 37-66 % of the frames (the user's six flights 2026-11-10: M2 Rafale 37 %, M9 Su-47 41 %, M4 F-15C 54 %,
   M6 MiG-29A 61 %, M3 F-5E 66 %: the sun's direction in the cockpit frame moves over 0.5 deg whenever the aircraft turns), with every
   solid and cut triangle of the drawn level. The casters' vertices are added to the model's buffer (their own copies) */
static int rqc_smlod_cfg(void) {
    static int v = -2;
    if (v == -2) { const char *e = getenv("ACZ_VR_RQ_COCKPIT_SM_LOD"); v = e && *e ? atoi(e) : 0; if (v < -1) v = 0; }
    return v;
}

/* shared by every model: shaders, pipelines of the eye pass, the shadow pass's pipeline */
static struct {
    VkShaderModule vs, fs, fsmp, gvs, gfs, ovs, smvs, wvs, wfs, lwvs, lwfs;
    VkDescriptorSetLayout texl, fl;
    VkPipelineLayout layout;
    VkSampler smp;
    VkRenderPass rp_for;
    VkPipeline solid[2], cut[2], glass, sm, wet;
    VkPipeline solid_c[2];                        /* the solid list that may be back-face culled (ACZ_VR_RQ_COCKPIT_CULL bit 1) */
    VkPipeline dback;                             /* depth only: rqc_depth_back */
    VkPipeline fleck;                             /* lean2: the debris flecks' decals (rqw_flecks) */
    VkShaderModule ffs;
    VkPipeline lwet[8];                           /* the lean wet canopy, by the active terms: bit 0 drops, 1 mist + film, 2 ice */
    VkPipelineLayout lwlayout;                    /* its sets: 0 texl, 1 the world copy, 2 the frame, 3 the baked textures */
    VkPipelineLayout wlayout;                     /* the wet canopy: sets 0 texl, 1 the world copy, 2 the frame; push: cpit_push */
    int sm_on, tried;
    /* the shadow map's change test (rqc_shadow_keep) */
    float sm_L[3], sm_su[4], sm_sv[4], sm_sd[4]; int sm_have; u32 sm_serial, sm_age; float *sm_big; u32 sm_nbig; u32 *sm_bigi;
    /* statistics for the frame */
    u64 n_frames, n_sm, n_sm_kept, n_gtris, n_gfr, n_wet, n_oldpass, n_wtris, tris_seen, tris_all;
    float dbg_r0, dbg_r1, dbg_r2, dbg_xl, dbg_xr, dbg_yb, dbg_yt; u8 dbg_in[3][RQC_CHUNKS + 1u];
    int mk_due; vk_frame *mk_fr;                  /* the combiner's mark, due at the world's end (rq_cockpit_solid with the cockpit first) */
    u32 fr_draws, fr_pipes;                       /* (this frame: the gate of at most 12 draws and 6 pipelines) */
    float depth_a, depth_b;
    rqc_frame fh[FRAMES_IN_FLIGHT];               /* PS2_RQ_MAP_HOST: the frame block in host memory (written, read, then copied to fp[slot]) */
    u8 fh_on[FRAMES_IN_FLIGHT];                   /* ... this slot's last rqc_frame_begin kept it there */
} rqc;
/* the frame block this slot's readers and later writers use: the host copy (PS2_RQ_MAP_HOST) or the mapped one */
static rqc_frame *rqc_F(const rqc_model *q, u32 slot) { return rqc.fh_on[slot] ? &rqc.fh[slot] : (rqc_frame *)q->fp[slot]; }
/* PS2_RQ_MAP_HOST: a host scratch for one index list of up to ntri triangles (built there, then one memcpy into gip); NULL: no memory */
static u32 *rqc_host_idx(u32 ntri) {
    static u32 *b; static u32 cap;
    if (cap < ntri) { u32 *t = (u32 *)realloc(b, (size_t)ntri * 3u * sizeof(u32)); if (!t) return NULL; b = t; cap = ntri; }
    return b ? b : (u32 *)(b = (u32 *)malloc(12u));
}

static char rqc_shader_base[1024];
/* the readable glass and the cracks in the eye pass (rq_glass.inc, R-57 / R-58) */
static void rqgx_load_shaders(const char *base);
static void rqgx_pipes(void);
static void rqgx_frame(rqc_frame *F, vk_frame *fr, int lit);
static void rqgx_wear_bake(vk_frame *st);
static void rqgx_specks_build(rqc_model *q, const cpit_model *m);
static int rqgx_on(void);
/* Self-lit display pages (D.1 of the second flight round, BUGS_RQ R-85; the Quest material only). The user's M11 flight in the F-22A:
   "a night mission, and the lighting: these things should probably be lit" -- a black panel with white squares. The PC frame at the
   same field is as dark (NOTES_RQ R-85): the converter made the display pages matte (tools/cockpit/build_f22a.sh: "FlightGear lights
   them by emission"), so at night they get only the night instrument lighting's flood, 0.10 x their albedo, on a panel whose material
   is 0.2 grey. ACZ_VR_RQ_COCKPIT_SELFLIT_MESH ("file=name,name;file=name", cpit_list_match's form; "0" none) names the pages; at night
   the Quest material adds their own texture x ACZ_VR_RQ_COCKPIT_SELFLIT (the strength) x the night level (rq_cockpit.vert): the page's
   picture glows, dimmed as a display set for the night; by day nothing changes. Default list: the pages with a picture that no HUD
   block covers -- the F-22A's static MFD pages (screen6: ENG, PRF, FUEL, SMS) and standby screens (StbyAIface, hsi, hsibg), the Rafale
   M's five page pictures, the F/A-18E's right DDI, the ADFX-01's panoramic display. The loader marks them (ps2_video.c cpit_load_one,
   ps2_vk.h PS2_RQ_SELFLIT_HOOK on its existing line: the PC object keeps its line numbers). */
#define CPIT_F_SELFLIT (1u << 30)                   /* mesh flag set at load (the Quest build): a display page lit from within */
static const char *rqsl_list(void) {
    static int init; static char list[1024];
    if (!init) {
        init = 1;
        const char *e = getenv("ACZ_VR_RQ_COCKPIT_SELFLIT_MESH");
        snprintf(list, sizeof list, "%s", e && strcmp(e, "1") ? (strcmp(e, "0") ? e : "")
                 : "f22a.cpit=screen6,StbyAIface,hsi,hsibg"
);
    }
    return list;
}
/* the loader's hook (ps2_vk.h PS2_RQ_SELFLIT_HOOK): a solid mesh named in the list for its model file */
void ps2_rq_selflit_mark(void *mesh, const char *path, const char *name32, unsigned idx) {
    cpit_mesh *m = (cpit_mesh *)mesh;
    if (!m || (m->flags & 1u) || !*rqsl_list()) return;
    char nm[33]; memcpy(nm, name32, 32); nm[32] = '\0';
    if (cpit_list_match(rqsl_list(), path, nm) != 1) return;
    m->flags |= CPIT_F_SELFLIT;
    ps2_log("cockpit: %s: \"%s\" (mesh %u) is a display page lit from within at night (ACZ_VR_RQ_COCKPIT_SELFLIT_MESH)", path, nm, idx);
}
/* The cockpit's dark screens put to use (rq21_screens 2026-11-16, BUGS_RQ R-280; the user's flight of 2026-11-16 10:43 on t13h, F-1, the
   fourth time they ask for the front screen: "This cockpit has two screens that are not used ... The screen in front: better a map there.
   Both the global map and the more local one ... And in the panel where the map is now, some interface element, for example a panel for
   commanding the allies"). ACZ_VR_MFD_FRONT (1, the Quest default; 0 = every model as built: the present layout) with
   ACZ_VR_MFD_FRONT_MODEL ("file=part:id,part:id;file=...": the model file's base name, a part's name -- a trailing * matches a prefix --
   and the screen id ac2cpit.py --screen would have given it, 0 = none; "0" = no model): at load, the named parts of a listed model
   become display screens -- the id in the mesh flags (bits 8-15) and, for a part that was no screen,
   texture coordinates 0..1 across its face with u to the pilot's right and v down (planar, from the area-weighted normal towards the
   eye: ac2cpit.py's planar mapping) -- so ps2_video.c's screen code (mfd_*, ACZ_VR_MFD, ACZ_VR_MFD_DUAL, the wingman display) takes
   them as if built in. Default, the Mirage 2000D (aircraft 22; tools/cockpit/build_m2000d.sh): the head-level display under the HUD
   (vtm_ac_object, FlightGear's canvas quad, 11.5 x 7.2 cm, el -17 deg at 0.45 m) = 3, the big map; the lower centre display
   (center_mfd.blackbkdOFF: the face FlightGear shows with the avionics off, the one our selects keep -- blackbkd's is false for good;
   14.7 cm square, el -35 deg at 0.75 m) = 6, the small radar beside it, both live (ACZ_VR_MFD_DUAL); the left display
   (left_mfd.canvasCadre, built as 1) = 5, the wingman commands; the right one stays 2, the weapon block. The two-seat model's rear
   cockpit (behind the seat, never seen from it) carries the same parts and takes the same ids, as the build marks both seats: no
   screen 1 is left in the model. Runs on the loader's image after the LOD side file's check value (cpit_lod_prehash), so
   the Quest LOD set still fits the model; nothing without PS2_RQ_SUPPORT (the PC objects unchanged). */
/* The user's choice 2026-11-16 ~14:40 from the crop present | A | B ("variant B is better"): layout B, the radar at eye level
   under the HUD and the big map on the larger lower display (A was m2000d.cpit=vtm_ac_object:3,center_mfd.blackbkdOFF:6,left_mfd.canvasCadre:5) */
#define RQMS_DEFAULT "m2000d.cpit=vtm_ac_object:6,center_mfd.blackbkdOFF:3,left_mfd.canvasCadre:5"
static const char *rqms_list(void) {
    static int init; static char list[1024];
    if (!init) {
        init = 1;
        const char *on = getenv("ACZ_VR_MFD_FRONT"), *e = getenv("ACZ_VR_MFD_FRONT_MODEL");
        if (on && *on == '0') list[0] = '\0';
        else snprintf(list, sizeof list, "%s", e && *e ? (strcmp(e, "0") ? e : "") : RQMS_DEFAULT);
    }
    return list;
}
/* the screen id the list gives part `name` of the model at `path`: 0..255, -1 = not listed */
static int rqms_id(const char *cfg, const char *path, const char *name) {
    const char *base = path, *s;
    for (s = path; *s; s++) if (*s == '/' || *s == '\\') base = s + 1;
    for (const char *p = cfg; *p; ) {
        const char *end = strchr(p, ';'), *eq = strchr(p, '=');
        size_t l = end ? (size_t)(end - p) : strlen(p);
        if (eq && eq < p + l && (size_t)(eq - p) == strlen(base) && !strncasecmp(p, base, (size_t)(eq - p))) {
            for (const char *q = eq + 1; q < p + l; ) {
                const char *c = (const char *)memchr(q, ',', (size_t)(p + l - q));
                size_t k = c ? (size_t)(c - q) : (size_t)(p + l - q);
                const char *col = (const char *)memchr(q, ':', k);
                if (col) {
                    size_t k1 = (size_t)(col - q);
                    int star = k1 > 0 && q[k1 - 1] == '*';
                    size_t kk = star ? k1 - 1 : k1;
                    if (k1 && strlen(name) >= kk && !strncasecmp(name, q, kk) && (star || name[kk] == '\0')) {
                        int id = atoi(col + 1);
                        return id < 0 ? 0 : id > 255 ? 255 : id;
                    }
                }
                q += k; if (q < p + l && *q == ',') q++;
            }
        }
        p += l; if (*p == ';') p++;
    }
    return -1;
}
/* the loader's hook (ps2_video.c cpit_load_one, on the line of cpit_lod_prehash): the listed parts of this model become screens */
void ps2_rq_screens_assign(unsigned char *d, size_t n, const char *path) {
    const char *cfg = rqms_list();
    size_t t0, a0, m0, e0; int hands;
    if (!*cfg || !path || !cpit_sections(d, n, &t0, &a0, &m0, &e0, &hands)) return;
    u32 nmesh; memcpy(&nmesh, d + 12, 4);
    size_t p = m0;
    for (u32 i = 0; i < nmesh; i++) {
        u32 ntri, fl; memcpy(&ntri, d + p + CPIT_MESH_HDR - 4, 4); memcpy(&fl, d + p + 52, 4);
        char nm[33]; memcpy(nm, d + p, 32); nm[32] = '\0';
        u8 *vx = d + p + CPIT_MESH_HDR;
        size_t nv = (size_t)ntri * 3u;
        p += CPIT_MESH_HDR + nv * CPIT_VTX;
        int id = rqms_id(cfg, path, nm);
        if (id < 0 || !ntri || (fl & 1u)) continue;     /* (not listed, empty, glass) */
        double c[3] = { 0.0, 0.0, 0.0 };
        for (size_t k = 0; k < nv; k++) { float q[3]; memcpy(q, vx + k * CPIT_VTX, 12); for (int j = 0; j < 3; j++) c[j] += q[j]; }
        for (int j = 0; j < 3; j++) c[j] /= (double)nv;
        u32 old = (fl >> 8) & 255u;
        fl = (fl & ~0xFF00u) | ((u32)id << 8);
        memcpy(vx - CPIT_MESH_HDR + 52, &fl, 4);
        double du = 0.0, dv = 0.0;
        if (id && !old) {                               /* texture coordinates 0..1 across the face (ac2cpit.py's planar mapping) */
            double nn[3] = { 0.0, 0.0, 0.0 };
            for (size_t t = 0; t < (size_t)ntri; t++) {
                float q[3][3];
                for (int j = 0; j < 3; j++) memcpy(q[j], vx + (t * 3u + (size_t)j) * CPIT_VTX, 12);
                double e1[3], e2[3], cr[3];
                for (int j = 0; j < 3; j++) { e1[j] = (double)q[1][j] - q[0][j]; e2[j] = (double)q[2][j] - q[0][j]; }
                cr[0] = e1[1] * e2[2] - e1[2] * e2[1]; cr[1] = e1[2] * e2[0] - e1[0] * e2[2]; cr[2] = e1[0] * e2[1] - e1[1] * e2[0];
                double sg = -(cr[0] * q[0][0] + cr[1] * q[0][1] + cr[2] * q[0][2]) >= 0.0 ? 1.0 : -1.0;   /* (towards the eye) */
                for (int j = 0; j < 3; j++) nn[j] += sg * cr[j];
            }
            double ln = sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
            if (!(ln > 0.0)) {
                ps2_log("cockpit: %s: \"%s\" (mesh %u) has no face: no screen (ACZ_VR_MFD_FRONT_MODEL)", path, nm, i);
                fl &= ~0xFF00u; memcpy(vx - CPIT_MESH_HDR + 52, &fl, 4);
                continue;
            }
            for (int j = 0; j < 3; j++) nn[j] /= ln;
            double ua[3] = { 1.0 - nn[0] * nn[0], -nn[0] * nn[1], -nn[0] * nn[2] };     /* the pilot's right, in the face */
            double la = sqrt(ua[0] * ua[0] + ua[1] * ua[1] + ua[2] * ua[2]);
            if (!(la > 1e-6)) { ua[0] = 1.0; ua[1] = ua[2] = 0.0; la = 1.0; }
            for (int j = 0; j < 3; j++) ua[j] /= la;
            double va[3] = { nn[1] * ua[2] - nn[2] * ua[1], nn[2] * ua[0] - nn[0] * ua[2], nn[0] * ua[1] - nn[1] * ua[0] };
            if (va[1] > 0.0) for (int j = 0; j < 3; j++) va[j] = -va[j];                 /* v down the screen */
            double u0 = 1e30, u1 = -1e30, v0 = 1e30, v1 = -1e30;
            for (size_t k = 0; k < nv; k++) {
                float q[3]; memcpy(q, vx + k * CPIT_VTX, 12);
                double u = q[0] * ua[0] + q[1] * ua[1] + q[2] * ua[2], v = q[0] * va[0] + q[1] * va[1] + q[2] * va[2];
                if (u < u0) u0 = u;
                if (u > u1) u1 = u;
                if (v < v0) v0 = v;
                if (v > v1) v1 = v;
            }
            du = u1 - u0; dv = v1 - v0;
            for (size_t k = 0; k < nv; k++) {
                float q[3]; memcpy(q, vx + k * CPIT_VTX, 12);
                double u = q[0] * ua[0] + q[1] * ua[1] + q[2] * ua[2], v = q[0] * va[0] + q[1] * va[1] + q[2] * va[2];
                float uv[2] = { du > 0.0 ? (float)((u - u0) / du) : 0.5f, dv > 0.0 ? (float)((v - v0) / dv) : 0.5f };
                memcpy(vx + k * CPIT_VTX + 24, uv, 8);
            }
        }
        double r = sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
        ps2_log("cockpit: %s: \"%s\" (mesh %u, %u triangles, centre az %.1f el %.1f deg at %.2f m) is screen %d, was %u "
                "(ACZ_VR_MFD_FRONT_MODEL)", path, nm, i, ntri, atan2(c[0], -c[2]) * 57.29578, atan2(c[1], sqrt(c[0] * c[0] + c[2] * c[2])) * 57.29578,
                r, id, old);
        if (du > 0.0) ps2_log("cockpit: %s: \"%s\": its face %.1f x %.1f cm, texture coordinates 0..1 across it", path, nm, du * 100.0, dv * 100.0);
    }
}
/* ACZ_VR_RQ_COCKPIT_WET=lean (default, the baked water) | full (cockpit.frag's canopy_water at the eye's size: about 11 ms in M1, the A/B) */
/* ACZ_VR_RQ_COCKPIT_WET_VUV (R-45b, note 6; default 1 since t6): 1 = the lean and lean2 wet canopy read the world copy at the
   position interpolated from their vertices (v_fuv) instead of gl_FragCoord, which the specialised stages get wrong in
   reduced bins under the runtime's density map (the half-size copies, the canopy ghosts); 0 = gl_FragCoord (as before) */
static int rqc_wet_vuv(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("ACZ_VR_RQ_COCKPIT_WET_VUV"); v = !(e && *e == '0');
                 ps2_log("cockpit material: the lean wet canopy reads the world at %s (ACZ_VR_RQ_COCKPIT_WET_VUV)", v ? "the interpolated position" : "gl_FragCoord"); }
    return v;
}
/* ACZ_VR_RQ_COCKPIT_WET_NOSPEC [test] (R-45b): 1 = the lean wet canopy's fragment stage without specialization constants
   (every term on, the same picture at a higher cost; Godot 113467: this Adreno mispositions per-fragment reads under
   foveation in shaders with specialization constants); 0 = specialised per combination (as before) */
static int rqc_wet_nospec(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("ACZ_VR_RQ_COCKPIT_WET_NOSPEC"); v = e && *e == '1';
                 if (v) ps2_log("cockpit material: the lean wet canopy without specialization constants (ACZ_VR_RQ_COCKPIT_WET_NOSPEC)"); }
    return v;
}
static int rqc_wet_cfg(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("ACZ_VR_RQ_COCKPIT_WET"); v = e && !strcmp(e, "full") ? 2 : e && !strcmp(e, "lean2") ? 3 : 1; ps2_log("cockpit material: the wet canopy: %s (ACZ_VR_RQ_COCKPIT_WET)", v == 1 ? "lean, baked" : v == 3 ? "lean2, the PC's look" : "full"); }
    return v;
}
static float rqc_wet_th(void) {                   /* ACZ_VR_RQ_COCKPIT_WET_TH (0.01): below this water weight a triangle stays on the dry glass */
    static float v = -1.0f;
    if (v < 0.0f) { const char *e = getenv("ACZ_VR_RQ_COCKPIT_WET_TH"); v = e && *e ? (float)atof(e) : 0.01f; }
    /* [test] ACZ_VR_RQ_COCKPIT_WET_AB=1 (R-87 / R-88): no water on even game fields -- the glass dry there, as with a huge threshold --
       so dumps at consecutive fields (PS2_XR_DUMP_EVERY=1) give the water's own pixels in one run (Part E's gate) */
    static int ab = -1;
    if (ab < 0) { const char *e = getenv("ACZ_VR_RQ_COCKPIT_WET_AB"); ab = e && *e == '1'; if (ab) ps2_log("cockpit material: [test] the wet canopy only on odd game fields (ACZ_VR_RQ_COCKPIT_WET_AB)"); }
    extern u32 ps2_gs_frame_count;
    if (ab && !(ps2_gs_frame_count & 1u)) return 1e30f;
    return v;
}
/* ACZ_VR_RQ_COCKPIT_DBG=id[,id...] [test: what costs what on the device; every id leaves the picture wrong or incomplete]:
   1 the solid fragment stage writes a constant; 2 its texture read is one fixed slot, no non-uniform index; 3 no eye adaptation pass and no
   scene-light copy; 4 the vertex stage's light is a constant; 5 no wet pipeline (the dry glass on all glass); 6 no glass at all; 7 no solid parts;
   8 the solid fragment stage with mediump uv and shadow interpolants; 9 the old model pass back (PS2_PERF_COCKPIT_FOLD ran it before this fix) */
static u32 rqc_dbg(void) {
    static int init; static u32 m;
    if (!init) {
        init = 1;
        const char *e = getenv("ACZ_VR_RQ_COCKPIT_DBG");
        while (e && *e) { int v = atoi(e); if (v > 0 && v < 32) m |= 1u << v; e = strchr(e, ','); if (e) e++; }
        if (m) ps2_log("cockpit material: ACZ_VR_RQ_COCKPIT_DBG mask 0x%X [test]", m);
    }
    return m;
}
#define RQC_DBG(n) ((rqc_dbg() >> (n)) & 1u)
/* the frame takes the Quest material: the fold's early old model pass (cpit_msaa_pass, the whole old shader) must not run as well */
static int rqc_no_fold(void) { return rqc_cfg() && rq_on() == 3 && cpit.rqc && cpit.rqc->ok && !RQC_DBG(9); }
static int rqc_first(void) {                       /* ACZ_VR_RQ_COCKPIT_FIRST=1 [test until the agent order's depth-state fix]: the solid cockpit as the pass's first draw */
    static int v = -1;
    if (v < 0) { const char *e = getenv("ACZ_VR_RQ_COCKPIT_FIRST"); v = e && *e == '1'; }
    return v;
}
static float rqc_glass_th(void) {                  /* ACZ_VR_RQ_COCKPIT_GLASS_TH (0.012): the weight below which a dry glass triangle is not drawn */
    static float v = -1.0f;
    if (v < 0.0f) { const char *e = getenv("ACZ_VR_RQ_COCKPIT_GLASS_TH"); v = e && *e ? (float)atof(e) : 0.012f; }
    return v;
}
static float rqc_sun_deg(const float *a, const float *b) {
    float c = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    c = c > 1.0f ? 1.0f : c < -1.0f ? -1.0f : c;
    return acosf(c) * 57.29578f;
}

/* The shaders: rq_load_shaders's list calls this */
static void rqc_load_shaders(const char *base) {
    char spv[512];
    snprintf(rqc_shader_base, sizeof rqc_shader_base, "%s", base);
    if (!rqc_cfg() || !vk_mv) return;
    static const struct { VkShaderModule *m; const char *n; } S[] = {
        { &rqc.vs, "rq_cockpit.vert" }, { &rqc.fs, "rq_cockpit.frag" }, { &rqc.fsmp, "rq_cockpit_mp.frag" }, { &rqc.gvs, "rq_cockpit_glass.vert" }, { &rqc.gfs, "rq_cockpit_glass.frag" },
        { &rqc.smvs, "rq_cockpit_sm.vert" }, { &rqc.ovs, "rq_cockpit_old.vert" }, { &rqc.wvs, "rq_cockpit_wet.vert" }, { &rqc.wfs, "rq_cockpit_wet.frag" }, { &rqc.lwvs, "rq_cockpit_lwet.vert" }, { &rqc.lwfs, "rq_cockpit_lwet.frag" } };
    for (u32 i = 0; i < sizeof S / sizeof S[0]; i++) {
        snprintf(spv, sizeof spv, "%s/%s.spv", base, S[i].n);
        *S[i].m = load_spv(spv);
        if (!*S[i].m) ps2_log("vk: ACZ_VR_COCKPIT_MATERIAL=quest: %s missing", spv);
    }
    rqgx_load_shaders(base);
    if (rqc_wet_cfg() == 1 && rqc_wet_nospec()) { /* (ACZ_VR_RQ_COCKPIT_WET_NOSPEC: the lean fragment stage without specialization constants) */
        snprintf(spv, sizeof spv, "%s/rq_cockpit_lwet_ns.frag.spv", base); VkShaderModule f = load_spv(spv);
        if (f) rqc.lwfs = f; else ps2_log("vk: ACZ_VR_RQ_COCKPIT_WET_NOSPEC: rq_cockpit_lwet_ns.frag missing: the specialised stage kept");
    }
    if (rqc_wet_cfg() == 3) {                     /* (lean2: the same pipelines, its own stages) */
        snprintf(spv, sizeof spv, "%s/rq_cockpit_lwet2.vert.spv", base); rqc.lwvs = load_spv(spv);
        snprintf(spv, sizeof spv, "%s/rq_cockpit_lwet2.frag.spv", base); rqc.lwfs = load_spv(spv);
        snprintf(spv, sizeof spv, "%s/rq_cockpit_fleck.frag.spv", base); rqc.ffs = load_spv(spv);
    }
    if ((rqc_wet_cfg() == 1 || rqc_wet_cfg() == 3) && rqc_wet_vuv() && !rqc_wet_nospec()) {
        /* (ACZ_VR_RQ_COCKPIT_WET_VUV: the lean / lean2 stages with the world read at a varying; after lean2's own load) */
        const char *nm = rqc_wet_cfg() == 3 ? "rq_cockpit_lwet2_vuv" : "rq_cockpit_lwet_vuv";
        snprintf(spv, sizeof spv, "%s/%s.vert.spv", base, nm); VkShaderModule v = load_spv(spv);
        snprintf(spv, sizeof spv, "%s/%s.frag.spv", base, nm); VkShaderModule f = load_spv(spv);
        if (v && f) { rqc.lwvs = v; rqc.lwfs = f; }
        else ps2_log("vk: ACZ_VR_RQ_COCKPIT_WET_VUV: %s stages missing: the gl_FragCoord read kept", nm);
    }
}

/* ---- at load: the merged vertex and index ranges ---- */
static void rqc_model_free(cpit_model *cm) {
    rqc_model *q = cm->rqc;
    if (!q) return;
    if (q->vb) vkDestroyBuffer(dev, q->vb, NULL);
    if (q->ib) vkDestroyBuffer(dev, q->ib, NULL);
    if (q->vbm) vkFreeMemory(dev, q->vbm, NULL);
    if (q->ibm) vkFreeMemory(dev, q->ibm, NULL);
    if (q->rows) vkDestroyBuffer(dev, q->rows, NULL);
    if (q->rowsm) vkFreeMemory(dev, q->rowsm, NULL);
    for (u32 k = 0; k < FRAMES_IN_FLIGHT; k++) {
        if (q->animb[k]) vkDestroyBuffer(dev, q->animb[k], NULL);
        if (q->animm[k]) vkFreeMemory(dev, q->animm[k], NULL);
        if (q->fb[k]) vkDestroyBuffer(dev, q->fb[k], NULL);
        if (q->fm[k]) vkFreeMemory(dev, q->fm[k], NULL);
        if (q->gib[k]) vkDestroyBuffer(dev, q->gib[k], NULL);
        if (q->gim[k]) vkFreeMemory(dev, q->gim[k], NULL);
    }
    if (q->gvb) vkDestroyBuffer(dev, q->gvb, NULL);
    if (q->gxsb) vkDestroyBuffer(dev, q->gxsb, NULL);
    if (q->gxsm) vkFreeMemory(dev, q->gxsm, NULL);
    free(q->gxst); free(q->gxtc); free(q->gxrl);
    if (q->gvbm) vkFreeMemory(dev, q->gvbm, NULL);
    if (q->pool) vkDestroyDescriptorPool(dev, q->pool, NULL);
    free(q->anm); free(q->gm); free(q->gix); free(q->gv); free(q->rowsh);
    free(q);
    cm->rqc = NULL;
}

/* ---- the lean wet canopy: its textures, baked once ----
   cockpit.frag's canopy_water finds the drops in 3D cells of hashes and the film and rime in value noise, for every fragment (about 660
   ALU a fragment on the Adreno, 11-12 ms over the canopy in M1). Here the same bead and rivulet shapes (bead_lens, rivulet_lens, the same
   hash) are evaluated once into tiling textures, the glass surface being the bake's plane: tdrop 1024^2, 32 cells of 14 mm beads with the 7 mm
   ones among them (RG the lens: rho x direction x steepness; B cover; A the drop's presence draw over its layer's share); triv 512 x 128, 16 x
   16 cells of rivulets (R the lens across, G cover, B presence); tnoise 512^2, a 0.48 m tile (R film noise, G clumps, B rime mottling, A grain) */
static struct { int ok, tried; VkImage img[6]; VkDeviceMemory mem[6]; VkImageView view[6]; VkSampler smp, csmp; VkDescriptorSetLayout dsl; VkDescriptorPool pool; VkDescriptorSet set;
                int bok, bfresh; u32 bw, bh; u32 next_free; } rqw;   /* (lean2: img[5] the feed's small mip chain, 3 levels, 2 layers; made at its first use) */
static void rqw_pcg3(u32 *v) {
    v[0] = v[0] * 1664525u + 1013904223u; v[1] = v[1] * 1664525u + 1013904223u; v[2] = v[2] * 1664525u + 1013904223u;
    v[0] += v[1] * v[2]; v[1] += v[2] * v[0]; v[2] += v[0] * v[1];
    v[0] ^= v[0] >> 16u; v[1] ^= v[1] >> 16u; v[2] ^= v[2] >> 16u;
    v[0] += v[1] * v[2]; v[1] += v[2] * v[0]; v[2] += v[0] * v[1];
}
static void rqw_hash4(int x, int y, int z, float *h) {          /* cockpit.frag hash4 */
    u32 a[3] = { (u32)x & 4095u, (u32)y & 4095u, (u32)z & 4095u };
    rqw_pcg3(a);
    u32 g[3] = { a[2] ^ 0x9E3779B9u, a[0] ^ 0x9E3779B9u, a[1] ^ 0x9E3779B9u };
    rqw_pcg3(g);
    const float k = 1.0f / 4294967296.0f;
    h[0] = (float)a[0] * k; h[1] = (float)a[1] * k; h[2] = (float)a[2] * k; h[3] = (float)g[0] * k;
}
static inline float rqw_fr(float x) { return x - floorf(x); }
static inline float rqw_ss(float a, float b, float x) { float t = (x - a) / (b - a); t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t; return t * t * (3.0f - 2.0f * t); }
/* bead_lens on the glass's plane: fx, fz in the cell (-0.5..0.5; z aft), tpc texels a cell; the trail as at about 350 kt */
static float rqw_bead(int cx, int cz, int seed, float fx, float fz, float tpc, float cellm, float rmin, float rmax, float ex, float *lx, float *lz, float *hx) {
    float h[4]; rqw_hash4(cx, seed, cz, h);
    *hx = h[0]; *lx = *lz = 0.0f;
    float u = rqw_fr(h[0] * 7.13f + h[3] * 3.7f), r = rmin + (rmax - rmin) * u * u * u;
    float o[3] = { (h[1] - 0.5f) * (0.9f - 2.0f * r), (h[2] - 0.5f) * (0.9f - 2.0f * r), (h[3] - 0.5f) * (0.9f - 2.0f * r) };
    if (ex != 3.0f) {                                           /* (the sizes evenly spread: the rain beads) */
        float mo = fabsf(o[0]) > fabsf(o[1]) ? fabsf(o[0]) : fabsf(o[1]); if (fabsf(o[2]) > mo) mo = fabsf(o[2]);
        float r2 = rmin + (rmax - rmin) * powf(u, ex);
        r = r2 < 0.48f - mo ? r2 : 0.48f - mo;
        if (r <= 0.0f) return 0.0f;
    }
    float hv = rqw_fr(h[3] * 13.7f + h[1] * 5.1f);
    float plane = rqw_fr(h[2] * 5.3f + h[0] * 11.9f) - 0.5f;    /* where the glass cuts this cell (the 3D cells' section) */
    float w[3] = { fx - o[0], plane - o[1], fz - o[2] };
    float L0 = (o[2] + 0.47f) - 0.25f * r, L = 1.65f * r * (0.35f + 0.95f * hv);
    if (L > L0) L = L0 > 0.0f ? L0 : 0.0f;
    float t = L > 0.0f ? -w[2] / L : 0.0f; t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    float v[3] = { w[0], w[1], w[2] + t * L };
    float rr = r * (1.0f + (0.2f - 1.0f) * sqrtf(t));
    if (v[0] * v[0] + v[1] * v[1] + v[2] * v[2] > 1.19f * rr * rr) return 0.0f;
    if (hv > 0.45f) rr *= 1.0f - 0.6f * rqw_ss(0.12f, 0.3f, t) * (0.5f - 0.5f * cosf(t * L / r * 4.4f));
    {   float k = 4.5f / rr, ph = hv * 6.2832f, d0 = rr * 0.05f * sinf(v[1] * k + ph), d1 = rr * 0.05f * sinf(v[2] * k + ph), d2 = rr * 0.05f * sinf(v[0] * k + ph);
        v[0] += d0; v[1] += d1; v[2] += d2; }
    float rs2 = rr * rr - v[1] * v[1];
    if (rs2 <= 0.0f) return 0.0f;
    float rs = sqrtf(rs2), lp = sqrtf(v[0] * v[0] + v[2] * v[2]), rho = lp / rs, kc = 1.0f + (0.55f - 1.0f) * t;
    float aa = 1.0f / (rs * tpc); if (aa < 1e-3f) aa = 1e-3f;
    float cover = 1.6f / aa - 0.6f; cover = cover < 0.0f ? 0.0f : cover > 1.0f ? 1.0f : cover;
    if (aa > 0.5f) aa = 0.5f;
    float m = (1.0f - rqw_ss(1.0f - aa, 1.0f, rho)) * cover;
    /* (cockpit.frag's sm: a drop under ~4 px of half width shows no lens, only its average; about 800 eye pixels a metre at the glass) */
    float sb = (rs * cellm * 800.0f - 1.5f) * 0.4f; sb = sb < 0.0f ? 0.0f : sb > 1.0f ? 1.0f : sb;
    if (lp > 1e-6f) { float k = (rho > 1.0f ? 1.0f : rho) * kc * sb / lp; *lx = v[0] * k; *lz = v[2] * k; }
    return m;
}
/* rivulet_lens on the glass's plane */
static float rqw_riv(int cx, int cz, float fx, float fz, float tpc, float *lx, float *hx) {
    float h[4]; rqw_hash4(cx + 1371, 2087, cz + 593, h);
    *hx = h[0]; *lx = 0.0f;
    float w = 0.035f + (0.09f - 0.035f) * h[3];
    float ox = (h[1] - 0.5f) * 0.6f + 0.06f * sinf(8.0f * fz + h[1] * 6.2832f), oy = (h[2] - 0.5f) * 0.6f;
    float t = fz + (h[3] - 0.5f) * 0.3f;
    float along = rqw_ss(-0.48f, -0.2f, t) * (1.0f - rqw_ss(0.3f, 0.42f, t));
    float hb = (t - 0.3f) * 20.0f;
    w *= 0.7f + 0.3f * along + 1.1f * expf(-hb * hb);
    float plane = rqw_fr(h[2] * 5.3f + h[0] * 11.9f) - 0.5f;                   /* (where the glass cuts the cell: a thin line shows only in a few of them, as in the 3D cells) */
    float vx = fx - ox, hd = plane - oy, rs2 = w * w - hd * hd;
    if (rs2 <= 0.0f) return 0.0f;
    float rs = sqrtf(rs2), lp = fabsf(vx), rho = lp / rs;
    float aa = 1.0f / (rs * tpc); if (aa < 1e-3f) aa = 1e-3f;
    float cover = 1.6f / aa - 0.6f; cover = cover < 0.0f ? 0.0f : cover > 1.0f ? 1.0f : cover;
    if (aa > 0.5f) aa = 0.5f;
    float m = (1.0f - rqw_ss(1.0f - aa, 1.0f, rho)) * rqw_ss(0.0f, 0.15f, along) * cover;
    *lx = (vx < 0.0f ? -1.0f : 1.0f) * (rho > 1.0f ? 1.0f : rho);
    return m;
}
/* value noise that tiles over (px, py) cells */
static float rqw_vn(float x, float y, int px, int py, u32 seed) {
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float fx = x - (float)ix, fy = y - (float)iy;
    fx = fx * fx * (3.0f - 2.0f * fx); fy = fy * fy * (3.0f - 2.0f * fy);
    float v[4];
    for (int k = 0; k < 4; k++) {
        u32 a = (u32)(((ix + (k & 1)) % px + px) % px), b = (u32)(((iy + (k >> 1)) % py + py) % py);
        u32 h = a * 0x8DA6B343u ^ b * 0xD8163841u ^ seed * 0xCB1AB31Fu;
        h ^= h >> 15u; h *= 0x2C1B3C6Du; h ^= h >> 12u; h *= 0x297A2D39u; h ^= h >> 15u;
        v[k] = (float)h * (1.0f / 4294967296.0f);
    }
    return (v[0] + (v[1] - v[0]) * fx) + ((v[2] + (v[3] - v[2]) * fx) - (v[0] + (v[1] - v[0]) * fx)) * fy;
}
static inline u8 rqw_u8(float x) { x = x * 255.0f + 0.5f; return (u8)(x < 0.0f ? 0.0f : x > 255.0f ? 255.0f : x); }
typedef struct { vk_frame *st; VkImage img; u32 w, h, levels; } rqw_up;
static void rqw_rec(VkCommandBuffer cmd, void *a) {
    rqw_up *u = (rqw_up *)a;
    cpit_lv_barrier(cmd, u->img, 0, u->levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkDeviceSize off = 0;
    for (u32 l = 0; l < u->levels; l++) {
        u32 lw = u->w >> l ? u->w >> l : 1u, lh = u->h >> l ? u->h >> l : 1u;
        VkBufferImageCopy rg; memset(&rg, 0, sizeof rg);
        rg.bufferOffset = off; rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rg.imageSubresource.mipLevel = l; rg.imageSubresource.layerCount = 1;
        rg.imageExtent.width = lw; rg.imageExtent.height = lh; rg.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(cmd, u->st->sbuf, u->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
        off += (VkDeviceSize)lw * lh * 4u;
    }
    cpit_lv_barrier(cmd, u->img, 0, u->levels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
}
/* one image: px (w x h RGBA8) and its box-filtered mips through the staging buffer */
static int rqw_image_to(u8 *px, u32 w, u32 h, vk_frame *st, VkImage *oimg, VkDeviceMemory *omem, VkImageView *oview) {
    u32 levels = 1u;
    while ((w >> levels) || (h >> levels)) levels++;
    VkImageCreateInfo ii; memset(&ii, 0, sizeof ii);
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8B8A8_UNORM; ii.extent.width = w; ii.extent.height = h; ii.extent.depth = 1;
    ii.mipLevels = levels; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(dev, &ii, NULL, oimg) != VK_SUCCESS) return 0;
    VkMemoryRequirements r; vkGetImageMemoryRequirements(dev, *oimg, &r);
    VkMemoryAllocateInfo ai; memset(&ai, 0, sizeof ai); ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = r.size; ai.memoryTypeIndex = find_mem(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == 0xFFFFFFFFu || vkAllocateMemory(dev, &ai, NULL, omem) != VK_SUCCESS || vkBindImageMemory(dev, *oimg, *omem, 0) != VK_SUCCESS) return 0;
    VkImageViewCreateInfo vi; memset(&vi, 0, sizeof vi); vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = *oimg; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = ii.format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount = levels; vi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(dev, &vi, NULL, oview) != VK_SUCCESS) return 0;
    u8 *dst = (u8 *)st->sptr; const u8 *lv = px; u32 lw = w, lh = h;
    for (u32 l = 0; l < levels; l++) {
        memcpy(dst, lv, (size_t)lw * lh * 4u);
        u32 nw = lw > 1u ? lw / 2u : 1u, nh = lh > 1u ? lh / 2u : 1u;
        u8 *next = dst + (size_t)lw * lh * 4u;
        if (l + 1u < levels)
            for (u32 y = 0; y < nh; y++) for (u32 x = 0; x < nw; x++) for (int c = 0; c < 4; c++) {
                u32 x0 = lw > 1u ? 2u * x : 0u, x1 = lw > 1u ? 2u * x + 1u : 0u, y0 = lh > 1u ? 2u * y : 0u, y1 = lh > 1u ? 2u * y + 1u : 0u;
                next[(y * nw + x) * 4u + (u32)c] = (u8)(((u32)lv[(y0 * lw + x0) * 4u + (u32)c] + lv[(y0 * lw + x1) * 4u + (u32)c] + lv[(y1 * lw + x0) * 4u + (u32)c] + lv[(y1 * lw + x1) * 4u + (u32)c] + 2u) / 4u);
            }
        lv = next; dst = next; lw = nw; lh = nh;
    }
    rqw_up u = { st, *oimg, w, h, levels };
    cpit_submit_once(rqw_rec, &u);
    return 1;
}
static int rqw_image(int i, u8 *px, u32 w, u32 h, vk_frame *st) { return rqw_image_to(px, w, h, st, &rqw.img[i], &rqw.mem[i], &rqw.view[i]); }
/* ---- ACZ_VR_RQ_COCKPIT_WET=lean2: the drops as parameters ----
   bead_lens on the glass's plane, in true proportions at the stretch 1.45 (the cell's length along the flow; the head is round at any speed), the tail as at
   450 kt (the fragment cuts it for the speed). What the fragment needs per drop: rho, the half width of the slice (m), the tail parameter, the size draw, the presence draw.
   rho and the lens run on beyond the rim (rho up to 2), so the filter does not pull them to the background at the edge */
#define RQW2_SB 1.45f
#define RQW2_TAIL 3.3f
typedef struct { float rho, lx, lz, rs, t, u, a; int ok, sized; } rqw_bd2;
static void rqw_bead2(int cx, int cz, int seed, float fx, float fz, float cellm, float rmin, float rmax, float ex, float share, rqw_bd2 *o) {
    float h[4]; rqw_hash4(cx, seed, cz, h);
    memset(o, 0, sizeof *o); o->rho = 3.0f; o->a = 1.0f;
    float u = rqw_fr(h[0] * 7.13f + h[3] * 3.7f), r = rmin + (rmax - rmin) * u * u * u;
    float oc[3] = { (h[1] - 0.5f) * (0.9f - 2.0f * r), (h[2] - 0.5f) * (0.9f - 2.0f * r), (h[3] - 0.5f) * (0.9f - 2.0f * r) };
    if (ex != 3.0f) {                                           /* (the sizes evenly spread: the rain beads) */
        float mo = fabsf(oc[0]) > fabsf(oc[1]) ? fabsf(oc[0]) : fabsf(oc[1]); if (fabsf(oc[2]) > mo) mo = fabsf(oc[2]);
        float r2 = rmin + (rmax - rmin) * powf(u, ex);
        r = r2 < 0.48f - mo ? r2 : 0.48f - mo;
        if (r <= 0.0f) return;
    }
    o->a = h[0] / share; o->u = u;
    float hv = rqw_fr(h[3] * 13.7f + h[1] * 5.1f);
    float plane = rqw_fr(h[2] * 5.3f + h[0] * 11.9f) - 0.5f;    /* where the glass cuts this cell */
    float w[3] = { fx - oc[0], plane - oc[1], (fz - oc[2]) * RQW2_SB };
    float L0 = (oc[2] + 0.47f) * RQW2_SB - 0.25f * r, L = RQW2_TAIL * r * (0.35f + 0.95f * hv);
    if (L > L0) L = L0 > 0.0f ? L0 : 0.0f;
    float t = L > 0.0f ? -w[2] / L : 0.0f; t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    float v[3] = { w[0], w[1], w[2] + t * L };
    float rr = r * (1.0f - 0.8f * sqrtf(t));
    if (hv > 0.45f) rr *= 1.0f - 0.6f * rqw_ss(0.12f, 0.3f, t) * (0.5f - 0.5f * cosf(t * L / r * 4.4f));
    {   float k = 4.5f / rr, ph = hv * 6.2832f, d0 = rr * 0.05f * sinf(v[1] * k + ph), d1 = rr * 0.05f * sinf(v[2] * k + ph), d2 = rr * 0.05f * sinf(v[0] * k + ph);
        v[0] += d0; v[1] += d1; v[2] += d2; }
    float rs2 = rr * rr - v[1] * v[1];
    if (rs2 <= 0.0f) return;                                    /* (the glass's plane misses this drop) */
    float rs = sqrtf(rs2), lp = sqrtf(v[0] * v[0] + v[2] * v[2]);
    o->ok = 1; o->rho = lp / rs; o->rs = rs * cellm; o->t = t;
    if (lp > 1e-6f) { float k = (o->rho > 1.0f ? 1.0f : o->rho) * (1.0f - 0.45f * t) / lp; o->lx = v[0] * k; o->lz = v[2] * k; }
}
static void rqw_riv2(int cx, int cz, float fx, float fz, u8 *o) {
    float h[4]; rqw_hash4(cx + 1371, 2087, cz + 593, h);
    float w = 0.035f + (0.09f - 0.035f) * h[3];
    float ox = (h[1] - 0.5f) * 0.6f + 0.06f * sinf(8.0f * fz + h[1] * 6.2832f), oy = (h[2] - 0.5f) * 0.6f;
    float t = fz + (h[3] - 0.5f) * 0.3f;
    float along = rqw_ss(-0.48f, -0.2f, t) * (1.0f - rqw_ss(0.3f, 0.42f, t));
    float hb = (t - 0.3f) * 20.0f;
    w *= 0.7f + 0.3f * along + 1.1f * expf(-hb * hb);
    float plane = rqw_fr(h[2] * 5.3f + h[0] * 11.9f) - 0.5f;
    float vx = fx - ox, hd = plane - oy, rs2 = w * w - hd * hd;
    float pres = h[0] / 0.35f;
    if (rs2 <= 0.0f) { o[0] = 128u; o[1] = 255u; o[2] = 0u; o[3] = 255u; return; }
    float rs = sqrtf(rs2), rho = fabsf(vx) / rs;
    if (rho > 2.0f) rho = 2.0f;
    o[0] = rqw_u8((vx < 0.0f ? -1.0f : 1.0f) * (rho > 1.0f ? 1.0f : rho) * 0.5f + 0.5f);    /* (signed: the lens; the filter may average it to 0 in a line narrower than a texel, as a flat lens) */
    o[1] = rqw_u8(rho * 0.5f);                                                               /* (unsigned, so the filter does not bring it to 0 beside a thin line) */
    o[2] = rqw_u8(rs * 0.016f / 0.0032f);
    o[3] = rqw_u8(pres + (1.0f - rqw_ss(0.0f, 0.15f, along)) * 1.2f);                         /* (the line's ends: the presence draw pushed past any water) */
}
static int rqw_bake2(vk_frame *st) {
    enum { DW = 1024, RW = 1024, RH = 128, NW = 512, IW = 1024 };
    u8 *a = (u8 *)malloc((size_t)DW * DW * 4u), *b = (u8 *)malloc((size_t)DW * DW * 4u), *r = (u8 *)malloc((size_t)RW * RH * 4u), *n = (u8 *)malloc((size_t)NW * NW * 4u), *ic = (u8 *)malloc((size_t)IW * IW * 4u);
    int ok = 0;
    if (a && b && r && n && ic) {
        for (u32 y = 0; y < (u32)DW; y++) for (u32 x = 0; x < (u32)DW; x++) {
            /* 32 big cells (14 mm) and 64 fine ones (7 mm) across, the same counts along the flow at their 1.45 length; 69 of the rain beads' */
            float bx = ((float)x + 0.5f) / 32.0f, bz = ((float)y + 0.5f) / 32.0f, fx = bx * 2.0f, fz = bz * 2.0f;
            float rx = ((float)x + 0.5f) * 69.0f / (float)DW, rz = ((float)y + 0.5f) * 69.0f / (float)DW;
            int cx = (int)floorf(bx), cz = (int)floorf(bz), ex = (int)floorf(fx), ez = (int)floorf(fz), gx = (int)floorf(rx), gz = (int)floorf(rz);
            rqw_bd2 c[3];
            rqw_bead2(cx, cz, 17, bx - (float)cx - 0.5f, bz - (float)cz - 0.5f, 0.014f, 0.08f, 0.4f, 3.0f, 0.7f, &c[0]);
            rqw_bead2(ex, ez, 41, fx - (float)ex - 0.5f, fz - (float)ez - 0.5f, 0.007f, 0.1f, 0.34f, 3.0f, 0.6f, &c[1]);
            rqw_bead2(gx, gz, 73, rx - (float)gx - 0.5f, rz - (float)gz - 0.5f, 0.0065f, 0.14f, 0.42f, 1.5f, 4.0f * 0.216f, &c[2]);
            /* of the drops over this texel, the one that appears first (the lowest presence draw); no drop here: the nearest, so the filter finds its values */
            int best = -1; float ba = 2.0f;
            for (int i = 0; i < 3; i++) if (c[i].ok && c[i].rho < 1.25f && c[i].a < ba) { ba = c[i].a; best = i; }
            if (best < 0) { float br = 9.0f; for (int i = 0; i < 3; i++) if (c[i].ok && c[i].rho < br) { br = c[i].rho; best = i; } }
            u8 *oa = a + ((size_t)y * DW + x) * 4u, *ob = b + ((size_t)y * DW + x) * 4u;
            if (best < 0) { oa[0] = oa[1] = 128u; oa[2] = 255u; oa[3] = 255u; ob[0] = ob[1] = ob[2] = 0u; ob[3] = 255u; continue; }
            const rqw_bd2 *d = &c[best];
            oa[0] = rqw_u8(d->lx * 0.5f + 0.5f); oa[1] = rqw_u8(d->lz * 0.5f + 0.5f); oa[2] = rqw_u8((d->rho > 2.0f ? 2.0f : d->rho) * 0.5f); oa[3] = rqw_u8(d->a);
            ob[0] = rqw_u8(d->rs / 0.0075f); ob[1] = rqw_u8(d->t); ob[2] = rqw_u8(d->u); ob[3] = best == 2 ? 0u : best == 1 ? 128u : 255u;     /* (A: the layer) */
        }
        for (u32 y = 0; y < (u32)RH; y++) for (u32 x = 0; x < (u32)RW; x++) {
            float cxf = ((float)x + 0.5f) / 64.0f, czf = ((float)y + 0.5f) / 8.0f;
            int cx = (int)floorf(cxf), cz = (int)floorf(czf);
            rqw_riv2(cx, cz, cxf - (float)cx - 0.5f, czf - (float)cz - 0.5f, r + ((size_t)y * RW + x) * 4u);
        }
        for (u32 y = 0; y < (u32)NW; y++) for (u32 x = 0; x < (u32)NW; x++) {
            float u = ((float)x + 0.5f) / (float)NW, v = ((float)y + 0.5f) / (float)NW;
            u8 *o = n + ((size_t)y * NW + x) * 4u;
            o[0] = rqw_u8(rqw_vn(u * 16.0f, v * 16.0f, 16, 16, 1u) * 0.62f + rqw_vn(u * 46.0f, v * 46.0f, 46, 46, 2u) * 0.38f);
            o[1] = rqw_u8(rqw_vn(u * 8.0f, v * 8.0f, 8, 8, 3u));
            o[2] = rqw_u8(rqw_vn(u * 62.0f, v * 24.0f, 62, 24, 4u));
            o[3] = rqw_u8(rqw_vn(u * 256.0f, v * 256.0f, 256, 256, 5u));
        }
        /* the rime's map, a 0.24 m tile, from the PC's own functions: the feathers (p x 520 across, 55 along), the fronds' mottling (130 across, 52 along), the grain (700);
           A = the crust's height h. The lower mips are the soft copy the front's edge and the thin crust read */
        for (u32 y = 0; y < (u32)IW; y++) for (u32 x = 0; x < (u32)IW; x++) {
            float u = ((float)x + 0.5f) / (float)IW, v = ((float)y + 0.5f) / (float)IW;
            float fe1 = rqw_vn(u * 125.0f, v * 13.0f, 125, 13, 21u), fn = rqw_vn(u * 31.0f, v * 12.0f, 31, 12, 22u), gr1 = rqw_vn(u * 168.0f, v * 168.0f, 168, 168, 23u);
            u8 *o = ic + ((size_t)y * IW + x) * 4u;
            o[0] = rqw_u8(fe1); o[1] = rqw_u8(fn); o[2] = rqw_u8(gr1); o[3] = rqw_u8(0.5f * fn + 0.25f * fe1 + 0.25f * gr1);
        }
        ok = rqw_image(0, a, DW, DW, st) && rqw_image(1, b, DW, DW, st) && rqw_image(2, n, NW, NW, st) && rqw_image(3, r, RW, RH, st) && rqw_image(4, ic, IW, IW, st);
    }
    free(a); free(b); free(r); free(n); free(ic);
    return ok;
}
static void rqw_bake(vk_frame *st) {
    if (rqw.tried) return;
    rqw.tried = 1;
    u64 t0 = vk_mono_ns();
    int two = rqc_wet_cfg() == 3;
    enum { DW = 1024, RW = 512, RH = 128, NW = 512 };
    u8 *d = two ? NULL : (u8 *)malloc((size_t)DW * DW * 4u), *r = two ? NULL : (u8 *)malloc((size_t)RW * RH * 4u), *n = two ? NULL : (u8 *)malloc((size_t)NW * NW * 4u);
    if (!two && (!d || !r || !n)) { free(d); free(r); free(n); return; }
    if (!two) {
    for (u32 y = 0; y < (u32)DW; y++) for (u32 x = 0; x < (u32)DW; x++) {
        /* 32 big cells (14 mm: 32 texels each) and 64 fine ones (7 mm) in the tile; the drop on top wins */
        float bx = ((float)x + 0.5f) / 32.0f, bz = ((float)y + 0.5f) / 32.0f, fx = bx * 2.0f, fz = bz * 2.0f;
        int cx = (int)floorf(bx), cz = (int)floorf(bz), ex = (int)floorf(fx), ez = (int)floorf(fz);
        float l1x, l1z, h1, l2x, l2z, h2;
        float m1 = rqw_bead(cx, cz, 17, bx - (float)cx - 0.5f, bz - (float)cz - 0.5f, 32.0f, 0.014f, 0.08f, 0.4f, 3.0f, &l1x, &l1z, &h1);
        float m2 = rqw_bead(ex, ez, 41, fx - (float)ex - 0.5f, fz - (float)ez - 0.5f, 16.0f, 0.007f, 0.1f, 0.34f, 3.0f, &l2x, &l2z, &h2);
        /* the rain beads (6.5 mm, 69 cells in the tile; sizes evenly spread): their share of the water at the drop intensity 0.45 */
        float rx = ((float)x + 0.5f) * 69.0f / (float)DW, rz = ((float)y + 0.5f) * 69.0f / (float)DW, l3x, l3z, h3;
        int gx = (int)floorf(rx), gz = (int)floorf(rz);
        float m3 = rqw_bead(gx, gz, 73, rx - (float)gx - 0.5f, rz - (float)gz - 0.5f, (float)DW / 69.0f, 0.0065f, 0.14f, 0.42f, 1.5f, &l3x, &l3z, &h3);
        float a3 = h3 / (4.0f * 0.216f);
        u8 *o = d + ((size_t)y * DW + x) * 4u;
        int two = m2 > m1 && h2 / 0.6f < 1.0f;
        float lx = two ? l2x : l1x, lz = two ? l2z : l1z, m = two ? m2 : m1, a = two ? h2 / 0.6f : h1 / 0.7f;
        if (m3 > m && a3 < 1.0f) { lx = l3x; lz = l3z; m = m3; a = a3; }
        if (m <= 0.0f) { lx = lz = 0.0f; a = 1.0f; }
        o[0] = rqw_u8(lx * 0.5f + 0.5f); o[1] = rqw_u8(lz * 0.5f + 0.5f); o[2] = rqw_u8(m); o[3] = rqw_u8(a);
    }
    for (u32 y = 0; y < (u32)RH; y++) for (u32 x = 0; x < (u32)RW; x++) {
        float cxf = ((float)x + 0.5f) / 32.0f, czf = ((float)y + 0.5f) / 8.0f;
        int cx = (int)floorf(cxf), cz = (int)floorf(czf);
        float lx, hx, m = rqw_riv(cx, cz, cxf - (float)cx - 0.5f, czf - (float)cz - 0.5f, 32.0f, &lx, &hx);
        u8 *o = r + ((size_t)y * RW + x) * 4u;
        o[0] = rqw_u8(lx * 0.5f + 0.5f); o[1] = rqw_u8(m); o[2] = rqw_u8(m > 0.0f ? hx / 0.35f : 1.0f); o[3] = 255u;
    }
    for (u32 y = 0; y < (u32)NW; y++) for (u32 x = 0; x < (u32)NW; x++) {
        float u = ((float)x + 0.5f) / (float)NW, v = ((float)y + 0.5f) / (float)NW;
        u8 *o = n + ((size_t)y * NW + x) * 4u;
        o[0] = rqw_u8(rqw_vn(u * 16.0f, v * 16.0f, 16, 16, 1u) * 0.62f + rqw_vn(u * 46.0f, v * 46.0f, 46, 46, 2u) * 0.38f);
        o[1] = rqw_u8(rqw_vn(u * 8.0f, v * 8.0f, 8, 8, 3u));
        o[2] = rqw_u8(rqw_vn(u * 62.0f, v * 24.0f, 62, 24, 4u));        /* (mottling drawn out along the flow) */
        o[3] = rqw_u8(rqw_vn(u * 256.0f, v * 256.0f, 256, 256, 5u));
    }
    }
    int ok = two ? rqw_bake2(st) : rqw_image(0, d, DW, DW, st) && rqw_image(1, r, RW, RH, st) && rqw_image(2, n, NW, NW, st);
    free(d); free(r); free(n);
    if (!ok) { ps2_log("cockpit material: the wet canopy's textures could not be made"); return; }
    VkSamplerCreateInfo ms; memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO; ms.magFilter = VK_FILTER_LINEAR; ms.minFilter = VK_FILTER_LINEAR; ms.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    ms.addressModeU = ms.addressModeV = ms.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT; ms.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(dev, &ms, NULL, &rqw.smp) != VK_SUCCESS) return;
    {   VkSamplerCreateInfo cs = ms; cs.addressModeU = cs.addressModeV = cs.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; if (vkCreateSampler(dev, &cs, NULL, &rqw.csmp) != VK_SUCCESS) return; }
    VkDescriptorSetLayoutBinding b[6]; memset(b, 0, sizeof b);
    for (u32 k = 0; k < 6u; k++) { b[k].binding = k; b[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b[k].descriptorCount = 1; b[k].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; }
    VkDescriptorSetLayoutCreateInfo lc; memset(&lc, 0, sizeof lc); lc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; lc.bindingCount = 6; lc.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev, &lc, NULL, &rqw.dsl) != VK_SUCCESS) return;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6 };
    VkDescriptorPoolCreateInfo pc; memset(&pc, 0, sizeof pc); pc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; pc.maxSets = 1; pc.poolSizeCount = 1; pc.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(dev, &pc, NULL, &rqw.pool) != VK_SUCCESS) return;
    VkDescriptorSetAllocateInfo ai; memset(&ai, 0, sizeof ai); ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; ai.descriptorPool = rqw.pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &rqw.dsl;
    if (vkAllocateDescriptorSets(dev, &ai, &rqw.set) != VK_SUCCESS) return;
    VkDescriptorImageInfo im[5]; VkWriteDescriptorSet w[5]; memset(im, 0, sizeof im); memset(w, 0, sizeof w);
    u32 nimg = two ? 5u : 3u;
    for (u32 k = 0; k < nimg; k++) {
        im[k].sampler = rqw.smp; im[k].imageView = rqw.view[k]; im[k].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[k].dstSet = rqw.set; w[k].dstBinding = k; w[k].descriptorCount = 1; w[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[k].pImageInfo = &im[k];
    }
    vkUpdateDescriptorSets(dev, nimg, w, 0, NULL);
    rqw.ok = 1;
    ps2_log("cockpit material: the wet canopy's textures baked in %.0f ms", (double)(vk_mono_ns() - t0) * 1e-6);
}

/* set layouts + the common pipeline layout (once) */
static int rqc_layouts(void) {
    if (rqc.layout) return 1;
    if (!rqc.smp) {
        float an = 4.0f;
        const char *e = getenv("ACZ_VR_RQ_COCKPIT_ANISO");
        if (e && *e) an = (float)atof(e);
        VkSamplerCreateInfo ms; memset(&ms, 0, sizeof ms);
        ms.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        ms.magFilter = VK_FILTER_LINEAR; ms.minFilter = VK_FILTER_LINEAR; ms.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        ms.addressModeU = ms.addressModeV = ms.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        ms.maxLod = VK_LOD_CLAMP_NONE;
        if (an >= 2.0f && max_aniso >= 2.0f) { ms.anisotropyEnable = VK_TRUE; ms.maxAnisotropy = an < max_aniso ? an : max_aniso; }
        if (vkCreateSampler(dev, &ms, NULL, &rqc.smp) != VK_SUCCESS) { rqc.smp = VK_NULL_HANDLE; return 0; }
        ps2_log("vk: cockpit material: textures sampled with anisotropy %.0f (ACZ_VR_RQ_COCKPIT_ANISO, default 4)", ms.anisotropyEnable ? ms.maxAnisotropy : 1.0f);
    }
    VkDescriptorSetLayoutBinding b; memset(&b, 0, sizeof b);
    b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b.descriptorCount = RQC_TEX_MAX; b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorBindingFlags bf = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bi; memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO; bi.bindingCount = 1; bi.pBindingFlags = &bf;
    VkDescriptorSetLayoutCreateInfo lc; memset(&lc, 0, sizeof lc);
    lc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; lc.pNext = &bi; lc.bindingCount = 1; lc.pBindings = &b;
    if (vkCreateDescriptorSetLayout(dev, &lc, NULL, &rqc.texl) != VK_SUCCESS) return 0;
    VkDescriptorSetLayoutBinding bb[3]; memset(bb, 0, sizeof bb);
    for (u32 k = 0; k < 3; k++) { bb[k].binding = k; bb[k].descriptorCount = 1; bb[k].stageFlags = VK_SHADER_STAGE_VERTEX_BIT; }
    bb[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; bb[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bb[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; bb[2].stageFlags |= VK_SHADER_STAGE_FRAGMENT_BIT;
    lc.pNext = NULL; lc.bindingCount = 3; lc.pBindings = bb;
    if (vkCreateDescriptorSetLayout(dev, &lc, NULL, &rqc.fl) != VK_SUCCESS) return 0;
    VkDescriptorSetLayout sl[3] = { rqc.texl, deslayout, rqc.fl };
    VkPushConstantRange pr; memset(&pr, 0, sizeof pr); pr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT; pr.size = 48;
    VkPipelineLayoutCreateInfo pl; memset(&pl, 0, sizeof pl);
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pl.setLayoutCount = 3; pl.pSetLayouts = sl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev, &pl, NULL, &rqc.layout) != VK_SUCCESS) return 0;
    return 1;
}

/* ACZ_VR_RQ_COCKPIT_QGRID (rq3_cockpit 2026-11-10, BUGS_RQ R-62): the grid of the vertex's 16-bit positions. 0 = each mesh's own box
   (before: two parts with the same vertices -- the Rafale's split nodes, a panel over its sub-panel -- got different rebuilt positions,
   so the PC's exact depth tie became a z-fight and the hidden layer showed through: the green and brown garbage at the Rafale's
   legs, M2 66 s); 1 = the model's box for every mesh (one grid: ties again, a step of 0.21-0.23 mm); 2 (default) = meshes sharing
   vertex positions on one grid per group (rqc_qgrid_boxes: ties again, the step at most 0.06 mm on the Rafale) */
static int rqc_qgrid_cfg(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("ACZ_VR_RQ_COCKPIT_QGRID"); v = e && *e ? atoi(e) : 2; if (v < 0 || v > 2) v = 2; }
    return v;
}
/* ACZ_VR_RQ_COCKPIT_CULL (default 3): bit 1 back-face culling of the closed components, bit 2 the frustum test of the spatial chunks */
static int rqc_cull_cfg(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("ACZ_VR_RQ_COCKPIT_CULL"); v = e && *e ? atoi(e) : 3; ps2_log("cockpit material: culling %d (ACZ_VR_RQ_COCKPIT_CULL: 1 back faces, 2 chunks)", v); }
    return v;
}
/* Which triangles of a mesh may lose their back faces. A cockpit part is often an open shell seen from both sides, so a triangle is only cullable
   when its connected component (corners welded by position at 0.1 mm) is closed: every edge met exactly twice, in opposite directions; and the head
   cannot be inside it (the component's box grown by 0.3 m does not hold the design eye). A closed component wound inside out (negative volume) gets
   its triangles flipped in `tri` (three merged indices each). cull[t] = 1 for those that qualify. Returns the number of cullable triangles */
static u32 rqc_closed(const u8 *uq, const u32 *ix, u32 first, u32 nt, u32 *tri, u8 *cull) {
    memset(cull, 0, nt);
    if (!nt) return 0;
    u32 nc = nt * 3u, hn = 16u, cnt = 0;
    while (hn < nc * 4u) hn <<= 1;
    u32 *wid = (u32 *)malloc(nc * sizeof(u32)), *vh = (u32 *)malloc(hn * sizeof(u32)), *par = (u32 *)malloc(nt * sizeof(u32));
    float *vp = (float *)malloc(nc * 3u * sizeof(float));
    u64 *ek = (u64 *)malloc(hn * sizeof(u64)); u32 *et = (u32 *)malloc(hn * sizeof(u32)); u8 *ec = (u8 *)calloc(hn, 1), *bad = (u8 *)calloc(nt, 1);
    if (!wid || !vh || !par || !vp || !ek || !et || !ec || !bad) goto done;
    memset(vh, 0xFF, hn * sizeof(u32));
    u32 nw = 0;
    for (u32 k = 0; k < nc; k++) {
        float p[3]; memcpy(p, uq + (size_t)ix[first + k] * CPIT_VTX, 12);
        int q[3]; for (int a = 0; a < 3; a++) q[a] = (int)floorf(p[a] * 10000.0f + 0.5f);
        u32 h = ((u32)q[0] * 73856093u ^ (u32)q[1] * 19349663u ^ (u32)q[2] * 83492791u) & (hn - 1u);
        for (;;) {
            u32 e = vh[h];
            if (e == 0xFFFFFFFFu) { vh[h] = nw; memcpy(vp + 3u * nw, p, 12); wid[k] = nw++; break; }
            int r[3]; for (int a = 0; a < 3; a++) r[a] = (int)floorf(vp[3u * e + (u32)a] * 10000.0f + 0.5f);
            if (r[0] == q[0] && r[1] == q[1] && r[2] == q[2]) { wid[k] = e; break; }
            h = (h + 1u) & (hn - 1u);
        }
    }
    for (u32 t = 0; t < nt; t++) par[t] = t;
#define RQC_FIND(x) ({ u32 _r = (x); while (par[_r] != _r) { par[_r] = par[par[_r]]; _r = par[_r]; } _r; })
    for (u32 t = 0; t < nt; t++) for (u32 j = 0; j < 3u; j++) {
        u32 a = wid[t * 3u + j], b = wid[t * 3u + (j + 1u) % 3u];
        if (a == b) { bad[t] = 1; continue; }                    /* (a degenerate triangle) */
        u32 lo = a < b ? a : b, hi = a < b ? b : a, dir = a < b ? 1u : 0u;
        u64 key = ((u64)lo << 32) | hi;
        u32 h = (u32)((key * 0x9E3779B97F4A7C15ull) >> 40) & (hn - 1u);
        for (;;) {
            if (!ec[h]) { ek[h] = key; et[h] = t | (dir << 31); ec[h] = 1; break; }
            if (ek[h] == key) {
                u32 o = et[h] & 0x7FFFFFFFu, od = et[h] >> 31;
                if (ec[h] == 1u && od != dir) { ec[h] = 2; u32 ra = RQC_FIND(t), rb = RQC_FIND(o); par[ra] = rb; }
                else { ec[h] = 3; bad[t] = 1; bad[o] = 1; }              /* (more than two faces, or two wound the same way) */
                break;
            }
            h = (h + 1u) & (hn - 1u);
        }
    }
    for (u32 h = 0; h < hn; h++) if (ec[h] == 1u) bad[et[h] & 0x7FFFFFFFu] = 1;                  /* an open edge */
    {   /* per component: closed? the head outside it? its orientation */
        float *cv = (float *)calloc(nt, 7u * sizeof(float));
        if (!cv) goto done;
        for (u32 t = 0; t < nt; t++) { float *c = cv + 7u * t; c[1] = c[2] = c[3] = 1e30f; c[4] = c[5] = c[6] = -1e30f; }
        for (u32 t = 0; t < nt; t++) {
            u32 r = RQC_FIND(t);
            if (bad[t]) bad[r] = 1;
            float a[3], b[3], d[3];
            memcpy(a, vp + 3u * wid[t * 3u], 12); memcpy(b, vp + 3u * wid[t * 3u + 1u], 12); memcpy(d, vp + 3u * wid[t * 3u + 2u], 12);
            float *c = cv + 7u * r;
            c[0] += a[0] * (b[1] * d[2] - b[2] * d[1]) + a[1] * (b[2] * d[0] - b[0] * d[2]) + a[2] * (b[0] * d[1] - b[1] * d[0]);
            for (int j = 0; j < 3; j++) {
                float lo = a[j] < b[j] ? (a[j] < d[j] ? a[j] : d[j]) : (b[j] < d[j] ? b[j] : d[j]);
                float hi = a[j] > b[j] ? (a[j] > d[j] ? a[j] : d[j]) : (b[j] > d[j] ? b[j] : d[j]);
                if (lo < c[1 + j]) c[1 + j] = lo;
                if (hi > c[4 + j]) c[4 + j] = hi;
            }
        }
        for (u32 t = 0; t < nt; t++) {
            u32 r = RQC_FIND(t);
            const float *c = cv + 7u * r;
            if (bad[r]) continue;
            if (c[1] - 0.3f < 0.0f && c[4] + 0.3f > 0.0f && c[2] - 0.3f < 0.0f && c[5] + 0.3f > 0.0f && c[3] - 0.3f < 0.0f && c[6] + 0.3f > 0.0f) continue;
            cull[t] = 1; cnt++;
            if (c[0] < 0.0f) { u32 x = tri[t * 3u + 1u]; tri[t * 3u + 1u] = tri[t * 3u + 2u]; tri[t * 3u + 2u] = x; }
        }
        free(cv);
    }
done:
    free(wid); free(vh); free(par); free(vp); free(ek); free(et); free(ec); free(bad);
    return cnt;
}
/* ---- the readable glass (rq_glass.inc, R-57): its light, rim and sun lobes are per vertex, so a glass mesh's long triangles
   (the F-15C's canopy has slivers 30 cm and longer, fanned from its seams) are split at load until no edge is longer than
   ACZ_VR_RQ_GLASS_SPLIT (0.15 m; at 0.05 the F-15C's 10.7k glass triangles became 70k and the CPU's need test with them; the
   interior's reflection is read per fragment, ACZ_VR_RQ_GLASS_REFLECT_FS, so it needs no fine mesh): every edge over it takes its midpoint (one vertex an edge, shared by both triangles: no
   T-junction), each triangle re-cut by how many of its edges split, repeated. The midpoints lie on the old flat triangles:
   the shape is unchanged. Only the fx_glass meshes (the canopy and windscreen) and only while the readable glass is on. ---- */
static float rqgx_split_cfg(void) {
    static float v = -1.0f;
    if (v < 0.0f) { const char *e = getenv("ACZ_VR_RQ_GLASS_SPLIT"); v = e && *e ? (float)atof(e) : 0.15f; if (!(v >= 0.0f)) v = 0.0f; }
    return v;
}
typedef struct { rqc_vtx **vtx; u32 *nv, *cap_v; float **gv; u32 *ngv, *cap_gv; u32 **gix; u32 *cap_gix; u32 gv0; } rqgx_sp;
static u32 rqgx_mid(rqgx_sp *S, u64 *hk, u32 *hv, u32 hmask, u32 a, u32 b) {
    u64 key = a < b ? ((u64)a << 32 | b) : ((u64)b << 32 | a);
    u32 h = (u32)((key * 0x9E3779B97F4A7C15ull) >> 40) & hmask;
    while (hk[h] != ~0ull) { if (hk[h] == key) return hv[h]; h = (h + 1u) & hmask; }
    if (*S->nv >= *S->cap_v) {
        u32 nc = *S->cap_v * 2u + 1024u; rqc_vtx *t = (rqc_vtx *)realloc(*S->vtx, (size_t)nc * sizeof *t);
        if (!t) return 0xFFFFFFFFu;
        *S->vtx = t; *S->cap_v = nc;
    }
    if (*S->ngv >= *S->cap_gv) {
        u32 nc = *S->cap_gv * 2u + 1024u; float *t = (float *)realloc(*S->gv, (size_t)nc * 6u * sizeof *t);
        if (!t) return 0xFFFFFFFFu;
        *S->gv = t; *S->cap_gv = nc;
    }
    rqc_vtx *V = *S->vtx, *A = &V[a], *B = &V[b], *M = &V[*S->nv];
    for (int j = 0; j < 3; j++) M->q[j] = (u16)(((u32)A->q[j] + B->q[j] + 1u) / 2u);
    M->id = A->id;
    float n[3] = { (float)A->n[0] + B->n[0], (float)A->n[1] + B->n[1], (float)A->n[2] + B->n[2] }, nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    for (int j = 0; j < 3; j++) { float t = nl > 1e-3f ? n[j] / nl * 127.0f : (float)A->n[j]; M->n[j] = (s8)(t >= 0.0f ? t + 0.5f : t - 0.5f); }
    M->ao = (s8)(((int)A->ao + B->ao) / 2);
    M->uv[0] = (u16)(((u32)A->uv[0] + B->uv[0] + 1u) / 2u); M->uv[1] = (u16)(((u32)A->uv[1] + B->uv[1] + 1u) / 2u);
    float *G = *S->gv, *ga = G + 6u * (a - S->gv0), *gb = G + 6u * (b - S->gv0), *gm = G + 6u * *S->ngv;
    for (int j = 0; j < 3; j++) gm[j] = 0.5f * (ga[j] + gb[j]);
    float g[3] = { ga[3] + gb[3], ga[4] + gb[4], ga[5] + gb[5] }, gl = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    for (int j = 0; j < 3; j++) gm[3 + j] = gl > 1e-6f ? g[j] / gl : ga[3 + j];
    u32 id = (*S->nv)++; (*S->ngv)++;
    hk[h] = key; hv[h] = id;
    return id;
}
/* the mesh's triangles at gix[first .. first + 3 nt): split in place (new ones appended); returns the new triangle count */
static u32 rqgx_split_mesh(rqgx_sp *S, u32 first, u32 nt, float L) {
    u32 hsz = 1u; while (hsz < nt * 8u + 64u) hsz <<= 1;
    u64 *hk = (u64 *)malloc((size_t)hsz * sizeof *hk); u32 *hv = (u32 *)malloc((size_t)hsz * sizeof *hv);
    if (!hk || !hv) { free(hk); free(hv); return nt; }
    memset(hk, 0xFF, (size_t)hsz * sizeof *hk);
    float L2 = L * L;
    u32 cap_t = nt * 16u + 64u;                      /* (at most 16x a mesh: a sliver of 80 cm over a 5 cm limit) */
    for (int pass = 0; pass < 6; pass++) {
        /* count the triangles after this pass; stop when nothing splits */
        u32 add = 0, any = 0;
        const u32 *T = *S->gix + first;
        const float *G = *S->gv;
        for (u32 t = 0; t < nt; t++) {
            int ns = 0;
            for (int e = 0; e < 3; e++) {
                const float *p = G + 6u * (T[t * 3u + e] - S->gv0), *q = G + 6u * (T[t * 3u + (e + 1) % 3] - S->gv0);
                float d[3] = { p[0] - q[0], p[1] - q[1], p[2] - q[2] };
                ns += d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > L2;
            }
            add += (u32)ns; any |= ns != 0;
        }
        if (!any || nt + add > cap_t) break;
        if ((first + (nt + add) * 3u) > *S->cap_gix) {
            u32 nc = (first + (nt + add) * 3u) * 2u; u32 *t2 = (u32 *)realloc(*S->gix, (size_t)nc * sizeof *t2);
            if (!t2) break;
            *S->gix = t2; *S->cap_gix = nc;
        }
        u32 *Tw = *S->gix + first, nn = nt;
        for (u32 t = 0; t < nt; t++) {
            u32 v[3] = { Tw[t * 3u], Tw[t * 3u + 1u], Tw[t * 3u + 2u] }, m[3];
            int ns = 0;
            for (int e = 0; e < 3; e++) {
                const float *G2 = *S->gv;
                const float *p = G2 + 6u * (v[e] - S->gv0), *q = G2 + 6u * (v[(e + 1) % 3] - S->gv0);
                float d[3] = { p[0] - q[0], p[1] - q[1], p[2] - q[2] };
                m[e] = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > L2 ? rqgx_mid(S, hk, hv, hsz - 1u, v[e], v[(e + 1) % 3]) : 0xFFFFFFFFu;
                if (m[e] == 0xFFFFFFFFu && d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > L2) { free(hk); free(hv); return nt; }   /* (out of memory) */
                ns += m[e] != 0xFFFFFFFFu;
            }
            if (!ns) continue;
            Tw = *S->gix + first;                    /* (rqgx_mid may not move gix; vtx / gv may move) */
            u32 o[4][3]; int no = 0;
            if (ns == 3) {
                u32 a[4][3] = { { v[0], m[0], m[2] }, { m[0], v[1], m[1] }, { m[2], m[1], v[2] }, { m[0], m[1], m[2] } };
                memcpy(o, a, sizeof a); no = 4;
            } else if (ns == 1) {
                int e = m[0] != 0xFFFFFFFFu ? 0 : m[1] != 0xFFFFFFFFu ? 1 : 2;
                u32 a0 = v[e], a1 = v[(e + 1) % 3], a2 = v[(e + 2) % 3], mm = m[e];
                o[0][0] = a0; o[0][1] = mm; o[0][2] = a2; o[1][0] = mm; o[1][1] = a1; o[1][2] = a2; no = 2;
            } else {                                     /* two: rotate so the unsplit edge is (a2, a0) */
                int e = m[0] == 0xFFFFFFFFu ? 1 : m[1] == 0xFFFFFFFFu ? 2 : 0;      /* (e: the first split edge after the unsplit one) */
                u32 a0 = v[e], a1 = v[(e + 1) % 3], a2 = v[(e + 2) % 3], m01 = m[e], m12 = m[(e + 1) % 3];
                o[0][0] = m01; o[0][1] = a1; o[0][2] = m12;
                o[1][0] = a0; o[1][1] = m01; o[1][2] = m12;
                o[2][0] = a0; o[2][1] = m12; o[2][2] = a2; no = 3;
            }
            memcpy(Tw + t * 3u, o[0], 12);
            for (int k = 1; k < no; k++) { memcpy(Tw + nn * 3u, o[k], 12); nn++; }
        }
        nt = nn;
    }
    free(hk); free(hv);
    return nt;
}
/* ACZ_VR_RQ_COCKPIT_QGRID's grids (rq3_cockpit 2026-11-10, R-62): per mesh 4 floats, the grid's lo[3] and size (0 = the mesh's own
   box). Mode 1: the model's box for every mesh. Mode 2: meshes that share at least 3 exact vertex positions (a node split by material,
   a panel and the layer under it, parts meeting at a seam) are joined, and every mesh of a group takes the group's box -- the parts
   that tie in depth on the PC (the same vertices) tie again, the seams between parts close, and the step stays per group (the Rafale:
   at most 0.058 mm, against 0.234 mm for the model's box). Over the indices the model draws (its LOD level). NULL = no memory */
static float *rqc_qgrid_boxes(const cpit_model *m, const u8 *uq, u32 nuq, const u32 *ix, int lodl, int mode) {
    u32 nm = m->nmesh;
    float *bx = (float *)calloc((size_t)nm * 4u + 4u, sizeof(float)), *lh = (float *)malloc(((size_t)nm * 6u + 6u) * sizeof(float));
    u32 *par = (u32 *)malloc(((size_t)nm + 1u) * sizeof(u32));
    if (!bx || !lh || !par) { free(bx); free(lh); free(par); return NULL; }
    for (u32 i = 0; i < nm; i++) {
        par[i] = i;
        float *b = lh + 6u * i;
        b[0] = b[1] = b[2] = 1e30f; b[3] = b[4] = b[5] = -1e30f;
        const cpit_mesh *me = &m->mesh[i];
        u32 first = me->first, count = me->count;
        if (lodl > 0 && !(me->flags & 1u) && count && me->lcount[lodl - 1] != CPIT_LOD_NONE) { first = me->lfirst[lodl - 1]; count = me->lcount[lodl - 1]; }
        for (u32 k = 0; k < count; k++) {
            float p[3]; memcpy(p, uq + (size_t)ix[first + k] * CPIT_VTX, 12);
            for (int j = 0; j < 3; j++) { if (p[j] < b[j]) b[j] = p[j]; if (p[j] > b[3 + j]) b[3 + j] = p[j]; }
        }
    }
#define RQC_QFIND(x) ({ u32 _r = (x); while (par[_r] != _r) { par[_r] = par[par[_r]]; _r = par[_r]; } _r; })
    u32 nj = 0;
    if (mode == 1) { for (u32 i = 1; i < nm; i++) par[i] = 0; }
    else {
        u32 hn = 1024u; while (hn < nuq + nuq / 2u + 16u) hn <<= 1;
        u32 *key = (u32 *)malloc((size_t)hn * sizeof(u32)), *own = (u32 *)malloc((size_t)hn * sizeof(u32)), *last = (u32 *)malloc((size_t)hn * sizeof(u32));
        u32 *stamp = (u32 *)calloc((size_t)nuq + 1u, sizeof(u32));
        const u32 PN = 1u << 16;
        u64 *pk = (u64 *)malloc((size_t)PN * sizeof(u64)); u32 *pc = (u32 *)calloc(PN, sizeof(u32));
        if (key && own && last && stamp && pk && pc) {
            memset(own, 0xFF, (size_t)hn * sizeof(u32));
            for (u32 i = 0; i < nm; i++) {
                const cpit_mesh *me = &m->mesh[i];
                u32 first = me->first, count = me->count;
                if (lodl > 0 && !(me->flags & 1u) && count && me->lcount[lodl - 1] != CPIT_LOD_NONE) { first = me->lfirst[lodl - 1]; count = me->lcount[lodl - 1]; }
                for (u32 k = 0; k < count; k++) {
                    u32 o = ix[first + k];
                    if (stamp[o] == i + 1u) continue;
                    stamp[o] = i + 1u;
                    const u8 *p = uq + (size_t)o * CPIT_VTX;
                    u32 h = 2166136261u;
                    for (int c = 0; c < 12; c++) h = (h ^ p[c]) * 16777619u;
                    for (h &= hn - 1u;; h = (h + 1u) & (hn - 1u)) {
                        if (own[h] == 0xFFFFFFFFu) { own[h] = i; key[h] = o; last[h] = i; break; }
                        if (memcmp(uq + (size_t)key[h] * CPIT_VTX, p, 12)) continue;
                        u32 j = own[h];
                        if (j != i && last[h] != i) {        /* (a position of mesh j met again in mesh i: count the pair once) */
                            last[h] = i;
                            u64 pkey = ((u64)j << 32) | i;
                            u32 s = (u32)((pkey * 0x9E3779B97F4A7C15ull) >> 48) & (PN - 1u);
                            for (u32 t = 0; t < PN; t++, s = (s + 1u) & (PN - 1u)) {
                                if (!pc[s]) { pk[s] = pkey; pc[s] = 1; break; }
                                if (pk[s] == pkey) {
                                    if (++pc[s] == 3u) { u32 ra = RQC_QFIND(j), rb = RQC_QFIND(i); if (ra != rb) { par[ra] = rb; nj++; } }
                                    break;
                                }
                            }
                        }
                        break;
                    }
                }
            }
        }
        free(key); free(own); free(last); free(stamp); free(pk); free(pc);
    }
    /* the groups' boxes, then each mesh's */
    float *gb = (float *)malloc(((size_t)nm * 6u + 6u) * sizeof(float));
    u32 *gn = (u32 *)calloc((size_t)nm + 1u, sizeof(u32));
    u32 ng = 0; float smax = 0.0f, sjoin = 0.0f;
    if (gb && gn) {
        for (u32 i = 0; i < nm; i++) { float *g = gb + 6u * i; g[0] = g[1] = g[2] = 1e30f; g[3] = g[4] = g[5] = -1e30f; }
        for (u32 i = 0; i < nm; i++) {
            u32 r = RQC_QFIND(i); const float *b = lh + 6u * i; float *g = gb + 6u * r;
            gn[r]++;
            for (int j = 0; j < 3; j++) { if (b[j] < g[j]) g[j] = b[j]; if (b[3 + j] > g[3 + j]) g[3 + j] = b[3 + j]; }
        }
        for (u32 i = 0; i < nm; i++) {
            u32 r = RQC_QFIND(i); const float *g = gb + 6u * r;
            if (!(g[3] >= g[0])) continue;                  /* (nothing drawn) */
            float s = 0.0f;
            for (int j = 0; j < 3; j++) { float d = g[3 + j] - g[j]; if (d > s) s = d; }
            if (s < 1e-5f) s = 1e-5f;
            bx[4u * i] = g[0]; bx[4u * i + 1u] = g[1]; bx[4u * i + 2u] = g[2]; bx[4u * i + 3u] = s;
            if (r == i) ng++;
            if (s > smax) smax = s;
            if (gn[r] > 1u && s > sjoin) sjoin = s;
        }
    } else { free(bx); bx = NULL; }
    free(gb); free(gn);
#undef RQC_QFIND
    ps2_log("cockpit material: positions on %s (ACZ_VR_RQ_COCKPIT_QGRID %d): %u grids for %u meshes (%u joins); the coarsest shared grid %.3f m, "
            "a step of %.4f mm (the coarsest of all %.3f m, %.4f mm)", mode == 1 ? "the model's grid" : "shared grids", mode, ng, nm, nj,
            (double)sjoin, (double)(sjoin / 65535.0f * 1000.0f), (double)smax, (double)(smax / 65535.0f * 1000.0f));
    free(lh); free(par);
    return bx;
}
/* build the merged buffers; called by cpit_load_one at the end of the geometry (uq = the distinct vertices, ix = all indices
   with the LOD levels, nidx their count; the model's meshes know their ranges). Host-visible memory only (the vertex data
   goes through the staging helper of cpit_load_one). */
static void rqc_model_build(cpit_model *m, vk_frame *st, size_t ulim, const u8 *uq, u32 nuq, const u32 *ix) {
    (void)nuq;
    if (!rqc_cfg() || !vk_mv || !uq || !ix) return;
    if (!rqc_layouts()) { ps2_log("cockpit material: layouts failed"); return; }
    int lodl = cpit_lod_lvl(m);                          /* (ACZ_VR_COCKPIT_TRI_BUDGET: the model's level) */
    if (lodl > m->lod_n) lodl = m->lod_n;
    rqc_model *q = (rqc_model *)calloc(1, sizeof *q);
    if (!q) return;
    float *qbox = NULL;                                  /* (ACZ_VR_RQ_COCKPIT_QGRID: per mesh the grid's lo and size) */
    u32 *ish = NULL, nsh = 0;                            /* (ACZ_VR_RQ_COCKPIT_SM_LOD: the casters' indices) */
    u32 cap_v = m->nverts + 16u;
    rqc_vtx *vtx = (rqc_vtx *)malloc((size_t)cap_v * sizeof *vtx);
    u32 *is = (u32 *)malloc((size_t)m->nverts * 3u * sizeof *is), *ic = (u32 *)malloc((size_t)m->nverts * 3u * sizeof *ic);
    u32 *remap = (u32 *)malloc((size_t)nuq * sizeof *remap), *rstamp = (u32 *)calloc(nuq, sizeof *rstamp);
    rqc_row *rows = (rqc_row *)calloc(m->nmesh ? m->nmesh : 1u, sizeof *rows);
    rqc_gmesh *gm = (rqc_gmesh *)calloc(m->nmesh ? m->nmesh : 1u, sizeof *gm);
    u32 *gix = (u32 *)malloc((size_t)m->nverts * 3u * sizeof *gix), cap_gix = m->nverts * 3u, cap_gv = m->nverts + 1u;
    u32 *anm = (u32 *)malloc((size_t)(m->nmesh + 1u) * sizeof *anm);
    float *gv = (float *)malloc((size_t)(m->nverts + 1u) * 6u * sizeof *gv);
    u8 *tf = (u8 *)calloc((size_t)m->nverts + 1u, 1);                     /* per solid triangle: 1 cullable, 2 animated, 4 animated and always drawn (a part that swings far) */
    float *tc = (float *)malloc(((size_t)m->nverts + 1u) * 3u * sizeof *tc);   /* its centre */
    u32 *inr = (u32 *)malloc((size_t)m->nverts * 3u * sizeof *inr);        /* the solid parts round the combiner */
    if (!vtx || !is || !ic || !remap || !rstamp || !rows || !gm || !gix || !gv || !anm || !tf || !tc || !inr) goto oom;
    u32 nv = 0, nis = 0, nic = 0, ng = 0, ngt = 0, nan = 0, ngv = 0, gv0 = 0, ncull = 0, nin = 0, nnm = 0;
    if (rqc_qgrid_cfg()) qbox = rqc_qgrid_boxes(m, uq, nuq, ix, lodl, rqc_qgrid_cfg());
    for (u32 pass = 0; pass < 2u; pass++) {              /* 0: solid and cut; 1: glass (its own vertex range, at the end) */
        if (pass == 1u) gv0 = nv;
        for (u32 i = 0; i < m->nmesh; i++) {
            cpit_mesh *me = &m->mesh[i];
            int glass = (me->flags & 1u) != 0;
            if ((pass == 1u) != glass) continue;
            u32 first = me->first, count = me->count;
            if (!count) continue;
            if (lodl > 0 && !glass && me->lcount[lodl - 1] != CPIT_LOD_NONE) { first = me->lfirst[lodl - 1]; count = me->lcount[lodl - 1]; }
            /* bounding box and UV range over the indices used */
            float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f }, ulo[2] = { 1e30f, 1e30f }, uhi[2] = { -1e30f, -1e30f };
            for (u32 k = 0; k < count; k++) {
                const float *v = (const float *)(uq + (size_t)ix[first + k] * CPIT_VTX);
                float vf[9]; memcpy(vf, v, sizeof vf);
                for (int j = 0; j < 3; j++) { if (vf[j] < lo[j]) lo[j] = vf[j]; if (vf[j] > hi[j]) hi[j] = vf[j]; }
                for (int j = 0; j < 2; j++) { if (vf[6 + j] < ulo[j]) ulo[j] = vf[6 + j]; if (vf[6 + j] > uhi[j]) uhi[j] = vf[6 + j]; }
            }
            float s = 0.0f;
            for (int j = 0; j < 3; j++) { float d = hi[j] - lo[j]; if (d > s) s = d; }
            if (qbox && qbox[4u * i + 3u] > 0.0f) { lo[0] = qbox[4u * i]; lo[1] = qbox[4u * i + 1u]; lo[2] = qbox[4u * i + 2u]; s = qbox[4u * i + 3u]; }   /* (ACZ_VR_RQ_COCKPIT_QGRID) */
            if (s < 1e-5f) s = 1e-5f;
            float us = uhi[0] - ulo[0], vs = uhi[1] - ulo[1];
            if (!(us > 0.0f)) us = 1.0f;
            if (!(vs > 0.0f)) vs = 1.0f;
            rqc_row *row = &rows[i];
            row->qp[0] = lo[0]; row->qp[1] = lo[1]; row->qp[2] = lo[2]; row->qp[3] = s;
            row->qt[0] = ulo[0]; row->qt[1] = ulo[1]; row->qt[2] = us; row->qt[3] = vs;
            memcpy(row->col, me->color, sizeof row->col);
            row->sp[0] = glass ? me->spec : me->spec * m->spec; row->sp[1] = me->shi;
            if (glass && me->spec > 0.0f && gsp_on()) {                /* (the glass's own sun glint: ACZ_VR_COCKPIT_GLASS_SHINE, as cpit_draw) */
                float s0 = me->shi > 1.0f ? me->shi : 1.0f, s1 = gsp_shine > s0 ? gsp_shine : s0;
                row->sp[0] = me->spec * gsp_k * (s1 + 1.0f) / (s0 + 1.0f);
                row->sp[1] = s1;
            }
            row->sp[2] = (float)(((me->flags & CPIT_F_MIRROR) ? 2u : 0u) | (glass && (me->flags & CPIT_F_GLASSFX) ? 4u : 0u)   /* (4: the readable glass's terms, rq_glass.inc) */
                                 | (!glass && (me->flags & CPIT_F_SELFLIT) ? 8u : 0u));                                      /* (8: a display page lit from within, R-85) */
            row->sp[3] = me->tex != 0xFFFFFFFFu && me->tex + 1u < RQC_TEX_MAX ? (float)(me->tex + 1u) : 0.0f;      /* 0: the white texture */
            int animated = me->anim[0] != 0xFFFFFFFFu || ((me->flags >> 8) & 255u) || (me->flags & 32u);
            if (animated && nan < m->nmesh) anm[nan++] = i;
            u32 gen = i + 1u;
            int cut = m->tex_cut && me->tex != 0xFFFFFFFFu && m->tex_cut[me->tex];
            if (glass && ngt * 3u + count > cap_gix) {   /* (the readable glass's split meshes ate the slack) */
                u32 nc = (ngt * 3u + count) * 2u; u32 *t2 = (u32 *)realloc(gix, (size_t)nc * sizeof *t2);
                if (!t2) goto oom;
                gix = t2; cap_gix = nc;
            }
            u32 *out = glass ? gix + ngt * 3u : cut ? ic + nic : is + nis;
            u32 vbase = nv;
            for (u32 k = 0; k < count; k++) {
                u32 o = ix[first + k];
                if (rstamp[o] != gen) {
                    rstamp[o] = gen; remap[o] = nv;
                    if (nv >= cap_v) goto oom;
                    float vf[9]; memcpy(vf, uq + (size_t)o * CPIT_VTX, sizeof vf);
                    rqc_vtx *w = &vtx[nv];
                    for (int j = 0; j < 3; j++) { float t = (vf[j] - lo[j]) / s * 65535.0f + 0.5f; w->q[j] = (u16)(t < 0.0f ? 0.0f : t > 65535.0f ? 65535.0f : t); }
                    w->id = (u16)i;
                    for (int j = 0; j < 3; j++) { float t = vf[3 + j] * 127.0f; w->n[j] = (s8)(t >= 0.0f ? t + 0.5f : t - 0.5f); }
                    {   float a = vf[8]; if (a < 0.0f) a = 0.0f; if (a > 1.0f) a = 1.0f; w->ao = (s8)(a * 127.0f + 0.5f); }
                    {   float t = (vf[6] - ulo[0]) / us * 65535.0f + 0.5f; w->uv[0] = (u16)(t < 0.0f ? 0.0f : t > 65535.0f ? 65535.0f : t);
                        t = (vf[7] - ulo[1]) / vs * 65535.0f + 0.5f; w->uv[1] = (u16)(t < 0.0f ? 0.0f : t > 65535.0f ? 65535.0f : t); }
                    if (glass) { memcpy(gv + 6u * ngv, vf, 12); memcpy(gv + 6u * ngv + 3u, vf + 3, 12); ngv++; }
                    nv++;
                }
                *out++ = remap[o];
            }
            (void)vbase;
            if (glass && (me->flags & CPIT_F_GLASSFX) && rqgx_split_cfg() > 0.0f && rqgx_on()) {   /* (R-57: per-vertex terms need short edges) */
                rqgx_sp S = { &vtx, &nv, &cap_v, &gv, &ngv, &cap_gv, &gix, &cap_gix, gv0 };
                u32 nt0 = count / 3u, nt1 = rqgx_split_mesh(&S, ngt * 3u, nt0, rqgx_split_cfg());
                if (nt1 != nt0) ps2_log("cockpit material: the readable glass: mesh %u's %u triangles split to %u (edges up to %.3f m; ACZ_VR_RQ_GLASS_SPLIT)", i, nt0, nt1, (double)rqgx_split_cfg());
                count = nt1 * 3u;
            }
            if (glass) { gm[ng].mesh = i; gm[ng].ifirst = ngt * 3u; gm[ng].icount = count; ng++; ngt += count / 3u; }
            else if (cut) nic += count;
            else {
                u32 nt = count / 3u, t0 = nis / 3u;
                if (animated) memset(tf + t0, me->bs[3] > 0.5f ? 6 : 2, nt);
                else ncull += rqc_closed(uq, ix, first, nt, is + nis, tf + t0);
                for (u32 t = 0; t < nt; t++) for (int a = 0; a < 3; a++) {
                    float v0[3], v1[3], v2[3];
                    memcpy(v0, uq + (size_t)ix[first + t * 3u] * CPIT_VTX, 12); memcpy(v1, uq + (size_t)ix[first + t * 3u + 1u] * CPIT_VTX, 12); memcpy(v2, uq + (size_t)ix[first + t * 3u + 2u] * CPIT_VTX, 12);
                    tc[(t0 + t) * 3u + (u32)a] = (v0[a] + v1[a] + v2[a]) * (1.0f / 3.0f);
                }
                if (!animated && m->nap) {                          /* within 8 cm of the combiner's bounding spheres */
                    int nr = 0;
                    for (u32 a = 0; a < m->nap && !nr; a++) {
                        const float *pb = m->mesh[m->ap_idx[a]].bs, *mb = me->bs;
                        float d[3] = { pb[0] - mb[0], pb[1] - mb[1], pb[2] - mb[2] }, r = pb[3] + mb[3] + 0.08f;
                        nr = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < r * r;
                    }
                    if (nr) {                                       /* (by triangle: one part can be the whole panel) */
                        u32 n0 = nin;
                        for (u32 t = 0; t < nt; t++) {
                            const float *cc = tc + (t0 + t) * 3u;
                            for (u32 a = 0; a < m->nap; a++) {
                                const float *pb = m->mesh[m->ap_idx[a]].bs;
                                float d[3] = { pb[0] - cc[0], pb[1] - cc[1], pb[2] - cc[2] }, r = pb[3] + 0.08f;
                                if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < r * r) { memcpy(inr + nin, is + nis + t * 3u, 12); nin += 3u; break; }
                            }
                        }
                        if (nin > n0) nnm++;
                    }
                }
                nis += count;
            }
        }
    }
    {   u32 ct = 0, ctn = 0, cu = 0, cun = 0, tr = 0; float amin = 2.0f, amax = -1.0f;          /* (the dry glass's by-need test: what it can rule out) */
        for (u32 g = 0; g < ng; g++) {
            const cpit_mesh *me = &m->mesh[gm[g].mesh]; u32 t = gm[g].icount / 3u; tr += t;
            int tx = me->tex != 0xFFFFFFFFu, op = me->color[3] >= 0.012f;
            if (tx && op) ct += t; else if (tx) ctn += t; else if (op) cu += t; else cun += t;
            if (me->color[3] < amin) amin = me->color[3];
            if (me->color[3] > amax) amax = me->color[3];
        }
        ps2_log("cockpit material: glass %u tris: textured + colour alpha %u, textured + alpha 0 %u, untextured + colour alpha %u, clear %u; colour alpha %.3f .. %.3f", tr, ct, ctn, cu, cun, (double)amin, (double)amax);
    }
    ps2_log("cockpit material: %s: %u meshes (LOD %d): solid %u tris, cut %u tris, glass %u tris in %u meshes, %u vertices (%u x 16 bytes)",
            m->path, m->nmesh, lodl, nis / 3u, nic / 3u, ngt, ng, nv, nv);
    q->nvtx = nv; q->n_solid = nis; q->n_cut = nic; q->nmesh = m->nmesh;
    q->tris_solid = nis / 3u; q->tris_cut = nic / 3u;
    ps2_log("cockpit material: back-face culling may drop %u of %u solid triangles' far sides (closed components the head cannot be inside)", ncull, nis / 3u);
    /* (ACZ_VR_RQ_COCKPIT_SM_LOD) the shadow's casters from a coarser level: its solid and cut indices, over vertices of their own
       (quantised on each mesh's row as drawn; a vertex outside the row's box is clamped to it) */
    int sml = rqc_smlod_cfg() < 0 ? m->lod_n : rqc_smlod_cfg();
    if (sml > m->lod_n) sml = m->lod_n;
    if (sml > lodl) {
        size_t cap = 0;
        for (u32 i = 0; i < m->nmesh; i++) {
            const cpit_mesh *me = &m->mesh[i];
            if ((me->flags & 1u) || !me->count) continue;
            cap += me->lcount[sml - 1] != CPIT_LOD_NONE ? me->lcount[sml - 1] : me->count;
        }
        ish = (u32 *)malloc(cap * sizeof(u32) + 4u);
        for (u32 i = 0; ish && i < m->nmesh; i++) {
            const cpit_mesh *me = &m->mesh[i];
            if ((me->flags & 1u) || !me->count) continue;
            u32 first = me->first, count = me->count, gen = m->nmesh + 1u + i;
            if (me->lcount[sml - 1] != CPIT_LOD_NONE) { first = me->lfirst[sml - 1]; count = me->lcount[sml - 1]; }
            const rqc_row *row = &rows[i];
            for (u32 k = 0; k < count; k++) {
                u32 o = ix[first + k];
                if (rstamp[o] != gen) {
                    if (nv >= cap_v) { free(ish); ish = NULL; nsh = 0; break; }
                    rstamp[o] = gen; remap[o] = nv;
                    float vf[9]; memcpy(vf, uq + (size_t)o * CPIT_VTX, sizeof vf);
                    rqc_vtx *w = &vtx[nv];
                    for (int j = 0; j < 3; j++) { float t = (vf[j] - row->qp[j]) / row->qp[3] * 65535.0f + 0.5f; w->q[j] = (u16)(t < 0.0f ? 0.0f : t > 65535.0f ? 65535.0f : t); }
                    w->id = (u16)i;
                    for (int j = 0; j < 3; j++) { float t = vf[3 + j] * 127.0f; w->n[j] = (s8)(t >= 0.0f ? t + 0.5f : t - 0.5f); }
                    {   float a = vf[8]; if (a < 0.0f) a = 0.0f; if (a > 1.0f) a = 1.0f; w->ao = (s8)(a * 127.0f + 0.5f); }
                    {   float t = (vf[6] - row->qt[0]) / row->qt[2] * 65535.0f + 0.5f; w->uv[0] = (u16)(t < 0.0f ? 0.0f : t > 65535.0f ? 65535.0f : t);
                        t = (vf[7] - row->qt[1]) / row->qt[3] * 65535.0f + 0.5f; w->uv[1] = (u16)(t < 0.0f ? 0.0f : t > 65535.0f ? 65535.0f : t); }
                    nv++;
                }
                ish[nsh++] = remap[o];
            }
        }
        if (ish) ps2_log("cockpit material: the sun shadow's casters at mesh LOD level %d: %u triangles instead of %u (ACZ_VR_RQ_COCKPIT_SM_LOD; %u vertices in all)",
                         sml, nsh / 3u, (nis + nic) / 3u, nv);
        q->nvtx = nv;
    }
    /* the GPU's copies */
    u32 nidx = nis + nic + nin + nsh;
    q->near_first = nis + nic; q->near_count = nin;
    q->sm_first = nis + nic + nin; q->sm_count = nsh; q->sm_lod = nsh ? sml : lodl;
    if (m->nap) ps2_log("cockpit material: %u solid parts (%u tris) round the HUD's combiner: their depth back before the glass", nnm, nin / 3u);
    VkDeviceSize vb_size = (VkDeviceSize)nv * sizeof(rqc_vtx), ib_size = (VkDeviceSize)nidx * sizeof(u32);
    if (make_buffer(vb_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &q->vb, &q->vbm, NULL) != 0
        || make_buffer(ib_size ? ib_size : 4u, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &q->ib, &q->ibm, NULL) != 0) {
        ps2_log("cockpit material: no buffers"); goto oom;
    }
    {   u32 *ia = (u32 *)malloc(ib_size ? (size_t)ib_size : 4u);
        if (!ia) goto oom;
        {   /* the solid triangles by list and sector (the centre's azimuth round the design eye, -180..180 degrees: straight ahead is the middle) */
            u32 nts = nis / 3u, w = 0, nalw = 0;
            u8 *sec = (u8 *)malloc((size_t)nts + 1u);
            if (!sec) { free(ia); goto oom; }
            for (u32 t = 0; t < nts; t++) {
                int c = (int)floorf((atan2f(tc[t * 3u], -tc[t * 3u + 2u]) + 3.14159265f) / 6.2831853f * (float)RQC_CHUNKS);
                sec[t] = (u8)(c < 0 ? 0 : c >= (int)RQC_CHUNKS ? (int)RQC_CHUNKS - 1 : c);
                float cp[3][3], rmin = 1e30f, emax = 0.0f;
                for (int j = 0; j < 3; j++) {
                    const rqc_vtx *v = &vtx[is[t * 3u + (u32)j]];
                    const float *qp = rows[v->id].qp;
                    for (int e = 0; e < 3; e++) cp[j][e] = (float)v->q[e] * (qp[3] / 65535.0f) + qp[e];
                    float r = cp[j][0] * cp[j][0] + cp[j][1] * cp[j][1] + cp[j][2] * cp[j][2];
                    if (r < rmin) rmin = r;
                }
                for (int j = 0; j < 3; j++) {
                    const float *a = cp[j], *b = cp[(j + 1) % 3];
                    float e = (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]);
                    if (e > emax) emax = e;
                }
                {   /* (the azimuth of a triangle over or under the head says nothing: the floor and the canopy arch) */
                    float h2 = tc[t * 3u] * tc[t * 3u] + tc[t * 3u + 2u] * tc[t * 3u + 2u], v2 = tc[t * 3u + 1u] * tc[t * 3u + 1u];
                    if (rmin < 0.09f || emax > 0.16f || (tf[t] & 4u) || h2 < 0.0625f || h2 < v2 * 0.33f) { sec[t] = (u8)RQC_CHUNKS; nalw++; }
                }
            }
            for (int list = 0; list < 3; list++) for (u32 c = 0; c <= RQC_CHUNKS; c++) {
                float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
                q->ch[list][c].first = w * 3u;
                for (u32 t = 0; t < nts; t++) {
                    if (((tf[t] & 2u) ? 2 : (tf[t] & 1u) ? 0 : 1) != list || sec[t] != c) continue;
                    ia[w * 3u] = is[t * 3u]; ia[w * 3u + 1u] = is[t * 3u + 1u]; ia[w * 3u + 2u] = is[t * 3u + 2u]; w++;
                    for (int j = 0; j < 3; j++) {                      /* (rest positions of its corners, as the vertex stage rebuilds them) */
                        const rqc_vtx *v = &vtx[is[t * 3u + (u32)j]];
                        const float *qp = rows[v->id].qp;
                        for (int e = 0; e < 3; e++) { float x = (float)v->q[e] * (qp[3] / 65535.0f) + qp[e]; if (x < lo[e]) lo[e] = x; if (x > hi[e]) hi[e] = x; }
                    }
                }
                q->ch[list][c].count = w * 3u - q->ch[list][c].first;
                for (int e = 0; e < 3; e++) { q->ch[list][c].c[e] = 0.5f * (lo[e] + hi[e]); q->ch[list][c].h[e] = 0.5f * (hi[e] - lo[e]); }
            }
            q->anim_count = q->ch[2][0].first;   /* (the first index of the animated list: the log) */
            q->tris_cull = ncull;
            ps2_log("cockpit material: sectors: %u of %u solid triangles always drawn (near the eye or very long), %u animated", nalw, nts, (w * 3u - q->ch[2][0].first) / 3u);
            free(sec);
        }
        memcpy(ia + nis, ic, (size_t)nic * sizeof(u32));
        if (nin) memcpy(ia + nis + nic, inr, (size_t)nin * sizeof(u32));
        if (nsh) memcpy(ia + nis + nic + nin, ish, (size_t)nsh * sizeof(u32));
        for (int pass2 = 0; pass2 < 2; pass2++) {
            const u8 *src = pass2 ? (const u8 *)ia : (const u8 *)vtx;
            size_t total = pass2 ? (size_t)ib_size : (size_t)vb_size, done = 0;
            while (done < total) {
                size_t c = total - done > ulim ? ulim : total - done;
                memcpy(st->sptr, src + done, c);
                cpit_up u = { st, NULL, 0, NULL, pass2 ? q->ib : q->vb, (VkDeviceSize)done, (VkDeviceSize)c };
                cpit_submit_once(cpit_rec_buf, &u);
                done += c;
            }
        }
        free(ia);
    }
    m->vram += vb_size + ib_size;
    if (make_buffer((VkDeviceSize)m->nmesh * sizeof(rqc_row) + 64u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &q->rows, &q->rowsm, &q->rowsp) != 0) goto oom;
    memcpy(q->rowsp, rows, (size_t)m->nmesh * sizeof(rqc_row));
    if ((q->rowsh = (rqc_row *)malloc((size_t)(m->nmesh ? m->nmesh : 1u) * sizeof(rqc_row))) != NULL) memcpy(q->rowsh, rows, (size_t)m->nmesh * sizeof(rqc_row));
    VkDeviceSize asz = (VkDeviceSize)m->nmesh * sizeof(rqc_anim) + 64u;
    VkDeviceSize gsz = (VkDeviceSize)ngt * 6u * sizeof(u32) + 64u;               /* (twice: the dry list + the wet list; lean2's flecks, a list of their own, behind them) */
    for (u32 k = 0; k < FRAMES_IN_FLIGHT; k++) {
        if (make_buffer(asz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &q->animb[k], &q->animm[k], &q->animp[k]) != 0
            || make_buffer(sizeof(rqc_frame) + 64u, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &q->fb[k], &q->fm[k], &q->fp[k]) != 0
            || make_buffer(gsz, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &q->gib[k], &q->gim[k], &q->gip[k]) != 0) goto oom;
        /* every row identity, shown */
        rqc_anim *a = (rqc_anim *)q->animp[k];
        for (u32 i = 0; i < m->nmesh; i++) { memset(&a[i], 0, sizeof a[i]); a[i].m[0] = a[i].m[5] = a[i].m[10] = 1.0f; a[i].vis = 1.0f; }
    }
    /* descriptors */
    VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, RQC_TEX_MAX }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2u * FRAMES_IN_FLIGHT }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, FRAMES_IN_FLIGHT } };
    VkDescriptorPoolCreateInfo pci; memset(&pci, 0, sizeof pci);
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; pci.maxSets = 1u + FRAMES_IN_FLIGHT; pci.poolSizeCount = 3; pci.pPoolSizes = ps;
    if (vkCreateDescriptorPool(dev, &pci, NULL, &q->pool) != VK_SUCCESS) goto oom;
    VkDescriptorSetAllocateInfo ai; memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; ai.descriptorPool = q->pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &rqc.texl;
    if (vkAllocateDescriptorSets(dev, &ai, &q->texset) != VK_SUCCESS) goto oom;
    {   VkDescriptorImageInfo *ii = (VkDescriptorImageInfo *)calloc(m->ntex + 1u, sizeof *ii);
        if (!ii) goto oom;
        ii[0].sampler = rqc.smp; ii[0].imageView = white_view; ii[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        for (u32 i = 0; i < m->ntex; i++) {
            ii[i + 1u].sampler = rqc.smp; ii[i + 1u].imageView = m->tex[i].view ? m->tex[i].view : white_view;
            ii[i + 1u].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        VkWriteDescriptorSet w; memset(&w, 0, sizeof w);
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = q->texset; w.descriptorCount = m->ntex + 1u < RQC_TEX_MAX ? m->ntex + 1u : RQC_TEX_MAX;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = ii;
        vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
        free(ii);
    }
    {   VkDescriptorSetLayout L[FRAMES_IN_FLIGHT];
        for (u32 k = 0; k < FRAMES_IN_FLIGHT; k++) L[k] = rqc.fl;
        ai.descriptorSetCount = FRAMES_IN_FLIGHT; ai.pSetLayouts = L;
        if (vkAllocateDescriptorSets(dev, &ai, q->fset) != VK_SUCCESS) goto oom;
        for (u32 k = 0; k < FRAMES_IN_FLIGHT; k++) {
            VkDescriptorBufferInfo bi[3]; memset(bi, 0, sizeof bi);
            bi[0].buffer = q->rows; bi[0].range = VK_WHOLE_SIZE;
            bi[1].buffer = q->animb[k]; bi[1].range = VK_WHOLE_SIZE;
            bi[2].buffer = q->fb[k]; bi[2].range = VK_WHOLE_SIZE;
            VkWriteDescriptorSet wr[3]; memset(wr, 0, sizeof wr);
            for (u32 j = 0; j < 3; j++) {
                wr[j].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[j].dstSet = q->fset[k]; wr[j].dstBinding = j; wr[j].descriptorCount = 1;
                wr[j].descriptorType = j == 2u ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[j].pBufferInfo = &bi[j];
            }
            vkUpdateDescriptorSets(dev, 3, wr, 0, NULL);
        }
    }
    /* per glass vertex, metres along the glass (across, along the flow): a flat pane by its own axes, a curved one (a canopy) by the arc round the aircraft's axis */
    if (ngv && (rqc_wet_cfg() & 1)) {
        rqw_bake(st);
        float *gl = (float *)calloc(ngv, 5u * sizeof(float));
        void *gp = NULL;
        if (gl) for (u32 g = 0; g < ng; g++) {
            const u32 *gi = gix + gm[g].ifirst; u32 cnt = gm[g].icount;
            float N[3] = { 0, 0, 0 }, ymin = 1e30f;
            for (u32 k = 0; k < cnt; k++) { const float *v = gv + 6u * (gi[k] - gv0); for (int a = 0; a < 3; a++) N[a] += v[3 + a]; if (v[1] < ymin) ymin = v[1]; }
            float nl = sqrtf(N[0] * N[0] + N[1] * N[1] + N[2] * N[2]); if (nl < 1e-6f) nl = 1.0f;
            N[0] /= nl; N[1] /= nl; N[2] /= nl;
            float flat = 1.0f, Rs = 0.0f;
            for (u32 k = 0; k < cnt; k++) {
                const float *v = gv + 6u * (gi[k] - gv0);
                float c = fabsf(v[3] * N[0] + v[4] * N[1] + v[5] * N[2]); if (c < flat) flat = c;
                Rs += sqrtf(v[0] * v[0] + (v[1] - ymin) * (v[1] - ymin));
            }
            Rs = cnt ? Rs / (float)cnt : 1.0f;
            float Vm[3] = { -N[0] * N[2], -N[1] * N[2], 1.0f - N[2] * N[2] };
            float vl = sqrtf(Vm[0] * Vm[0] + Vm[1] * Vm[1] + Vm[2] * Vm[2]);
            if (vl < 1e-4f) { Vm[0] = 0.0f; Vm[1] = 1.0f; Vm[2] = 0.0f; vl = 1.0f; }
            Vm[0] /= vl; Vm[1] /= vl; Vm[2] /= vl;
            float Um[3] = { N[1] * Vm[2] - N[2] * Vm[1], N[2] * Vm[0] - N[0] * Vm[2], N[0] * Vm[1] - N[1] * Vm[0] };
            for (u32 k = 0; k < cnt; k++) {
                u32 id = gi[k] - gv0; const float *v = gv + 6u * id;
                float *g5 = gl + 5u * id;
                if (flat > 0.9f) { g5[0] = v[0] * Um[0] + v[1] * Um[1] + v[2] * Um[2]; g5[1] = v[0] * Vm[0] + v[1] * Vm[1] + v[2] * Vm[2]; g5[2] = Um[0]; g5[3] = Um[1]; g5[4] = Um[2]; }
                else {
                    float ry = v[1] - ymin + 1e-4f, rl = sqrtf(v[0] * v[0] + ry * ry);
                    g5[0] = atan2f(v[0], ry) * Rs; g5[1] = v[2];
                    g5[2] = ry / rl; g5[3] = -v[0] / rl; g5[4] = 0.0f;         /* (the way the arc grows) */
                }
            }
        }
        if (gl && make_buffer((VkDeviceSize)ngv * 20u, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &q->gvb, &q->gvbm, &gp) == 0) memcpy(gp, gl, (size_t)ngv * 20u);
        free(gl);
    }
    q->anm = anm; q->nan = nan; q->gm = gm; q->ng = ng; q->ngtri = ngt; q->gix = gix; q->gv = gv; q->gvx0 = gv0; q->ngv = ngv;
    anm = NULL; gm = NULL; gix = NULL; gv = NULL;   /* (owned by the model now) */
    if (ngt && q->gvb) {                          /* the readable glass (rq_glass.inc): the need test's states, the wear texture, the dust */
        q->gxst = (u8 *)calloc(ngt, 1);
        q->gxtc = (float *)malloc((size_t)ngt * 6u * sizeof(float));
        if (q->gxtc) for (u32 t = 0; t < ngt; t++) {
            const u32 *ti = q->gix + t * 3u; float *o = q->gxtc + t * 6u, nn[3] = { 0, 0, 0 };
            o[0] = o[1] = o[2] = 0.0f;
            for (int j = 0; j < 3; j++) { const float *v = q->gv + 6u * (ti[j] - q->gvx0); for (int a = 0; a < 3; a++) { o[a] += v[a] * (1.0f / 3.0f); nn[a] += v[3 + a]; } }
            float nl = sqrtf(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]); if (nl < 1e-5f) nl = 1.0f;
            o[3] = nn[0] / nl; o[4] = nn[1] / nl; o[5] = nn[2] / nl;
        }
        if (rqgx_on()) { rqgx_wear_bake(st); rqgx_specks_build(q, m); }
    }
    {   u32 lv = 0;                                   /* (P-5: what rqc_anim_fill and the glass lists do a frame, for the CPU's budget) */
        for (u32 n = 0; n < q->nan; n++) { const cpit_mesh *am = &m->mesh[q->anm[n]];
            for (int k = 0; k < 4 && am->anim[k] != 0xFFFFFFFFu; k++) lv += m->anim[am->anim[k]].type != 1u && m->anim[am->anim[k]].type != 2u; }
        ps2_log("cockpit material: per frame %u animation rows (%u matrix levels), %u glass meshes, %u glass triangles", q->nan, lv, q->ng, q->ngtri);
    }
    q->ok = 1;
    m->rqc = q;
    q = NULL;
oom:
    if (q) { ps2_log("cockpit material: out of memory"); free(q); }
    free(vtx); free(is); free(ic); free(remap); free(rstamp); free(rows); free(gm); free(gix); free(anm); free(gv); free(tf); free(tc); free(inr);
    free(qbox); free(ish);
}

/* ---- the pipelines of the eye pass (made once, when the pass exists) ---- */
static VkPipeline rqc_mk(VkPipelineShaderStageCreateInfo *st, u32 nst, const VkPipelineVertexInputStateCreateInfo *vin, const VkPipelineRasterizationStateCreateInfo *rs,
                         const VkPipelineDepthStencilStateCreateInfo *dss, const VkPipelineColorBlendStateCreateInfo *cb) {
    VkPipelineInputAssemblyStateCreateInfo ia; memset(&ia, 0, sizeof ia); ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vs; memset(&vs, 0, sizeof vs); vs.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vs.viewportCount = 1; vs.scissorCount = 1;
    VkPipelineMultisampleStateCreateInfo ms; memset(&ms, 0, sizeof ms); ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = rq_samples();
    static const VkDynamicState dy[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BOUNDS };
    VkPipelineDynamicStateCreateInfo dsi; memset(&dsi, 0, sizeof dsi); dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; dsi.dynamicStateCount = dss->depthBoundsTestEnable ? 3 : 2; dsi.pDynamicStates = dy;
    VkGraphicsPipelineCreateInfo gp; memset(&gp, 0, sizeof gp);
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; gp.stageCount = nst; gp.pStages = st; gp.pVertexInputState = vin; gp.pInputAssemblyState = &ia; gp.pViewportState = &vs;
    gp.pRasterizationState = rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = dss; gp.pColorBlendState = cb; gp.pDynamicState = &dsi; gp.layout = rqc.layout; gp.renderPass = rqe.rp;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev, pipe_cache, 1, &gp, NULL, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
    return p;
}
static VkPipelineVertexInputStateCreateInfo rqc_vin(VkVertexInputBindingDescription *bd, VkVertexInputAttributeDescription *at) {
    bd->binding = 0; bd->stride = sizeof(rqc_vtx); bd->inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    memset(at, 0, 3u * sizeof *at);
    at[0].location = 0; at[0].format = VK_FORMAT_R16G16B16A16_UINT; at[0].offset = 0;
    at[1].location = 1; at[1].format = VK_FORMAT_R8G8B8A8_SNORM; at[1].offset = 8;
    at[2].location = 2; at[2].format = VK_FORMAT_R16G16_UINT; at[2].offset = 12;
    VkPipelineVertexInputStateCreateInfo v; memset(&v, 0, sizeof v);
    v.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    v.vertexBindingDescriptionCount = 1; v.pVertexBindingDescriptions = bd; v.vertexAttributeDescriptionCount = 3; v.pVertexAttributeDescriptions = at;
    return v;
}
static int rqc_pipes(void) {
    if (rqc.rp_for == rqe.rp && rqc.solid[0]) return 1;
    if (rqc.tried) return 0;
    rqc.tried = 1;
    const u64 rqpw_t0 = vk_mono_ns();
    if (!rqe.rp || !rqc.vs || !rqc.fs || !rqc_layouts()) { ps2_log("cockpit material: no pass or shaders: the old cockpit"); return 0; }
    rqc.sm_on = cpit.sm_ok;
    /* (PS2_RQ_ZPRE: the solid parts also write the coverage mark into alpha: rq_zpre.inc) */
    const VkPipelineColorBlendAttachmentState noblend = { VK_FALSE, 0, 0, 0, 0, 0, 0, VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | (rqz_cfg() ? VK_COLOR_COMPONENT_A_BIT : 0u) };
    VkPipelineColorBlendStateCreateInfo cb; memset(&cb, 0, sizeof cb); cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cb.attachmentCount = 1; cb.pAttachments = &noblend;
    VkPipelineDepthStencilStateCreateInfo dss; memset(&dss, 0, sizeof dss); dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    dss.depthTestEnable = VK_TRUE; dss.depthWriteEnable = VK_TRUE; dss.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; dss.maxDepthBounds = 1.0f;
    VkPipelineRasterizationStateCreateInfo rs; memset(&rs, 0, sizeof rs); rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkVertexInputBindingDescription bd; VkVertexInputAttributeDescription at[3];
    VkPipelineVertexInputStateCreateInfo vin = rqc_vin(&bd, at);
    struct { int cut, night, sm, dbg; } sp;
    VkSpecializationMapEntry me[4] = { { 0, 0, 4 }, { 1, 4, 4 }, { 2, 8, 4 }, { 3, 12, 4 } };
    VkSpecializationInfo si; memset(&si, 0, sizeof si); si.mapEntryCount = 4; si.pMapEntries = me; si.dataSize = sizeof sp; si.pData = &sp;
    sp.dbg = (RQC_DBG(1) ? 1 : 0) | (RQC_DBG(2) ? 2 : 0) | (RQC_DBG(4) ? 4 : 0);
    VkPipelineShaderStageCreateInfo st[2]; memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = rqc.vs; st[0].pName = "main"; st[0].pSpecializationInfo = &si;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = RQC_DBG(8) && rqc.fsmp ? rqc.fsmp : rqc.fs; st[1].pName = "main"; st[1].pSpecializationInfo = &si;
    sp.sm = rqc.sm_on;
    /* (ACZ_VR_RQ_COCKPIT_NIGHT_FS, R-85; default 1: the night pipelines take the panel's area per fragment, NIGHT 2; 0: per vertex, as before) */
    int nfs; { const char *e = getenv("ACZ_VR_RQ_COCKPIT_NIGHT_FS"); nfs = !(e && *e == '0');
               ps2_log("cockpit material: the night panel's area per %s (ACZ_VR_RQ_COCKPIT_NIGHT_FS)", nfs ? "fragment" : "vertex"); }
    for (int night = 0; night < 2; night++) {
        sp.night = night ? (nfs ? 2 : 1) : 0; sp.cut = 0; rqc.solid[night] = rqc_mk(st, 2, &vin, &rs, &dss, &cb);
        {   VkPipelineRasterizationStateCreateInfo rc = rs; rc.cullMode = VK_CULL_MODE_BACK_BIT; rc.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rqc.solid_c[night] = rqc_mk(st, 2, &vin, &rc, &dss, &cb); }
        sp.cut = 1;                   rqc.cut[night] = rqc_mk(st, 2, &vin, &rs, &dss, &cb);
        if (!rqc.solid[night] || !rqc.cut[night]) { ps2_log("cockpit material: pipelines failed"); return 0; }
    }
    {   /* depth only: rqc_depth_back */
        VkPipelineColorBlendAttachmentState zb; memset(&zb, 0, sizeof zb);
        VkPipelineColorBlendStateCreateInfo zc = cb; zc.pAttachments = &zb;
        sp.night = 0; sp.cut = 0;
        rqc.dback = rqc_mk(st, 1, &vin, &rs, &dss, &zc);
    }
    /* the glass: premultiplied blending, no depth write, RGB only (the game's destination alpha stays) */
    if (rqc.gvs && rqc.gfs) {
        VkPipelineColorBlendAttachmentState gb; memset(&gb, 0, sizeof gb);
        gb.blendEnable = VK_TRUE; gb.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; gb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; gb.colorBlendOp = VK_BLEND_OP_ADD;
        gb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; gb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; gb.alphaBlendOp = VK_BLEND_OP_ADD;
        gb.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
        VkPipelineColorBlendStateCreateInfo gc = cb; gc.pAttachments = &gb;
        VkPipelineDepthStencilStateCreateInfo gd = dss; gd.depthWriteEnable = VK_FALSE;
        VkPipelineShaderStageCreateInfo gs[2]; memcpy(gs, st, sizeof gs);
        gs[0].module = rqc.gvs; gs[0].pSpecializationInfo = NULL; gs[1].module = rqc.gfs; gs[1].pSpecializationInfo = NULL;
        rqc.glass = rqc_mk(gs, 2, &vin, &rs, &gd, &gc);
        rqgx_pipes();                             /* (the readable glass and the cracks: rq_glass.inc) */
        if (rqc.wvs && rqc.wfs) {                 /* the wet canopy's own pipeline (the world copy read, ps2_video.c cockpit.frag's water) */
            VkPushConstantRange pr; memset(&pr, 0, sizeof pr); pr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT; pr.size = sizeof(cpit_push);
            VkDescriptorSetLayout sl[3] = { rqc.texl, deslayout, rqc.fl };
            VkPipelineLayoutCreateInfo pl; memset(&pl, 0, sizeof pl);
            pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pl.setLayoutCount = 3; pl.pSetLayouts = sl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pr;
            if (!rqc.wlayout && vkCreatePipelineLayout(dev, &pl, NULL, &rqc.wlayout) != VK_SUCCESS) rqc.wlayout = VK_NULL_HANDLE;
            if (rqc.wlayout) {
                VkPipelineLayout keep = rqc.layout;
                gs[0].module = rqc.wvs; gs[1].module = rqc.wfs;
                rqc.layout = rqc.wlayout;
                rqc.wet = rqc_mk(gs, 2, &vin, &rs, &gd, &gc);
                if (rqc_wet_cfg() == 3 && rqc.ffs) {
                    VkPipelineShaderStageCreateInfo fsg[2]; memcpy(fsg, gs, sizeof fsg); fsg[1].module = rqc.ffs;
                    rqc.fleck = rqc_mk(fsg, 2, &vin, &rs, &gd, &gc);
                }
                rqc.layout = keep;
            }
        }
    }
    /* the lean wet canopy: a pipeline per combination of active terms (specialised: the driver compiles only their code) */
    if (rqc.lwvs && rqc.lwfs && rqw.ok && rqc.glass) {
        VkDescriptorSetLayout sl[4] = { rqc.texl, deslayout, rqc.fl, rqw.dsl };
        VkPipelineLayoutCreateInfo pl; memset(&pl, 0, sizeof pl); pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pl.setLayoutCount = 4; pl.pSetLayouts = sl;
        if (!rqc.lwlayout && vkCreatePipelineLayout(dev, &pl, NULL, &rqc.lwlayout) != VK_SUCCESS) rqc.lwlayout = VK_NULL_HANDLE;
        VkVertexInputBindingDescription wbd[2]; memset(wbd, 0, sizeof wbd);
        wbd[0].binding = 0; wbd[0].stride = sizeof(rqc_vtx); wbd[1].binding = 1; wbd[1].stride = 20u;
        VkVertexInputAttributeDescription wat[5]; memcpy(wat, at, 3u * sizeof at[0]);
        memset(&wat[3], 0, 2u * sizeof wat[3]); wat[3].location = 3; wat[3].binding = 1; wat[3].format = VK_FORMAT_R32G32_SFLOAT;
        wat[4].location = 4; wat[4].binding = 1; wat[4].format = VK_FORMAT_R32G32B32_SFLOAT; wat[4].offset = 8u;
        VkPipelineVertexInputStateCreateInfo wvin = vin; wvin.vertexBindingDescriptionCount = 2; wvin.pVertexBindingDescriptions = wbd; wvin.vertexAttributeDescriptionCount = 5; wvin.pVertexAttributeDescriptions = wat;
        VkPipelineColorBlendAttachmentState lb; memset(&lb, 0, sizeof lb);
        lb.blendEnable = VK_TRUE; lb.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; lb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; lb.colorBlendOp = VK_BLEND_OP_ADD;
        lb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; lb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; lb.alphaBlendOp = VK_BLEND_OP_ADD;
        lb.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
        VkPipelineColorBlendStateCreateInfo lc = cb; lc.pAttachments = &lb;
        VkPipelineDepthStencilStateCreateInfo ld = dss; ld.depthWriteEnable = VK_FALSE;
        int wsp[5];
        VkSpecializationMapEntry wme[5] = { { 0, 0, 4 }, { 1, 4, 4 }, { 2, 8, 4 }, { 3, 12, 4 }, { 4, 16, 4 } };
        {   int two = rqc_wet_cfg() == 3;
            const char *e = getenv(two ? "ACZ_VR_RQ_COCKPIT_WET2" : "ACZ_VR_RQ_COCKPIT_WET_Q0");
            wsp[3] = two ? (e && *e ? atoi(e) : 31) : (e && *e == '1');                    /* (lean: the veils as blend factors [test]; lean2: its parts) */
            e = getenv("ACZ_VR_RQ_COCKPIT_WET_BIAS"); wsp[4] = e && *e ? (int)floorf((float)atof(e) * 4.0f + 0.5f) : (two ? -4 : 0); }
        VkSpecializationInfo wsi; memset(&wsi, 0, sizeof wsi); wsi.mapEntryCount = 5; wsi.pMapEntries = wme; wsi.dataSize = sizeof wsp; wsi.pData = wsp;
        VkPipelineShaderStageCreateInfo ws[2]; memcpy(ws, st, sizeof ws);
        ws[0].module = rqc.lwvs; ws[0].pSpecializationInfo = NULL; ws[1].module = rqc.lwfs; ws[1].pSpecializationInfo = &wsi;
        if (rqc_wet_cfg() == 1 && rqc_wet_nospec()) ws[1].pSpecializationInfo = NULL;
        VkPipelineLayout keep = rqc.layout;
        rqc.layout = rqc.lwlayout;
        for (int k = 0; rqc.lwlayout && k < 8; k++) {
            wsp[0] = k & 1; wsp[1] = (k >> 1) & 1; wsp[2] = (k >> 2) & 1;
            rqc.lwet[k] = rqc_mk(ws, 2, &wvin, &rs, &ld, &lc);
        }
        rqc.layout = keep;
    }
    /* the shadow map's merged casters (dynamic rendering, depth only) */
    if (rqc.smvs) {
        VkPipelineShaderStageCreateInfo sg; memset(&sg, 0, sizeof sg);
        sg.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; sg.stage = VK_SHADER_STAGE_VERTEX_BIT; sg.module = rqc.smvs; sg.pName = "main";
        VkPipelineVertexInputStateCreateInfo svi = vin;
        VkVertexInputAttributeDescription sat = at[0];
        svi.vertexAttributeDescriptionCount = 1; svi.pVertexAttributeDescriptions = &sat;
        VkPipelineInputAssemblyStateCreateInfo ia; memset(&ia, 0, sizeof ia); ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vs; memset(&vs, 0, sizeof vs); vs.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vs.viewportCount = 1; vs.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs2 = rs; rs2.depthBiasEnable = VK_TRUE; rs2.depthBiasConstantFactor = 1.5f; rs2.depthBiasSlopeFactor = 2.0f;
        VkPipelineMultisampleStateCreateInfo ms; memset(&ms, 0, sizeof ms); ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo sd = dss; sd.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendStateCreateInfo sb; memset(&sb, 0, sizeof sb); sb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        static const VkDynamicState dy2[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dsi; memset(&dsi, 0, sizeof dsi); dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; dsi.dynamicStateCount = 2; dsi.pDynamicStates = dy2;
        VkPipelineRenderingCreateInfo rci; memset(&rci, 0, sizeof rci); rci.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO; rci.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
        VkGraphicsPipelineCreateInfo gp; memset(&gp, 0, sizeof gp);
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; gp.pNext = &rci; gp.stageCount = 1; gp.pStages = &sg; gp.pVertexInputState = &svi; gp.pInputAssemblyState = &ia; gp.pViewportState = &vs;
        gp.pRasterizationState = &rs2; gp.pMultisampleState = &ms; gp.pDepthStencilState = &sd; gp.pColorBlendState = &sb; gp.pDynamicState = &dsi; gp.layout = rqc.layout;
        if (vkCreateGraphicsPipelines(dev, pipe_cache, 1, &gp, NULL, &rqc.sm) != VK_SUCCESS) rqc.sm = VK_NULL_HANDLE;
    }
    rqc.depth_a = 1.0f - RQC_S; rqc.depth_b = RQC_S * RQC_NEAR;
    rqc.rp_for = rqe.rp;
    rqpw_made("the cockpit material's and the readable glass's pipelines (rqc_pipes)", rqpw_t0);
    ps2_log("vk: cockpit material: the eye pass's pipelines made (samples %d, shadow %d)", (int)rq_samples(), rqc.sm_on);
    return 1;
}

/* ---- each frame ---- */
/* The eye pass's viewport and scissor for every cockpit draw. Dynamic resolution (rqd, PS2_RQ_DYNRES): x and y need nothing here, they come
   through rqe.xf (F->xfx / xfy, copied each frame; the viewport stays the whole image); the scissor stays inside the render area. */
static void rqc_set_view(VkCommandBuffer cmd) {
    VkViewport vp; memset(&vp, 0, sizeof vp); vp.width = (float)rqe.w; vp.height = (float)rqe.h; vp.maxDepth = 1.0f;
    VkRect2D sc; memset(&sc, 0, sizeof sc); sc.offset.x = (int32_t)rqd.vx; sc.offset.y = (int32_t)rqd.vy; sc.extent.width = rqd.vw; sc.extent.height = rqd.vh;
    vkCmdSetViewport(cmd, 0, 1, &vp); vkCmdSetScissor(cmd, 0, 1, &sc);
}
/* the matrix rows of the animated meshes (needles, drums, select) into this frame slot's table */
static void rqc_anim_fill(rqc_model *q, int slot) {
    rqc_anim *a = (rqc_anim *)q->animp[slot];
    if (!a) return;
    for (u32 n = 0; n < q->nan; n++) {
        u32 i = q->anm[n];
        const cpit_mesh *m = &cpit.mesh[i];
        if (rqmh_now) {            /* (PS2_RQ_MAP_HOST: the row in host memory, then one store of its 64 bytes -- cpit_mesh_model reads its
                                      output back once per animation level: 12 floats each from the uncached table before) */
            rqc_anim t;
            cpit_mesh_model(m, t.m);
            float uv[2]; cpit_mesh_uv(m, uv); t.uv[0] = uv[0]; t.uv[1] = uv[1];
            t.vis = cpit_mesh_hidden(m) ? 0.0f : 1.0f;
            if (((m->flags >> 8) & 255u) && mfd_rp.any && mfd_screen_blk((m->flags >> 8) & 255u)) t.vis = 0.0f;
            t.pad = 0.0f;          /* (never written: 0 since the table was made) */
            memcpy(&a[i], &t, sizeof t);
            continue;
        }
        rqc_anim *w = &a[i];
        cpit_mesh_model(m, w->m);
        float uv[2]; cpit_mesh_uv(m, uv); w->uv[0] = uv[0]; w->uv[1] = uv[1];
        w->vis = cpit_mesh_hidden(m) ? 0.0f : 1.0f;
        /* a screen shown as itself by the screens' own draw this frame */
        if (((m->flags >> 8) & 255u) && mfd_rp.any && mfd_screen_blk((m->flags >> 8) & 255u)) w->vis = 0.0f;
    }
}
/* ACZ_VR_RQ_COCKPIT_WET_LIGHT (R-88, Part E of the second flight round): in cloud the lean wet canopy's drops take the cloud's light in
   their turned-back rim and their unresolved lens (rq_cockpit_lwet.frag kl): water is never darker than the cloud behind it. 0..1; 0 =
   cockpit.frag's canopy_water as before (the dim interior in the rim, the lens fading to 0.6) */
static float rqw_light(void) {
    static float k = -1.0f;
    if (k < 0.0f) {
        const char *e = getenv("ACZ_VR_RQ_COCKPIT_WET_LIGHT");
        k = e && *e ? (float)atof(e) : 1.0f;
        if (!(k >= 0.0f)) k = 0.0f;
        if (k > 1.0f) k = 1.0f;
        ps2_log("cockpit material: the lean wet canopy in cloud: the drops take the cloud's light %.2f (ACZ_VR_RQ_COCKPIT_WET_LIGHT)", (double)k);
    }
    return k;
}
/* ACZ_VR_RQ_COCKPIT_WET_UNTINT (R-87): the lean wet canopy's world tap is the 1x feed, last frame's finished eye image with this glass's
   tint and light in it: 1 = the glass's layer taken out of the tap once (rq_cockpit_lwet.frag), 0 = read as it is (before) */
static int rqw_untint(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("ACZ_VR_RQ_COCKPIT_WET_UNTINT"); v = !(e && *e == '0');
                 ps2_log("cockpit material: the lean wet canopy's world tap without the glass's own layer: %s (ACZ_VR_RQ_COCKPIT_WET_UNTINT)", v ? "on" : "off"); }
    return v;
}
/* ACZ_VR_RQ_COCKPIT_SELFLIT (R-85, rqsl_list): the strength of the display pages' own light at night (x their texture x the
   night level; 0 = off: the pages as the PC draws them) */
static float rqsl_k(void) {
    static float k = -1.0f;
    if (k < 0.0f) {
        const char *e = getenv("ACZ_VR_RQ_COCKPIT_SELFLIT");
        k = e && *e ? (float)atof(e) : 0.5f;
        if (!(k >= 0.0f)) k = 0.0f;
        if (k > 2.0f) k = 2.0f;
        ps2_log("cockpit material: display pages lit from within at night: strength %.2f (ACZ_VR_RQ_COCKPIT_SELFLIT; pages by ACZ_VR_RQ_COCKPIT_SELFLIT_MESH)", (double)k);
    }
    return k;
}
/* This frame's material constants. 0 = the frame cannot use the Quest path. */
static int rqc_frame_begin(vk_frame *fr) {
    rqc_model *q = cpit.rqc;
    if (!q || !q->ok || !rqc_pipes()) return 0;
    float H[9], hp[3], hipd, f[2], c[2];
    if (!cpit_pose(H, hp, &hipd, f, c)) return 0;
    int slot = (int)(fr - frames);
    if (!cpit.sm_valid) cpit_anim_update();               /* (else the shadow pass did, this frame) */
    if (!cine_t) {                                        /* (as cpit_draw: the head fade's eyes, the crack sound's side) */
        for (int i = 0; i < 3; i++) { crk.head[i * 3] = H[i]; crk.head[i * 3 + 1] = H[3 + i]; crk.head[i * 3 + 2] = H[6 + i]; }
        crk.head_ok = 1;
        hvf_note(hp, H, hipd);
    }
    rqc_anim_fill(q, slot);
    rqc.fh_on[slot] = (u8)(rqmh_now != 0);               /* (PS2_RQ_MAP_HOST: built in host memory, copied at the end) */
    rqc_frame *F = rqc_F(q, (u32)slot);
    memset(F, 0, sizeof *F);
    for (int i = 0; i < 3; i++) { F->rot[i * 4] = H[i]; F->rot[i * 4 + 1] = H[3 + i]; F->rot[i * 4 + 2] = H[6 + i]; }
    F->head[0] = hp[0]; F->head[1] = hp[1]; F->head[2] = hp[2]; F->head[3] = hipd;
    F->proj[0] = f[0]; F->proj[1] = f[1]; F->proj[2] = c[0] - present_frame_off[0]; F->proj[3] = c[1] - present_frame_off[1];
    F->misc[0] = 2.0f / (float)rt_w; F->misc[1] = 2.0f / (float)rt_h; F->misc[2] = rqc.depth_a; F->misc[3] = rqc.depth_b;
    memcpy(F->xfx, rqe.xf, sizeof F->xfx); F->xfy[0] = rqe.xf[4]; F->xfy[1] = rqe.xf[5];
    F->eyesize[0] = (float)rqe.w; F->eyesize[1] = (float)rqe.h;
    F->eyesize[3] = rqw_untint() ? 1.0f : 0.0f;           /* (ACZ_VR_RQ_COCKPIT_WET_UNTINT, R-87: rq_cockpit_lwet.frag; here, before the block's one store) */
    float bright = cpit.bright;
    int lit = mesh_ok && cpit_lb_ok && cpit.game_light && fr->mcptr;
    if (bright < 0.0f) bright = lit ? 1.0f : 0.85f;
    float ckl = cloud_light(), csun = cloud_sun();
    float fc[3]; cloud_colour(fc);
    F->cloud[0] = fc[0]; F->cloud[1] = fc[1]; F->cloud[2] = fc[2]; F->cloud[3] = ckl;
    F->pack[1] = csun > 0.0f ? (csun > 1.0f ? 1.0f : csun) : 1.0f;     /* (cockpit.frag: the share left in a cloud's shadow; 0 = no effect) */
    F->pack[2] = bright;
    if (lit) {
        const ps2_vk_mesh_block *k = (const ps2_vk_mesh_block *)fr->mcptr + MESH_CPIT_BLOCK;
        const ps2_vk_mesh_block *s = (const ps2_vk_mesh_block *)fr->mcptr + MESH_SCENE_BLOCK;
        ps2_vk_mesh_block kh;                             /* (PS2_RQ_MAP_HOST: the light block read once, whole, from the mapped arena) */
        if (rqc.fh_on[slot]) { memcpy(&kh, k, sizeof kh); k = &kh; }
        memcpy(F->l, k->l, sizeof F->l); memcpy(F->r, k->r, sizeof F->r);
        memcpy(F->dome, s->m, sizeof F->dome);
        if (sl_off_now() || F->dome[3] <= 0.0f) F->dome[3] = 0.0f;
        /* the strongest light / the CPU's key (cockpit.frag: m[1].x = index + 1) */
        float len0, lum[3];
        for (int i = 0; i < 3; i++) {
            float d[3] = { k->l[i], k->l[4 + i], k->l[8 + i] };
            len0 = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            lum[i] = (k->r[i * 4] + k->r[i * 4 + 1] + k->r[i * 4 + 2]) * len0;
        }
        int sun = lum[0] >= lum[1] && lum[0] >= lum[2] ? 0 : (lum[1] >= lum[2] ? 1 : 2);
        int kf = (int)(k->m[4] + 0.5f);
        if (kf >= 1 && kf <= 3) sun = kf - 1;
        F->pack[0] = (float)sun;
        /* flashes */
        F->tub[0] = k->m[0]; F->tub[1] = k->m[1]; F->tub[2] = k->m[2]; F->tub[3] = k->m[3];
        memcpy(F->flp, k->mp, sizeof F->flp); memcpy(F->flc, k->c, sizeof F->flc);
        /* the night panel (bug #197) */
        {   float nq = k->p[15];
            if (nq >= 1.0f && !perf_ab_flipped("nightpanel")) {
                u32 q2 = (u32)(nq + 0.5f);
                F->night[0] = (float)(q2 & 255u) / 127.5f;
                F->night[1] = (float)((q2 >> 18) & 31u) / 31.0f; F->night[2] = (float)((q2 >> 13) & 31u) / 31.0f; F->night[3] = (float)((q2 >> 8) & 31u) / 31.0f;
                F->eyesize[2] = rqsl_k();                 /* (R-85: the self-lit pages' strength, rq_cockpit.vert) */
            }
        }
    } else {                                              /* no lights yet (never in flight): a fixed key light */
        F->pack[0] = 0.0f;
        F->l[0] = 0.15f; F->l[4] = 1.0f; F->l[8] = -0.35f;
        F->r[0] = F->r[1] = F->r[2] = 1.0f - cpit.ambient; F->r[12] = F->r[13] = F->r[14] = cpit.ambient;
    }
    if (cpit.sm_valid && (cpit.su[0] != 0.0f || cpit.su[1] != 0.0f || cpit.su[2] != 0.0f)) {
        memcpy(F->su, cpit.su, sizeof F->su); memcpy(F->sv, cpit.sv, sizeof F->sv); memcpy(F->sd, cpit.sd, sizeof F->sd);
        F->pack[3] = 1.0f;
    } else { F->su[3] = 3.0f; F->sv[3] = 3.0f; F->sd[3] = 0.5f; F->pack[3] = 0.0f; }     /* (outside the map: the border's 'lit') */
    /* rear-view mirrors: the sky's tone from the scene light's bands (a tonal stand-in; the PC's TONE reads our far clouds too) */
    {   float be[3], br[9];
        if (cpit_seq && veil_aircraft(cpit_seq, be, br)) { F->mir[0] = br[3]; F->mir[1] = br[4]; F->mir[2] = br[5]; }
        else { F->mir[0] = 0.0f; F->mir[1] = 1.0f; F->mir[2] = 0.0f; }
        F->mir[3] = 1.0f;
        float amb[3] = { F->r[12], F->r[13], F->r[14] };
        for (int j = 0; j < 3; j++) {
            F->mir[4 + j] = sl.bw[0] >= 0.05f ? sl.band[0][j] : amb[j];
            F->mir[8 + j] = sl.bw[1] >= 0.05f ? sl.band[1][j] : amb[j];
            F->mirg[j] = sl.bw[2] >= 0.05f ? sl.band[2][j] : amb[j] * 0.8f;
        }
    }
    rqgx_frame(F, fr, lit);                               /* (the readable glass's inputs, the cracks' buffer: rq_glass.inc) */
    if (rqc.fh_on[slot]) memcpy(q->fp[slot], F, sizeof *F);   /* (PS2_RQ_MAP_HOST: the block's one store) */
    rqc.n_frames++;
    rqc.fr_draws = rqc.fr_pipes = 0;
    return 1;
}
/* inside the eye pass, the solid cockpit: first thing in the pass (the recipe) or after the world */
static int rqc_draw_solid(VkCommandBuffer cmd, vk_frame *fr) {
    rqc_model *q = cpit.rqc;
    if (!q || !q->ok || !rqc.solid[0]) return 0;
    u32 slot = (u32)(fr - frames);
    rqc_set_view(cmd);
    VkDescriptorSet sets[3] = { q->texset, cpit.sm_ok ? cpit.sm_set : white_set, q->fset[slot] };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqc.layout, 0, 3, sets, 0, NULL);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &q->vb, &zero);
    vkCmdBindIndexBuffer(cmd, q->ib, 0, VK_INDEX_TYPE_UINT32);
    int night = rqc_F(q, slot)->night[0] > 0.0f;
    /* the chunks that can be in view (both eyes: the crop of each, the head's half separation added to the radius) */
    {   const rqc_frame *Fc = rqc_F(q, slot);            /* (PS2_RQ_MAP_HOST: the host copy; before, Fc->head was read from the uncached block per sector and plane) */
        int ccfg = rqc_cull_cfg();
        float xl = 1e9f, xr = -1e9f, yb, yt;
        float px = Fc->proj[0] * Fc->misc[0], cx0 = Fc->proj[2] * Fc->misc[0] - 1.0f, py = Fc->proj[1] * Fc->misc[1], cy0 = Fc->proj[3] * Fc->misc[1] - 1.0f;
        for (int e = 0; e < 2; e++) {
            float A = Fc->xfx[2 * e], B = Fc->xfx[2 * e + 1], n0 = (-1.0f - B) / A, n1 = (1.0f - B) / A, t0 = (n0 - cx0) / px, t1 = (n1 - cx0) / px;
            if (t0 < xl) xl = t0;
            if (t1 > xr) xr = t1;
        }
        {   float A = Fc->xfy[0], B = Fc->xfy[1], n0 = (-1.0f - B) / A, n1 = (1.0f - B) / A; yt = (cy0 - n0) / py; yb = (cy0 - n1) / py; }
        /* the view's planes (unit normals, the head's frame), turned into the cockpit's: a box outside one of them, by more than the half eye separation, is out */
        float nx = sqrtf(1.0f + xl * xl), nxr = sqrtf(1.0f + xr * xr), ny = sqrtf(1.0f + yb * yb), nyt = sqrtf(1.0f + yt * yt);
        const float vpl[5][4] = { { 1.0f / nx, 0.0f, xl / nx, 0.0f }, { -1.0f / nxr, 0.0f, -xr / nxr, 0.0f }, { 0.0f, 1.0f / ny, yb / ny, 0.0f },
                                  { 0.0f, -1.0f / nyt, -yt / nyt, 0.0f }, { 0.0f, 0.0f, -1.0f, -0.05f } };
        float cpl[5][4];
        for (int p = 0; p < 5; p++) {
            for (int j = 0; j < 3; j++) cpl[p][j] = Fc->rot[j] * vpl[p][0] + Fc->rot[4 + j] * vpl[p][1] + Fc->rot[8 + j] * vpl[p][2];
            cpl[p][3] = vpl[p][3];
        }
        float marg = Fc->head[3] + 0.01f;
        float tris = 0.0f, tall = 0.0f;
        int bound = -1;
        for (int list = 0; list < 3; list++) {
            int want = list == 0 && (ccfg & 1) ? 1 : 0;                /* (back faces off: both lists through the plain pipeline) */
            u32 start = 0, run = 0;
            for (u32 c = 0; c <= RQC_CHUNKS + 1u; c++) {
                int in = 0;
                if (c <= RQC_CHUNKS) {
                    if (!q->ch[list][c].count) continue;                /* (an empty chunk does not break a run: it has no indices) */
                    tall += (float)q->ch[list][c].count;
                    in = 1;
                    if ((ccfg & 2) && c < RQC_CHUNKS) {
                        const float *cc = q->ch[list][c].c, *hh = q->ch[list][c].h;
                        for (int p = 0; p < 5 && in; p++) {
                            const float *n = cpl[p];
                            float d = n[0] * (cc[0] - Fc->head[0]) + n[1] * (cc[1] - Fc->head[1]) + n[2] * (cc[2] - Fc->head[2]) + n[3]
                                    + fabsf(n[0]) * hh[0] + fabsf(n[1]) * hh[1] + fabsf(n[2]) * hh[2];
                            if (d < -(list == 2 ? marg + 0.30f : marg)) in = 0;              /* (an animated part: 30 cm for its travel) */
                        }
                    }
                }
                if (c <= RQC_CHUNKS && list < 3) rqc.dbg_in[list][c] = (u8)in;
                if (in) { if (!run) start = q->ch[list][c].first; run += q->ch[list][c].count; tris += (float)q->ch[list][c].count; }
                else if (run) {
                    if (bound != want) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want ? rqc.solid_c[night] : rqc.solid[night]); bound = want; rqc.fr_pipes++; }
                    vkCmdDrawIndexed(cmd, run, 1, start, 0, 0); rqc.fr_draws++;
                    run = 0;
                }
            }
        }
        rqc.tris_seen += (u64)tris; rqc.tris_all += (u64)tall;
        rqc.dbg_r0 = Fc->rot[8]; rqc.dbg_r1 = Fc->rot[9]; rqc.dbg_r2 = Fc->rot[10]; rqc.dbg_xl = xl; rqc.dbg_xr = xr; rqc.dbg_yb = yb; rqc.dbg_yt = yt;
    }
    if (q->n_cut) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqc.cut[night]);
        vkCmdDrawIndexed(cmd, q->n_cut, 1, q->n_solid, 0, 0);
        rqc.fr_pipes++; rqc.fr_draws++;
    }
    return 1;
}

/* ---- the screens and the HUD's combiner mark, from the old 36-byte buffer (cockpit.vert with the crop: rq_cockpit_old.vert) ---- */
static struct {
    VkPipelineLayout layout;                       /* set 0: a combined image (deslayout), push: cpit_push */
    VkPipeline scr, ap, apm;
    VkShaderModule sfs;
} rqco;
static int rqco_pipes(void) {
    if (rqco.layout && (rqco.ap || !depth_bounds_ok)) return 1;
    if (!rqc.ovs) return 0;
    const u64 rqpw_t0 = vk_mono_ns();
    VkPushConstantRange pr; memset(&pr, 0, sizeof pr); pr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT; pr.size = sizeof(cpit_push);
    VkPipelineLayoutCreateInfo lc; memset(&lc, 0, sizeof lc);
    lc.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; lc.setLayoutCount = 1; lc.pSetLayouts = &deslayout; lc.pushConstantRangeCount = 1; lc.pPushConstantRanges = &pr;
    if (!rqco.layout && vkCreatePipelineLayout(dev, &lc, NULL, &rqco.layout) != VK_SUCCESS) return 0;
    if (!rqco.sfs) {
        char spv[1400];
        snprintf(spv, sizeof spv, "%s/cockpit_screen_mv.frag.spv", rqc_shader_base);
        rqco.sfs = load_spv(spv);
    }
    VkVertexInputBindingDescription bd = { 0, CPIT_VTX, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription at[4] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 }, { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12 },
        { 2, 0, VK_FORMAT_R32G32_SFLOAT, 24 }, { 3, 0, VK_FORMAT_R32_SFLOAT, 32 } };
    VkPipelineVertexInputStateCreateInfo vin; memset(&vin, 0, sizeof vin);
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO; vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &bd;
    vin.vertexAttributeDescriptionCount = 4; vin.pVertexAttributeDescriptions = at;
    VkPipelineRasterizationStateCreateInfo rs; memset(&rs, 0, sizeof rs); rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineDepthStencilStateCreateInfo dss; memset(&dss, 0, sizeof dss); dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    dss.depthTestEnable = VK_TRUE; dss.depthWriteEnable = VK_TRUE; dss.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; dss.maxDepthBounds = 1.0f;
    VkPipelineColorBlendAttachmentState cba; memset(&cba, 0, sizeof cba);
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    VkPipelineColorBlendStateCreateInfo cb; memset(&cb, 0, sizeof cb); cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkPipelineInputAssemblyStateCreateInfo ia; memset(&ia, 0, sizeof ia); ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vs; memset(&vs, 0, sizeof vs); vs.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vs.viewportCount = 1; vs.scissorCount = 1;
    VkPipelineMultisampleStateCreateInfo ms; memset(&ms, 0, sizeof ms); ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = rq_samples();
    static const VkDynamicState dy[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BOUNDS };
    VkPipelineDynamicStateCreateInfo dsi; memset(&dsi, 0, sizeof dsi); dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; dsi.dynamicStateCount = 2; dsi.pDynamicStates = dy;
    VkPipelineShaderStageCreateInfo st[2]; memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = rqc.ovs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = rqco.sfs; st[1].pName = "main";
    VkGraphicsPipelineCreateInfo gp; memset(&gp, 0, sizeof gp);
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia; gp.pViewportState = &vs;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &dss; gp.pColorBlendState = &cb; gp.pDynamicState = &dsi; gp.layout = rqco.layout; gp.renderPass = rqe.rp;
    if (rqco.sfs && vkCreateGraphicsPipelines(dev, pipe_cache, 1, &gp, NULL, &rqco.scr) != VK_SUCCESS) rqco.scr = VK_NULL_HANDLE;
    /* the HUD's combiner: its depth (where nothing solid is nearer), then the mark over those pixels */
    if (depth_bounds_ok) {
        gp.stageCount = 1;
        cba.colorWriteMask = 0;
        if (vkCreateGraphicsPipelines(dev, pipe_cache, 1, &gp, NULL, &rqco.ap) != VK_SUCCESS) rqco.ap = VK_NULL_HANDLE;
        dss.depthCompareOp = VK_COMPARE_OP_ALWAYS; dss.depthBoundsTestEnable = VK_TRUE;
        dsi.dynamicStateCount = 3;
        if (vkCreateGraphicsPipelines(dev, pipe_cache, 1, &gp, NULL, &rqco.apm) != VK_SUCCESS) rqco.apm = VK_NULL_HANDLE;
    }
    rqpw_made("the cockpit screens' and combiner's pipelines (rqco_pipes)", rqpw_t0);
    return rqco.layout != VK_NULL_HANDLE;
}
/* the common fields of the push: cockpit.vert's, the crop in sd and wx.xy, misc.z/w the depth */
static void rqco_push(cpit_push *pc, const rqc_frame *F) {
    memset(pc, 0, sizeof *pc);
    memcpy(pc->rot, F->rot, sizeof pc->rot); memcpy(pc->head, F->head, sizeof pc->head); memcpy(pc->proj, F->proj, sizeof pc->proj);
    pc->misc[0] = F->misc[0]; pc->misc[1] = F->misc[1]; pc->misc[2] = F->misc[2]; pc->misc[3] = F->misc[3];
    memcpy(pc->sd, F->xfx, sizeof pc->sd);
    pc->wx[0] = F->xfy[0]; pc->wx[1] = F->xfy[1];
}
/* the screens that show HUD blocks (cpit_draw's filt 4): one draw each, opaque, unlit */
static void rqc_draw_screens(VkCommandBuffer cmd, const rqc_frame *F) {
    VkDescriptorSet scr_set = mfd_rp.pattern ? white_set : rts[RT_MFD].set;
    if (!rqco.scr || !mfd_rp.any || !scr_set) return;
    if (!mfd_rp.pattern && (!rts[RT_MFD].created || rts[RT_MFD].layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) return;
    cpit_push sp; rqco_push(&sp, F);
    int bound = 0;
    for (u32 i = 0; i < cpit.nmesh; i++) {
        const cpit_mesh *m = &cpit.mesh[i];
        u32 sid = (m->flags >> 8) & 255u;
        if (!sid || !m->count) continue;
        int sblk = mfd_screen_blk(sid);
        if (!sblk) continue;
        if (!bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqco.scr);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqco.layout, 0, 1, &scr_set, 0, NULL);
            cpit_bind_geometry(cmd);
            bound = 1;
            rqc.fr_pipes++;
        }
        cpit_mesh_model(m, sp.model);
        sp.uvoff[0] = sp.uvoff[1] = 0.0f;
        mfd_screen_rect(sblk, cpit.scr_asp ? cpit.scr_asp[i] : 0.0f, sid == MFD_SCR_SCOPE || (cpit.scr_round && cpit.scr_round[i]), sp.color, sp.su);
        sp.light[0] = mfd_rp.bright; sp.light[1] = mfd_rp.black; sp.light[2] = (float)mfd_rp.pattern; sp.light[3] = (float)sid;
        memset(sp.sv, 0, sizeof sp.sv);
        if (sid == MFD_SCR_SCOPE) mfd_scope_clip(cpit.scr_asp ? cpit.scr_asp[i] : 0.0f, sp.sv);
        vkCmdPushConstants(cmd, rqco.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof sp, &sp);
        vkCmdDrawIndexed(cmd, m->count, 1, m->first, 0, 0);
        rqc.fr_draws++;
    }
}
/* the combiner's mark (cpit_draw passes 5 / 6 / 7) */
static void rqc_draw_marks(VkCommandBuffer cmd, const rqc_frame *F) {
    if (!cpit.ap_ok || !depth_bounds_ok || !rqco.ap || !rqco.apm || (hg_ab_n > 0 && hg_ab_off && !perf_ab_pass) || perf_ab_flipped("hudglass")) return;
    float za = F->misc[2], zb = F->misc[3];
    cpit_push pc; rqco_push(&pc, F);
    VkViewport vpt; memset(&vpt, 0, sizeof vpt); vpt.width = (float)rqe.w; vpt.height = (float)rqe.h; vpt.maxDepth = 1.0f;
    float half_ipd = pc.head[3], head_c[3] = { pc.head[0], pc.head[1], pc.head[2] };
    VkPipeline seq[3]; int nps = 0, filt[3];
    seq[nps] = rqco.ap; filt[nps++] = 5;
    seq[nps] = rqco.apm; filt[nps++] = 6;
    if (hg_cfg.view == 2) { seq[nps] = rqco.apm; filt[nps++] = 7; }
    cpit_bind_geometry(cmd);
    for (int ps = 0; ps < nps; ps++) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, seq[ps]);
        rqc.fr_pipes++;
        if (hg_cfg.view == 2) {
            float s = filt[ps] == 7 ? half_ipd : -half_ipd;
            for (int j = 0; j < 3; j++) pc.head[j] = head_c[j] + s * pc.rot[j];
        }
        if (filt[ps] >= 6) {
            float zn = 1e9f, zf = -1e9f;
            for (u32 k = 0; k < cpit.nap; k++) {
                const float *b = cpit.mesh[cpit.ap_idx[k]].bs;
                float v0 = b[0] - pc.head[0], v1 = b[1] - pc.head[1], v2 = b[2] - pc.head[2];
                float w = -(pc.rot[8] * v0 + pc.rot[9] * v1 + pc.rot[10] * v2);
                if (w - b[3] < zn) zn = w - b[3];
                if (w + b[3] > zf) zf = w + b[3];
            }
            zn -= 0.02f; zf += 0.02f;
            if (zn < 0.01f) zn = 0.01f;
            float dlo = za + zb / zf, dhi = za + zb / zn;
            if (!(zf > zn)) { dlo = 1.0f; dhi = 1.0f; }
            if (dhi > 1.0f) dhi = 1.0f;
            if (filt[ps] == 7) { dlo = HG_MARK_ONE - HG_BAND; dhi = HG_MARK_ONE + HG_BAND; }
            vkCmdSetDepthBounds(cmd, dlo, dhi);
            VkViewport mv = vpt; mv.minDepth = mv.maxDepth = filt[ps] == 6 && hg_cfg.view == 2 ? HG_MARK_ONE : HG_MARK;
            vkCmdSetViewport(cmd, 0, 1, &mv);
        }
        if (hg_cfg.view != 1) pc.head[3] = 0.0f;
        for (u32 ii = 0; ii < cpit.nap; ii++) {
            const cpit_mesh *m = &cpit.mesh[cpit.ap_idx[ii]];
            if (!m->count) continue;
            cpit_mesh_model(m, pc.model);
            vkCmdPushConstants(cmd, rqco.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
            vkCmdDrawIndexed(cmd, m->count, 1, m->first, 0, 0);
            rqc.fr_draws++;
        }
        if (filt[ps] >= 6) vkCmdSetViewport(cmd, 0, 1, &vpt);
        pc.head[3] = half_ipd; pc.head[0] = head_c[0]; pc.head[1] = head_c[1]; pc.head[2] = head_c[2];
    }
    hg_drawn = 1;
}

/* ---- the dry glass: the triangles that show anything at this eye and sun (CPU), then one draw ---- */
static int rqw_mesh(const cpit_mesh *m);
static void rqw_tri_centre(const rqc_model *q, const u32 *ix, float *c);
static int rqw_tri_need(const float *c, float th);
static u32 rqc_glass_list(rqc_model *q, const rqc_frame *F, u32 *out, int skip_wet) {
    float th = rqc_glass_th();
    const float head[3] = { F->head[0], F->head[1], F->head[2] };   /* (F is mapped, uncached: read once, not per triangle; glass3) */
    u32 n = 0;
    float sun[3] = { F->l[0], F->l[4], F->l[8] };                  /* (the key light's direction: rows of the block's l) */
    {   int s = (int)(F->pack[0] + 0.5f);
        sun[0] = F->l[s]; sun[1] = F->l[4 + s]; sun[2] = F->l[8 + s];
        float len = sqrtf(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
        if (len > 1e-5f) { sun[0] /= len; sun[1] /= len; sun[2] /= len; }
    }
    float amb = (F->r[12] + F->r[13] + F->r[14]) * 0.33f;
    for (u32 g = 0; g < q->ng; g++) {
        const rqc_gmesh *gm = &q->gm[g];
        const cpit_mesh *m = &cpit.mesh[gm->mesh];
        if (cpit_mesh_hidden(m)) continue;
        if (skip_wet == 1 && cpit_glass_outer(m)) continue;         /* (the full wet pipeline's) */
        const rqc_row *rows = rqmh_now && q->rowsh ? q->rowsh : (const rqc_row *)q->rowsp;   /* (PS2_RQ_MAP_HOST: the host copy) */
        const float *row = rows[gm->mesh].col;
        float ca = row[3], spk = rows[gm->mesh].sp[0], shi = rows[gm->mesh].sp[1];
        const u32 *ix = q->gix + gm->ifirst;
        int wm = skip_wet == 2 && rqw_mesh(m);                       /* (the lean wet pipeline takes its triangles that show water) */
        for (u32 t = 0; t < gm->icount / 3u; t++, ix += 3) {
            if (wm) { float wc[3]; rqw_tri_centre(q, ix, wc); if (rqw_tri_need(wc, rqc_wet_th())) continue; }
            int keep = ca >= th;                               /* (the tint: col.a x texel alpha, so col.a bounds it, textured or not) */
            if (!keep) {
                /* the same test as the shader's fresnel (and its highlight), at the triangle's centre */
                float c[3] = { 0, 0, 0 }, nn[3] = { 0, 0, 0 };
                for (int j = 0; j < 3; j++) {
                    const float *v = q->gv + 6u * (ix[j] - q->gvx0);
                    for (int a = 0; a < 3; a++) { c[a] += v[a]; nn[a] += v[3 + a]; }
                }
                float nl = sqrtf(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
                if (nl > 1e-5f) { nn[0] /= nl; nn[1] /= nl; nn[2] /= nl; }
                float V[3] = { head[0] - c[0] / 3.0f, head[1] - c[1] / 3.0f, head[2] - c[2] / 3.0f };
                float vl = sqrtf(V[0] * V[0] + V[1] * V[1] + V[2] * V[2]);
                if (vl > 1e-5f) { V[0] /= vl; V[1] /= vl; V[2] /= vl; }
                float cv = fabsf(nn[0] * V[0] + nn[1] * V[1] + nn[2] * V[2]);
                float f = 1.0f - cv; f = f * f * f * f * f;
                float wgt = f * 0.35f * (amb + 0.5f);          /* (the reflection's contribution, a share of the world behind) */
                if (wgt >= th) keep = 1;
                else if (spk > 0.0f) {                         /* the sun's glint */
                    float H[3] = { sun[0] + V[0], sun[1] + V[1], sun[2] + V[2] };
                    float hl = sqrtf(H[0] * H[0] + H[1] * H[1] + H[2] * H[2]);
                    if (hl > 1e-5f) {
                        float nh = fabsf(nn[0] * H[0] + nn[1] * H[1] + nn[2] * H[2]) / hl;
                        if (spk * powf(nh, shi > 1.0f ? shi : 1.0f) >= th * 0.5f) keep = 1;
                    }
                }
            }
            if (keep) { out[n * 3u] = ix[0]; out[n * 3u + 1u] = ix[1]; out[n * 3u + 2u] = ix[2]; n++; }
        }
    }
    return n;
}
/* returns the triangles written at q->gip[slot] */
static u32 rqc_draw_glass_dry(VkCommandBuffer cmd, vk_frame *fr, int skip_wet) {
    rqc_model *q = cpit.rqc;
    if (!q || !q->ok || !rqc.glass || !q->ng) return 0;
    u32 slot = (u32)(fr - frames);
    const rqc_frame *F = rqc_F(q, slot);
    u32 *ho = rqc.fh_on[slot] ? rqc_host_idx(q->ngtri) : NULL;   /* (PS2_RQ_MAP_HOST: the list in host memory, one copy) */
    u32 ntri = rqc_glass_list(q, F, ho ? ho : (u32 *)q->gip[slot], skip_wet);
    if (ho) memcpy(q->gip[slot], ho, (size_t)ntri * 3u * sizeof(u32));
    rqc.n_gtris += ntri; rqc.n_gfr++;
    if (!ntri) return 0;
    rqc_set_view(cmd);
    VkDescriptorSet sets[3] = { q->texset, cpit.sm_ok ? cpit.sm_set : white_set, q->fset[slot] };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqc.layout, 0, 3, sets, 0, NULL);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &q->vb, &zero);
    vkCmdBindIndexBuffer(cmd, q->gib[slot], 0, VK_INDEX_TYPE_UINT32);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqc.glass);
    vkCmdDrawIndexed(cmd, ntri * 3u, 1, 0, 0, 0);
    rqc.fr_pipes++; rqc.fr_draws++;
    return ntri;
}
/* ---- lean2: the feed's small mip chain (film, rime): three levels at half the feed's size and down, 2 blits a frame on wet frames ---- */
static int rqw_blur_make(u32 w, u32 h) {
    const u64 rqpw_t0 = vk_mono_ns();
    VkImageCreateInfo ii; memset(&ii, 0, sizeof ii);
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8B8A8_UNORM; ii.extent.width = w; ii.extent.height = h; ii.extent.depth = 1;
    ii.mipLevels = 3; ii.arrayLayers = 2; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateImage(dev, &ii, NULL, &rqw.img[5]) != VK_SUCCESS) return 0;
    VkMemoryRequirements r; vkGetImageMemoryRequirements(dev, rqw.img[5], &r);
    VkMemoryAllocateInfo ai; memset(&ai, 0, sizeof ai); ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = r.size; ai.memoryTypeIndex = find_mem(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == 0xFFFFFFFFu || vkAllocateMemory(dev, &ai, NULL, &rqw.mem[5]) != VK_SUCCESS || vkBindImageMemory(dev, rqw.img[5], rqw.mem[5], 0) != VK_SUCCESS) return 0;
    VkImageViewCreateInfo vi; memset(&vi, 0, sizeof vi); vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = rqw.img[5]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; vi.format = ii.format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount = 3; vi.subresourceRange.layerCount = 2;
    if (vkCreateImageView(dev, &vi, NULL, &rqw.view[5]) != VK_SUCCESS) return 0;
    VkDescriptorImageInfo im; memset(&im, 0, sizeof im); im.sampler = rqw.csmp; im.imageView = rqw.view[5]; im.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet wr; memset(&wr, 0, sizeof wr); wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr.dstSet = rqw.set; wr.dstBinding = 5; wr.descriptorCount = 1; wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &im;
    vkUpdateDescriptorSets(dev, 1, &wr, 0, NULL);
    rqw.bw = w; rqw.bh = h; rqw.bok = 1;
    rqpw_made("lean2's blur chain (the wet canopy)", rqpw_t0);
    ps2_log("cockpit material: lean2's blur chain %ux%u, 3 levels", w, h);
    return 1;
}
static void rqw_blur_update(VkCommandBuffer cmd) {
    vk_target *f = &rq_feed;
    if (rqc_wet_cfg() != 3 || !rqw.ok || !rq_feed_valid || !f->created || f->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) return;
    u32 w = tew(f) * tsc(f), h = teh(f) * tsc(f);
    if (!rqw.bok && !rqw_blur_make(w / 2u, h / 2u)) return;
    const VkPipelineStageFlags2 FS = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, TR = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    const VkImageLayout RO = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, TS = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, TD = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    cpit_lv_barrier(cmd, f->color, 0, 1, RO, TS, FS, VK_ACCESS_2_SHADER_READ_BIT, TR, VK_ACCESS_2_TRANSFER_READ_BIT);
    cpit_lv_barrier(cmd, rqw.img[5], 0, 3, rqw.bfresh ? RO : VK_IMAGE_LAYOUT_UNDEFINED, TD, FS, VK_ACCESS_2_SHADER_READ_BIT, TR, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    u32 sw = w, sh = h;
    for (u32 l = 0; l < 3u; l++) {
        u32 dw = rqw.bw >> l, dh = rqw.bh >> l;
        VkImageBlit bl; memset(&bl, 0, sizeof bl);
        bl.srcSubresource.aspectMask = bl.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bl.srcSubresource.mipLevel = l ? l - 1u : 0u; bl.srcSubresource.layerCount = 2; bl.dstSubresource.mipLevel = l; bl.dstSubresource.layerCount = 2;
        bl.srcOffsets[1].x = (s32)sw; bl.srcOffsets[1].y = (s32)sh; bl.srcOffsets[1].z = 1;
        bl.dstOffsets[1].x = (s32)dw; bl.dstOffsets[1].y = (s32)dh; bl.dstOffsets[1].z = 1;
        vkCmdBlitImage(cmd, l ? rqw.img[5] : f->color, TS, rqw.img[5], TD, 1, &bl, VK_FILTER_LINEAR);
        if (l < 2u) cpit_lv_barrier(cmd, rqw.img[5], l, 1, TD, TS, TR, VK_ACCESS_2_TRANSFER_WRITE_BIT, TR, VK_ACCESS_2_TRANSFER_READ_BIT);
        sw = dw; sh = dh;
    }
    cpit_lv_barrier(cmd, rqw.img[5], 0, 2, TS, RO, TR, VK_ACCESS_2_TRANSFER_READ_BIT, FS, VK_ACCESS_2_SHADER_READ_BIT);
    cpit_lv_barrier(cmd, rqw.img[5], 2, 1, TD, RO, TR, VK_ACCESS_2_TRANSFER_WRITE_BIT, FS, VK_ACCESS_2_SHADER_READ_BIT);
    cpit_lv_barrier(cmd, f->color, 0, 1, TS, RO, TR, VK_ACCESS_2_TRANSFER_READ_BIT, FS, VK_ACCESS_2_SHADER_READ_BIT);
    rqw.bfresh = 1;
}
/* ---- the lean wet canopy ---- */
static struct { float wet, mist, film, gain, frost, frz, sdx, st, size, ak, dsin, stretch, wy, wz, grime, soot, pf, pv, rlen, di, smear, rbk, dint; int rime; } rqwp;
/* this frame's canopy inputs, as cpit_draw hands them to cockpit.frag and canopy_water decodes them */
static void rqw_params(void) {
    float sz = canopy_drops_size(canopy_size), aq = floorf(sz * 0.25f), ai = floorf(aq * (1.0f / 32.0f)), s0 = sz - 4.0f * aq;
    aq -= 32.0f * ai;
    int lensdrops = aq >= 1.0f;
    rqwp.size = lensdrops ? s0 : (sz > 0.0f ? sz : 1.0f);
    rqwp.ak = lensdrops ? aq * 0.25f : 0.6f;
    rqwp.dsin = sinf((45.0f + 5.0f * ai) * 0.017453293f);
    float su3 = canopy_drops_intensity(canopy_ab_drops && canopy_ice_ab_off() ? 0.45f : cx_now() ? cx_di : drop_i);
    float rq = lensdrops ? floorf((su3 > 0.0f ? su3 : 0.0f) * 0.5f) : 0.0f, dint = su3 - 2.0f * rq;
    float di = (dint < 0.0f ? 0.0f : dint > 1.0f ? 1.0f : dint) - 0.45f;
    rqwp.di = di; rqwp.rbk = rq * 0.25f; rqwp.dint = dint < 0.0f ? 0.0f : dint > 1.0f ? 1.0f : dint;
    rqwp.smear = canopy_v[6];
    rqwp.pf = 1.0f + 1.6f * di;
    rqwp.pv = 1.0f + di * (di > 0.0f ? 3.6f : 1.65f);
    rqwp.gain = canopy_gain > 0.0f ? canopy_gain : 1.0f;
    rqwp.wet = cx_now() ? cx_b + cx_r : canopy_v[0];
    rqwp.mist = cx_now() ? cx_mist : canopy_v[3];
    rqwp.film = canopy_film();
    rqwp.st = canopy_v[5] > 1.0f ? canopy_v[5] : 1.0f;
    rqwp.stretch = 1.0f + 0.3f * (rqwp.st - 1.0f);
    rqwp.rlen = 2.0f + 2.5f * rqwp.st;
    rqwp.sdx = canopy_v[7];
    rqwp.wy = lensdrops ? lens_flow : canopy_v[1];
    rqwp.wz = canopy_v[2];
    rqwp.grime = canopy_v[4];
    rqwp.soot = cs_soot_draw() > 0.02f && !perf_ab_flipped("soot") ? cs_soot_draw() : 0.0f;
    float ipk = canopy_ice_pack();
    rqwp.rime = ipk >= 1048576.0f;
    if (rqwp.rime) ipk -= 1048576.0f;
    if (ipk < 0.0f) ipk = 0.0f;
    float ifz = floorf(ipk * (1.0f / 1024.0f));
    rqwp.frz = ifz * (1.0f / 1023.0f); rqwp.frost = (ipk - ifz * 1024.0f) * (1.0f / 1023.0f);
}
/* does this triangle of outer glass show water at all (the same weights as the fragment stage, at its centre)? */
static int rqw_tri_need(const float *c, float th) {
    float side = rqw_ss(0.06f, 0.35f, fabsf(c[0]));
    if (rqwp.wet * (1.0f + (side - 1.0f) * 0.55f * rqwp.sdx) * rqwp.pf >= th * 4.0f) return 1;
    if (rqwp.mist * 0.35f * rqwp.gain * 0.25f >= th * 0.5f) return 1;
    float edge = rqw_ss(0.08f, 0.42f, fabsf(c[0])) * 0.6f + rqw_ss(0.1f, 0.6f, c[2] + 0.25f) * 0.4f, fth = 1.02f - rqwp.film * (0.75f + 0.55f * edge);
    if (rqwp.film > 0.0f && rqw_ss(fth - 0.1f, fth + 0.14f, 0.95f) * (0.55f + 0.45f * edge) * 0.5f >= th) return 1;
    if (rqwp.frost > 0.0f) {
        float l = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]); if (l < 0.01f) l = 0.01f;
        float dy = c[1] / l, ax = 1.0f + c[2] / l;
        float e = rqw_ss(0.2f, 1.05f, ax + 0.3f * (dy - 0.35f > 0.0f ? dy - 0.35f : 0.0f) + 0.25f * (-dy - 0.2f > 0.0f ? -dy - 0.2f : 0.0f));
        if (e + 0.24f > 1.0f - 0.72f * rqwp.frost - 0.02f) return 1;
    }
    return rqwp.grime > 0.002f || rqwp.soot > 0.0f || rqwp.smear > 0.002f;
}
/* the mesh is one of the wet glass's (the outer glass, once per shell) */
static int rqw_mesh(const cpit_mesh *m) { return cpit_glass_outer(m) && !((m->flags & CPIT_F_WXDUP) && (cpit_wx_once() ^ perf_ab_flipped("wxonce"))); }
static void rqw_tri_centre(const rqc_model *q, const u32 *ix, float *c) {
    c[0] = c[1] = c[2] = 0.0f;
    for (int j = 0; j < 3; j++) { const float *v = q->gv + 6u * (ix[j] - q->gvx0); for (int a = 0; a < 3; a++) c[a] += v[a] * (1.0f / 3.0f); }
}
#include "rq_glass.inc"                           /* R-57 / R-58: the readable glass and the cracks in the eye pass */
/* the wet part of the glass, drawn after the dry list (fi = the index buffer's first free triangle) */
static void rqw_draw(VkCommandBuffer cmd, vk_frame *fr, u32 fi) {
    rqc_model *q = cpit.rqc;
    if (!q || !q->ok || !q->gvb || !rqw.ok || !rq_feed_valid || !rq_feed.set || (rqc_wet_cfg() == 3 && !rqw.bfresh)) return;
    u32 slot = (u32)(fr - frames);
    rqc_frame *F = rqc_F(q, slot);
    int k = (rqwp.wet > 0.0f ? 1 : 0) | (rqwp.mist > 0.0f || rqwp.film > 0.0f ? 2 : 0) | (rqwp.frost > 0.0f || rqwp.frz > 0.0f ? 4 : 0);
    VkPipeline p = rqc.lwet[k];
    if (!p) return;
    float th = rqc_wet_th();
    u32 *out = (u32 *)q->gip[slot] + (size_t)fi * 3u, n = 0;
    u32 *outm = out;                                      /* (PS2_RQ_MAP_HOST: the list in host memory, one copy into the mapped index buffer) */
    if (rqc.fh_on[slot] && (out = rqc_host_idx(q->ngtri)) == NULL) out = outm;
    for (u32 g = 0; g < q->ng; g++) {
        const rqc_gmesh *gm = &q->gm[g];
        const cpit_mesh *m = &cpit.mesh[gm->mesh];
        if (cpit_mesh_hidden(m) || !rqw_mesh(m)) continue;
        const u32 *ix = q->gix + gm->ifirst;
        for (u32 t = 0; t < gm->icount / 3u; t++, ix += 3) {
            float c[3]; rqw_tri_centre(q, ix, c);
            if (!rqw_tri_need(c, th)) continue;
            out[n * 3u] = ix[0] - q->gvx0; out[n * 3u + 1u] = ix[1] - q->gvx0; out[n * 3u + 2u] = ix[2] - q->gvx0; n++;
        }
    }
    if (out != outm) memcpy(outm, out, (size_t)n * 3u * sizeof(u32));
    rqc.n_wtris += n;
    rqw.next_free = fi + n;
    if (!n) return;
    float g13 = rqwp.gain < 1.3f ? rqwp.gain : 1.3f, g1 = rqwp.gain < 1.0f ? rqwp.gain : 1.0f;
    F->wa[0] = rqwp.mist * 0.35f * rqwp.gain; F->wa[1] = 0.5f * g13; F->wa[2] = g1; F->wa[3] = g13;
    F->wb[0] = 1.0f - 0.72f * rqwp.frost; F->wb[1] = rqwp.frz; F->wb[2] = rqwp.ak; F->wb[3] = rqwp.dsin;
    F->wc[0] = rqwp.wet; F->wc[1] = rqwp.size; F->wc[2] = rqwp.stretch; F->wc[3] = rqwp.wy;
    F->wd[0] = rqwp.wz; F->wd[1] = rqwp.rlen; F->wd[2] = rqwp.sdx; F->wd[3] = rqwp.pf;
    F->we[0] = rqwp.pv; F->we[1] = rqwp.film; F->we[2] = rqwp.grime * 0.14f * rqwp.gain; F->we[3] = rqwp.soot * g13;
    {   int sun = (int)(F->pack[0] + 0.5f); float kfac = F->dome[7], ssl = F->dome[3], gh = F->dome[11]; gh = gh < 0.0f ? 0.0f : gh > 1.0f ? 1.0f : gh;
        for (int j = 0; j < 3; j++) {
            float sc = F->r[j] + F->r[4 + j] + F->r[8 + j]; sc = sc < 1.5f ? sc : 1.5f;
            if (ssl > 0.0f) { float kc = 2.0f * F->r[sun * 4 + j] * kfac; kc = kc < 1.5f ? kc : 1.5f; sc += (kc - sc) * ssl; }
            F->wf[j] = sc * (1.0f - gh);
        }
        F->wf[3] = rqwp.frost;
    }
    {   float tl = 1.1f * (rqwp.st - 1.0f), tk = tl / RQW2_TAIL * 1.4f;     /* lean2: the tail, the drops' size and the rivulets' width by speed and intensity */
        F->wg[0] = tk > 0.08f ? tk : 0.08f; F->wg[1] = 1.0f + 0.6f * rqwp.di; F->wg[2] = 3.0f - 2.4f * rqwp.di; F->wg[3] = 1.0f + 0.45f * rqwp.di;
        /* the bake's rain beads are at the intensity 0.45 and the amount 1; the density from the vertex stage holds pf (the big beads') */
        float rs = 4.0f * rqwp.rbk * rqw_ss(0.3f, 0.8f, rqwp.dint) / (4.0f * 0.216f) / (rqwp.pf > 0.05f ? rqwp.pf : 0.05f);
        F->wh[0] = rqwp.smear; F->wh[1] = rs; F->wh[2] = (1.0f - 0.55f * rqwp.di) / (rqwp.pf > 0.05f ? rqwp.pf : 0.05f); F->wh[3] = rqw_light(); }
    /* eye pixel -> the frame image's uv, per view: the inverse of the crop rqe.xf (eye_ndc = A ndc + B) */
    {   float W = (float)rqe.w, H = (float)rqe.h;
        for (int e = 0; e < 2; e++) { float A = F->xfx[2 * e], B = F->xfx[2 * e + 1]; F->uvk[2 * e] = 1.0f / (W * A); F->uvk[2 * e + 1] = (-1.0f - B) * 0.5f / A + 0.5f; }
        F->uvk[4] = 1.0f / (H * F->xfy[0]); F->uvk[5] = (-1.0f - F->xfy[1]) * 0.5f / F->xfy[0] + 0.5f;
    }
    if (rqc.fh_on[slot])                                  /* (PS2_RQ_MAP_HOST: wa .. wh into the mapped block, one copy) */
        memcpy(&((rqc_frame *)q->fp[slot])->wa, &F->wa, (size_t)((const u8 *)&F->gx - (const u8 *)&F->wa));
    rqc_set_view(cmd);
    VkDescriptorSet sets[4] = { q->texset, rq_feed.set, q->fset[slot], rqw.set };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqc.lwlayout, 0, 4, sets, 0, NULL);
    VkDeviceSize o0 = (VkDeviceSize)q->gvx0 * sizeof(rqc_vtx), o1 = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &q->vb, &o0);
    vkCmdBindVertexBuffers(cmd, 1, 1, &q->gvb, &o1);
    vkCmdBindIndexBuffer(cmd, q->gib[slot], 0, VK_INDEX_TYPE_UINT32);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdDrawIndexed(cmd, n * 3u, 1, fi * 3u, 0, 0);
    rqc.fr_pipes++; rqc.fr_draws++;
}
/* [test] ACZ_VR_RQ_COCKPIT_FLECK_TEST=x,y,z: a fresh big fleck at that direction from the eye point (the crops have no debris otherwise): canopy_mark_code's encoding */
static float rqw_mark(int i) {
    static int on = -1; static float code;
    if (on < 0) {
        const char *e = getenv("ACZ_VR_RQ_COCKPIT_FLECK_TEST"); float d[3]; on = 0;
        if (e && sscanf(e, "%f,%f,%f", &d[0], &d[1], &d[2]) == 3) {
            float n = fabsf(d[0]) + fabsf(d[1]) + fabsf(d[2]), ex = d[0] / n, ey = d[1] / n;
            if (d[2] < 0.0f) { float t = 1.0f - fabsf(ey), f = 1.0f - fabsf(ex); ey = ey >= 0.0f ? f : -f; ex = ex >= 0.0f ? t : -t; }
            u32 ux = (u32)((ex + 1.0f) * 127.5f + 0.5f), uy = (u32)((ey + 1.0f) * 127.5f + 0.5f);
            code = (float)((ux << 15u) | (uy << 7u) | 31u | (3u << 5u) | (1u << 23u)); on = 1;
        }
    }
    return on ? (i == 0 ? code : 0.0f) : canopy_mark_code(i);
}
/* lean2: the debris flecks as decals, on the glass triangles within 8 cm of an active fleck's line of sight (cockpit.frag's debris_flecks, a small shader; only on frames that have one) */
static void rqw_flecks(VkCommandBuffer cmd, vk_frame *fr) {
    rqc_model *q = cpit.rqc;
    if (!q || !q->ok || !rqc.fleck) return;
    u32 slot = (u32)(fr - frames);
    const rqc_frame *F = rqc_F(q, slot);
    float cmk[3] = { rqw_mark(0), rqw_mark(1), rqw_mark(2) };
    float dirs[3][3]; int nd = 0;
    for (int j = 0; j < 3; j++) {
        if (cmk[j] < 0.5f) continue;
        u32 u = (u32)(cmk[j] + 0.25f);
        float ex = (float)((u >> 15u) & 255u) * (1.0f / 127.5f) - 1.0f, ey = (float)((u >> 7u) & 255u) * (1.0f / 127.5f) - 1.0f, ez = 1.0f - fabsf(ex) - fabsf(ey);
        float t = ez < 0.0f ? -ez : 0.0f; ex += ex >= 0.0f ? -t : t; ey += ey >= 0.0f ? -t : t;
        float l = sqrtf(ex * ex + ey * ey + ez * ez); if (l < 1e-5f) continue;
        dirs[nd][0] = ex / l; dirs[nd][1] = ey / l; dirs[nd][2] = ez / l; nd++;
    }
    if (!nd) return;
    cpit_push pc; memset(&pc, 0, sizeof pc);
    memcpy(pc.rot, F->rot, sizeof pc.rot); memcpy(pc.head, F->head, sizeof pc.head); memcpy(pc.proj, F->proj, sizeof pc.proj);
    pc.misc[0] = F->misc[0]; pc.misc[1] = F->misc[1]; pc.light[3] = -1.0f;
    for (int j = 0; j < 3; j++) pc.su[j] = cmk[j];
    u32 fi = rqw.next_free, n = 0;
    u32 *out = (u32 *)q->gip[slot] + (size_t)fi * 3u;
    u32 *outm = out;                                      /* (PS2_RQ_MAP_HOST: the list in host memory, one copy) */
    if (rqc.fh_on[slot] && (out = rqc_host_idx(q->ngtri)) == NULL) out = outm;
    for (u32 g = 0; g < q->ng; g++) {
        const rqc_gmesh *gm = &q->gm[g];
        const cpit_mesh *m = &cpit.mesh[gm->mesh];
        if (cpit_mesh_hidden(m) || !rqw_mesh(m)) continue;
        const u32 *ix = q->gix + gm->ifirst;
        for (u32 t = 0; t < gm->icount / 3u; t++, ix += 3) {
            float c[3]; rqw_tri_centre(q, ix, c);
            int near = 0;
            for (int d = 0; d < nd && !near; d++) {
                float s = c[0] * dirs[d][0] + c[1] * dirs[d][1] + c[2] * dirs[d][2];
                float qx = c[0] - dirs[d][0] * s, qy = c[1] - dirs[d][1] * s, qz = c[2] - dirs[d][2] * s;
                near = s > 0.0f && qx * qx + qy * qy + qz * qz < 0.0064f;
            }
            if (!near) continue;
            memcpy(out + n * 3u, ix, 12); n++;
        }
    }
    if (out != outm) memcpy(outm, out, (size_t)n * 12u);
    if (!n) return;
    rqc_set_view(cmd);
    VkDescriptorSet sets[3] = { q->texset, rq_feed.set, q->fset[slot] };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqc.wlayout, 0, 3, sets, 0, NULL);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &q->vb, &zero);
    vkCmdBindIndexBuffer(cmd, q->gib[slot], 0, VK_INDEX_TYPE_UINT32);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqc.fleck);
    vkCmdPushConstants(cmd, rqc.wlayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDrawIndexed(cmd, n * 3u, 1, fi * 3u, 0, 0);
    rqc.fr_pipes++; rqc.fr_draws++;
}
/* the wet canopy on the outer glass (cockpit.frag's canopy_water, a separate pipeline; the world copy = the previous frame's 1x
   feed, read at most 4 times a pixel). One draw per outer mesh: the same inputs as cpit_draw's glass pass. fi = the index
   buffer's first free triangle (the dry list ahead of it) */
static void rqc_draw_glass_wet(VkCommandBuffer cmd, vk_frame *fr, u32 fi) {
    rqc_model *q = cpit.rqc;
    if (!q || !q->ok || !rqc.wet || !rq_feed_valid || !rq_feed.set) return;
    u32 slot = (u32)(fr - frames);
    const rqc_frame *F = rqc_F(q, slot);
    cpit_push pc; memset(&pc, 0, sizeof pc);
    memcpy(pc.rot, F->rot, sizeof pc.rot); memcpy(pc.head, F->head, sizeof pc.head); memcpy(pc.proj, F->proj, sizeof pc.proj);
    pc.misc[0] = F->misc[0]; pc.misc[1] = F->misc[1];
    float ckl = cloud_light(), cfilm = canopy_film();
    float cice = canopy_ice_pack();
    float cmk[3] = { rqw_mark(0), rqw_mark(1), rqw_mark(2) };
    pc.sv[0] = 1.0f + (cpit.sm_valid && (cpit.su[0] != 0.0f || cpit.su[1] != 0.0f || cpit.su[2] != 0.0f) ? 2.0f : 0.0f);
    pc.sv[2] = canopy_drops_size(canopy_size); pc.sv[3] = canopy_gain;
    pc.sd[0] = canopy_v[7]; pc.sd[2] = ckl;
    pc.su[3] = canopy_drops_intensity(canopy_ab_drops && canopy_ice_ab_off() ? 0.45f : cx_now() ? cx_di : drop_i);
    pc.uvoff[3] = canopy_v[5];
    pc.wx[1] = canopy_drops_lens() ? lens_flow : canopy_v[1]; pc.wx[2] = canopy_v[2];
    int cdry = !canopy_active();
    rqc_set_view(cmd);
    VkDescriptorSet sets[3] = { q->texset, rq_feed.set, q->fset[slot] };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqc.wlayout, 0, 3, sets, 0, NULL);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &q->vb, &zero);
    vkCmdBindIndexBuffer(cmd, q->gib[slot], 0, VK_INDEX_TYPE_UINT32);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqc.wet);
    rqc.fr_pipes++;
    u32 *out = (u32 *)q->gip[slot] + (size_t)fi * 3u;
    for (u32 g = 0; g < q->ng; g++) {
        const rqc_gmesh *gm = &q->gm[g];
        const cpit_mesh *m = &cpit.mesh[gm->mesh];
        if (cpit_mesh_hidden(m) || !cpit_glass_outer(m)) continue;
        int og = !((m->flags & CPIT_F_WXDUP) && (cpit_wx_once() ^ perf_ab_flipped("wxonce")));
        int outer = og && !cdry;
        pc.wx[0] = outer ? (cx_now() ? cx_b + cx_r : canopy_v[0]) : 0.0f;
        pc.wx[3] = outer ? (cx_now() ? cx_mist : canopy_v[3]) : 0.0f;
        pc.uvoff[2] = outer ? canopy_v[4] : 0.0f; pc.sv[1] = outer ? canopy_v[6] : 0.0f;
        if (outer && cs_soot_draw() > 0.02f && !perf_ab_flipped("soot")) {
            float gq = floorf((canopy_v[4] > 1.0f ? 1.0f : canopy_v[4]) * 255.0f + 0.5f);
            pc.uvoff[2] = -(1.0f + floorf(cs_soot_draw() * 255.0f + 0.5f) + 256.0f * gq);
        }
        pc.sd[3] = outer ? cfilm : 0.0f; pc.sd[1] = outer ? cice : 0.0f;
        for (int j = 0; j < 3; j++) pc.su[j] = og ? cmk[j] : 0.0f;
        pc.light[3] = -1.0f;
        if (m->thk_mm && !perf_ab_flipped("layers") && !rw_ab_off) pc.light[3] -= 256.0f * (float)m->thk_mm;
        memcpy(out, q->gix + gm->ifirst, (size_t)gm->icount * sizeof(u32));
        vkCmdPushConstants(cmd, rqc.wlayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        vkCmdDrawIndexed(cmd, gm->icount, 1, fi * 3u, 0, 0);
        rqc.fr_draws++;
        fi += gm->icount / 3u; out += gm->icount;
    }
}

/* ---- the sun's shadow map on change only ---- */
/* the animations that move a large part (stick, throttle, a hand, a select): a map is rendered again when one of them has moved */
static void rqc_sm_big_init(void) {
    if (rqc.sm_serial == cpit.serial) return;
    rqc.sm_serial = cpit.serial; rqc.sm_have = 0;
    free(rqc.sm_big); rqc.sm_big = NULL; free(rqc.sm_bigi); rqc.sm_bigi = NULL; rqc.sm_nbig = 0;
    if (!cpit.nanim) return;
    rqc.sm_bigi = (u32 *)malloc((size_t)cpit.nanim * sizeof(u32));
    rqc.sm_big = (float *)calloc((size_t)cpit.nanim * 12u, sizeof(float));
    if (!rqc.sm_bigi || !rqc.sm_big) { free(rqc.sm_bigi); free(rqc.sm_big); rqc.sm_bigi = NULL; rqc.sm_big = NULL; return; }
    for (u32 i = 0; i < cpit.nanim; i++) {
        const cpit_anim *an = &cpit.anim[i];
        if (an->type == 2u || an->type == 4u || (an->prop >= CPIT_P_ELEV && an->prop <= CPIT_P_THR)) rqc.sm_bigi[rqc.sm_nbig++] = i;
    }
}
/* 1 = keep the last map (sp: this frame's rows -- the map's own rows are returned in su/sv/sd) */
static int rqc_shadow_keep(const float *L, float *su, float *sv, float *sd) {
    if (!rqc_cfg() || !cpit.rqc || !cpit.amat) return 0;
    rqc_sm_big_init();
    static int every = -1;
    if (every < 0) { const char *e = getenv("ACZ_VR_RQ_COCKPIT_SM_FRAMES"); every = e && *e ? atoi(e) : 240; if (every < 1) every = 1; }
    int same = rqc.sm_have && rqc_sun_deg(L, rqc.sm_L) <= 0.5f && rqc.sm_age < (u32)every;
    for (u32 k = 0; same && k < rqc.sm_nbig; k++) {
        u32 i = rqc.sm_bigi[k];
        for (int j = 0; j < 12; j++) if (fabsf(cpit.amat[i * 12u + (u32)j] - rqc.sm_big[i * 12u + (u32)j]) > 0.0015f) { same = 0; break; }
    }
    if (same) { rqc.sm_age++; rqc.n_sm_kept++; memcpy(su, rqc.sm_su, 16); memcpy(sv, rqc.sm_sv, 16); memcpy(sd, rqc.sm_sd, 16); return 1; }
    memcpy(rqc.sm_L, L, 12); memcpy(rqc.sm_su, su, 16); memcpy(rqc.sm_sv, sv, 16); memcpy(rqc.sm_sd, sd, 16);
    if (rqc.sm_big) for (u32 k = 0; k < rqc.sm_nbig; k++) { u32 i = rqc.sm_bigi[k]; memcpy(rqc.sm_big + i * 12u, cpit.amat + i * 12u, 12u * sizeof(float)); }
    rqc.sm_have = 1; rqc.sm_age = 0; rqc.n_sm++;
    return 0;
}
/* the merged casters' draw (the pass's rendering is open, the map's viewport set); 0 = draw them the old way */
static int rqc_shadow_draw(VkCommandBuffer cmd, vk_frame *fr, const float *u, const float *v, const float *d) {
    rqc_model *q = cpit.rqc;
    if (!rqc_cfg() || !q || !q->ok || !rqc.sm) return 0;
    int slot = (int)(fr - frames);
    rqc_anim_fill(q, slot);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqc.sm);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqc.layout, 2, 1, &q->fset[slot], 0, NULL);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &q->vb, &zero);
    vkCmdBindIndexBuffer(cmd, q->ib, 0, VK_INDEX_TYPE_UINT32);
    float pc[12];
    memcpy(pc, u, 16); memcpy(pc + 4, v, 16); memcpy(pc + 8, d, 16);
    vkCmdPushConstants(cmd, rqc.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48, pc);
    if (q->sm_count) vkCmdDrawIndexed(cmd, q->sm_count, 1, q->sm_first, 0, 0);   /* (ACZ_VR_RQ_COCKPIT_SM_LOD) */
    else if (q->n_solid + q->n_cut) vkCmdDrawIndexed(cmd, q->n_solid + q->n_cut, 1, 0, 0, 0);
    return 1;
}

/* ---- the scene light from the feed: the previous frame's eye image, each pixel kept only if the depth says it is the world ---- */
static struct {
    VkBuffer buf[FRAMES_IN_FLIGHT]; VkDeviceMemory mem[FRAMES_IN_FLIGHT]; void *ptr[FRAMES_IN_FLIGHT];
    float el[FRAMES_IN_FLIGHT][SL_NP];
    int have[FRAMES_IN_FLIGHT], failed;
} rqc_sl;
/* frame slot fi's host buffer (at the slot's first eye frame, or before the first eye frame: PS2_RQ_PREWARM); 0 = none */
static int rqc_sl_make(int fi) {
    if (fi < 0 || fi >= (int)FRAMES_IN_FLIGHT || rqc_sl.failed) return 0;
    if (rqc_sl.buf[fi]) return 1;
    const u64 rqpw_t0 = vk_mono_ns();
    VkMemoryPropertyFlags mp = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (find_mem(~0u, mp | VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0xFFFFFFFFu) mp |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    if (make_buffer((VkDeviceSize)SL_NP * SL_PATCH * SL_PATCH * 8u, VK_BUFFER_USAGE_TRANSFER_DST_BIT, mp, &rqc_sl.buf[fi], &rqc_sl.mem[fi], &rqc_sl.ptr[fi]) != 0) { rqc_sl.failed = 1; return 0; }
    rqpw_made("a scene-light read-back buffer (rqc_sl)", rqpw_t0);
    return 1;
}
/* before the eye pass (no rendering open): the patches of the feed's left layer, colour and depth, into a host buffer */
static void rqc_sl_copy(VkCommandBuffer cmd, vk_frame *fr) {
    int fi = (int)(fr - frames);
    if (sl_off_now() || rqc_sl.failed || perf_ab_pass || bench_pass || cc_extra || xr_second_pass || fi < 0 || fi >= (int)FRAMES_IN_FLIGHT
        || !rq_feed_valid || !rq_feed.created || !cpit_seq) return;
    static u32 n_rec;
    if ((n_rec++ % SL_EVERY) != 0u && sl.bw[0] + sl.bw[1] > 0.0f) return;
    ps2_cloud_view v;
    if (!cl_view_make(&v, cpit_seq) || !(v.proj[0] > 1.0f) || !(v.proj[1] > 1.0f)) return;
    if (!rqc_sl_make(fi)) return;
    vk_target *t = &rq_feed;
    u32 w = tew(t) * tsc(t), h = teh(t) * tsc(t);
    VkBufferImageCopy rg[SL_NP], rd[SL_NP];
    memset(rg, 0, sizeof rg); memset(rd, 0, sizeof rd);
    for (int k = 0; k < SL_NP; k++) {
        float fx = 0.1f + 0.2f * (float)(k % SL_NX), fy = 0.17f + 0.33f * (float)(k / SL_NX);
        float px = v.vis[0][0] + fx * (v.vis[0][2] - v.vis[0][0]), py = v.vis[0][1] + fy * (v.vis[0][3] - v.vis[0][1]);
        s32 x = (s32)px - (s32)SL_PATCH / 2, y = (s32)py - (s32)SL_PATCH / 2;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x > (s32)w - (s32)SL_PATCH) x = (s32)w - (s32)SL_PATCH;
        if (y > (s32)h - (s32)SL_PATCH) y = (s32)h - (s32)SL_PATCH;
        rg[k].bufferOffset = (VkDeviceSize)k * SL_PATCH * SL_PATCH * 4u;
        rg[k].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rg[k].imageSubresource.layerCount = 1;
        rg[k].imageOffset.x = x; rg[k].imageOffset.y = y;
        rg[k].imageExtent.width = SL_PATCH; rg[k].imageExtent.height = SL_PATCH; rg[k].imageExtent.depth = 1;
        rd[k] = rg[k];
        rd[k].bufferOffset = (VkDeviceSize)SL_NP * SL_PATCH * SL_PATCH * 4u + (VkDeviceSize)k * SL_PATCH * SL_PATCH * 4u;
        rd[k].imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        float cx = (float)x + 0.5f * (float)SL_PATCH, cy = (float)y + 0.5f * (float)SL_PATCH;
        float dv[3] = { (cx - v.proj[2]) / v.proj[0], -(cy - v.proj[3]) / v.proj[1], -1.0f };
        float wy = v.cam_x[1] * dv[0] + v.cam_y[1] * dv[1] + v.cam_z[1] * dv[2];
        float n2 = dv[0] * dv[0] + dv[1] * dv[1] + 1.0f;
        float s = wy / sqrtf(n2);
        rqc_sl.el[fi][k] = asinf(s > 1.0f ? 1.0f : (s < -1.0f ? -1.0f : s));
    }
    VkImageMemoryBarrier2 bb[2];
    barrier_fill(&bb[0], t->color, VK_IMAGE_ASPECT_COLOR_BIT, t->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier_fill(&bb[1], t->depth, VK_IMAGE_ASPECT_DEPTH_BIT, t->dlayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier_flush(cmd, bb, 2);
    vkCmdCopyImageToBuffer(cmd, t->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rqc_sl.buf[fi], SL_NP, rg);
    vkCmdCopyImageToBuffer(cmd, t->depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rqc_sl.buf[fi], SL_NP, rd);
    barrier_fill(&bb[0], t->color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : t->layout, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    barrier_fill(&bb[1], t->depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->dlayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : t->dlayout, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    barrier_flush(cmd, bb, 2);
    {   VkMemoryBarrier2 mb; memset(&mb, 0, sizeof mb);
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2; mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo di; memset(&di, 0, sizeof di); di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO; di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &di);
    }
    rqc_sl.have[fi] = 1;
    sl.copies++;
}
/* after the frame's fence (sl_collect's twin): each patch's world pixels into the three bands. 1 = this frame's bands were handled */
static int rqc_sl_collect(vk_frame *fr) {
    int fi = (int)(fr - frames);
    if (fi < 0 || fi >= (int)FRAMES_IN_FLIGHT || !rqc_sl.have[fi] || !rqc_sl.ptr[fi]) return 0;
    rqc_sl.have[fi] = 0;
    u64 now = vk_mono_ns();
    float dt = sl.last_ns ? (float)(now - sl.last_ns) * 1e-9f : 0.02f;
    sl.last_ns = now;
    if (dt > 0.2f) dt = 0.2f;
    const u8 *b = (const u8 *)rqc_sl.ptr[fi];
    const float *zp = (const float *)(b + (size_t)SL_NP * SL_PATCH * SL_PATCH * 4u);
    float col[SL_NP][3], lum[SL_NP]; int ok[SL_NP];
    for (int k = 0; k < SL_NP; k++) {
        const u8 *p = b + (size_t)k * SL_PATCH * SL_PATCH * 4u;
        const float *z = zp + (size_t)k * SL_PATCH * SL_PATCH;
        u32 s[3] = { 0, 0, 0 }, n = 0;
        for (u32 j = 0; j < SL_PATCH * SL_PATCH; j++) {
            if (z[j] >= 0.99f) continue;                                   /* (the cockpit, not the world) */
            s[0] += p[j * 4u]; s[1] += p[j * 4u + 1u]; s[2] += p[j * 4u + 2u]; n++;
        }
        ok[k] = n >= (SL_PATCH * SL_PATCH) / 4u;
        for (int c = 0; c < 3; c++) col[k][c] = ok[k] ? (float)s[c] / (255.0f * (float)n) : 0.0f;
        lum[k] = 0.299f * col[k][0] + 0.587f * col[k][1] + 0.114f * col[k][2];
    }
    float a = 1.0f - expf(-dt / sl.tau);
    for (int bnd = 0; bnd < 3; bnd++) {
        int idx[SL_NP], n = 0, hi = -1;
        for (int k = 0; k < SL_NP; k++) {
            float e = rqc_sl.el[fi][k];
            int bk = e > 0.35f ? 0 : (e < -0.26f ? 2 : 1);
            if (bk != bnd || !ok[k]) continue;
            idx[n++] = k;
            if (hi < 0 || lum[k] > lum[hi]) hi = k;
        }
        if (!n) continue;
        float m[3] = { 0, 0, 0 }; int c = 0;
        for (int i = 0; i < n; i++) {
            if (n >= 3 && idx[i] == hi) continue;
            for (int j = 0; j < 3; j++) m[j] += col[idx[i]][j];
            c++;
        }
        if (!c) continue;
        float ab = a * (float)n / (float)SL_NX;
        if (ab > 1.0f) ab = 1.0f;
        if (sl.bw[bnd] < 0.05f) ab = 1.0f;
        for (int j = 0; j < 3; j++) sl.band[bnd][j] += (m[j] / (float)c - sl.band[bnd][j]) * ab;
        sl.bw[bnd] += (1.0f - sl.bw[bnd]) * (sl.bw[bnd] < 0.05f ? a : ab);
    }
    sl.collects++;
    return 1;
}

/* ---- eye adaptation in the eye pass (the cockpit's after-the-world moment): one full-screen multiply, only while |E - 1| shows ---- */
static VkPipeline rqc_pexpo;
static VkShaderModule rqc_expo_vs, rqc_expo_fs;
/* its pipeline (at the first frame whose exposure shows, or before the first eye frame: PS2_RQ_PREWARM); 0 = none */
static int rqc_expo_make(void) {
    if (rqc_pexpo) return 1;
    if (!expo.ok || !rqe.rp) return 0;
    const u64 rqpw_t0 = vk_mono_ns();
    {
        char spv[1400];
        if (!rqc_expo_vs) { snprintf(spv, sizeof spv, "%s/cockpit_comp.vert.spv", rqc_shader_base); rqc_expo_vs = load_spv(spv); }
        if (!rqc_expo_fs) { snprintf(spv, sizeof spv, "%s/eye_adapt.frag.spv", rqc_shader_base); rqc_expo_fs = load_spv(spv); }
        if (!rqc_expo_vs || !rqc_expo_fs) return 0;
        VkPipelineShaderStageCreateInfo st[2]; memset(st, 0, sizeof st);
        st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = rqc_expo_vs; st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = rqc_expo_fs; st[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vin; memset(&vin, 0, sizeof vin); vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia; memset(&ia, 0, sizeof ia); ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vs; memset(&vs, 0, sizeof vs); vs.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vs.viewportCount = 1; vs.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs; memset(&rs, 0, sizeof rs); rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms; memset(&ms, 0, sizeof ms); ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = rq_samples();
        VkPipelineDepthStencilStateCreateInfo dss; memset(&dss, 0, sizeof dss); dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO; dss.maxDepthBounds = 1.0f;
        VkPipelineColorBlendAttachmentState cba; memset(&cba, 0, sizeof cba);
        cba.blendEnable = VK_TRUE; cba.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR; cba.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba.alphaBlendOp = VK_BLEND_OP_ADD;
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
        VkPipelineColorBlendStateCreateInfo cb; memset(&cb, 0, sizeof cb); cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cb.attachmentCount = 1; cb.pAttachments = &cba;
        static const VkDynamicState dy[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dsi; memset(&dsi, 0, sizeof dsi); dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; dsi.dynamicStateCount = 2; dsi.pDynamicStates = dy;
        VkGraphicsPipelineCreateInfo gp; memset(&gp, 0, sizeof gp);
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia; gp.pViewportState = &vs;
        gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &dss; gp.pColorBlendState = &cb; gp.pDynamicState = &dsi; gp.layout = expo.layout; gp.renderPass = rqe.rp;
        if (vkCreateGraphicsPipelines(dev, pipe_cache, 1, &gp, NULL, &rqc_pexpo) != VK_SUCCESS) { rqc_pexpo = VK_NULL_HANDLE; return 0; }
    }
    rqpw_made("the eye adaptation's pipeline (rqc_expo)", rqpw_t0);
    return 1;
}
static void rqc_expo(VkCommandBuffer cmd) {
    if (!expo.ok) return;
    if (!perf_ab_pass && !cc_extra && !bench_pass && !xr_second_pass && expo.k > 0.0f) expo_update();
    float E = expo.test > 0.0f ? expo.test : expo.k > 0.0f ? expo.E : 1.0f;
    if (fabsf(E - 1.0f) < 0.003f || expo_ab_off() || perf_ab_flipped("expo")) return;
    if (!rqc_expo_make()) return;
    rqc_set_view(cmd);
    float pcv[4] = { E > 1.0f ? E - 1.0f : 0.0f, E < 1.0f ? E : 1.0f, 0.0f, 0.0f };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqc_pexpo);
    vkCmdPushConstants(cmd, expo.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pcv, pcv);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    expo.passes++;
}

/* ---- the frame: before the pass (ready = the Quest path draws the model this frame), the draws for the agent `order` ---- */
static int rqc_before_pass(VkCommandBuffer cmd, vk_frame *fr) {
    if (!rqc_cfg()) return 0;
    canopy_update();                                      /* the cloud feel's state (veil_pass's clock, the canopy's water) */
    cf_flash = 0.0f;                                      /* (the cloud volume's flash: only veil_pass sets it) */
    {   static u32 nl;                                    /* (why a frame stays on the old cockpit: the first few) */
        if (nl < 6u && (!cpit.rqc || !cpit.rqc->ok || !cpit_ready())) { nl++; ps2_log("cockpit material: frame without the material: model %s, rqc %p ok %d, ready %d, serial %u", cpit.path, (void *)cpit.rqc, cpit.rqc ? cpit.rqc->ok : -1, cpit_ready(), cpit.serial); }
    }
    if (!cpit_ready() || !rqc_frame_begin(fr) || !rqco_pipes()) return 0;
    if (cpf_pre) rqc.n_oldpass++;                         /* (the fold's old model pass ran for this frame: only with DBG 9) */
    if (!RQC_DBG(3)) rqc_sl_copy(cmd, fr);
    if (canopy_active() && !RQC_DBG(5)) rqw_blur_update(cmd);
    return 1;
}
/* inside the pass: the solid parts, the screens, the combiner's mark. rq_cockpit_solid = first thing in the pass */
static void rq_cockpit_solid(VkCommandBuffer cmd, vk_frame *fr) {
    if (!rqc_cfg() || !rql.cp_want || !rql.cp_ok || rql.cp_drawn) return;
    if (RQC_DBG(7)) { rql.cp_drawn = 1; return; }
    if (!rqc_draw_solid(cmd, fr)) return;
    rql.cp_drawn = 1;
    const rqc_frame *F = rqc_F(cpit.rqc, (u32)(fr - frames));
    rqc_draw_screens(cmd, F);
    /* The HUD's combiner mark is depth (the plate's, then HG_MARK over it, 0.995: above the world's range). Drawn here, at the start of the pass
       (ACZ_VR_RQ_COCKPIT_FIRST), it hides the world behind the plate: every world draw after it fails the depth test there, and the plate showed the clear
       colour with the cloud composite's blur over it (the F-5E on the headset, 2026-11-09). The PC marks after the world. So with the cockpit first the
       mark waits for the world: rq_cockpit_after_world, the boundary before the HUD that reads it. ACZ_VR_RQ_COCKPIT_MARKS_LATE=0: at the start, as before */
    static int late = -1;
    if (late < 0) { const char *e = getenv("ACZ_VR_RQ_COCKPIT_MARKS_LATE"); late = !(e && *e == '0'); }
    rqc.mk_due = 0;
    if (late && rqc_first()) { rqc.mk_due = 1; rqc.mk_fr = fr; }
    else rqc_draw_marks(cmd, F);
    cpit_frame_samples = (int)rq_samples();
}
/* The combiner's mark writes HG_MARK over every pixel inside the combiner's depth range, so also over solid parts about as far away as the plate (the A-10A's
   up-front panel, nearer than the plate's foot). The PC draws its glass before the mark exists; here the glass comes after the HUD (the plan's order) and would pass on those
   pixels: the plate's tint over the panel. So once the HUD is done, the parts round the combiner put their true depth back, in one depth-only draw (ACZ_VR_RQ_COCKPIT_DEPTHBACK=0: off) */
static void rqc_depth_back(VkCommandBuffer cmd, vk_frame *fr) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("ACZ_VR_RQ_COCKPIT_DEPTHBACK"); on = !(e && *e == '0'); }
    rqc_model *q = cpit.rqc;
    if (!on || !q || !q->near_count || !rqc.dback || !cpit.ap_ok || !hg_drawn) return;
    u32 slot = (u32)(fr - frames);
    rqc_set_view(cmd);
    VkDescriptorSet sets[3] = { q->texset, cpit.sm_ok ? cpit.sm_set : white_set, q->fset[slot] };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqc.layout, 0, 3, sets, 0, NULL);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &q->vb, &zero);
    vkCmdBindIndexBuffer(cmd, q->ib, 0, VK_INDEX_TYPE_UINT32);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rqc.dback);
    vkCmdDrawIndexed(cmd, q->near_count, 1, q->near_first, 0, 0);
    rqc.fr_pipes++; rqc.fr_draws++;
}
/* inside the pass after the HUD: the dry glass by need (the wet canopy has its own pipeline) */
static void rq_cockpit_glass(VkCommandBuffer cmd, vk_frame *fr) {
    if (!rqc_cfg() || !rql.cp_ok || rql.cp_glass) return;
    rql.cp_glass = 1;
    /* the wet canopy's own pipeline only while it shows anything: the water, the film, frost, flecks */
    int flecks = rqw_mark(0) != 0.0f || rqw_mark(1) != 0.0f || rqw_mark(2) != 0.0f;
    static int glass_on = -1;                             /* (ACZ_VR_COCKPIT_GLASS=0, as cpit_draw: this path did not read it) */
    if (glass_on < 0) { const char *e = getenv("ACZ_VR_COCKPIT_GLASS"); glass_on = !(e && *e == '0'); }
    if (!glass_on || RQC_DBG(6)) return;
    rqc_depth_back(cmd, fr);
    int wet = rqc.wet && rq_feed_valid && (canopy_active() || flecks) && cpit.rqc && !RQC_DBG(5);
    int lean = wet && (rqc_wet_cfg() & 1) && rqw.ok && rqc.lwet[0] && cpit.rqc->gvb && canopy_active();   /* (ACZ_VR_RQ_COCKPIT_WET) */
    if (wet && (rqc_wet_cfg() & 1) && !lean && !(canopy_active())) wet = 0;                                 /* (the lean path draws no debris flecks) */
    if (lean) rqw_params();
    int gx = rqgx.live && rqgx.pok && cpit.rqc->gvb && cpit.rqc->gxst && cpit.rqc->gxtc;   /* (the readable glass: rq_glass.inc; every ACZ_VR_RQ_GLASS_* 0: the dry glass as before) */
    u32 fi = gx ? rqgx_draw_glass(cmd, fr, lean ? 2 : wet) : rqc_draw_glass_dry(cmd, fr, lean ? 2 : wet);
    if (lean) {
        rqw_draw(cmd, fr, fi); rqc.n_wet++;
        if (gx) rqw.next_free += rqgx_draw_wet_extra(cmd, fr, rqw.next_free);
        if (flecks && rqc_wet_cfg() == 3) rqw_flecks(cmd, fr);
    }
    else if (flecks && rqc_wet_cfg() == 3) { rqw.next_free = fi; rqw_flecks(cmd, fr); }
    else if (wet) { rqc_draw_glass_wet(cmd, fr, fi); rqc.n_wet++; }
    if (gx) rqgx_draw_dust(cmd, fr);
    rqgx_draw_cracks(cmd, fr);                            /* (R-58: ACZ_VR_RQ_COCKPIT_CRACKS) */
    static u32 nf;
    if (!(++nf % 240u) && cpit.rqc) {
        char ib[64]; int ipos = 0;
        for (int li = 0; li < 3; li++) { for (u32 c = 0; c <= RQC_CHUNKS; c++) ipos += snprintf(ib + ipos, sizeof ib - (size_t)ipos, "%c", rqc.dbg_in[li][c] ? '#' : '.'); ib[ipos++] = ' '; ib[ipos] = 0; }
        ps2_log("cockpit material: sectors in view %s (eye z in the cockpit frame %.2f %.2f %.2f; tangents x %.2f %.2f y %.2f %.2f)", ib, (double)rqc.dbg_r0, (double)rqc.dbg_r1, (double)rqc.dbg_r2, (double)rqc.dbg_xl, (double)rqc.dbg_xr, (double)rqc.dbg_yb, (double)rqc.dbg_yt);
        ps2_log("cockpit material: canopy terms now: water %.4f mist %.4f grime %.4f smear %.4f film %.4f rime %.4f soot %.4f flecks %d; old model pass ran %llu frames",
                (double)(cx_now() ? cx_b + cx_r : canopy_v[0]), (double)(cx_now() ? cx_mist : canopy_v[3]), (double)canopy_v[4], (double)canopy_v[6], (double)canopy_film(), (double)ice_v[1], (double)cs_soot_draw(), flecks, (unsigned long long)rqc.n_oldpass);
        ps2_log("cockpit material: %llu frames (%llu wet, %.0f wet tris a wet frame); chunks in view %.0f%% of the solid indices; this frame %u draws, %u pipeline binds; shadow map %llu drawn, %llu kept; glass triangles drawn %.0f a frame of %u; solid %u + cut %u tris",
                (unsigned long long)rqc.n_frames, (unsigned long long)rqc.n_wet, rqc.n_wet ? (double)rqc.n_wtris / (double)rqc.n_wet : 0.0, rqc.tris_all ? 100.0 * (double)rqc.tris_seen / (double)rqc.tris_all : 0.0, rqc.fr_draws, rqc.fr_pipes, (unsigned long long)rqc.n_sm, (unsigned long long)rqc.n_sm_kept,
                rqc.n_gfr ? (double)rqc.n_gtris / (double)rqc.n_gfr : 0.0, cpit.rqc->ngtri, cpit.rqc->tris_solid, cpit.rqc->tris_cut);
        rqgx_log();
    }
}
/* inside the pass, the world drawn (before the HUD): the eye adaptation's multiply */
static void rq_cockpit_after_world(VkCommandBuffer cmd) {
    if (rqc.mk_due && rqc_cfg() && cpit.rqc) {          /* (the combiner's mark held back from the pass's start: rq_cockpit_solid) */
        rqc.mk_due = 0;
        rqc_set_view(cmd);
        rqc_draw_marks(cmd, rqc_F(cpit.rqc, (u32)(rqc.mk_fr - frames)));
    }
    if (!rqc_cfg() || !rql.cp_ok || RQC_DBG(3)) return;
    rqc_expo(cmd);
}
