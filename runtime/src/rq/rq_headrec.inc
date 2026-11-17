/* rq_headrec.inc -- the head recorder (PS2_XR_HEAD_RECORD) and head scripts of any length (PS2_XR_TEST_HEAD_SCRIPT_FILE);
   included by ps2_xr.c before recenter(), PS2_RQ_SUPPORT only (the default PC objects stay as they were). Agent tape,
   2026-11-09: a head moved by a person in the headset is written down in the format of PS2_XR_TEST_HEAD_SCRIPT and played
   back into later unattended runs, on the headset (with PS2_XR_TEST_HEAD_LEVEL) or the PC's OpenXR simulator.

   The recorder. Every PS2_XR_HEAD_RECORD_EVERY fields (default 6) locate_views hands over the head as the frame is drawn
   from it (the left eye's orientation, the point between the eyes; every test pose included), and it is written relative
   to the frame F as one entry "field~yaw,pitch,roll,x,y,z" (the first one "field:...") all on one line, separated by
   spaces: the file's content is a PS2_XR_TEST_HEAD_SCRIPT value (or a PS2_XR_TEST_HEAD_SCRIPT_FILE). Where no views were
   located (the cinema screen: menus, loading, the flight start's hand-over in the simulator's runs took 40 fields at one
   time and none at another) the next entry still ramps: a played-back run whose own screen time falls elsewhere then
   sees the head move across the gap instead of holding and jumping (a hold + jump measured 14 deg off a repeat run of
   the same script, the ramp at most the cut corner). Angles in degrees, the script's convention
   (q = Ry(yaw) Rx(pitch) Rz(-roll): + = left, up, right ear down); yaw and roll are unwrapped across +-180 so that no
   ramp takes the long way round; position in metres in F's axes (+ = right, up, back); 0.01 deg and 0.1 mm.
   F is the reference set by the last recenter() (the session's first frame, the recentre once focused, F9, the
   gamepad's Select hold, a moved LOCAL space), not by the flight-start hand-over (recenter_hand_over). Why: a played-back
   run does its recenter()s on the tracked head, which on the simulator (or a levelled headset) is the reference the
   script is relative to, and its hand-over on the played head (xr_latest), which then sees the same angle against its
   reference as the recorded run did and lands where it landed -- so every frame of the played-back run sees the
   recorded head relative to its reference, a hand-over included. Not reproduced: a recenter() of the recorded run after
   a hand-over that moved the reference (F9, Select, the headset's own recentre): the played run has nothing to do it.
   Near pitch +-90 deg yaw and roll are ill-defined (a 0.1 s ramp may swing there).
   The file is written in one buffered stream, flushed every 10 entries in whole entries (a killed process loses at most
   the last second at the default interval, never half an entry) and closed with a line end at ps2_xr_shutdown.

   The script reader. hrs_load takes PS2_XR_TEST_HEAD_SCRIPT_FILE (when it is set and readable; it wins over
   PS2_XR_TEST_HEAD_SCRIPT) or PS2_XR_TEST_HEAD_SCRIPT, with the inline parser's grammar but any number of entries and
   any blanks between them (spaces, tabs, line ends: the headset's commandline.txt cuts lines at 4095 characters, about
   80 recorded entries, so long scripts go in a file). hrs_find picks the entry in force at a field: the last one whose
   field is reached (by halves when the fields never decrease, as recorded; in order otherwise, as before). */
extern u64 ps2_kernel_vblank_count(void);
typedef struct { long f; float y, p, r, x[3]; int ramp; } hrs_entry;
static int hrs_sorted = 1;

