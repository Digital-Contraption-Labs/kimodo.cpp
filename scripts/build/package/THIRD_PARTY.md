# Third-party code in the Kimodo library

The Kimodo library (`kimodo.dll`, `libkimodo.so`, `libkimodo.dylib`) is
kimodo.cpp, under Apache-2.0 (`LICENSE`, `NOTICE`), with the following
compiled into it.  Their licence texts are in `licenses/`.

| Component | What it does here | Licence | Text |
|---|---|---|---|
| [ggml](https://github.com/ggml-org/ggml), version and commit in `VERSION.json` | tensor library: CPU and GPU (Vulkan) execution | MIT | `licenses/ggml-LICENSE` |
| Vulkan headers, from the Vulkan SDK | compile-time headers for ggml's Vulkan backend | Apache-2.0 or MIT, used under MIT | `licenses/Vulkan-Headers-LICENSE` |
| [nlohmann/json](https://github.com/nlohmann/json) 3.11.3 | reads NVIDIA's constraints JSON | MIT | `licenses/nlohmann-json-LICENSE.MIT` |
| NVIDIA Kimodo MotionCorrection, commit `58e781898b3d` | post-processing: foot-skate cleanup and IK onto constraints | Apache-2.0 | `licenses/MotionCorrection-LICENSE` |
| [Eigen](https://eigen.tuxfamily.org) 3.4.0, built with `EIGEN_MPL2_ONLY` | linear solvers behind MotionCorrection | MPL-2.0 | `licenses/Eigen-COPYING.MPL2` |

MotionCorrection and Eigen are compiled in only when `VERSION.json` says
`"post_processing": true`.

The model weights are not code and keep their own terms; see `WEIGHTS.md`.
