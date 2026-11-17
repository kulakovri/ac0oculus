/* ps2_crash.c -- the crash report (ACZ_CRASH_REPORT, default 1; night_setup 2026-11-04; RELEASE_RESEARCH R4 item 4).
   ps2_main.c's exception filter calls ps2_crash_report(): crash_<stamp>.dmp (MiniDumpWriteDump) and crash_<stamp>.txt (what
   happened, the exe, the last 200 log lines) go to crash/ next to the exe, or to %TEMP%\ac5_crash when that folder cannot be
   written. new_report.txt in that folder names the newest report; the next start logs it and removes the marker. The eight
   newest reports are kept. ps2_fatal_report() is the same text report without a minidump, for fatal conditions that are not
   exceptions (ps2_fatal calls it; the stall exit and a lost GPU device are the next callers, stab_audit U5, U8). Nothing is
   written unless something fails: before a crash this is one sleeping thread and a memcpy per log line.
   The dump leaves out heaps and module paths (no guest memory, no folder names); the log lines can hold folder names. These
   files are never sent anywhere; with usage statistics on (ps2_stats.c, docs/PRIVACY.md) the next start sends the "what" line
   without addresses or paths, the exception code, the address relative to the exe and the module offsets of the stack.
   The writing happens on a thread of its own, started at start-up: a stack overflow leaves no stack to write from, and
   MiniDumpWriteDump should not run on the thread it describes. dbghelp.dll is loaded from System32 at start-up, not during
   the crash, when the loader lock or the heap may be taken.
   ACZ_CRASH_TEST=av|so|abort|fatal (test only, tools/docs/env_template.md): from field ACZ_CRASH_TEST_FIELD (default 300) a
   new thread writes to a null pointer, overflows its stack, calls abort(), or asks for a fatal report and the game goes on. */
#include "ps2_crash.h"
#include "ps2_profile.h"
#include "ps2_runtime.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <signal.h>

#define RING_LINES     300                     /* 200 before the crash + the filter's own lines (frames, call ring) */
#define RING_LINE_MAX  384
#define SHOW_LINES     200
#define KEEP_REPORTS   8
#define MARKER_NAME    L"new_report.txt"

extern u32 ps2_gs_frame_count;

static int crash_on;
static char ring[RING_LINES][RING_LINE_MAX];
static short ring_len[RING_LINES];
static volatile LONG ring_pos;                 /* lines so far */
static volatile LONG ring_mark = -1;           /* ring_pos when the filter started */

/* minidump: the signature of MiniDumpWriteDump without dbghelp.h's import library (its structures are packed to 4) */
#pragma pack(push, 4)
typedef struct {
    DWORD ThreadId;
    EXCEPTION_POINTERS *ExceptionPointers;
    BOOL ClientPointers;
} crash_mdei;
#pragma pack(pop)
typedef BOOL (WINAPI *minidump_fn)(HANDLE, DWORD, HANDLE, DWORD, crash_mdei *, void *, void *);
static minidump_fn minidump_write;
#define DUMP_FLAGS (0x00000000u /* MiniDumpNormal */ | 0x00000080u /* MiniDumpFilterModulePaths */ \
                    | 0x00000020u /* MiniDumpWithUnloadedModules */ | 0x00001000u /* MiniDumpWithThreadInfo */)

static HANDLE ev_request, ev_done;
static const char *req_why;
static EXCEPTION_POINTERS *req_ep;
static DWORD req_tid;
static LPTOP_LEVEL_EXCEPTION_FILTER installed_filter;
static const char *test_kind;
static u32 test_field;

int ps2_crash_enabled(void) { return crash_on; }

void ps2_crash_log_line(const char *line, int len) {
    LONG i;
    unsigned k;
    if (!crash_on) return;
    i = InterlockedIncrement(&ring_pos) - 1;
    k = (unsigned)i % RING_LINES;
    if (len < 0) len = 0;
    if (len > RING_LINE_MAX) len = RING_LINE_MAX;
    memcpy(ring[k], line, (size_t)len);
    ring_len[k] = (short)len;
}

void ps2_crash_mark(void) { InterlockedCompareExchange(&ring_mark, ring_pos, -1); }

/* ---- files (wide names: the folder may be anything the player has) ---- */
static int exe_folder(wchar_t *out, unsigned cap) {
    DWORD n = GetModuleFileNameW(NULL, out, cap);
    wchar_t *sl;
    if (!n || n >= cap) return 0;
    sl = wcsrchr(out, L'\\');
    if (!sl) return 0;
    sl[1] = 0;
    return 1;
}

static int folder_exe(wchar_t *out, unsigned cap) {
    if (!exe_folder(out, cap) || wcslen(out) + 8 >= cap) return 0;
    wcscat(out, L"crash");
    return 1;
}

