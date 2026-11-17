/* rq_slots.inc -- the off-screen slots' versions in the eye pass (BUGS_RQ R-59; included by ps2_video.c after rq_passes.inc, PS2_RQ_SUPPORT only).
   The hoist (fg_hoist) moves an off-screen build in front of the scene; a scene run it crosses that reads the slot the build rewrites reads a
   snapshot of the slot, which must hold the version the crossed readers saw in the game's order.
   - PS2_RQ_SNAP_AT: where the snapshots are taken. `0` (as before): one snapshot a slot (rq_snap_t), taken at the first draw into the slot this
     list -- the slot's FIRST writer, which is hoisted too, so the snapshot held the previous list's last version: in M2 / M4 / M6 / M9 the
     trees read last frame's cloud impostor from the slot the game shares between a tree type's impostor and its cloud impostors (white cards
     on the ground, the flights of 2026-11-10), and every reader of a slot read that one image whatever run had crossed it. `1`: the snapshot
     at the first draw of the run that crossed the readers (each crossing a claim); a second crossing run on a slot that already serves one is
     not hoisted (its list then keeps several slot-0 runs and takes the old path, counted). `2`: as 1, and a second crossing run gets a
     snapshot of its own (up to RQS_NV more images a list, made at the eye's slot size), so a chain of builds and readers interleaved in one
     slot (M2's take-off: a draw into slot 4, a slot-0 DATE draw reading it, 24 times) stays in the eye pass with each reader on its version.
     A reader keeps the first crossing's snapshot (the earliest writer after it in the game's order). A snapshot whose run did not reach the
     draw loop is taken at the list's first slot-0 draw (counted).
   - PS2_RQ_IMPDROP_RUNS=1: with our far clouds replacing the game's cloud sprites, the game's cloud-impostor builds that only those dropped
     sprites read are left out BEFORE the hoist, by version: walking the hoist's input order backwards, a run into a slot of the impostors'
     family (the slots the cloud sprites read, and what their builds read, transitively) is dead when no kept draw reads the slot before a
     later full overwrite of it (an opaque rectangle over the whole target) or the list's end (the next list predicted from this one: needed
     unless its first access to the slot is a full overwrite). rqo_imp_mask decides per slot and keeps a slot the trees read too; this walk
     drops the dead versions in it, so the hoist has nothing to cross there and the trees read the live slot.
   - PS2_RQ_SLOTCHECK=1 [debug]: every read of an off-screen slot in the eye order (not slot 0: the feed by design; not a self-read) is checked
     against the game's order (the list's index order): the last draw into the slot before the read (or the snapshot's, when the draw reads
     a snapshot) must be the one the game's order has. Mismatches are logged per family (slot, reader, expected and seen writer) with the
     first examples, and summed in a `rq slotcheck:` line every 600 checked lists.
   - PS2_RQ_SNAP_DUMP=1 [test]: in an eye-dump frame (PS2_XR_DUMP_EYES) the first snapshots as their crossed readers read them (_s) and the
     slot as the crossing run found it (_w: the version the game's order gives them), layer 0, over a grey checker where transparent:
     out/eye_<field>_snap<slot>v<tag>_s.ppm / _w.ppm, and a `rq snap:` line each. */
static int rqs_sw(const char *n, int *v) {
    if (*v < 0) { const char *e = getenv(n); *v = e && *e >= '1' && *e <= '9' ? *e - '0' : 0; if (*v) ps2_log("vk: %s=%d", n, *v); }
    return *v;
}
static int rqs_c_at = -1, rqs_c_imp = -1, rqs_c_chk = -1, rqs_c_dump = -1;
#define RQS_AT()   rqs_sw("PS2_RQ_SNAP_AT", &rqs_c_at)
#define RQS_IMP()  rqs_sw("PS2_RQ_IMPDROP_RUNS", &rqs_c_imp)
#define RQS_CHK()  rqs_sw("PS2_RQ_SLOTCHECK", &rqs_c_chk)
#define RQS_DUMP() rqs_sw("PS2_RQ_SNAP_DUMP", &rqs_c_dump)
#define RQS_NCL 64u
static struct {
    u8 *w;                        /* per draw: 1 + the claim whose snapshot is taken before this draw (the draws of the crossing run, its colour slot) */
    u8 *imp;                      /* per draw: 1 = a dead build of the game's cloud impostors (PS2_RQ_IMPDROP_RUNS) */
    int wset, impset, imp_now;
    u32 ncl, nver;
    struct { u8 slot, tag, done; u32 run, first; } cl[RQS_NCL];   /* the list's claims: a crossing run, its slot, the snapshot (tag 1 rq_snap_t, 2.. rqs_vt) */
    u8 cl_of[RT_SLOTS][RQS_NV + 2u];                               /* (slot, tag) -> 1 + claim */
    u8 cur_tag[RT_SLOTS];         /* (the run being hoisted) the tag its crossing gives readers of the slot */
    u8 vslot[RQS_NV];             /* the slot each extra snapshot serves this list */
    u64 n_lists, n_claim, n_second, n_full, n_late, n_imp_lists, n_imp_runs, n_imp_draws, n_snaps, n_vers;
    u32 said_second;
} rqs;
#define RQS_DMAX 6u
static struct {                   /* PS2_RQ_SNAP_DUMP */
    VkBuffer buf; VkDeviceMemory mem; void *ptr; VkDeviceSize cap;
    u32 n, slot[RQS_DMAX], tag[RQS_DMAX], w[RQS_DMAX], h[RQS_DMAX], kind[RQS_DMAX];
    VkDeviceSize off[RQS_DMAX];
    u64 done_s, done_w;           /* (per list: the claims recorded) */
} rqsd;

