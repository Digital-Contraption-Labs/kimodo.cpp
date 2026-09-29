#pragma once

#include "skeleton.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace kimodo::detail {

// Upstream smooth_root.smooth_signal for a [frames, 2] ground-plane path:
// multigrid ADMM smoothing that keeps every frame within 6 cm of the input.
std::vector<float> smooth_root_path(std::span<const float> xz, std::size_t frames);

// Upstream KimodoMotionRep.__call__ with to_normalize=False: parent-local XYZW
// rotations [frames, J, 4] and root (hips) positions [frames, 3] -> raw
// motion-representation rows [frames, motion_dim], with the smoothed root,
// velocities and foot contacts recomputed from the pose.
std::vector<float> encode_motion(const skeleton_spec &skeleton, std::span<const float> local_xyzw,
                                 std::span<const float> roots, std::size_t frames, float fps = 30.F);

} // namespace kimodo::detail