static int folder_temp(wchar_t *out, unsigned cap) {
    DWORD n = GetTempPathW(cap, out);
    if (!n || n + 12 >= cap) return 0;
    wcscat(out, L"ac5_crash");
    return 1;
}

static int folder_ready(const wchar_t *dir) {
    if (CreateDirectoryW(dir, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) return 1;
    return 0;
}

static void put(HANDLE h, const char *s, size_t n) {
    DWORD w;
    if (n) WriteFile(h, s, (DWORD)n, &w, NULL);
}

static void putf(HANDLE h, const char *fmt, ...) {
    char b[1024];
    int n;
    va_list ap;
    va_start(ap, fmt);
    n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) put(h, b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
}

static void to_utf8(const wchar_t *w, char *out, int cap) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL);
    if (n <= 0) out[0] = 0;
}

/* The report. ep == NULL and no dump wanted: the fatal report. Uses the stack, Win32 and vsnprintf, not the heap. */
static char last_note[MAX_PATH * 3 + 300];

static void describe(EXCEPTION_POINTERS *ep, char *out, size_t cap) {
    const EXCEPTION_RECORD *r = ep ? ep->ExceptionRecord : NULL;
    const char *b = (const char *)GetModuleHandleA(NULL);
    if (!r) { snprintf(out, cap, "unknown"); return; }
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
        snprintf(out, cap, "exception %08lX at %p (image %p, rva %08llX), %s address %p", (unsigned long)r->ExceptionCode,
                 r->ExceptionAddress, (const void *)b, (unsigned long long)((const char *)r->ExceptionAddress - b),
                 r->ExceptionInformation[0] ? "writing" : "reading", (void *)r->ExceptionInformation[1]);
    else
        snprintf(out, cap, "exception %08lX at %p (image %p, rva %08llX)", (unsigned long)r->ExceptionCode,
                 r->ExceptionAddress, (const void *)b, (unsigned long long)((const char *)r->ExceptionAddress - b));
}

