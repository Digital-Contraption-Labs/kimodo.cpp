// Kinematic constraints -> denoiser condition.  The reference is the
// multi-prompt hand-off (prepare_sequence_transition), whose rows the upstream
// fixture checks: upstream builds them from exactly a fullbody plus a
// four-end-effector constraint set, so our builder must reproduce them from
// the same poses given as local rotations.  Needs no weights.
#include "constraints.hpp"
#include "denoiser.hpp"
#include "skeleton.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {
int failures = 0;
#define CHECK(condition, ...) do { if (!(condition)) { ++failures; std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); } } while (0)

using kimodo::constraint_type;
using kimodo::motion_constraint;
using kimodo::detail::skeleton_spec;
using mat3 = std::array<double, 9>;

mat3 random_rotation(std::mt19937 &rng) {
    std::normal_distribution<double> normal;
    double q[4]; double n = 0;
    for (double &v : q) { v = normal(rng); n += v * v; }
    n = std::sqrt(n);
    const double x = q[0] / n, y = q[1] / n, z = q[2] / n, w = q[3] / n;
    return {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
            2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
            2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)};
}
mat3 transpose_mul(const mat3 &a, const mat3 &b) { // a^T b
    mat3 r{};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) r[i * 3 + j] += a[k * 3 + i] * b[k * 3 + j];
    return r;
}
std::array<float, 4> quaternion_xyzw(const mat3 &m) {
    double x, y, z, w;
    const double trace = m[0] + m[4] + m[8];
    if (trace > 0) { const double s = std::sqrt(trace + 1) * 2; w = s / 4; x = (m[7] - m[5]) / s; y = (m[2] - m[6]) / s; z = (m[3] - m[1]) / s; }
    else if (m[0] > m[4] && m[0] > m[8]) { const double s = std::sqrt(1 + m[0] - m[4] - m[8]) * 2; w = (m[7] - m[5]) / s; x = s / 4; y = (m[1] + m[3]) / s; z = (m[2] + m[6]) / s; }
    else if (m[4] > m[8]) { const double s = std::sqrt(1 + m[4] - m[0] - m[8]) * 2; w = (m[2] - m[6]) / s; x = (m[1] + m[3]) / s; y = s / 4; z = (m[5] + m[7]) / s; }
    else { const double s = std::sqrt(1 + m[8] - m[0] - m[4]) * 2; w = (m[3] - m[1]) / s; x = (m[2] + m[6]) / s; y = (m[5] + m[7]) / s; z = s / 4; }
    return {float(x), float(y), float(z), float(w)};
}

// Random raw rows as the sampler produces them, and the same poses as
// (root position, local XYZW rotations, smoothed root) constraint input.
struct synthetic_motion {
    std::vector<float> rows;
    std::vector<float> roots, rotations, smooth;
};
synthetic_motion synthesize(const skeleton_spec &s, size_t frames, std::mt19937 &rng) {
    const size_t J = s.joints(), D = s.motion_dim(), rotation_begin = 5 + 3 * J;
    std::uniform_real_distribution<float> uniform(-1.F, 1.F);
    synthetic_motion m;
    m.rows.resize(frames * D);
    for (float &v : m.rows) v = uniform(rng);
    for (size_t t = 0; t < frames; ++t) {
        float *row = m.rows.data() + t * D;
        row[6] = .9F + .1F * uniform(rng); // hips height
        std::vector<mat3> global(J);
        for (size_t j = 0; j < J; ++j) {
            global[j] = random_rotation(rng);
            for (size_t d = 0; d < 6; ++d) row[rotation_begin + j * 6 + d] = float(global[j][(d % 3) * 3 + d / 3]);
        }
        // The hand-off decodes 6D back to exactly these matrices.
        m.roots.insert(m.roots.end(), {row[0] + row[5], row[6], row[2] + row[7]});
        m.smooth.insert(m.smooth.end(), {row[0], row[2]});
        for (size_t j = 0; j < J; ++j) {
            const int parent = s.parents[j];
            const auto q = quaternion_xyzw(parent < 0 ? global[j] : transpose_mul(global[static_cast<size_t>(parent)], global[j]));
            m.rotations.insert(m.rotations.end(), q.begin(), q.end());
        }
    }
    return m;
}