/* ---- PS2_RQ_SLOTCHECK: each read against the game's order ---- */
#define RQK_FAM 64u
static struct {
    int on;
    u32 *exp;                     /* per draw: 1 + the draw whose write its read sees in the game's order, 0 = the previous list's, ~0 = no read */
    u32 cur[RT_SLOTS], curz[RT_SLOTS], snapc[RQS_NCL];          /* the eye order as drawn: last colour / depth writer, each claim's snapshot */
    u64 lists, reads, mis, lists_mis;
    int list_mis;
    struct { u64 key, n; } fam[RQK_FAM];
    u32 nfam;
} rqk;
static int rqk_reads(const vk_draw *d) { return d->tex_rt >= 2u && d->tex_rt <= RT_SLOTS && !d->tex_self && d->tex_rt != d->rt + 1u; }
static int rqk_zw(const vk_draw *d) { return ((d->depth >> 2) & 1u) || (d->depth & ((1u << 9) | (1u << 10) | (1u << 12))); }
static u32 rqk_zt(const vk_draw *d) { return d->zrt < RT_SLOTS ? d->zrt : d->rt; }
/* the eye list's start (rq_now): the version each read sees in the game's order */
static void rqk_begin(void) {
    rqk.on = RQS_CHK() && !perf_ab_pass && !bench_pass && !cc_extra && !xr_second_pass;
    if (!rqk.on) return;
    if (!rqk.exp && !(rqk.exp = (u32 *)malloc(sizeof(u32) * VK_MAX_DRAWS))) { rqk.on = 0; return; }
    u32 cg[RT_SLOTS], zg[RT_SLOTS];
    memset(cg, 0, sizeof cg); memset(zg, 0, sizeof zg);
    for (u32 i = 0; i < rep_ndraws; i++) {
        const vk_draw *d = &rep_draws[i];
        rqk.exp[i] = ~0u;
        if (rqk_reads(d)) rqk.exp[i] = d->tex_depth ? zg[d->tex_rt - 1u] : cg[d->tex_rt - 1u];
        if (d->rt < RT_SLOTS) cg[d->rt] = i + 1u;
        if (rqk_zw(d) && rqk_zt(d) < RT_SLOTS) zg[rqk_zt(d)] = i + 1u;
    }
    memset(rqk.cur, 0, sizeof rqk.cur); memset(rqk.curz, 0, sizeof rqk.curz);
    for (u32 c = 0; c < RQS_NCL; c++) rqk.snapc[c] = ~1u;          /* (not copied this list: an older list's) */
    rqk.lists++; rqk.list_mis = 0;
}
static void rqk_wdesc(char *s, size_t n, u32 v) {
    if (v == 0u) { snprintf(s, n, "the previous list's"); return; }
    if (v == ~1u) { snprintf(s, n, "a snapshot of an earlier list"); return; }
    if (v - 1u >= rep_ndraws) { snprintf(s, n, "?"); return; }
    const vk_draw *w = &rep_draws[v - 1u];
    snprintf(s, n, "draw %u (rt %u xcls %u %s n %u blend %03X tex %d texrt %u)", v - 1u, w->rt, w->xcls, w->native == PS2_VK_NATIVE_MESH ? "mesh" : w->native ? "nat" : "gs",
             w->count, w->blend & 0x1FFu, (int)(s16)w->tex, w->tex_rt);
}
/* draw i reads its slot now; tag: the snapshot it reads (0 = the live slot) */
static void rqk_read(const vk_draw *d, u32 i, u32 tag, const char *how) {
    if (!rqk.on || !rqk_reads(d) || i >= rep_ndraws) return;
    u32 Y = d->tex_rt - 1u, seen, e = rqk.exp[i];
    if (d->tex_depth) seen = rqk.curz[Y];
    else if (tag) { u32 c = tag < RQS_NV + 2u ? rqs.cl_of[Y][tag] : 0u; seen = c ? rqk.snapc[c - 1u] : ~1u; }
    else seen = rqk.cur[Y];
    rqk.reads++;
    if (seen == e) return;
    rqk.mis++;
    if (!rqk.list_mis) { rqk.list_mis = 1; rqk.lists_mis++; }
    int mesh = d->native == PS2_VK_NATIVE_MESH && rep_mesh && d->mblock < rep_mesh->nblocks;
    u32 prog = mesh ? rep_mesh->blocks[d->mblock].prog : 15u, pass = mesh ? rep_mesh->blocks[d->mblock].pass : 3u;
    u32 ew = e && e - 1u < rep_ndraws ? rep_draws[e - 1u].rt : 63u, sw = seen && seen - 1u < rep_ndraws ? rep_draws[seen - 1u].rt : 63u;
    u64 key = (u64)Y | (u64)(d->rt & 63u) << 6 | (u64)(d->xcls & 7u) << 12 | (u64)(prog & 15u) << 15 | (u64)(pass & 3u) << 19 | (u64)(tag != 0) << 21 | (u64)(d->tex_depth != 0) << 22
            | (u64)(e == 0u) << 23 | (u64)(seen == 0u) << 24 | (u64)(seen == ~1u) << 25 | (u64)(ew & 63u) << 26 | (u64)(sw & 63u) << 32 | (u64)(cl_sprite(d) != 0) << 38;
    u32 f = 0;
    while (f < rqk.nfam && rqk.fam[f].key != key) f++;
    if (f == rqk.nfam && rqk.nfam < RQK_FAM) { rqk.fam[f].key = key; rqk.fam[f].n = 0; rqk.nfam++; }
    u64 nth = f < RQK_FAM ? ++rqk.fam[f].n : 0;
    if (f >= RQK_FAM || (nth > 3u && nth != 100u && nth != 10000u)) return;
    char a[160], b[160];
    rqk_wdesc(a, sizeof a, e); rqk_wdesc(b, sizeof b, seen);
    ps2_log("rq slotcheck: family %u (%llu): field %u: draw %u (rt %u xcls %u %s prog %u pass %u n %u blend %03X%s%s) reads slot %u%s %s%s: the game's order gives %s, the eye order %s", f,
            (unsigned long long)nth, ps2_gs_frame_count, i, d->rt, d->xcls, mesh ? "mesh" : d->native ? "nat" : "gs", prog, pass, d->count, d->blend & 0x1FFu, cl_sprite(d) ? " cloud sprite" : "",
            d->date ? " date" : "", Y, d->tex_depth ? " (depth)" : "", how, tag ? " through a snapshot" : "", a, b);
}
/* draw i is drawn: its writes */
static void rqk_write(const vk_draw *d, u32 i) {
    if (!rqk.on) return;
    if (d->rt < RT_SLOTS) rqk.cur[d->rt] = i + 1u;
    if (rqk_zw(d) && rqk_zt(d) < RT_SLOTS) rqk.curz[rqk_zt(d)] = i + 1u;
}
static void rqk_log(void) {
    if (!rqk.on || rqk.lists % 600u) return;
    char l[700]; int o = 0; l[0] = 0;
    for (u32 f = 0; f < rqk.nfam && o < 640; f++)
        o += snprintf(l + o, sizeof l - (size_t)o, " [%u] slot %u reader rt %u x%u p%u/%u%s%s exp %s%u seen %s%u: %llu;", f, (u32)(rqk.fam[f].key & 63u), (u32)((rqk.fam[f].key >> 6) & 63u),
                      (u32)((rqk.fam[f].key >> 12) & 7u), (u32)((rqk.fam[f].key >> 15) & 15u), (u32)((rqk.fam[f].key >> 19) & 3u), (rqk.fam[f].key >> 21) & 1u ? " snap" : "",
                      (rqk.fam[f].key >> 38) & 1u ? " cloud" : "", (rqk.fam[f].key >> 23) & 1u ? "prev/" : "", (u32)((rqk.fam[f].key >> 26) & 63u),
                      (rqk.fam[f].key >> 24) & 1u ? "prev/" : (rqk.fam[f].key >> 25) & 1u ? "stale/" : "", (u32)((rqk.fam[f].key >> 32) & 63u), (unsigned long long)rqk.fam[f].n);
    ps2_log("rq slotcheck: %llu eye lists checked, %llu reads of off-screen slots, %llu mismatches in %llu lists, %u families:%s", (unsigned long long)rqk.lists,
            (unsigned long long)rqk.reads, (unsigned long long)rqk.mis, (unsigned long long)rqk.lists_mis, rqk.nfam, l);
}