static void write_report(const char *why, EXCEPTION_POINTERS *ep, DWORD tid, int want_dump) {
    wchar_t dir[MAX_PATH], base[MAX_PATH], path[MAX_PATH + 16];
    char dumpnote[300], u8[MAX_PATH * 3], whatbuf[256];
    SYSTEMTIME st;
    HANDLE h = INVALID_HANDLE_VALUE;
    LONG pos, from, end, mark;
    DWORD dump_line_at;
    int attempt;

    last_note[0] = 0;
    if (!why) {
        describe(ep, whatbuf, sizeof whatbuf);
        why = whatbuf;
    }
    if (!(folder_exe(dir, MAX_PATH) && folder_ready(dir))
        && !(folder_temp(dir, MAX_PATH) && folder_ready(dir))) {
        snprintf(last_note, sizeof last_note, "crash: no folder to write the report to");
        return;
    }
    GetLocalTime(&st);
    for (attempt = 0; attempt < 100; attempt++) {           /* two reports in one second get _1, _2 ... */
        if (attempt)
            _snwprintf(base, MAX_PATH, L"%ls\\crash_%04u-%02u-%02u_%02u-%02u-%02u_%d", dir, st.wYear, st.wMonth, st.wDay,
                       st.wHour, st.wMinute, st.wSecond, attempt);
        else
            _snwprintf(base, MAX_PATH, L"%ls\\crash_%04u-%02u-%02u_%02u-%02u-%02u", dir, st.wYear, st.wMonth, st.wDay,
                       st.wHour, st.wMinute, st.wSecond);
        base[MAX_PATH - 1] = 0;
        _snwprintf(path, MAX_PATH + 16, L"%ls.txt", base);
        path[MAX_PATH + 15] = 0;
        h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) break;
    }
    if (h == INVALID_HANDLE_VALUE) {
        snprintf(last_note, sizeof last_note, "crash: cannot create the report file (%lu)", (unsigned long)GetLastError());
        return;
    }

    putf(h, "Crash report of the game (ACZ_CRASH_REPORT=1). This file is not sent anywhere. To report the problem, attach this\r\n"
            "file and the .dmp; the log lines below can hold your folder names, so read them first.\r\n\r\n");
    putf(h, "what:     %s\r\n", why ? why : "?");
    putf(h, "when:     %04u-%02u-%02u %02u:%02u:%02u (local)\r\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    putf(h, "version:  %s\r\n", ps2_version_line());
    putf(h, "field:    %u\r\n", (unsigned)ps2_gs_frame_count);
    {
        wchar_t ex[MAX_PATH];
        const char *b = (const char *)GetModuleHandleA(NULL);
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(b + ((const IMAGE_DOS_HEADER *)b)->e_lfanew);
        DWORD n = GetModuleFileNameW(NULL, ex, MAX_PATH);
        const wchar_t *nm = n ? wcsrchr(ex, L'\\') : NULL;
        to_utf8(nm ? nm + 1 : L"?", u8, sizeof u8);
        putf(h, "exe:      %s, link stamp %08lX, image at %p\r\n", u8, (unsigned long)nt->FileHeader.TimeDateStamp, (const void *)b);
    }
#if defined(__VERSION__)
    putf(h, "compiler: %s\r\n", __VERSION__);
#endif
    {
        typedef LONG (WINAPI *rtlgv_fn)(RTL_OSVERSIONINFOW *);
        rtlgv_fn gv = (rtlgv_fn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
        RTL_OSVERSIONINFOW v;
        memset(&v, 0, sizeof v);
        v.dwOSVersionInfoSize = sizeof v;
        if (gv && gv(&v) == 0)
            putf(h, "windows:  %lu.%lu build %lu\r\n", (unsigned long)v.dwMajorVersion, (unsigned long)v.dwMinorVersion,
                 (unsigned long)v.dwBuildNumber);
    }
    /* a fixed-width line, filled in when the dump is done */
    dump_line_at = SetFilePointer(h, 0, NULL, FILE_CURRENT);
    snprintf(dumpnote, sizeof dumpnote, "%s", want_dump ? "not finished" : "none (a report without a minidump)");
    putf(h, "dump:     %-120s\r\n", dumpnote);

    pos = ring_pos;
    mark = ring_mark;
    end = pos;
    from = (mark >= 0 ? mark : end) - SHOW_LINES;
    if (from < end - RING_LINES) from = end - RING_LINES;
    if (from < 0) from = 0;
    putf(h, "\r\n---- the last %ld log lines, oldest first ----\r\n", (long)(end - from));
    for (; from < end; from++) {
        unsigned k = (unsigned)from % RING_LINES;
        int n = ring_len[k];
        if (n > 0 && ring[k][n - 1] == '\n') n--;
        put(h, ring[k], (size_t)n);
        put(h, "\r\n", 2);
    }
    FlushFileBuffers(h);

    /* the marker for the next start */
    {
        wchar_t mpath[MAX_PATH + 16];
        HANDLE m;
        _snwprintf(mpath, MAX_PATH + 16, L"%ls\\%ls", dir, MARKER_NAME);
        mpath[MAX_PATH + 15] = 0;
        m = CreateFileW(mpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (m != INVALID_HANDLE_VALUE) {
            to_utf8(path, u8, sizeof u8);
            putf(m, "%s\r\n%s\r\n", u8, why ? why : "?");
            CloseHandle(m);
        }
    }
    /* the dump last: it may hang on a lock of a suspended thread, and the caller's wait then ends the process */
    if (want_dump) {
        if (!minidump_write) {
            snprintf(dumpnote, sizeof dumpnote, "none (dbghelp.dll not available)");
        } else {
            wchar_t dpath[MAX_PATH + 16];
            HANDLE d;
            _snwprintf(dpath, MAX_PATH + 16, L"%ls.dmp", base);
            dpath[MAX_PATH + 15] = 0;
            d = CreateFileW(dpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (d == INVALID_HANDLE_VALUE) {
                snprintf(dumpnote, sizeof dumpnote, "none (cannot create the file, error %lu)", (unsigned long)GetLastError());
            } else {
                crash_mdei mei = { tid, ep, FALSE };
                BOOL ok = minidump_write(GetCurrentProcess(), GetCurrentProcessId(), d, DUMP_FLAGS, ep ? &mei : NULL, NULL, NULL);
                DWORD err = GetLastError(), hi = 0, lo = GetFileSize(d, &hi);
                CloseHandle(d);
                if (ok) snprintf(dumpnote, sizeof dumpnote, "the .dmp beside this file, %lu bytes", (unsigned long)lo);
                else snprintf(dumpnote, sizeof dumpnote, "none (MiniDumpWriteDump failed, error %08lX)", (unsigned long)err);
            }
        }
        SetFilePointer(h, (LONG)dump_line_at, NULL, FILE_BEGIN);
        putf(h, "dump:     %-120s\r\n", dumpnote);
    }
    CloseHandle(h);
    to_utf8(path, u8, sizeof u8);
    snprintf(last_note, sizeof last_note, "crash: %s: report written: %s (dump: %s)", why, u8, dumpnote);
}

/* ---- the next start ---- */
static char new_report[MAX_PATH * 3 + 300];   /* the last run's marker: the report's path, a line, what happened (ps2_stats.c) */
const char *ps2_crash_new_report(void) { return new_report; }
static void scan_folder(const wchar_t *dir) {
    wchar_t mpath[MAX_PATH + 32], pat[MAX_PATH + 32];
    char text[MAX_PATH * 3 + 300];
    HANDLE m;
    WIN32_FIND_DATAW fd;
    HANDLE f;
    wchar_t (*names)[64] = NULL;
    int count = 0, cap = 256;

    _snwprintf(mpath, MAX_PATH + 32, L"%ls\\%ls", dir, MARKER_NAME);
    mpath[MAX_PATH + 31] = 0;
    m = CreateFileW(mpath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (m != INVALID_HANDLE_VALUE) {
        DWORD got = 0;
        ReadFile(m, text, sizeof text - 1, &got, NULL);
        CloseHandle(m);
        text[got] = 0;
        if (!new_report[0]) snprintf(new_report, sizeof new_report, "%s", text);
        {
            char *second = strstr(text, "\r\n");
            const char *what = "";
            if (second) {
                char *e;
                *second = 0;
                what = second + 2;
                e = strstr(second + 2, "\r\n");
                if (e) *e = 0;
            }
            ps2_log("crash: the last run ended with a crash report: %s (%s)", text, what);
        }
        DeleteFileW(mpath);
    }

    /* keep the newest reports (the stamp in the name sorts by time) */
    _snwprintf(pat, MAX_PATH + 32, L"%ls\\crash_*.txt", dir);
    pat[MAX_PATH + 31] = 0;
    f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    names = (wchar_t (*)[64])malloc((size_t)cap * sizeof *names);
    if (names) {
        do {
            if (count < cap && wcslen(fd.cFileName) < 60) wcscpy(names[count++], fd.cFileName);
        } while (FindNextFileW(f, &fd));
    }
    FindClose(f);
    if (names && count > KEEP_REPORTS) {
        int i, j;
        for (i = 1; i < count; i++) {                         /* a short list: insertion sort */
            wchar_t t[64];
            wcscpy(t, names[i]);
            for (j = i - 1; j >= 0 && wcscmp(names[j], t) > 0; j--) wcscpy(names[j + 1], names[j]);
            wcscpy(names[j + 1], t);
        }
        for (i = 0; i < count - KEEP_REPORTS; i++) {
            wchar_t p[MAX_PATH + 96];
            size_t l;
            _snwprintf(p, MAX_PATH + 96, L"%ls\\%ls", dir, names[i]);
            p[MAX_PATH + 95] = 0;
            DeleteFileW(p);
            l = wcslen(p);
            if (l > 4) { wcscpy(p + l - 4, L".dmp"); DeleteFileW(p); }
        }
    }
    free(names);
}

/* ---- the report thread ---- */
static DWORD WINAPI test_fault(void *arg);

static void re_arm_filter(void) {
    LPTOP_LEVEL_EXCEPTION_FILTER prev;
    if (!installed_filter) return;
    prev = SetUnhandledExceptionFilter(installed_filter);
    if (prev != installed_filter) {
        HMODULE m = NULL;
        char nm[MAX_PATH] = "?";
        if (prev && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCSTR)(void *)prev, &m))
            GetModuleFileNameA(m, nm, sizeof nm);
        ps2_log("crash: a library replaced the exception filter (%p, %s): put back", (void *)prev, nm);
    }
}

static DWORD WINAPI report_thread(void *arg) {
    DWORD tick = test_kind ? 50u : 30000u;
    int test_done = 0;
    (void)arg;
    for (;;) {
        DWORD w = WaitForSingleObject(ev_request, tick);
        if (w == WAIT_OBJECT_0) {
            write_report(req_why, req_ep, req_tid, 1);
            SetEvent(ev_done);                    /* before the log line: the crashed thread may hold the log's lock */
            if (last_note[0]) ps2_log("%s", last_note);
            continue;
        }
        re_arm_filter();
        if (test_kind && !test_done && ps2_gs_frame_count >= test_field) {
            HANDLE t;
            test_done = 1;
            ps2_log("crash: ACZ_CRASH_TEST=%s at field %u (test only)", test_kind, (unsigned)ps2_gs_frame_count);
            t = CreateThread(NULL, 0, test_fault, NULL, 0, NULL);
            if (t) CloseHandle(t);
            tick = 30000u;
        }
    }
    return 0;
}

void ps2_crash_report(const char *why, void *exception_pointers, unsigned long thread_id) {
    if (!crash_on || !ev_request) return;
    req_why = why;
    req_ep = (EXCEPTION_POINTERS *)exception_pointers;
    req_tid = (DWORD)thread_id;
    SetEvent(ev_request);
    if (WaitForSingleObject(ev_done, 20000) == WAIT_OBJECT_0)   /* never hang: after this the process ends anyway */
        Sleep(50);                                /* its last log line */
}

void ps2_fatal_report(const char *why) {
    if (!crash_on) return;
    write_report(why ? why : "fatal", NULL, 0, 0);
    if (last_note[0]) ps2_log("%s", last_note);
}

static void on_abort(int sig) {
    (void)sig;
    ps2_log("crash: abort() was called");
    ps2_crash_mark();
    ps2_crash_report("abort()", NULL, GetCurrentThreadId());
}

/* Test only: a function the optimiser cannot turn into a loop */
static int test_recurse(volatile char *up, int depth) {
    volatile char pad[4096];
    pad[0] = (char)depth;
    if (up) pad[1] = up[0];
    if (depth < 0) return 0;
    return test_recurse(pad, depth + 1) + pad[0];
}

static DWORD WINAPI test_fault(void *arg) {
    (void)arg;
    if (!strcmp(test_kind, "so")) {
        return (DWORD)test_recurse(NULL, 0);
    } else if (!strcmp(test_kind, "abort")) {
        abort();
    } else if (!strcmp(test_kind, "fatal")) {
        ps2_fatal_report("ACZ_CRASH_TEST=fatal (the game goes on)");
    } else {                                       /* av */
        volatile int *volatile bad = (volatile int *)(uintptr_t)0x10;
        *bad = 1;
    }
    return 0;
}

void ps2_crash_init(void *filter) {
    const char *e = getenv("ACZ_CRASH_REPORT");
    wchar_t dir[MAX_PATH];
    HMODULE dbg;
    HANDLE t;
    if (e && *e == '0') return;
    crash_on = 1;
    if (folder_exe(dir, MAX_PATH)) scan_folder(dir);
    if (folder_temp(dir, MAX_PATH)) scan_folder(dir);
    dbg = LoadLibraryExA("dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (dbg) minidump_write = (minidump_fn)(void *)GetProcAddress(dbg, "MiniDumpWriteDump");
    installed_filter = (LPTOP_LEVEL_EXCEPTION_FILTER)filter;
    e = getenv("ACZ_CRASH_TEST");
    if (e && *e) {
        const char *f = getenv("ACZ_CRASH_TEST_FIELD");
        test_kind = e;
        test_field = f && *f ? (u32)strtoul(f, NULL, 0) : 300u;
    }
    ev_request = CreateEventA(NULL, FALSE, FALSE, NULL);
    ev_done = CreateEventA(NULL, FALSE, FALSE, NULL);
    t = ev_request && ev_done ? CreateThread(NULL, 256 * 1024, report_thread, NULL, 0, NULL) : NULL;
    if (t) CloseHandle(t);
    else ev_request = NULL;
    signal(SIGABRT, on_abort);
}

#elif defined(__linux__)   /* Linux / Android (quest_port 2026-11-07): fatal signals, no minidump */
/* The Quest build has no minidump and no report thread. ps2_crash_init installs one handler for SIGSEGV, SIGBUS, SIGILL,
   SIGFPE, SIGTRAP and SIGABRT (SA_SIGINFO | SA_ONSTACK). It writes, with raw write()s only (async-signal-safe; dladdr is
   not formally, but bionic's walks the loaded-library list without allocating, as debuggerd's own handler does), the
   signal, its code, the fault address, the thread, the field, PC / LR / SP from the ucontext (aarch64) resolved with dladdr
   to library + offset, and a frame-pointer walk (x29 records, read through a pipe so a bad pointer gives EFAULT instead of
   a second fault) into the log (stderr) and into crash.txt in the folder the process started in (the app's files folder on
   the Quest), crash.txt with the last 200 log lines; then it puts the previous handler back and re-raises, so the
   system's tombstone (adb logcat -b crash) still comes (MarathonRecomp-Android's TECHNICAL.md section 2). A fault comes
   back by itself when the handler returns (the instruction runs again); SIGABRT and sent signals are raised again. A second
   thread that crashes meanwhile waits up to 5 s for the report. Stack overflows: bionic gives every thread (the main one
   too) its own 16 KB alternate signal stack at creation, which SA_ONSTACK uses; this handler needs about 1 KB of it (desktop
   glibc has no such stack: an overflow there goes straight to the kernel). The next start logs the last crash's `what`
   line and renames crash.txt to crash_prev.txt (one kept). ps2_fatal_report writes the same file without a signal.
   ACZ_CRASH_TEST=av|so|abort|fatal (test only): from field ACZ_CRASH_TEST_FIELD (default 300), as on Windows. */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#if defined(__ANDROID__)
#include <dlfcn.h>                             /* (glibc declares dladdr only with _GNU_SOURCE: Android only) */
#endif

#define RING_LINES     300
#define RING_LINE_MAX  384
#define SHOW_LINES     200

extern u32 ps2_gs_frame_count;

static int crash_on;
static char ring[RING_LINES][RING_LINE_MAX];
static short ring_len[RING_LINES];
static int ring_pos;                           /* lines so far (atomic) */
static int ring_mark = -1;                     /* ring_pos when a report started */
static char crash_path[1100], prev_path[1100];
static struct sigaction crash_prev[NSIG];
static int crash_owner, crash_done;            /* the reporting thread's id; 1 when its report is written */
static int probe_fd[2] = { -1, -1 };           /* non-blocking pipe: copying a pointer's memory into it fails with EFAULT */
static const char *test_kind;
static u32 test_field;
static char new_report[1300];                  /* the last run's report: "crash_prev.txt\r\n<what>", "" none */
static const char *version_text = "?";

const char *ps2_crash_new_report(void) { return new_report; }
int ps2_crash_enabled(void) { return crash_on; }

void ps2_crash_log_line(const char *line, int len) {
    if (!crash_on) return;
    int i = __atomic_fetch_add(&ring_pos, 1, __ATOMIC_RELAXED);
    unsigned k = (unsigned)i % RING_LINES;
    if (len < 0) len = 0;
    if (len > RING_LINE_MAX) len = RING_LINE_MAX;
    memcpy(ring[k], line, (size_t)len);
    ring_len[k] = (short)len;
}

void ps2_crash_mark(void) {
    int none = -1;
    __atomic_compare_exchange_n(&ring_mark, &none, __atomic_load_n(&ring_pos, __ATOMIC_RELAXED), 0, __ATOMIC_ACQ_REL,
                                __ATOMIC_RELAXED);
}

/* ---- raw output to up to two descriptors (the log, crash.txt) ---- */
static int out_fd[2] = { -1, -1 };
static void out_raw(int which, const char *s, size_t n) {   /* which: 1 the log, 2 the file, 3 both */
    for (int i = 0; i < 2; i++) {
        const char *p = s;
        size_t left = n;
        if (!(which & (1 << i)) || out_fd[i] < 0) continue;
        while (left) {
            ssize_t w = write(out_fd[i], p, left);
            if (w <= 0) break;
            p += w; left -= (size_t)w;
        }
    }
}
static void out_s(int which, const char *s) { out_raw(which, s, strlen(s)); }
static void out_hex(int which, u64 v) {
    char b[20], *p = b + sizeof b;
    do { *--p = "0123456789abcdef"[v & 15u]; v >>= 4; } while (v);
    *--p = 'x'; *--p = '0';
    out_raw(which, p, (size_t)(b + sizeof b - p));
}
static void out_dec(int which, u64 v) {
    char b[24], *p = b + sizeof b;
    do { *--p = (char)('0' + v % 10u); v /= 10u; } while (v);
    out_raw(which, p, (size_t)(b + sizeof b - p));
}
static void out_addr(int which, u64 a) {        /* 0x... (libmain.so+0x...) */
    out_hex(which, a);
#if defined(__ANDROID__)
    Dl_info di;
    memset(&di, 0, sizeof di);
    if (a && dladdr((const void *)(uintptr_t)a, &di) && di.dli_fname) {
        const char *sl = strrchr(di.dli_fname, '/');
        out_s(which, " (");
        out_s(which, sl ? sl + 1 : di.dli_fname);
        out_s(which, "+");
        out_hex(which, a - (u64)(uintptr_t)di.dli_fbase);
        out_s(which, ")");
    }
#endif
}
static int probe16(u64 a, u64 out[2]) {
    if (probe_fd[1] < 0 || !a || (a & 7u)) return 0;
    if (write(probe_fd[1], (const void *)(uintptr_t)a, 16) != 16) return 0;
    return read(probe_fd[0], out, 16) == 16;
}

static const char *sig_name(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV"; case SIGBUS: return "SIGBUS"; case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE"; case SIGTRAP: return "SIGTRAP"; case SIGABRT: return "SIGABRT";
    default: return "signal";
    }
}

/* The body of crash.txt and the log lines: what (a signal with its context, or a fatal condition's text), then the file's
   last log lines. Raw writes only, so the signal handler and ps2_fatal_report share it. */
static void report_body(int sig, const siginfo_t *si, const void *ucv, const char *why) {
    const int tid = (int)syscall(SYS_gettid);
    char tname[20];
    memset(tname, 0, sizeof tname);
    prctl(PR_GET_NAME, tname, 0, 0, 0);
    struct timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);
    out_s(2, "Crash report of the game (ACZ_CRASH_REPORT=1). This file is not sent anywhere; the system's own report is\n"
             "`adb logcat -b crash` (the tombstone). The log lines below can hold folder names.\n\n");
    out_s(3, sig ? "crash: fatal signal " : "crash: fatal: ");
    if (sig) {
        out_dec(3, (u64)sig); out_s(3, " ("); out_s(3, sig_name(sig)); out_s(3, "), code ");
        if (si && si->si_code < 0) { out_s(3, "-"); out_dec(3, (u64)(-(long long)si->si_code)); }
        else out_dec(3, si ? (u64)si->si_code : 0u);
        if (si && (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE || sig == SIGTRAP)) {
            out_s(3, ", fault address "); out_hex(3, (u64)(uintptr_t)si->si_addr);
        }
    } else out_s(3, why ? why : "?");
    out_s(3, ", thread "); out_dec(3, (u64)(unsigned)tid); out_s(3, " \""); out_s(3, tname); out_s(3, "\", field ");
    out_dec(3, (u64)ps2_gs_frame_count); out_s(3, ", unix time "); out_dec(3, (u64)rt.tv_sec); out_s(3, "\n");
    out_s(2, "version:  "); out_s(2, version_text); out_s(2, "\n");
#if defined(__aarch64__)
    if (ucv) {
        const ucontext_t *uc = (const ucontext_t *)ucv;
        const u64 va = 0x0000FFFFFFFFFFFFull;   /* (a return address may carry a PAC signature or a tag above bit 47) */
        out_s(3, "crash: pc "); out_addr(3, (u64)uc->uc_mcontext.pc);
        out_s(3, " lr "); out_addr(3, (u64)uc->uc_mcontext.regs[30] & va);
        out_s(3, " sp "); out_hex(3, (u64)uc->uc_mcontext.sp); out_s(3, "\n");
        u64 fp = (u64)uc->uc_mcontext.regs[29];
        for (int i = 2; i < 34; i++) {                       /* frame records: x29 -> { previous x29, return address } */
            u64 rec[2];
            if (!probe16(fp, rec)) break;
            u64 ra = rec[1] & va;
            if (!ra) break;
            out_s(3, "crash:   frame "); if (i < 10) out_s(3, "0"); out_dec(3, (u64)i); out_s(3, " "); out_addr(3, ra);
            out_s(3, "\n");
            if (rec[0] <= fp) break;
            fp = rec[0];
        }
    }
#else
    (void)ucv;
#endif
    {   /* the last log lines (oldest first), to the file only: the log has them already */
        int end = __atomic_load_n(&ring_pos, __ATOMIC_ACQUIRE), mark = __atomic_load_n(&ring_mark, __ATOMIC_RELAXED);
        int from = (mark >= 0 ? mark : end) - SHOW_LINES;
        if (from < end - RING_LINES) from = end - RING_LINES;
        if (from < 0) from = 0;
        out_s(2, "\n---- the last "); out_dec(2, (u64)(end - from)); out_s(2, " log lines, oldest first ----\n");
        for (; from < end; from++) {
            unsigned k = (unsigned)from % RING_LINES;
            int n = ring_len[k];
            if (n > 0 && ring[k][n - 1] == '\n') n--;
            if (n > 0) out_raw(2, ring[k], (size_t)n);
            out_s(2, "\n");
        }
    }
}

static void report_open(void) {
    out_fd[0] = 2;
    out_fd[1] = crash_path[0] ? open(crash_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644) : -1;
}
static void report_close(const char *note) {
    if (out_fd[1] >= 0) { fsync(out_fd[1]); close(out_fd[1]); }
    out_s(1, out_fd[1] >= 0 ? "crash: report written: " : "crash: could not write ");
    out_s(1, crash_path[0] ? crash_path : "crash.txt");
    out_s(1, note);
    out_s(1, "\n");
    out_fd[1] = -1;
}

static void crash_signal(int sig, siginfo_t *si, void *ucv) {
    int me = (int)syscall(SYS_gettid), owner = 0;
    if (__atomic_compare_exchange_n(&crash_owner, &owner, me, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        int none = -1;
        __atomic_compare_exchange_n(&ring_mark, &none, __atomic_load_n(&ring_pos, __ATOMIC_RELAXED), 0, __ATOMIC_ACQ_REL,
                                    __ATOMIC_RELAXED);
        report_open();
        report_body(sig, si, ucv, NULL);
        report_close("; the system's tombstone follows (adb logcat -b crash)");
        __atomic_store_n(&crash_done, 1, __ATOMIC_RELEASE);
    } else if (owner != me) {                       /* another thread reports: let it finish (5 s at most) */
        for (int i = 0; i < 500 && !__atomic_load_n(&crash_done, __ATOMIC_ACQUIRE); i++) {
            struct timespec ts = { 0, 10000000L };
            nanosleep(&ts, NULL);
        }
    }                                               /* (owner == me: a fault inside the report -- straight on) */
    sigaction(sig, &crash_prev[sig], NULL);
    if (si && si->si_code > 0 && sig != SIGABRT) return;   /* a fault: it comes again, to the previous handler */
    raise(sig);                                     /* (blocked until this returns, then the previous handler) */
}

void ps2_crash_report(const char *why, void *exception_pointers, unsigned long thread_id) {
    (void)exception_pointers; (void)thread_id;
    ps2_fatal_report(why ? why : "crash report");
}

void ps2_fatal_report(const char *why) {
    static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
    if (!crash_on) return;
    pthread_mutex_lock(&mx);
    report_open();
    report_body(0, NULL, NULL, why ? why : "fatal");
    report_close("");
    pthread_mutex_unlock(&mx);
}

/* Test only: a function the optimiser cannot turn into a loop */
static int test_recurse(volatile char *up, int depth) {
    volatile char pad[4096];
    pad[0] = (char)depth;
    if (up) pad[1] = up[0];
    if (depth < 0) return 0;
    return test_recurse(pad, depth + 1) + pad[0];
}
static void *test_fault(void *arg) {
    (void)arg;
    if (!strcmp(test_kind, "so")) test_recurse(NULL, 0);
    else if (!strcmp(test_kind, "abort")) abort();
    else if (!strcmp(test_kind, "fatal")) ps2_fatal_report("ACZ_CRASH_TEST=fatal (the game goes on)");
    else { volatile int *volatile bad = (volatile int *)(uintptr_t)0x10; *bad = 1; }   /* av */
    return NULL;
}
static void *test_watch(void *arg) {
    (void)arg;
    while (ps2_gs_frame_count < test_field) { struct timespec ts = { 0, 50000000L }; nanosleep(&ts, NULL); }
    pthread_t t;
    ps2_log("crash: ACZ_CRASH_TEST=%s at field %u (test only)", test_kind, (unsigned)ps2_gs_frame_count);
    if (pthread_create(&t, NULL, test_fault, NULL) == 0) pthread_detach(t);
    return NULL;
}

void ps2_crash_init(void *filter) {
    static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT };
    const char *e = getenv("ACZ_CRASH_REPORT");
    (void)filter;
    if (e && *e == '0') return;
    {   char cwd[1000];
        if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, ".");
        snprintf(crash_path, sizeof crash_path, "%s/crash.txt", cwd);
        snprintf(prev_path, sizeof prev_path, "%s/crash_prev.txt", cwd);
    }
    version_text = ps2_version_line();             /* (formatted now: the handler only reads it) */
    {   /* the last run's report */
        FILE *f = fopen(crash_path, "r");
        if (f) {
            char line[1100], what[1000] = "";
            while (fgets(line, sizeof line, f))
                if (!strncmp(line, "crash: fatal", 12)) {
                    line[strcspn(line, "\r\n")] = 0;
                    snprintf(what, sizeof what, "%s", line + 7);
                    break;
                }
            fclose(f);
            rename(crash_path, prev_path);
            snprintf(new_report, sizeof new_report, "%s\r\n%s", prev_path, what);
            ps2_log("crash: the last run ended with a crash report: %s (%s)", prev_path, what[0] ? what : "?");
        }
    }
    if (pipe(probe_fd) == 0) {
        for (int i = 0; i < 2; i++) {
            fcntl(probe_fd[i], F_SETFL, fcntl(probe_fd[i], F_GETFL) | O_NONBLOCK);
            fcntl(probe_fd[i], F_SETFD, FD_CLOEXEC);
        }
    } else probe_fd[0] = probe_fd[1] = -1;
    crash_on = 1;
    {   struct sigaction sa;
        int got = 0;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = crash_signal;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++) got += sigaction(sigs[i], &sa, &crash_prev[sigs[i]]) == 0;
        ps2_log("crash: %d fatal signals (SEGV BUS ILL FPE TRAP ABRT) write %s, then the system's handler", got, crash_path);
    }
    e = getenv("ACZ_CRASH_TEST");
    if (e && *e) {
        const char *f = getenv("ACZ_CRASH_TEST_FIELD");
        pthread_t t;
        test_kind = e;
        test_field = f && *f ? (u32)strtoul(f, NULL, 0) : 300u;
        if (pthread_create(&t, NULL, test_watch, NULL) == 0) pthread_detach(t);
    }
}

#else   /* neither Windows nor Linux: no handler */
const char *ps2_crash_new_report(void) { return ""; }
int ps2_crash_enabled(void) { return 0; }
void ps2_crash_log_line(const char *line, int len) { (void)line; (void)len; }
void ps2_crash_mark(void) {}
void ps2_crash_report(const char *why, void *exception_pointers, unsigned long thread_id) { (void)why; (void)exception_pointers; (void)thread_id; }
void ps2_fatal_report(const char *why) { (void)why; }
void ps2_crash_init(void *filter) { (void)filter; }
#endif
