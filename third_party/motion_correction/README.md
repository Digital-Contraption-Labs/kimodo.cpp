# motion_correction (vendored)

The C++ core of NVIDIA Kimodo's `MotionCorrection` package: foot-skate
cleanup, root-trajectory correction and IK that pins constrained keyframes and
end effectors. It is what upstream's `post_processing=True` runs.

- Source: <https://github.com/nv-tlabs/kimodo>, `MotionCorrection/src/cpp`
  (`Math/`, `AnimProcessing/`, `Compiler.h`, `Debug.h`, `Platform.h`) at commit
  `58e781898b3d7e328a676a75d3e338c45dce3ad9`.
- License: Apache-2.0, see `LICENSE`, the same as the rest of this port.
- Unmodified. The Python bindings (`BindingsPython.cpp`) are not vendored;
  `src/postprocess.cpp` is their C++ equivalent.
- Needs Eigen 3.4 (the `eigen` submodule), used under `EIGEN_MPL2_ONLY` as
  upstream does, and x86 SSE intrinsics (`Math/SIMD.h`).
