// Re-encoding a pose sequence into Kimodo's motion representation: a port of
// upstream kimodo/motion_rep/reps/kimodo_motionrep.py (__call__),
// smooth_root.py, feet.py and feature_utils.compute_vel_xyz.  Upstream runs
// it on every post-processed segment so the next segment's hand-off is
// conditioned on the corrected motion.
#include "motion_encode.hpp"

#include <Eigen/Sparse>

#include <algorithm>
#include <array>
#include <cmath>

namespace kimodo::detail {
namespace {
constexpr float root_margin = .06F;        // get_smooth_root_pos
constexpr float admm_overrelax = 1.8F;     // smooth_signal defaults
constexpr int admm_iterations = 500;
constexpr float contact_velocity = .15F;   // KimodoMotionRep foot detection
constexpr float contact_height = .10F;

using path = std::vector<std::array<float, 2>>;

// TrajectorySmoother with pos_weight 0: minimise accelerations while each
// frame stays within `root_margin` of its target, by ADMM from `start`.
// Storage is float32 as upstream's NumPy arrays are; solves are float64.
path admm_smooth(const path &targets, const path &start) {
    const auto n = static_cast<int>(targets.size());
    if (n < 3) return targets; // no accelerations to minimise (upstream's solve is singular)
    std::vector<Eigen::Triplet<double>> a;
    for (int i = 1; i + 1 < n; ++i) a.insert(a.end(), {{i, i - 1, -1.}, {i, i, 2.}, {i, i + 1, -1.}});
    Eigen::SparseMatrix<double> acceleration(n, n), identity(n, n);
    acceleration.setFromTriplets(a.begin(), a.end());
    identity.setIdentity();
    Eigen::SparseMatrix<double> system = Eigen::SparseMatrix<double>(acceleration.transpose()) * acceleration;
    const double step = .25 * std::sqrt(system.diagonal().cwiseAbs().maxCoeff());
    system += step * identity;
    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> solver(system);

    path x = start, z(targets.size()), u(targets.size());
    Eigen::VectorXd r(n);
    for (int iteration = 0; iteration < admm_iterations; ++iteration) {
        for (size_t i = 0; i < targets.size(); ++i) {
            // z: the offset from the target, projected into the margin.
            for (int k = 0; k < 2; ++k) z[i][static_cast<size_t>(k)] = x[i][static_cast<size_t>(k)] + u[i][static_cast<size_t>(k)] - targets[i][static_cast<size_t>(k)];
            const float norm = std::hypot(z[i][0], z[i][1]);
            if (norm > root_margin) {
                const double scale = double(root_margin) / norm;
                for (float &v : z[i]) v = static_cast<float>(v * scale);
            }
            for (int k = 0; k < 2; ++k) z[i][static_cast<size_t>(k)] += targets[i][static_cast<size_t>(k)];
            for (int k = 0; k < 2; ++k) u[i][static_cast<size_t>(k)] += admm_overrelax * (x[i][static_cast<size_t>(k)] - z[i][static_cast<size_t>(k)]);
        }
        for (int k = 0; k < 2; ++k) {
            for (Eigen::Index i = 0; i < n; ++i) r[i] = step * double(z[static_cast<size_t>(i)][static_cast<size_t>(k)] - u[static_cast<size_t>(i)][static_cast<size_t>(k)]);
            const Eigen::VectorXd solved = solver.solve(r);
            for (Eigen::Index i = 0; i < n; ++i) x[static_cast<size_t>(i)][static_cast<size_t>(k)] = static_cast<float>(solved[i]);
        }
    }
    return x;
}

using mat3 = std::array<double, 9>;
mat3 mul(const mat3 &a, const mat3 &b) {
    mat3 r{};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) r[i * 3 + j] += a[i * 3 + k] * b[k * 3 + j];
    return r;
}
mat3 quaternion_matrix(const float *q) {
    const double n = std::sqrt(double(q[0]) * q[0] + double(q[1]) * q[1] + double(q[2]) * q[2] + double(q[3]) * q[3]);
    const double x = q[0] / n, y = q[1] / n, z = q[2] / n, w = q[3] / n;
    return {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
            2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
            2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)};
}
} // namespace

