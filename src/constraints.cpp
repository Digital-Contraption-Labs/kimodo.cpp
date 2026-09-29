// Kinematic constraints -> the concat-mask denoiser's observed features and
// mask.  A port of upstream kimodo/constraints.py (Root2D, FullBody and
// EndEffector constraint sets) and KimodoMotionRep.create_conditions.
//
// Feature row layout (raw, before normalization), J joints:
//   [0,3)            smoothed root x, y (hips height), z
//   [3,5)            global root heading (cos, sin)
//   [5,5+3J)         joint positions relative to the smoothed root's XZ
//   [5+3J,5+9J)      global joint rotations, 6D (first two matrix columns)
//   [5+9J,9+12J)     velocities and foot contacts, never constrained
#include "constraints.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace kimodo::detail {
namespace {
using mat3 = std::array<double, 9>; // row-major
using vec3 = std::array<double, 3>;

mat3 mul(const mat3 &a, const mat3 &b) {
    mat3 r{};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) r[i * 3 + j] += a[i * 3 + k] * b[k * 3 + j];
    return r;
}

vec3 rotate(const mat3 &m, const std::array<float, 3> &v) {
    vec3 r{};
    for (int i = 0; i < 3; ++i) r[i] = m[i * 3] * v[0] + m[i * 3 + 1] * v[1] + m[i * 3 + 2] * v[2];
    return r;
}

// XYZW, normalized here so a client's float drift is harmless.
std::expected<mat3, std::string> quaternion_matrix(const float *q) {
    const double n = std::sqrt(double(q[0]) * q[0] + double(q[1]) * q[1] + double(q[2]) * q[2] + double(q[3]) * q[3]);
    if (!std::isfinite(n) || n < 1.e-6) return std::unexpected("constraint rotations must be non-zero finite quaternions");
    const double x = q[0] / n, y = q[1] / n, z = q[2] / n, w = q[3] / n;
    return mat3{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
                2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)};
}

struct posed_frame {
    std::vector<mat3> global;
    std::vector<vec3> positions;
};

// Upstream SkeletonBase.fk: the root joint sits at the root position and each
// child at its parent plus the parent's global rotation of its rest offset.
std::expected<posed_frame, std::string> forward_kinematics(const skeleton_spec &s, const float *root, const float *quaternions) {
    const size_t J = s.joints();
    posed_frame pose{std::vector<mat3>(J), std::vector<vec3>(J)};
    for (size_t j = 0; j < J; ++j) {
        auto local = quaternion_matrix(quaternions + j * 4);
        if (!local) return std::unexpected(local.error());
        const int parent = s.parents[j];
        if (parent < 0) {
            pose.global[j] = *local;
            pose.positions[j] = {root[0], root[1], root[2]};
            continue;
        }
        const auto p = static_cast<size_t>(parent);
        pose.global[j] = mul(pose.global[p], *local);
        const vec3 offset = rotate(pose.global[p], s.offsets[j]);
        for (int k = 0; k < 3; ++k) pose.positions[j][k] = pose.positions[p][k] + offset[k];
    }
    return pose;
}

bool finite(std::span<const float> values) {
    return std::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v); });
}

struct builder {
    const skeleton_spec &s;
    size_t T, J, D, rotation_begin;
    constraint_condition out;
    // Joint positions are placed last, relative to the final smoothed root.
    std::vector<float> positions;
    std::vector<unsigned char> position_set;

    builder(const skeleton_spec &skeleton, size_t frames)
        : s(skeleton), T(frames), J(skeleton.joints()), D(skeleton.motion_dim()), rotation_begin(5 + 3 * skeleton.joints()),
          out{std::vector<float>(frames * skeleton.motion_dim()), std::vector<float>(frames * skeleton.motion_dim())},
          positions(frames * skeleton.joints() * 3), position_set(frames * skeleton.joints()) {}