/* the list's start (before the RQ hoist): the marks of the last list cleared */
static void rqs_list_begin(void) {
    if (!rqs.w) rqs.w = (u8 *)calloc(VK_MAX_DRAWS, 1);
    else if (rqs.wset) memset(rqs.w, 0, VK_MAX_DRAWS);
    rqs.wset = 0; rqs.ncl = 0; rqs.nver = 0;
    memset(rqs.cl_of, 0, sizeof rqs.cl_of);
    rqs.imp_now = 0;
    rqs.n_lists++;
    rqsd.done_s = rqsd.done_w = 0;
    if (!(rqs.n_lists % 1200u) && (rqs.n_claim || rqs.n_imp_lists || RQS_AT() || RQS_IMP()))
        ps2_log("rq slots: %llu lists (PS2_RQ_SNAP_AT=%d): snapshots %llu (%llu crossings, %llu of them extra versions), second crossings not hoisted %llu, refused for room %llu, taken at the slot-0 start %llu; "
                "dead impostor builds left out in %llu lists: %.1f runs, %.1f draws a list", (unsigned long long)rqs.n_lists, RQS_AT(), (unsigned long long)rqs.n_snaps,
                (unsigned long long)rqs.n_claim, (unsigned long long)rqs.n_vers, (unsigned long long)rqs.n_second, (unsigned long long)rqs.n_full, (unsigned long long)rqs.n_late,
                (unsigned long long)rqs.n_imp_lists, rqs.n_imp_lists ? (double)rqs.n_imp_runs / (double)rqs.n_imp_lists : 0.0, rqs.n_imp_lists ? (double)rqs.n_imp_draws / (double)rqs.n_imp_lists : 0.0);
}
/* (fg_hoist) run q (positions a..b of order, colour slot rt) rewrites the slots m that a crossed scene run reads: 1 = the crossing goes on (rqs.cur_tag
   holds the snapshot the crossed readers of each slot of m take), 0 = refused (the run stays behind that reader) */