void transition_parity(const skeleton_spec &s) {
    std::mt19937 rng(1234);
    constexpr unsigned overlap = 5;
    constexpr size_t previous_frames = 12, continuation = 30;
    const size_t J = s.joints(), D = s.motion_dim(), rotation_end = 5 + 9 * J;
    const auto previous = synthesize(s, previous_frames, rng);
    const auto reference = kimodo::detail::prepare_sequence_transition(s, previous.rows, continuation, overlap);
    CHECK(reference.has_value(), "%s: transition: %s", std::string(s.key).c_str(), reference ? "" : reference.error().c_str());
    if (!reference) return;

    // The last `overlap` poses, moved to the hand-off origin, as constraints
    // on frames 0..overlap-1 of the continuation.
    motion_constraint full{constraint_type::fullbody}, ends{constraint_type::end_effector};
    ends.end_effectors = KIMODO_END_EFFECTOR_LEFT_FOOT | KIMODO_END_EFFECTOR_RIGHT_FOOT | KIMODO_END_EFFECTOR_LEFT_HAND | KIMODO_END_EFFECTOR_RIGHT_HAND;
    const size_t first = previous_frames - overlap;
    const float origin_x = previous.smooth[first * 2], origin_z = previous.smooth[first * 2 + 1];
    for (unsigned t = 0; t < overlap; ++t) {
        const size_t f = first + t;
        for (auto *c : {&full, &ends}) {
            c->frames.push_back(t);
            c->root_positions.insert(c->root_positions.end(), {previous.roots[f * 3] - origin_x, previous.roots[f * 3 + 1], previous.roots[f * 3 + 2] - origin_z});
            c->smooth_root_2d.insert(c->smooth_root_2d.end(), {previous.smooth[f * 2] - origin_x, previous.smooth[f * 2 + 1] - origin_z});
            c->local_rotations_xyzw.insert(c->local_rotations_xyzw.end(), previous.rotations.begin() + static_cast<std::ptrdiff_t>(f * J * 4),
                                           previous.rotations.begin() + static_cast<std::ptrdiff_t>((f + 1) * J * 4));
        }
    }
    const std::vector<motion_constraint> constraints{full, ends};
    const auto built = kimodo::detail::build_constraint_condition(s, constraints, continuation + overlap);
    CHECK(built.has_value(), "%s: build: %s", std::string(s.key).c_str(), built ? "" : built.error().c_str());
    if (!built) return;
    double worst = 0;
    size_t mask_mismatches = 0;
    for (size_t t = 0; t < continuation + overlap; ++t)
        for (size_t d = 0; d < D; ++d) {
            const size_t i = t * D + d;
            // The hand-off copies the unconstrained velocity/contact tail too;
            // only masked features reach the denoiser.
            if (d >= rotation_end && t < overlap) continue;
            if (built->mask[i] != reference->observed_mask[i]) ++mask_mismatches;
            if (reference->observed_mask[i] != 0.F) worst = std::max(worst, std::abs(double(built->observed[i]) - reference->observed[i]));
        }
    CHECK(mask_mismatches == 0, "%s: %zu mask features differ from the hand-off", std::string(s.key).c_str(), mask_mismatches);
    CHECK(worst < 2.e-5, "%s: constrained values differ from the hand-off by %g", std::string(s.key).c_str(), worst);
    std::printf("%s transition parity: max |diff| %.3g\n", std::string(s.key).c_str(), worst);
}

size_t masked(const kimodo::detail::constraint_condition &c, size_t t, size_t D) {
    size_t n = 0;
    for (size_t d = 0; d < D; ++d) n += c.mask[t * D + d] != 0.F;
    return n;
}

void rest_pose(const skeleton_spec &s, motion_constraint &c, unsigned frame, std::array<float, 3> root) {
    c.frames.push_back(frame);
    c.root_positions.insert(c.root_positions.end(), root.begin(), root.end());
    for (size_t j = 0; j < s.joints(); ++j) c.local_rotations_xyzw.insert(c.local_rotations_xyzw.end(), {0.F, 0.F, 0.F, 1.F});
}