    void set(size_t t, size_t feature, double value) {
        out.observed[t * D + feature] = static_cast<float>(value);
        out.mask[t * D + feature] = 1.F;
    }
    void set_position(size_t t, size_t joint, const vec3 &p) {
        for (int k = 0; k < 3; ++k) positions[(t * J + joint) * 3 + static_cast<size_t>(k)] = static_cast<float>(p[k]);
        position_set[t * J + joint] = 1;
    }
    void set_rotation(size_t t, size_t joint, const mat3 &m) {
        // matrix_to_cont6d: the first two columns, column-major.
        for (size_t d = 0; d < 6; ++d) set(t, rotation_begin + joint * 6 + d, m[(d % 3) * 3 + d / 3]);
    }

    std::expected<void, std::string> add(const motion_constraint &c) {
        const size_t F = c.frames.size();
        if (!F) return std::unexpected("each constraint needs at least one frame");
        for (unsigned t : c.frames)
            if (t >= T) return std::unexpected("constraint frame " + std::to_string(t) + " is outside the " + std::to_string(T) + "-frame clip");
        const bool has_smooth = !c.smooth_root_2d.empty();
        if (has_smooth && c.smooth_root_2d.size() != F * 2) return std::unexpected("smooth_root_2d must hold two values per constrained frame");
        if (!finite(c.smooth_root_2d) || !finite(c.root_heading) || !finite(c.root_positions))
            return std::unexpected("constraint values must be finite");
        if (c.type == constraint_type::root2d) return add_root2d(c, F, has_smooth);
        if (c.type != constraint_type::fullbody && c.type != constraint_type::end_effector)
            return std::unexpected("unknown constraint type");
        return add_pose(c, F, has_smooth);
    }

    std::expected<void, std::string> add_root2d(const motion_constraint &c, size_t F, bool has_smooth) {
        if (!has_smooth) return std::unexpected("a root2d constraint needs smooth_root_2d");
        if (!c.root_positions.empty() || !c.local_rotations_xyzw.empty() || c.end_effectors)
            return std::unexpected("a root2d constraint takes only smooth_root_2d and root_heading");
        if (!c.root_heading.empty() && c.root_heading.size() != F * 2)
            return std::unexpected("root_heading must hold (cos, sin) per constrained frame");
        for (size_t i = 0; i < F; ++i) {
            const size_t t = c.frames[i];
            set(t, 0, c.smooth_root_2d[i * 2]);
            set(t, 2, c.smooth_root_2d[i * 2 + 1]);
            if (c.root_heading.empty()) continue;
            const double x = c.root_heading[i * 2], y = c.root_heading[i * 2 + 1], n = std::hypot(x, y);
            if (n < 1.e-6) return std::unexpected("root_heading must be a non-zero (cos, sin) direction");
            set(t, 3, x / n);
            set(t, 4, y / n);
        }
        return {};
    }

