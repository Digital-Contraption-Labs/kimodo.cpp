# sse2neon (vendored)

SSE intrinsics implemented with ARM NEON, so MotionCorrection's SSE maths
(`third_party/motion_correction`) builds for ARM64: Android, Apple Silicon
and iOS.  That gives post-processing -- foot-skate cleanup and the IK that
pins constraints -- on those targets.

- Source: <https://github.com/DLTcollab/sse2neon>, release v1.9.1 (commit
  `92f6de174717aef09033ad21568d5bb9e5470404`), `sse2neon.h`.
  SHA-256 `78632498a57bf7e080e84cb8d74c47ce93b204cece77eab79de2908eb8bc92ed`.
- License: MIT, see `LICENSE`.
- Unmodified.  `include/immintrin.h` is this repository's: it is the header
  MotionCorrection includes on ARM, setting sse2neon's precise min/max and
  square root and adding the one AVX intrinsic MotionCorrection uses,
  `_mm_permutevar_ps`.
