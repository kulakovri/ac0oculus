#ifndef PS2_SIMD_H
#define PS2_SIMD_H
/* The runtime's SIMD islands on both hosts (Quest port, 2026-11-07): the VU's vector helpers and VIF unpack (ps2_vu.c),
   the GS's swizzle bands, palette decodes and streaming stores (ps2_gs.c), the renderer's region copy (ps2_video.c).

   x86 (the PC build, the reference): the compiler's own SSE2 header, and the SSSE3 one for _mm_shuffle_epi8, which runs
   only inside PS2_TARGET_SSSE3 functions behind a CPUID check (ps2_gs.c, gs_has_ssse3): the build has no -mssse3, as
   before. Every macro below expands to the very intrinsic the code called before, so the x86 objects are unchanged.

   arm64 (the Quest build): sse2neon (third_party/sse2neon, v1.9.1, tag commit 92f6de1, MIT) maps the same intrinsics to
   NEON, so both builds run the same vector algorithm. Its precision switches are all on: none of them touches an
   intrinsic the runtime uses today (min/max, rcp/rsqrt/sqrt/div and dp are not used), they only make a future use pick
   the x86-like variant. What the runtime uses was checked against the header for bit-exact results (NOTES_QUEST):
   integer, logic, shuffle, compare and load/store intrinsics are exact by construction; _mm_add/sub/mul_ps are the IEEE
   operations (FADD/FSUB/FMUL, round to nearest like the MXCSR default) and differ from SSE only in which NaN comes out
   (the VU clamps every NaN to +FMAX, so that never shows) and in denormal handling (see ps2_fp_ftz_enter);
   _mm_cvttps_epi32 has sse2neon's x86 fix-up (NaN and out of range give 0x80000000 as on x86); _mm_movemask_ps reads
   the sign bits; _mm_shuffle_epi8 is TBL with the index ANDed with 0x8F (bit 7 = zero, the low 4 bits pick, as PSHUFB).

   ps2_stream_store_si128: x86 MOVNTDQ; arm64 a plain 16-byte store (Cortex-A cores switch to write streaming on their
   own for long sequential runs; sse2neon's STNP would cost a lane move for each vector).
   ps2_sfence: x86 SFENCE (the streamed stores are weakly ordered there, unlike normal ones); arm64 a release fence
   (DMB ISH): every store is weakly ordered on arm64, and the fence gives the call sites the x86 contract -- all the
   stores before it are visible before any store after it (the flag or counter that publishes them) -- whatever kind of
   store or lock publishes them afterwards. It runs once per band / region copy, not per pixel.
   ps2_tsc and ps2_cpu_relax are in ps2_runtime.h (not SIMD; ps2_kernel.c and ps2_gfxq.c need them too). */

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#  define PS2_SIMD_X86 1
#  include <emmintrin.h>
#  include <tmmintrin.h>
#  define PS2_TARGET_SSSE3 __attribute__((target("ssse3")))
#  define ps2_stream_store_si128(p, v) _mm_stream_si128((p), (v))
#  define ps2_sfence() _mm_sfence()
#elif defined(__aarch64__)
#  define PS2_SIMD_NEON 1
#  ifndef SSE2NEON_PRECISE_MINMAX
#    define SSE2NEON_PRECISE_MINMAX 1
#  endif
#  ifndef SSE2NEON_PRECISE_DIV
#    define SSE2NEON_PRECISE_DIV 1
#  endif
#  ifndef SSE2NEON_PRECISE_SQRT
#    define SSE2NEON_PRECISE_SQRT 1
#  endif
#  ifndef SSE2NEON_PRECISE_DP
#    define SSE2NEON_PRECISE_DP 1
#  endif
#  include "sse2neon.h"
#  define PS2_TARGET_SSSE3                   /* (NEON has TBL on every arm64 core) */
static inline __attribute__((always_inline)) void ps2_stream_store_si128(__m128i *p, __m128i v) {
    vst1q_s32((int32_t *)(void *)p, vreinterpretq_s32_m128i(v));
}
static inline __attribute__((always_inline)) void ps2_sfence(void) { __atomic_thread_fence(__ATOMIC_RELEASE); }

/* The VU's flush-to-zero window (ps2_vu.c vu_run; x86: MXCSR.FTZ, bit 15, set around the run and restored after it).
   arm64: FPCR.FZ (bit 24) in the same window. The difference: x86 FTZ only flushes denormal results (DAZ, bit 6, is
   not set on the PC), FPCR.FZ flushes denormal results and denormal inputs of every FP instruction. The VU helpers
   flush the inputs of their add / sub / mul themselves (vu_ftz4) and the MADD/MSUB accumulator is always an earlier FZ
   result, so the vector operations see the same operands; what differs is only a denormal VF lane that reaches a
   scalar FP instruction unflushed inside the window (CLIP's compares, the EFU's `a == 0` tests and sums), where x86
   computes with the denormal and arm64 with a signed zero. Also, x86 decides "tiny" after rounding, arm64 before: a
   result within 2^-25 relative below FLT_MIN is FLT_MIN on the PC and 0 here. NOTES_QUEST has the full list.
   The write is skipped when FZ is already set (FPCR writes are slow on some cores); the memory clobber keeps the
   window's loads and stores (every VU operand is in memory) on their side of the switch. */
typedef uint64_t ps2_fpenv;
static inline __attribute__((always_inline)) ps2_fpenv ps2_fp_ftz_enter(void) {
    uint64_t c;
    __asm__ volatile("mrs %0, fpcr" : "=r"(c) : : "memory");
    if (!(c & (1ull << 24))) __asm__ volatile("msr fpcr, %0" : : "r"(c | (1ull << 24)) : "memory");
    return c;
}
static inline __attribute__((always_inline)) void ps2_fp_ftz_leave(ps2_fpenv c) {
    if (!(c & (1ull << 24))) __asm__ volatile("msr fpcr, %0" : : "r"(c) : "memory");
}
#endif

#endif