static int hrs_blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
/* s -> *out (malloc'd, NULL when empty); the number of entries; *stop = the bytes left unread (a malformed entry) */
static int hrs_parse(const char *s, hrs_entry **out, long *stop) {
    int n = 0, cap = 0;
    hrs_entry *e = NULL;
    const char *p = s;
    *out = NULL;
    if (stop) *stop = 0;
    while (p && *p) {
        char *end;
        while (hrs_blank(*p)) p++;
        if (!*p) break;
        long f = strtol(p, &end, 10);
        if (end == p || (*end != ':' && *end != '~')) break;
        if (n == cap) {
            int nc = cap ? 2 * cap : 256;
            hrs_entry *ne = (hrs_entry *)realloc(e, (size_t)nc * sizeof *e);
            if (!ne) break;
            e = ne; cap = nc;
        }
        hrs_entry *h = &e[n];
        h->f = f; h->ramp = *end == '~';
        p = end + 1;
        float a[6] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        for (int k = 0; k < 6; k++) {
            a[k] = strtof(p, &end);
            if (end == p) break;
            p = end;
            if (*p != ',') break;
            p++;
        }
        h->y = a[0]; h->p = a[1]; h->r = a[2];
        h->x[0] = a[3]; h->x[1] = a[4]; h->x[2] = a[5];
        n++;
        while (*p && !hrs_blank(*p)) p++;
    }
    if (stop && p) { while (hrs_blank(*p)) p++; *stop = (long)strlen(p); }
    if (!n) { free(e); e = NULL; }
    *out = e;
    return n;
}
/* the whole file as a NUL-terminated string (malloc'd), NULL when it cannot be read (or is above 64 MB) */
static char *hrs_read_file(const char *path, long *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *b = NULL;
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0) n = ftell(f);
    if (n >= 0 && n <= (64L << 20) && fseek(f, 0, SEEK_SET) == 0 && (b = (char *)malloc((size_t)n + 1u)) != NULL) {
        if ((long)fread(b, 1, (size_t)n, f) != n) { free(b); b = NULL; }
        else b[n] = 0;
    }
    fclose(f);
    if (size) *size = n;
    return b;
}
/* the script in force: PS2_XR_TEST_HEAD_SCRIPT_FILE, else sc (PS2_XR_TEST_HEAD_SCRIPT) */
static int hrs_load(const char *sc, hrs_entry **out) {
    const char *fn = getenv("PS2_XR_TEST_HEAD_SCRIPT_FILE");
    char *buf = NULL;
    if (fn && *fn) {
        long sz = 0;
        buf = hrs_read_file(fn, &sz);
        if (buf) {
            ps2_log("xr: test head script from %s, %ld bytes (PS2_XR_TEST_HEAD_SCRIPT_FILE)%s", fn, sz,
                    sc && *sc ? "; PS2_XR_TEST_HEAD_SCRIPT ignored" : "");
            sc = buf;
        } else ps2_log("xr: PS2_XR_TEST_HEAD_SCRIPT_FILE %s cannot be read%s", fn,
                       sc && *sc ? "; PS2_XR_TEST_HEAD_SCRIPT used" : "");
    }
    long stop = 0;
    int n = hrs_parse(sc, out, &stop);
    if (stop) ps2_log("xr: test head script: read %d entries, then stopped at a malformed entry (%ld bytes left)", n, stop);
    free(buf);
    hrs_sorted = 1;
    for (int k = 1; k < n; k++) if ((*out)[k].f < (*out)[k - 1].f) hrs_sorted = 0;
    return n;
}
/* [test] PS2_PAD_PLAY_SYNC=L (rq10_artefacts 2026-11-15; ps2_hle.h PS2_RQ_PAD_SYNC, called by ps2_hle_pad.c at every pad latch of a replay):
   a user's tapes replayed on another machine. The device's intro ran a different number of fields (its disc loads; the user's START to
   skip the rest of it lands in flight here, opens the pause menu, and the flight diverges): the replay is held at the tape's first latch
   (neutral) until the mission starts here (ps2_imm_flight), then goes on from latch L (the latch of the device's mission start: its
   "canopy state reset (mission start, field L)"), and the head script's lookups move by the same offset (L - this field). Test runs only */