    std::expected<void, std::string> add_pose(const motion_constraint &c, size_t F, bool has_smooth) {
        if (c.root_positions.size() != F * 3) return std::unexpected("root_positions must hold three values per constrained frame");
        if (c.local_rotations_xyzw.size() != F * J * 4)
            return std::unexpected("local_rotations_xyzw must hold " + std::to_string(J) + " XYZW quaternions per constrained frame");
        if (!c.root_heading.empty()) return std::unexpected("root_heading applies to root2d constraints; a pose's heading comes from its hips");
        const bool full = c.type == constraint_type::fullbody;
        constexpr std::uint32_t known = KIMODO_END_EFFECTOR_LEFT_FOOT | KIMODO_END_EFFECTOR_RIGHT_FOOT |
                                        KIMODO_END_EFFECTOR_LEFT_HAND | KIMODO_END_EFFECTOR_RIGHT_HAND | KIMODO_END_EFFECTOR_HIPS;
        if (full && c.end_effectors) return std::unexpected("end_effectors applies to end-effector constraints");
        if (!full && (!c.end_effectors || (c.end_effectors & ~known)))
            return std::unexpected("an end-effector constraint needs one or more known end effectors");
        for (size_t i = 0; i < F; ++i) {
            const size_t t = c.frames[i];
            auto pose = forward_kinematics(s, c.root_positions.data() + i * 3, c.local_rotations_xyzw.data() + i * J * 4);
            if (!pose) return std::unexpected(pose.error());
            const vec3 &hips = pose->positions[0];
            set(t, 0, has_smooth ? c.smooth_root_2d[i * 2] : hips[0]);
            set(t, 1, hips[1]);
            set(t, 2, has_smooth ? c.smooth_root_2d[i * 2 + 1] : hips[2]);
            // compute_heading_angle: the right-to-left hip vector's facing.
            const vec3 &right = pose->positions[s.hips[0]], &left = pose->positions[s.hips[1]];
            const double heading = std::atan2(right[2] - left[2], -(right[0] - left[0]));
            set(t, 3, std::cos(heading));
            set(t, 4, std::sin(heading));
            if (full) {
                for (size_t j = 0; j < J; ++j) set_position(t, j, pose->positions[j]);
                continue;
            }
            // expand_joint_names: a chain's positions, its first joint's rotation.
            for (size_t e = 0; e < 4; ++e) {
                if (!(c.end_effectors & (1u << e))) continue;
                const unsigned base = s.end_effectors[e];
                set_position(t, base, pose->positions[base]);
                set_rotation(t, base, pose->global[base]);
                if (const int tip = s.end_effector_tips[e]; tip >= 0) set_position(t, static_cast<size_t>(tip), pose->positions[static_cast<size_t>(tip)]);
            }
            if (c.end_effectors & KIMODO_END_EFFECTOR_HIPS) {
                set_position(t, 0, hips);
                set_rotation(t, 0, pose->global[0]);
            }
        }
        return {};
    }

    constraint_condition finish() {
        for (size_t t = 0; t < T; ++t)
            for (size_t j = 0; j < J; ++j) {
                if (!position_set[t * J + j]) continue;
                // Every pose constraint also pins its frame's smoothed root.
                const float *p = positions.data() + (t * J + j) * 3;
                set(t, 5 + j * 3, double(p[0]) - out.observed[t * D]);
                set(t, 6 + j * 3, p[1]);
                set(t, 7 + j * 3, double(p[2]) - out.observed[t * D + 2]);
            }
        return std::move(out);
    }
};
} // namespace

bool constraint_condition::any(std::size_t first, std::size_t rows, std::size_t motion_dim) const noexcept {
    const size_t begin = std::min(first * motion_dim, mask.size()), end = std::min((first + rows) * motion_dim, mask.size());
    return std::any_of(mask.begin() + static_cast<std::ptrdiff_t>(begin), mask.begin() + static_cast<std::ptrdiff_t>(end),
                       [](float m) { return m != 0.F; });
}

std::expected<constraint_condition, std::string> build_constraint_condition(
    const skeleton_spec &skeleton, std::span<const motion_constraint> constraints, std::size_t frames) {
    if (!frames) return std::unexpected("constraints need a non-empty clip");
    builder b(skeleton, frames);
    for (size_t index = 0; index < constraints.size(); ++index)
        if (auto added = b.add(constraints[index]); !added)
            return std::unexpected("constraint " + std::to_string(index) + ": " + added.error());
    return b.finish();
}

void normalize_motion_rows(std::span<float> rows, std::size_t motion_dim,
                           std::span<const float> global_mean, std::span<const float> global_std,
                           std::span<const float> body_mean, std::span<const float> body_std) {
    const auto scale = [](float stddev) { return std::sqrt(stddev * stddev + 1.e-5F); };
    for (size_t row = 0; row < rows.size() / motion_dim; ++row) {
        float *v = rows.data() + row * motion_dim;
        for (size_t d = 0; d < 5; ++d) v[d] = (v[d] - global_mean[d]) / scale(global_std[d]);
        for (size_t d = 0; d + 5 < motion_dim; ++d) v[5 + d] = (v[5 + d] - body_mean[d]) / scale(body_std[d]);
    }
}

} // namespace kimodo::detail