static int rqs_claim(u64 m, u32 q, u32 a, u32 b, const u32 *order, u32 rt) {
    int mode = RQS_AT();
    u32 need = 0;
    for (u32 k = 1; k < RT_SLOTS; k++) {                        /* (first: may every slot of m be served?) */
        if (!((m >> k) & 1u)) continue;
        u32 mine = 0, other = 0;
        for (u32 c = 0; c < rqs.ncl; c++) if (rqs.cl[c].slot == k) { if (rqs.cl[c].run == q + 1u) mine = c + 1u; else other = 1; }
        if (mine || !other || !mode) continue;
        if (mode == 1) {
            rqs.n_second++;
            if (rqs.said_second++ < 6u)
                ps2_log("rq slots: field %u: a second run into slot %u (draw %u, rt %u) crosses its readers: not hoisted (PS2_RQ_SNAP_AT=1)", ps2_gs_frame_count, k, order ? order[a] : a, rt);
            return 0;
        }
        need++;
    }
    if (rqs.nver + need > RQS_NV || rqs.ncl + 2u > RQS_NCL) { rqs.n_full++; return 0; }
    for (u32 k = 1; k < RT_SLOTS; k++) {
        if (!((m >> k) & 1u)) continue;
        u32 mine = 0, other = 0;
        for (u32 c = 0; c < rqs.ncl; c++) if (rqs.cl[c].slot == k) { if (rqs.cl[c].run == q + 1u) mine = c + 1u; else other = c + 1u; }
        if (mine) { rqs.cur_tag[k] = rqs.cl[mine - 1u].tag; continue; }
        if (other && !mode) { rqs.cur_tag[k] = 1u; continue; }  /* (as before: the slot's one snapshot) */
        u32 tag = other ? 2u + rqs.nver : 1u;
        if (other) { rqs.vslot[rqs.nver++] = (u8)k; rqs.n_vers++; }
        u32 c = rqs.ncl++;
        rqs.cl[c].slot = (u8)k; rqs.cl[c].tag = (u8)tag; rqs.cl[c].done = 0; rqs.cl[c].run = q + 1u; rqs.cl[c].first = order ? order[a] : a;
        rqs.cl_of[k][tag] = (u8)(c + 1u);
        rqs.cur_tag[k] = (u8)tag; rqs.n_claim++; rqs.wset = 1;
        if (k == rt && rqs.w) for (u32 x = a; x < b; x++) rqs.w[order ? order[x] : x] = (u8)(c + 1u);
    }
    return 1;
}
/* (fg_hoist, PS2_RQ_SNAP_AT >= 1) the crossed run's draws (positions a..b) that read a slot of m take its claim's snapshot, unless an earlier crossing gave them one */
static void rqs_flag(u32 a, u32 b, const u32 *order, u64 m) {
    for (u32 x = a; x < b; x++) {
        u32 i = order ? order[x] : x;
        const vk_draw *d = &rep_draws[i];
        if (d->tex_rt < 2u || d->tex_rt > RT_SLOTS || !((m >> (d->tex_rt - 1u)) & 1u) || rq_snapd[i]) continue;
        rq_snapd[i] = rqs.cur_tag[d->tex_rt - 1u];
    }
}
/* after the hoist: the extra snapshots' images (PS2_RQ_SNAP_AT=2), as rq_snap_prepare makes the slots' own; 0 = not available */
static int rqs_prepare(void) {
    for (u32 v = 0; v < rqs.nver; v++) {
        vk_target *s = &rts[rqs.vslot[v]], *t = &rqs_vt[v];
        if (!s->created) return 0;
        if (t->created && (t->ew != s->ew || t->eh != s->eh || t->sc != s->sc || t->mono != s->mono || tew(t) != tew(s) || teh(t) != teh(s))) {
            vkDeviceWaitIdle(dev);
            destroy_target_images(t);
        }
        if (!t->created) {
            t->ew = s->ew; t->eh = s->eh; t->sc = s->sc; t->mono = s->mono;
            if (create_target(t) != 0) return 0;
        }
    }
    return 1;
}
/* slot k's colour as it is now, into the image t (rq_snap_copy's copy) */
static void rqs_copy(VkCommandBuffer cmd, u32 k, vk_target *t) {
    vk_target *s = &rts[k];
    if (!s->created || !t->created || s->layout == VK_IMAGE_LAYOUT_UNDEFINED) return;
    if (rendering_open) { vkCmdEndRendering(cmd); rendering_open = 0; }
    VkImageLayout sold = s->layout;
    VkImageMemoryBarrier2 b[2];
    u32 w = tew(s) * tsc(s), h = teh(s) * tsc(s);
    barrier_fill(&b[0], s->color, VK_IMAGE_ASPECT_COLOR_BIT, sold, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier_fill(&b[1], t->color, VK_IMAGE_ASPECT_COLOR_BIT, t->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    barrier_flush(cmd, b, 2);
    VkImageCopy c; memset(&c, 0, sizeof c);
    c.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.srcSubresource.layerCount = rt_layers; c.dstSubresource = c.srcSubresource;
    c.extent.width = w; c.extent.height = h; c.extent.depth = 1;
    vkCmdCopyImage(cmd, s->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier_fill(&b[0], s->color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sold,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    barrier_fill(&b[1], t->color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    barrier_flush(cmd, b, 2);
    t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

/* ---- PS2_RQ_IMPDROP_RUNS: the dead versions of the game's cloud impostors ---- */
/* a draw that leaves nothing of what its target held where this list reads it: one opaque rectangle, RGBA written, no depth test, no blend,
   no DATE, no alpha test that can fail, no self-read (rq_passes.inc's version end), covering rb (the union of the boxes this list's draws read of
   the slot, target pixels; none read: the whole target -- slot targets grow to the largest scissor ever seen, 512 x 448 for a 128 x 128 impostor) */
static float rqs_rb[RT_SLOTS][4];
static int rqs_full(const vk_draw *e) {
    if (e->rt == 0u || e->rt >= RT_SLOTS || !rts[e->rt].created) return 0;
    rqg_rc R;
    if ((e->blend & 0x100u) || !(e->depth & 8u) || (e->depth & 0xE0u) || !((e->depth >> 4) & 1u) || ((e->depth >> 8) & 1u) || e->date || (e->depth & 3u) != 1u
        || rqg_real_at(e->frag_flags) || e->tex_self || ((e->frag_flags >> 24) & 1u) || !rqg_rect(e, &R)) return 0;
    const float *b = rqs_rb[e->rt];
    float x0 = 0.0f, y0 = 0.0f, x1 = (float)tew(&rts[e->rt]), y1 = (float)teh(&rts[e->rt]);
    if (b[2] > b[0] && b[3] > b[1]) { x0 = b[0]; y0 = b[1]; x1 = b[2]; y1 = b[3]; }
    if ((float)R.dx0 > x0 || (float)R.dy0 > y0 || (float)R.dx1 < x1 || (float)R.dy1 < y1) return 0;
    if ((float)e->scissor[0] > x0 || (float)e->scissor[2] > y0 || (float)(e->scissor[1] + 1) < x1 || (float)(e->scissor[3] + 1) < y1) return 0;
    return 1;
}
static int rqs_fixed(const vk_draw *e) {
    return e->rt >= RT_SLOTS || e->cockpit || e->hudblk || e->mfd || e->occl || (e->native && e->native != PS2_VK_NATIVE_2D && e->native != PS2_VK_NATIVE_MESH);
}
/* before the RQ hoist, with cld (our far clouds drop the game's cloud sprites): rqs.imp marks the dead runs; returns their draws */
static u32 rqs_imp_plan(const u32 *order, int cld) {
    rqs.imp_now = 0;
    if (!RQS_IMP() || !cld || rep_ndraws < 2u || rep_ndraws > VK_MAX_DRAWS) return 0;
    if (!rqs.imp && !(rqs.imp = (u8 *)calloc(VK_MAX_DRAWS, 1))) return 0;
    if (rqs.impset) { memset(rqs.imp, 0, VK_MAX_DRAWS); rqs.impset = 0; }
    u32 n = rep_ndraws;
    u64 fam = 0;
    for (u32 q = 0; q < n; q++) { const vk_draw *d = &rep_draws[order ? order[q] : q]; if (d->tex_rt >= 2u && d->tex_rt <= RT_SLOTS && cl_sprite(d)) fam |= 1ull << (d->tex_rt - 1u); }
    for (int ch = 1; ch && fam;) {                               /* (the slots those builds read, transitively) */
        ch = 0;
        for (u32 q = 0; q < n; q++) {
            const vk_draw *d = &rep_draws[order ? order[q] : q];
            if (d->rt == 0u || d->rt >= RT_SLOTS || !((fam >> d->rt) & 1u) || d->tex_rt < 2u || d->tex_rt > RT_SLOTS) continue;
            u64 s = 1ull << (d->tex_rt - 1u);
            if (!(fam & s)) { fam |= s; ch = 1; }
        }
    }
    fam &= ~1ull;
    if (present_rt < RT_SLOTS) fam &= ~(1ull << present_rt);
    if (RT_MFD < 64u) fam &= ~(1ull << RT_MFD);
    if (!fam) return 0;
    for (u32 k = 0; k < RT_SLOTS; k++) { rqs_rb[k][0] = rqs_rb[k][1] = 1e9f; rqs_rb[k][2] = rqs_rb[k][3] = -1e9f; }
    for (u32 q = 0; q < n; q++) {                                /* (what this list reads of each slot: rqs_full's box) */
        const vk_draw *d = &rep_draws[order ? order[q] : q];
        if (d->tex_rt < 2u || d->tex_rt > RT_SLOTS || d->tex_depth || !rts[d->tex_rt - 1u].created) continue;
        float b[4], *u = rqs_rb[d->tex_rt - 1u];
        rqg_read_box(d, &rts[d->tex_rt - 1u], b);
        if (b[0] < u[0]) u[0] = b[0];
        if (b[1] < u[1]) u[1] = b[1];
        if (b[2] > u[2]) u[2] = b[2];
        if (b[3] > u[3]) u[3] = b[3];
    }
    u8 need[RT_SLOTS], seen[RT_SLOTS];
    memset(seen, 0, sizeof seen);
    for (u32 k = 0; k < RT_SLOTS; k++) need[k] = 1;
    for (u32 q = 0; q < n; q++) {                                /* (the list's end: what the next list reads first, predicted from this one) */
        const vk_draw *d = &rep_draws[order ? order[q] : q];
        if (d->tex_rt >= 2u && d->tex_rt <= RT_SLOTS && !cl_sprite(d)) {
            u32 Y = d->tex_rt - 1u;
            if (!seen[Y]) { seen[Y] = 1; need[Y] = 1; }
        }
        if (d->rt > 0u && d->rt < RT_SLOTS && !seen[d->rt]) { seen[d->rt] = 1; need[d->rt] = rqs_full(d) ? 0 : 1; }
    }
    u32 nd = 0, nruns = 0;
    for (u32 q = n; q-- > 0; ) {
        const vk_draw *d = &rep_draws[order ? order[q] : q];
        u32 rt = d->rt, zt = rqk_zt(d);
        u32 a = q;                                               /* (the run ending at q: consecutive draws into the same colour and depth slot, as fg_hoist's) */
        while (a > 0u) { const vk_draw *e = &rep_draws[order ? order[a - 1u] : a - 1u]; if (e->rt != rt || rqk_zt(e) != zt) break; a--; }
        int fixed = 0;
        for (u32 x = a; x <= q && !fixed; x++) fixed = rqs_fixed(&rep_draws[order ? order[x] : x]);
        int dead = rt > 0u && rt < RT_SLOTS && ((fam >> rt) & 1u) && !fixed && !need[rt] && (zt == rt || (zt < RT_SLOTS && !need[zt]));
        if (dead) {
            for (u32 x = a; x <= q; x++) rqs.imp[order ? order[x] : x] = 1;
            nd += q - a + 1u; nruns++;
            q = a;
            continue;
        }
        for (u32 x = q + 1u; x-- > a; ) {                         /* (kept: its writes end older versions where they cover the target, its reads need theirs) */
            const vk_draw *e = &rep_draws[order ? order[x] : x];
            if (e->rt > 0u && e->rt < RT_SLOTS && rqs_full(e)) need[e->rt] = 0;
            if (e->tex_rt >= 2u && e->tex_rt <= RT_SLOTS && !(cld && cl_sprite(e))) need[e->tex_rt - 1u] = 1;
        }
        q = a;
    }
    if (nd) { rqs.impset = 1; rqs.imp_now = 1; rqs.n_imp_lists++; rqs.n_imp_runs += nruns; rqs.n_imp_draws += nd; }
    if (xr_dump_now && getenv("PS2_RQ_LIST")) {
        ps2_log("rq slots: field %u: impostor family 0x%llX, dead runs %u (%u draws) left out before the hoist", ps2_gs_frame_count, (unsigned long long)fam, nruns, nd);
        for (u32 k = 1; k < RT_SLOTS; k++) {
            if (!((fam >> k) & 1u)) continue;
            u32 nw = 0, nr = 0, nf = 0, nds = 0, fq = ~0u;
            for (u32 q = 0; q < n; q++) {
                const vk_draw *d = &rep_draws[order ? order[q] : q];
                if (d->rt == k) { nw++; if (rqs_full(d)) nf++; if (rqs.imp[order ? order[q] : q]) nds++; if (fq == ~0u) fq = q; }
                if (d->tex_rt == k + 1u) nr++;
            }
            const vk_draw *f0 = fq != ~0u ? &rep_draws[order ? order[fq] : fq] : NULL;
            ps2_log("rq slots:   slot %u: %u draws into it (%u version ends, %u left out), %u reads, read box %.0f,%.0f..%.0f,%.0f; first draw blend %03X depth %03X frag %08X n %u",
                    k, nw, nf, nds, nr, (double)rqs_rb[k][0], (double)rqs_rb[k][1], (double)rqs_rb[k][2], (double)rqs_rb[k][3],
                    f0 ? f0->blend & 0x1FFu : 0u, f0 ? f0->depth : 0u, f0 ? f0->frag_flags : 0u, f0 ? f0->count : 0u);
        }
    }
    return nd;
}
/* (fg_hoist) the run at position a of order is a dead impostor build: it neither reads nor writes for the hoist */
static int rqs_run_dead(const u32 *order, u32 a) { return rqs.imp_now && rqs.imp && rqs.imp[order ? order[a] : a]; }

/* ---- PS2_RQ_SNAP_DUMP: the snapshots' images ---- */
/* one image (layer 0 of t->color, now in layout lay) into the dump buffer; kind 0 = the snapshot, 1 = the slot at the crossing run */
static void rqsd_rec(VkCommandBuffer cmd, vk_target *t, VkImageLayout lay, u32 slot, u32 tag, u32 kind) {
    if (!t->created || lay == VK_IMAGE_LAYOUT_UNDEFINED || rqsd.n >= RQS_DMAX) return;
    u32 w = tew(t) * tsc(t), h = teh(t) * tsc(t);
    VkDeviceSize one = (VkDeviceSize)w * h * 4u, at = rqsd.n ? rqsd.off[rqsd.n - 1u] + (VkDeviceSize)rqsd.w[rqsd.n - 1u] * rqsd.h[rqsd.n - 1u] * 4u : 0;
    if (!rqsd.buf) {
        VkDeviceSize cap = (VkDeviceSize)32u << 20;
        if (make_buffer(cap, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &rqsd.buf, &rqsd.mem, &rqsd.ptr) != 0) { rqsd.buf = VK_NULL_HANDLE; return; }
        rqsd.cap = cap;
    }
    if (at + one > rqsd.cap) return;
    if (rendering_open) { vkCmdEndRendering(cmd); rendering_open = 0; }
    VkImageMemoryBarrier2 b;
    barrier_fill(&b, t->color, VK_IMAGE_ASPECT_COLOR_BIT, lay, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier_flush(cmd, &b, 1);
    VkBufferImageCopy rc; memset(&rc, 0, sizeof rc);
    rc.bufferOffset = at;
    rc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rc.imageSubresource.layerCount = 1;
    rc.imageExtent.width = w; rc.imageExtent.height = h; rc.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd, t->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rqsd.buf, 1, &rc);
    barrier_fill(&b, t->color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, lay, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    barrier_flush(cmd, &b, 1);
    {   VkMemoryBarrier2 mb; memset(&mb, 0, sizeof mb);           /* (visible to the host after the frame) */
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        mb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT; mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo di; memset(&di, 0, sizeof di);
        di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO; di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &di);
    }
    rqsd.slot[rqsd.n] = slot; rqsd.tag[rqsd.n] = tag; rqsd.w[rqsd.n] = w; rqsd.h[rqsd.n] = h; rqsd.kind[rqsd.n] = kind; rqsd.off[rqsd.n] = at; rqsd.n++;
}
static void rqsd_write(u32 field) {                             /* (eyedump_write, after the queue is idle) */
    if (!rqsd.n || !rqsd.ptr) { rqsd.n = 0; return; }
    for (u32 j = 0; j < rqsd.n; j++) {
        char nm[96];
        snprintf(nm, sizeof nm, "out/eye_%06u_snap%uv%u_%c.ppm", field, rqsd.slot[j], rqsd.tag[j], rqsd.kind[j] ? 'w' : 's');
        FILE *fp = fopen(nm, "wb");
        if (!fp) continue;
        u32 w = rqsd.w[j], h = rqsd.h[j];
        fprintf(fp, "P6\n%u %u\n255\n", w, h);
        const u8 *px = (const u8 *)rqsd.ptr + rqsd.off[j];
        u64 sum[4] = { 0, 0, 0, 0 }, nop = 0;
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++, px += 4) {
                u32 a = px[3] >= 128u ? 255u : px[3] * 2u;            /* (GS alpha: 0x80 = opaque) */
                u32 g = ((x / 8u) ^ (y / 8u)) & 1u ? 96u : 160u;
                u8 o[3];
                for (int c = 0; c < 3; c++) o[c] = (u8)((px[c] * a + g * (255u - a)) / 255u);
                fwrite(o, 1, 3, fp);
                if (px[3]) { nop++; for (int c = 0; c < 4; c++) sum[c] += px[c]; }
            }
        fclose(fp);
        ps2_log("rq snap: %s: slot %u %ux%u (%s): %llu px with alpha, their mean rgb %.0f %.0f %.0f alpha %.0f", nm, rqsd.slot[j], w, h,
                rqsd.kind[j] ? "the slot as the crossing run found it: the version the game's order gives the readers" : "the snapshot its crossed readers read",
                (unsigned long long)nop, nop ? (double)sum[0] / (double)nop : 0.0, nop ? (double)sum[1] / (double)nop : 0.0, nop ? (double)sum[2] / (double)nop : 0.0,
                nop ? (double)sum[3] / (double)nop : 0.0);
    }
    rqsd.n = 0;
}

/* the draw loop, before draw i is drawn: the snapshots due now and the dumps; 1 = something was recorded (the loop's target state is stale) */
static void rqs_take(VkCommandBuffer cmd, u32 c, int late) {
    u32 k = rqs.cl[c].slot, tag = rqs.cl[c].tag;
    vk_target *t = tag > 1u ? &rqs_vt[tag - 2u] : &rq_snap_t[k];
    if (tag > 1u) rqs_copy(cmd, k, t); else rq_snap_copy(cmd, k);
    rqs.cl[c].done = 1; rqs.n_snaps++;
    if (late) rqs.n_late++;
    if (rqk.on) rqk.snapc[c] = rqk.cur[k];
    if (xr_dump_now && RQS_DUMP() && !perf_ab_pass && c < 64u && !((rqsd.done_s >> c) & 1u)) { rqsd.done_s |= 1ull << c; rqsd_rec(cmd, t, t->layout, k, tag, 0u); }
}
static int rqs_at_draw(VkCommandBuffer cmd, u32 i, const vk_draw *d) {
    int did = 0, dmp = xr_dump_now && RQS_DUMP() && !perf_ab_pass;
    u32 wc = rqs.wset ? rqs.w[i] : 0u;                          /* (1 + claim: a draw of a crossing run) */
    if (dmp && wc && wc - 1u < 64u && !((rqsd.done_w >> (wc - 1u)) & 1u)) {
        u32 k = rqs.cl[wc - 1u].slot;
        rqsd.done_w |= 1ull << (wc - 1u);
        ps2_log("rq snap: field %u: slot %u claim %u (tag %u): its crossing run starts at draw %u (the run's first %u); the snapshot %s", ps2_gs_frame_count, k, wc - 1u, rqs.cl[wc - 1u].tag,
                i, rqs.cl[wc - 1u].first, RQS_AT() ? "is taken now" : ((rq_snap_done >> k) & 1u) ? "was taken before (at the slot's first draw this list)" : "is taken now");
        rqsd_rec(cmd, &rts[k], rts[k].layout, k, rqs.cl[wc - 1u].tag, 1u); did = 1;
    }
    if (!RQS_AT()) {                                            /* (as before: each slot's snapshot at its first draw this list) */
        u64 pend = rq_snap_mask & ~rq_snap_done;
        if (d->rt < RT_SLOTS && ((pend >> d->rt) & 1u)) {
            u32 c = rqs.cl_of[d->rt][1] ? rqs.cl_of[d->rt][1] - 1u : RQS_NCL;
            if (c < RQS_NCL) rqs_take(cmd, c, 0);
            else { rq_snap_copy(cmd, d->rt); rqs.n_snaps++; }
            did = 1;
        }
        return did;
    }
    if (wc && !rqs.cl[wc - 1u].done) { rqs_take(cmd, wc - 1u, 0); did = 1; }
    if (d->rt == 0u)
        for (u32 c = 0; c < rqs.ncl; c++) if (!rqs.cl[c].done) { rqs_take(cmd, c, 1); did = 1; }
    return did;
}
#include "rq_treemip.inc"                              /* PS2_RQ_TREE_AA bit 16 (R-242): the tree impostors' mip chains */