long ps2_rq_head_off;
void ps2_rq_pad_sync(u64 *used, u64 n) {
    static long S = -2; static int done;
    if (S == -2) { const char *e = getenv("PS2_PAD_PLAY_SYNC"); S = e && *e ? atol(e) : -1; if (S >= 0) ps2_log("pad: [test] PS2_PAD_PLAY_SYNC=%ld: the tape held until the mission starts", S); }
    if (S < 0 || done) return;
    extern volatile u32 ps2_imm_flight;
    if (!ps2_imm_flight) { *used = 0; return; }
    extern u64 ps2_kernel_vblank_count(void);
    u64 vb = ps2_kernel_vblank_count();
    if ((u64)S < n) *used = (u64)S;
    ps2_rq_head_off = S - (long)vb;
    done = 1;
    ps2_log("pad: [test] PS2_PAD_PLAY_SYNC: the mission started at vblank %llu: the tape on from latch %ld, the head script %+ld fields",
            (unsigned long long)vb, S, ps2_rq_head_off);
}
/* the entry in force at field vb (-1 = none yet) */
static int hrs_find(const hrs_entry *e, int n, long vb) {
    int i = -1;
    if (hrs_sorted) {
        int lo = 0, hi = n;                     /* the first entry beyond vb */
        while (lo < hi) { int mid = (lo + hi) / 2; if (e[mid].f <= vb) lo = mid + 1; else hi = mid; }
        i = lo - 1;
    } else for (int k = 0; k < n; k++) if (vb >= e[k].f) i = k;
    return i;
}
/* q (x, y, z, w) -> the script's angles in degrees: the inverse of locate_views' qd = Ry(yaw) Rx(pitch) Rz(-roll) */
static void hrs_euler(const float q[4], float *yaw, float *pitch, float *roll) {
    double x = q[0], y = q[1], z = q[2], w = q[3];
    double m02 = 2.0 * (x * z + w * y), m22 = 1.0 - 2.0 * (x * x + y * y), m12 = 2.0 * (y * z - w * x);
    double m10 = 2.0 * (x * y + w * z), m11 = 1.0 - 2.0 * (x * x + z * z);
    if (m12 > 1.0) m12 = 1.0;
    if (m12 < -1.0) m12 = -1.0;
    *yaw = (float)(atan2(m02, m22) * 57.29577951308232);
    *pitch = (float)(asin(-m12) * 57.29577951308232);
    *roll = (float)(-atan2(m10, m11) * 57.29577951308232);
}
/* PS2_XR_TEST_HEAD_LEVEL: for a tracked head orientation o, ly = the level orientation at its yaw (recenter()'s formula),
   tl = ly conj(o) (turns the tracked head, and its eyes about their middle, to ly), and qd turned into ly's frame
   (ly qd conj(ly), in place): the head locate_views then draws is ly qd, relative to recenter()'s reference qd itself */
