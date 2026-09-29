#pragma once

#include <kimodo/kimodo.hpp>

#include "skeleton.hpp"

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace kimodo::detail {

// What post-processing pins, frame by frame: upstream post_process_motion's
// constraint masks and the target motion extract_input_motion_from_constraints
// builds from the constraint sets.  Unconstrained frames hold an identity pose.
struct postprocess_targets {
    std::vector<float> hips;             // [T,3]
    std::vector<float> rotations_xyzw;   // [T,J,4] parent-local
    std::vector<float> full_body, left_hand, right_hand, left_foot, right_foot, root; // [T] 0/1
    std::vector<unsigned char> full_body_pose; // [T] target posed by a fullbody constraint

    postprocess_targets() = default;
    postprocess_targets(std::size_t frames, std::size_t joints);
    [[nodiscard]] std::size_t frames() const noexcept { return full_body.size(); }
    // Rows [first, first + count), frames outside the clip unconstrained.
    [[nodiscard]] postprocess_targets slice(std::ptrdiff_t first, std::size_t count, std::size_t joints) const;
};

// The targets of already validated constraints over a clip of `frames`.
postprocess_targets build_postprocess_targets(const skeleton_spec &skeleton,
                                              std::span<const motion_constraint> constraints, std::size_t frames);

// Upstream post_process_motion (MotionCorrection's CorrectMotion): foot-skate
// cleanup from the model's foot contacts [T,4] (> 0.5 is a contact), and IK
// and root correction that pin the targets' constrained frames.  Corrects
// `hips` [T,3] and `local_xyzw` [T,J,4] in place.
std::expected<void, std::string> correct_motion(const skeleton_spec &skeleton, std::vector<float> &hips,
                                                std::vector<float> &local_xyzw, std::span<const float> contacts,
                                                const postprocess_targets &targets, float root_margin);

} // namespace kimodo::detail