std::vector<float> smooth_root_path(std::span<const float> xz, std::size_t frames) {
    path x(frames), smoothed(frames);
    std::array<double, 2> mean{};
    for (size_t t = 0; t < frames; ++t)
        for (size_t k = 0; k < 2; ++k) { x[t][k] = xz[t * 2 + k]; mean[k] += x[t][k]; }
    for (auto &p : smoothed) p = {static_cast<float>(mean[0] / double(frames)), static_cast<float>(mean[1] / double(frames))};

    // Coarse to fine: smooth every `stride`-th frame, then interpolate the
    // frames in between as the initial guess for the next level.
    const int levels = std::max(static_cast<int>(std::floor(std::log2(double(frames)))) - 4, 1);
    for (size_t stride = size_t{1} << levels;; stride /= 2) {
        path targets, start;
        for (size_t t = 0; t < frames; t += stride) { targets.push_back(x[t]); start.push_back(smoothed[t]); }
        const path level = admm_smooth(targets, start);
        for (size_t i = 0; i < level.size(); ++i) smoothed[i * stride] = level[i];

        // Upstream writes x_smoothed[half::stride] from x_smoothed[::stride].
        // On the finest level half is 0, so both name every frame: the last
        // is extrapolated and the rest become the mean of each frame and its
        // successor.  Reproduced as is.
        const size_t half = stride / 2, steps = level.size();
        std::vector<size_t> between;
        for (size_t t = half; t < frames; t += stride) between.push_back(t);
        size_t interpolated = between.size();
        const auto coarse = [&](size_t i) { return smoothed[i * stride]; };
        if (interpolated == steps && steps >= 2) {
            const auto last = coarse(steps - 1), before = coarse(steps - 2);
            smoothed[between.back()] = {last[0] + (last[0] - before[0]) / 2, last[1] + (last[1] - before[1]) / 2};
            --interpolated;
        }
        path means(interpolated);
        for (size_t i = 0; i < interpolated; ++i) {
            const auto a = coarse(i), b = coarse(i + 1);
            means[i] = {(a[0] + b[0]) / 2, (a[1] + b[1]) / 2};
        }
        for (size_t i = 0; i < interpolated; ++i) smoothed[between[i]] = means[i];
        if (stride == 1) break;
    }
    std::vector<float> out(frames * 2);
    for (size_t t = 0; t < frames; ++t) { out[t * 2] = smoothed[t][0]; out[t * 2 + 1] = smoothed[t][1]; }
    return out;
}

std::vector<float> encode_motion(const skeleton_spec &s, std::span<const float> local_xyzw,
                                 std::span<const float> roots, std::size_t frames, float fps) {
    const size_t J = s.joints(), D = s.motion_dim(), rotation_begin = 5 + 3 * J, velocity_begin = 5 + 9 * J, contact_begin = 5 + 12 * J;
    std::vector<mat3> global(frames * J);
    std::vector<std::array<double, 3>> posed(frames * J);
    for (size_t t = 0; t < frames; ++t)
        for (size_t j = 0; j < J; ++j) {
            const mat3 local = quaternion_matrix(local_xyzw.data() + (t * J + j) * 4);
            const int parent = s.parents[j];
            if (parent < 0) {
                global[t * J + j] = local;
                posed[t * J + j] = {roots[t * 3], roots[t * 3 + 1], roots[t * 3 + 2]};
                continue;
            }
            const size_t p = t * J + static_cast<size_t>(parent);
            global[t * J + j] = mul(global[p], local);
            const auto &o = s.offsets[j];
            const mat3 &m = global[p];
            for (int k = 0; k < 3; ++k) posed[t * J + j][static_cast<size_t>(k)] = posed[p][static_cast<size_t>(k)] + m[k * 3] * o[0] + m[k * 3 + 1] * o[1] + m[k * 3 + 2] * o[2];
        }
    std::vector<float> ground(frames * 2);
    for (size_t t = 0; t < frames; ++t) { ground[t * 2] = roots[t * 3]; ground[t * 2 + 1] = roots[t * 3 + 2]; }
    const auto smooth = smooth_root_path(ground, frames);

    std::vector<float> rows(frames * D);
    for (size_t t = 0; t < frames; ++t) {
        float *row = rows.data() + t * D;
        const double sx = smooth[t * 2], sz = smooth[t * 2 + 1];
        row[0] = static_cast<float>(sx); row[1] = roots[t * 3 + 1]; row[2] = static_cast<float>(sz);
        const auto &right = posed[t * J + s.hips[0]], &left = posed[t * J + s.hips[1]];
        const double heading = std::atan2(right[2] - left[2], -(right[0] - left[0]));
        row[3] = static_cast<float>(std::cos(heading)); row[4] = static_cast<float>(std::sin(heading));
        for (size_t j = 0; j < J; ++j) {
            const auto &p = posed[t * J + j];
            row[5 + j * 3] = static_cast<float>(p[0] - sx); row[6 + j * 3] = static_cast<float>(p[1]); row[7 + j * 3] = static_cast<float>(p[2] - sz);
            for (size_t d = 0; d < 6; ++d) row[rotation_begin + j * 6 + d] = static_cast<float>(global[t * J + j][(d % 3) * 3 + d / 3]);
            // compute_vel_xyz: forward differences, the last one repeated.
            const size_t a = frames < 2 ? t : std::min(t, frames - 2);
            for (size_t k = 0; k < 3; ++k)
                row[velocity_begin + j * 3 + k] = frames < 2 ? 0.F : static_cast<float>(fps * (posed[(a + 1) * J + j][k] - posed[a * J + j][k]));
        }
        // foot_detect_from_pos_and_vel: [left foot, left toe, right foot, right toe].
        const std::array<int, 4> feet{static_cast<int>(s.end_effectors[0]), s.end_effector_tips[0], static_cast<int>(s.end_effectors[1]), s.end_effector_tips[1]};
        for (size_t c = 0; c < 4; ++c) {
            const auto j = static_cast<size_t>(feet[c]);
            const float *v = row + velocity_begin + j * 3;
            const bool still = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) < contact_velocity;
            row[contact_begin + c] = still && posed[t * J + j][1] < contact_height ? 1.F : 0.F;
        }
    }
    return rows;
}

} // namespace kimodo::detail
