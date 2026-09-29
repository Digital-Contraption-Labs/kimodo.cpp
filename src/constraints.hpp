#pragma once

#include <kimodo/kimodo.hpp>

#include "skeleton.hpp"

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace kimodo::detail {

// The raw (unnormalized) condition the concat-mask denoiser consumes, rows
// [frames, motion_dim]: observed feature values and their 0/1 mask, built
// the way upstream's KimodoMotionRep.create_conditions builds them.
struct constraint_condition {
    std::vector<float> observed;
    std::vector<float> mask;

    // Whether any feature of rows [first, first + rows) is constrained.
    [[nodiscard]] bool any(std::size_t first, std::size_t rows, std::size_t motion_dim) const noexcept;
};

// Validates `constraints` against the skeleton and a clip of `frames` frames
// and builds their condition.  When constraints overlap on a feature, the
// later one wins; joint positions are taken relative to the final smoothed
// root of their frame, as upstream does.
std::expected<constraint_condition, std::string> build_constraint_condition(
    const skeleton_spec &skeleton, std::span<const motion_constraint> constraints, std::size_t frames);

// Upstream normalization of raw motion-representation rows, in place.
void normalize_motion_rows(std::span<float> rows, std::size_t motion_dim,
                           std::span<const float> global_mean, std::span<const float> global_std,
                           std::span<const float> body_mean, std::span<const float> body_std);

} // namespace kimodo::detail