static void hrs_qmul(const float a[4], const float b[4], float r[4]) {
    float t[4];
    t[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    t[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    t[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    t[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    r[0] = t[0]; r[1] = t[1]; r[2] = t[2]; r[3] = t[3];
}
static void hrs_level(const float o[4], float ly[4], float tl[4], float qd[4]) {
    float fx = -(2 * (o[0] * o[2] + o[3] * o[1])), fz = -(1 - 2 * (o[0] * o[0] + o[1] * o[1]));
    float l = sqrtf(fx * fx + fz * fz); if (l < 1e-4f) { fx = 0; fz = -1; l = 1; }
    float yl = 0.5f * atan2f(-fx / l, -fz / l);
    ly[0] = 0.0f; ly[1] = sinf(yl); ly[2] = 0.0f; ly[3] = cosf(yl);
    float oc[4] = { -o[0], -o[1], -o[2], o[3] }, lc[4] = { -ly[0], -ly[1], -ly[2], ly[3] }, t[4];
    hrs_qmul(ly, oc, tl);
    hrs_qmul(ly, qd, t);
    hrs_qmul(t, lc, qd);
}
/* one entry as the recorder writes it (no separator) */
static int hrs_format(char *b, size_t cap, long long f, int ramp, const float a[6]) {
    return snprintf(b, cap, "%lld%c%.2f,%.2f,%.2f,%.4f,%.4f,%.4f", f, ramp ? '~' : ':', (double)a[0], (double)a[1],
                    (double)a[2], (double)a[3], (double)a[4], (double)a[5]);
}

static struct {
    int on;                     /* -1 = the switch not read yet, 0 = off, 1 = writing */
    FILE *f;
    long every;                 /* fields between entries */
    long long last;             /* the field of the last entry, -1 = none yet */
    long n;                     /* entries written */
    float py, pr;               /* the last yaw and roll written (unwrapping) */
    float fq[4], fp[3];         /* F: the reference of the last recenter() */
} hrr = { -1, NULL, 6, -1, 0, 0.0f, 0.0f, { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f } };

/* recenter(): F = the new reference (an orientation of yaw only, and a position) */
static void hrr_rebase(const float q[4], const float p[3]) {
    for (int k = 0; k < 4; k++) hrr.fq[k] = q[k];
    for (int k = 0; k < 3; k++) hrr.fp[k] = p[k];
}
/* the head relative to F: r[0..2] = yaw, pitch, roll (degrees, not unwrapped), r[3..5] = position (metres) */
static void hrr_relative(const float q[4], const float p[3], float r[6]) {
    double a[4] = { -hrr.fq[0], -hrr.fq[1], -hrr.fq[2], hrr.fq[3] };      /* conj(F) */
    double b[4] = { q[0], q[1], q[2], q[3] };
    float qr[4];                                                          /* conj(F) * q */
    qr[0] = (float)(a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1]);
    qr[1] = (float)(a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0]);
    qr[2] = (float)(a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3]);
    qr[3] = (float)(a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]);
    hrs_euler(qr, &r[0], &r[1], &r[2]);
    double d[3] = { p[0] - hrr.fp[0], p[1] - hrr.fp[1], p[2] - hrr.fp[2] };   /* conj(F) rotates d: d + 2w (u x d) + 2 u x (u x d) */
    double u[3] = { a[0], a[1], a[2] }, w = a[3];
    double c1[3] = { u[1] * d[2] - u[2] * d[1], u[2] * d[0] - u[0] * d[2], u[0] * d[1] - u[1] * d[0] };
    double c2[3] = { u[1] * c1[2] - u[2] * c1[1], u[2] * c1[0] - u[0] * c1[2], u[0] * c1[1] - u[1] * c1[0] };
    for (int k = 0; k < 3; k++) r[3 + k] = (float)(d[k] + 2.0 * (w * c1[k] + c2[k]));
}
/* locate_views: the head as drawn at field vb (q = the left eye's orientation, p = the point between the eyes, LOCAL) */
static void hrr_sample(u64 vb, const float q[4], const float p[3]) {
    if (hrr.on < 0) {
        const char *e = getenv("PS2_XR_HEAD_RECORD");
        hrr.on = 0;
        if (e && *e) {
            const char *ev = getenv("PS2_XR_HEAD_RECORD_EVERY");
            long n = ev && *ev ? strtol(ev, NULL, 10) : 6;
            if (n < 1) n = 1;
            if (n > 600) n = 600;
            hrr.every = n;
            hrr.f = fopen(e, "w");
            if (hrr.f) {
                setvbuf(hrr.f, NULL, _IOFBF, 1 << 16);
                hrr.on = 1;
                ps2_log("xr: head record into %s, an entry every %ld fields (PS2_XR_HEAD_RECORD)", e, n);
            } else ps2_log("xr: PS2_XR_HEAD_RECORD %s cannot be written", e);
        }
    }
    if (hrr.on != 1) return;
    long long f = (long long)vb;
    if (hrr.last >= 0 && f < hrr.last + hrr.every) return;
    int ramp = hrr.last >= 0;
    float r[6];
    hrr_relative(q, p, r);
    if (hrr.n) {
        while (r[0] - hrr.py > 180.0f) r[0] -= 360.0f;
        while (r[0] - hrr.py < -180.0f) r[0] += 360.0f;
        while (r[2] - hrr.pr > 180.0f) r[2] -= 360.0f;
        while (r[2] - hrr.pr < -180.0f) r[2] += 360.0f;
    }
    hrr.py = r[0]; hrr.pr = r[2];
    char b[192];
    int len = 0;
    if (hrr.n) b[len++] = ' ';
    int m = hrs_format(b + len, sizeof b - (size_t)len, f, ramp, r);
    if (m > 0 && m < (int)(sizeof b) - len) len += m;
    fwrite(b, 1, (size_t)len, hrr.f);
    hrr.last = f; hrr.n++;
    if (hrr.n % 10 == 0) fflush(hrr.f);
}
/* ps2_xr_shutdown */
static void hrr_close(void) {
    if (hrr.on != 1) return;
    fputc('\n', hrr.f);
    fclose(hrr.f);
    hrr.f = NULL; hrr.on = 0;
    ps2_log("xr: head record closed: %ld entries, fields to %lld (PS2_XR_HEAD_RECORD)", hrr.n, hrr.last);
}
