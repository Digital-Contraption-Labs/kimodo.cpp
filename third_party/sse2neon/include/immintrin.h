/*
 * <immintrin.h> for ARM builds of MotionCorrection (third_party/
 * motion_correction), which is SSE code: its SIMD layer includes
 * <immintrin.h>, and CMake puts this folder first on the include path of
 * the kimodo-postprocess target on ARM64 only.  The SSE intrinsics come from
 * sse2neon; the one AVX intrinsic MotionCorrection uses is added below.
 */
#pragma once

/* sse2neon's "balanced" settings: x86's NaN handling in min/max, and square
 * roots refined past NEON's estimates.  Division is exact on ARMv8. */
#define SSE2NEON_PRECISE_MINMAX 1
#define SSE2NEON_PRECISE_SQRT 1
/* sse2neon defines its own FORCE_INLINE for its functions and restores the
 * previous one after them; MotionCorrection has one already, so it is set
 * aside here to spare the redefinition warning. */
#pragma push_macro("FORCE_INLINE")
#undef FORCE_INLINE
#include "../sse2neon.h"
#pragma pop_macro("FORCE_INLINE")

#ifndef KIMODO_SSE2NEON_PERMUTEVAR
#define KIMODO_SSE2NEON_PERMUTEVAR
/* AVX's vpermilps on 128 bits: lane i of the result is a's lane
 * (control[i] & 3).  A byte table lookup, each lane's four bytes at
 * 4 * index + {0, 1, 2, 3}. */
static inline __m128 _mm_permutevar_ps(__m128 a, __m128i control) {
    const uint32x4_t index = vandq_u32(vreinterpretq_u32_s64(control), vdupq_n_u32(3));
    const uint32x4_t bytes = vaddq_u32(vmulq_n_u32(index, 0x04040404u), vdupq_n_u32(0x03020100u));
    return vreinterpretq_f32_u8(vqtbl1q_u8(vreinterpretq_u8_f32(a), vreinterpretq_u8_u32(bytes)));
}
#endif