void shapes_and_rules() {
    const auto &soma = kimodo::detail::soma30_spec;
    const size_t D = soma.motion_dim(), J = soma.joints();

    motion_constraint path{constraint_type::root2d};
    path.frames = {0, 10};
    path.smooth_root_2d = {0.F, 0.F, .5F, 1.F};
    path.root_heading = {0.F, 2.F, 1.F, 0.F};
    auto built = kimodo::detail::build_constraint_condition(soma, std::vector{path}, 20);
    CHECK(built && masked(*built, 10, D) == 4 && masked(*built, 5, D) == 0, "root2d masks x, z and heading only");
    CHECK(built && built->observed[10 * D] == .5F && built->observed[10 * D + 2] == 1.F, "root2d places (x, z)");
    CHECK(built && built->observed[3] == 0.F && built->observed[4] == 1.F, "root2d heading is normalized");

    // Left hand: root (3) + heading (2) + hand and hand-end positions (6) +
    // hand rotation (6).
    motion_constraint hand{constraint_type::end_effector};
    hand.end_effectors = KIMODO_END_EFFECTOR_LEFT_HAND;
    rest_pose(soma, hand, 3, {.2F, .95F, .4F});
    built = kimodo::detail::build_constraint_condition(soma, std::vector{hand}, 20);
    CHECK(built && masked(*built, 3, D) == 17, "SOMA left hand masks 17 features, got %zu", built ? masked(*built, 3, D) : 0);
    CHECK(built && built->mask[3 * D + 5 + 13 * 3] == 1.F && built->mask[3 * D + 5 + 15 * 3] == 1.F && built->mask[3 * D + 5 + 3 * J + 13 * 6] == 1.F,
          "SOMA left hand constrains LeftHand, LeftHandMiddleEnd and LeftHand's rotation");
    // A rest pose faces +Z: the right hip is at -X.
    CHECK(built && std::abs(built->observed[3 * D + 3] - 1.F) < 1.e-4F && std::abs(built->observed[3 * D + 4]) < 1.e-2F, "rest pose heading is about 0");
    CHECK(built && std::abs(built->observed[3 * D + 1] - .95F) < 1.e-6F, "pose constraints pin the hips height");

    const auto &smplx = kimodo::detail::smplx22_spec;
    motion_constraint wrist{constraint_type::end_effector};
    wrist.end_effectors = KIMODO_END_EFFECTOR_RIGHT_HAND;
    rest_pose(smplx, wrist, 0, {0.F, .9F, 0.F});
    built = kimodo::detail::build_constraint_condition(smplx, std::vector{wrist}, 4);
    CHECK(built && masked(*built, 0, smplx.motion_dim()) == 14, "SMPL-X wrist is a one-joint chain");

    // A later constraint overrides the smoothed root, and the joint positions
    // follow the final smoothed root of their frame.
    motion_constraint body{constraint_type::fullbody};
    rest_pose(soma, body, 3, {.2F, .95F, .4F});
    motion_constraint waypoint{constraint_type::root2d};
    waypoint.frames = {3};
    waypoint.smooth_root_2d = {1.F, 2.F};
    built = kimodo::detail::build_constraint_condition(soma, std::vector{body, waypoint}, 20);
    CHECK(built && built->observed[3 * D] == 1.F && std::abs(built->observed[3 * D + 5] - (.2F - 1.F)) < 1.e-6F,
          "positions are relative to the final smoothed root");
    CHECK(built && built->any(3, 1, D) && !built->any(4, 16, D), "any() sees only constrained rows");

    const auto fails = [&](motion_constraint c, const char *what) {
        const auto r = kimodo::detail::build_constraint_condition(soma, std::vector{c}, 20);
        CHECK(!r, "%s should be rejected", what);
    };
    motion_constraint late = body; late.frames = {20};
    fails(late, "a frame past the clip");
    motion_constraint short_rotations = body; short_rotations.local_rotations_xyzw.resize(4 * (J - 1));
    fails(short_rotations, "a pose with too few rotations");
    motion_constraint zero = body; std::fill(zero.local_rotations_xyzw.begin(), zero.local_rotations_xyzw.begin() + 4, 0.F);
    fails(zero, "a zero quaternion");
    motion_constraint no_path{constraint_type::root2d}; no_path.frames = {1};
    fails(no_path, "root2d without smooth_root_2d");
    motion_constraint no_bits = hand; no_bits.end_effectors = 0;
    fails(no_bits, "an end-effector constraint without end effectors");
    motion_constraint heading_on_pose = body; heading_on_pose.root_heading = {1.F, 0.F};
    fails(heading_on_pose, "root_heading on a pose");
    motion_constraint nan = path; nan.smooth_root_2d[1] = NAN;
    fails(nan, "a non-finite value");
    motion_constraint bad_type = path; bad_type.type = static_cast<constraint_type>(9);
    fails(bad_type, "an unknown type");
}
} // namespace

int main() {
    transition_parity(kimodo::detail::soma30_spec);
    transition_parity(kimodo::detail::g1skel34_spec);
    transition_parity(kimodo::detail::smplx22_spec);
    shapes_and_rules();
    if (failures) std::fprintf(stderr, "%d constraint check(s) failed\n", failures);
    else std::printf("constraint checks passed\n");
    return failures ? 1 : 0;
}
