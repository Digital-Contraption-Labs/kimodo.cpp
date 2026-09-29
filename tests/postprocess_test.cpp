// Post-processing: the root smoother against upstream's own output
// (reference/dump_smooth_root_reference.py), re-encoding against the
// constraint builder, and MotionCorrection through our adapter pinning what
// upstream pins.  Needs no weights.
#include "constraints.hpp"
#include "motion_encode.hpp"
#include "postprocess.hpp"
#include "skeleton.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {
int failures = 0;
#define CHECK(condition, ...) do { if (!(condition)) { ++failures; std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); } } while (0)

using kimodo::constraint_type;
using kimodo::motion_constraint;
using kimodo::detail::skeleton_spec;
using vec3 = std::array<double, 3>;

void smoother_matches_upstream() {
    constexpr size_t frames = 97;
    std::vector<float> path(frames * 2);
    for (size_t t = 0; t < frames; ++t) {
        const double f = double(t);
        path[t * 2] = static_cast<float>(.045 * f + .03 * std::sin(1.3 * f) + .01 * std::cos(4.7 * f));
        path[t * 2 + 1] = static_cast<float>(.4 * std::sin(.05 * f) + .02 * std::cos(2.1 * f));
    }
    // smooth_signal(path, margins=0.06) from upstream, sampled.
    constexpr std::array<std::array<float, 3>, 13> reference{{
        {0, 0.0300652273F, 0.0583936721F}, {1, 0.0745509639F, 0.0734461695F}, {2, 0.119040705F, 0.0884781182F},
        {7, 0.341847718F, 0.161970615F}, {15, 0.700071037F, 0.265170276F}, {31, 1.41506445F, 0.365898699F},
        {48, 2.17674351F, 0.243103012F}, {63, 2.8492434F, 0.00471332483F}, {64, 2.89448619F, -0.0130630098F},
        {80, 3.62197781F, -0.276211321F}, {94, 4.24091816F, -0.426667154F}, {95, 4.29589939F, -0.438607305F},
        {96, 4.32888699F, -0.445764959F}}};
    const auto smoothed = kimodo::detail::smooth_root_path(path, frames);
    double worst = 0;
    for (const auto &[frame, x, z] : reference) {
        const auto t = static_cast<size_t>(frame);
        worst = std::max({worst, std::abs(double(smoothed[t * 2]) - x), std::abs(double(smoothed[t * 2 + 1]) - z)});
    }
    CHECK(worst < 5.e-5, "smoothed root differs from upstream by %g m", worst);
    std::printf("root smoother vs upstream: max |diff| %.3g m\n", worst);
}

std::array<float, 4> axis_angle(double x, double y, double z) {
    const double angle = std::sqrt(x * x + y * y + z * z);
    if (angle < 1.e-12) return {0.F, 0.F, 0.F, 1.F};
    const double s = std::sin(angle / 2) / angle;
    return {float(x * s), float(y * s), float(z * s), float(std::cos(angle / 2))};
}

// A SOMA walk: hips travelling +Z at 1 m/s with a sway, legs and arms swinging.
struct walk { std::vector<float> hips, rotations; };
walk synthetic_walk(const skeleton_spec &s, size_t frames) {
    const size_t J = s.joints();
    walk w{std::vector<float>(frames * 3), std::vector<float>(frames * J * 4)};
    for (size_t t = 0; t < frames; ++t) {
        const double time = double(t) / 30, phase = 2 * 3.14159265358979 * time;
        w.hips[t * 3] = static_cast<float>(.03 * std::sin(phase / 2));
        w.hips[t * 3 + 1] = static_cast<float>(.93 + .01 * std::cos(phase));
        w.hips[t * 3 + 2] = static_cast<float>(time);
        for (size_t j = 0; j < J; ++j) {
            const std::string_view name = s.names[j];
            double swing = 0;
            if (name == "LeftLeg" || name == "RightArm") swing = .4 * std::sin(phase);
            if (name == "RightLeg" || name == "LeftArm") swing = -.4 * std::sin(phase);
            if (name.ends_with("Shin")) swing = .3 * (1 + std::sin(phase + (name.starts_with("Left") ? 0 : 3.14159265358979)));
            const auto q = axis_angle(swing, 0, 0);
            std::copy(q.begin(), q.end(), w.rotations.begin() + static_cast<std::ptrdiff_t>((t * J + j) * 4));
        }
    }
    return w;
}

std::vector<vec3> forward(const skeleton_spec &s, const float *hips, const float *xyzw) {
    const size_t J = s.joints();
    std::vector<std::array<double, 9>> global(J);
    std::vector<vec3> posed(J);
    for (size_t j = 0; j < J; ++j) {
        const float *q = xyzw + j * 4;
        const double n = std::sqrt(double(q[0]) * q[0] + double(q[1]) * q[1] + double(q[2]) * q[2] + double(q[3]) * q[3]);
        const double x = q[0] / n, y = q[1] / n, z = q[2] / n, w = q[3] / n;
        const std::array<double, 9> local{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
                                          2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                                          2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)};
        if (s.parents[j] < 0) { global[j] = local; posed[j] = {hips[0], hips[1], hips[2]}; continue; }
        const auto p = static_cast<size_t>(s.parents[j]);
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) {
            double sum = 0;
            for (int k = 0; k < 3; ++k) sum += global[p][r * 3 + k] * local[k * 3 + c];
            global[j][r * 3 + c] = sum;
        }
        for (int k = 0; k < 3; ++k)
            posed[j][k] = posed[p][k] + global[p][k * 3] * s.offsets[j][0] + global[p][k * 3 + 1] * s.offsets[j][1] + global[p][k * 3 + 2] * s.offsets[j][2];
    }
    return posed;
}

double distance(const vec3 &a, const vec3 &b) { return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); }

void encoding_matches_conditioning() {
    const auto &s = kimodo::detail::soma30_spec;
    constexpr size_t frames = 64;
    const size_t J = s.joints(), D = s.motion_dim(), rotation_end = 5 + 9 * J;
    const auto w = synthetic_walk(s, frames);
    const auto rows = kimodo::detail::encode_motion(s, w.rotations, w.hips, frames);
    // A fullbody keyframe with the encoding's own smoothed root must describe
    // the same features the encoder wrote.
    motion_constraint key{constraint_type::fullbody};
    for (unsigned t : {0U, 20U, 63U}) {
        key.frames.push_back(t);
        key.root_positions.insert(key.root_positions.end(), w.hips.begin() + t * 3, w.hips.begin() + t * 3 + 3);
        key.local_rotations_xyzw.insert(key.local_rotations_xyzw.end(), w.rotations.begin() + static_cast<std::ptrdiff_t>(t * J * 4), w.rotations.begin() + static_cast<std::ptrdiff_t>((t + 1) * J * 4));
        key.smooth_root_2d.insert(key.smooth_root_2d.end(), {rows[t * D], rows[t * D + 2]});
    }
    const auto built = kimodo::detail::build_constraint_condition(s, std::vector{key}, frames);
    CHECK(built.has_value(), "build: %s", built ? "" : built.error().c_str());
    double worst = 0;
    for (unsigned t : key.frames)
        for (size_t d = 0; d < 5 + 3 * J; ++d) worst = std::max(worst, std::abs(double(rows[t * D + d]) - built->observed[t * D + d]));
    CHECK(worst < 1.e-5, "encoded root, heading and positions differ from the keyframe condition by %g", worst);
    // Velocities: forward differences of the posed joints, the last repeated.
    const auto p10 = forward(s, &w.hips[30], &w.rotations[10 * J * 4]), p11 = forward(s, &w.hips[33], &w.rotations[11 * J * 4]);
    const float *v10 = &rows[10 * D + rotation_end], *v63 = &rows[63 * D + rotation_end], *v62 = &rows[62 * D + rotation_end];
    CHECK(std::abs(v10[2] - 30 * (p11[0][2] - p10[0][2])) < 1.e-3, "hips velocity %g", double(v10[2]));
    CHECK(v63[5] == v62[5], "the last velocity repeats the one before");
    // The walker's feet never rest below 10 cm in this swing, the contacts
    // stay within 0/1, and the hips height is kept.
    CHECK(rows[5 * D + D - 4] == 0.F || rows[5 * D + D - 4] == 1.F, "contacts are 0/1");
    CHECK(rows[5 * D + 1] == w.hips[16], "root height is the hips height");
}

void correction_pins_targets() {
    const auto &s = kimodo::detail::soma30_spec;
    constexpr size_t frames = 90;
    const size_t J = s.joints();
    const auto w = synthetic_walk(s, frames);
    std::vector<float> contacts(frames * 4);

    // A different pose to reach at frame 45: arms up, crouched a little.
    motion_constraint key{constraint_type::fullbody};
    key.frames = {45};
    key.root_positions = {w.hips[135] + .1F, .8F, w.hips[137]};
    key.local_rotations_xyzw.assign(w.rotations.begin() + 45 * static_cast<std::ptrdiff_t>(J) * 4, w.rotations.begin() + 46 * static_cast<std::ptrdiff_t>(J) * 4);
    for (const char *arm : {"LeftArm", "RightArm"})
        for (size_t j = 0; j < J; ++j)
            if (s.names[j] == arm) {
                const auto q = axis_angle(0, 0, s.names[j].starts_with("Left") ? 1.2 : -1.2);
                std::copy(q.begin(), q.end(), key.local_rotations_xyzw.begin() + static_cast<std::ptrdiff_t>(j * 4));
            }
    // The left hand somewhere new at frame 70, the root 30 cm aside at 20.
    motion_constraint hand{constraint_type::end_effector};
    hand.end_effectors = KIMODO_END_EFFECTOR_LEFT_HAND;
    hand.frames = {70};
    hand.root_positions.assign(w.hips.begin() + 210, w.hips.begin() + 213);
    hand.local_rotations_xyzw.assign(w.rotations.begin() + 70 * static_cast<std::ptrdiff_t>(J) * 4, w.rotations.begin() + 71 * static_cast<std::ptrdiff_t>(J) * 4);
    const auto elbow = axis_angle(0, -.9, 0);
    std::copy(elbow.begin(), elbow.end(), hand.local_rotations_xyzw.begin() + 12 * 4); // LeftForeArm
    motion_constraint aside{constraint_type::root2d};
    aside.frames = {20};
    aside.smooth_root_2d = {w.hips[60] + .3F, w.hips[62]};

    const std::vector<motion_constraint> constraints{key, hand, aside};
    const auto targets = kimodo::detail::build_postprocess_targets(s, constraints, frames);
    CHECK(targets.full_body[45] == 1.F && targets.left_hand[70] == 1.F && targets.root[20] == 1.F && targets.full_body_pose[45] == 1,
          "targets carry upstream's masks");
    auto hips = w.hips, rotations = w.rotations;
    const auto corrected = kimodo::detail::correct_motion(s, hips, rotations, contacts, targets, .04F);
    CHECK(corrected.has_value(), "correct_motion: %s", corrected ? "" : corrected.error().c_str());

    const auto target_pose = forward(s, key.root_positions.data(), key.local_rotations_xyzw.data());
    const auto got_pose = forward(s, &hips[45 * 3], &rotations[45 * J * 4]);
    double pose_error = 0;
    for (size_t j = 0; j < J; ++j) pose_error = std::max(pose_error, distance(target_pose[j], got_pose[j]));
    CHECK(pose_error < .005, "full-body keyframe reached within %g m", pose_error);

    const auto hand_target = forward(s, hand.root_positions.data(), hand.local_rotations_xyzw.data())[13];
    const auto hand_before = forward(s, &w.hips[70 * 3], &w.rotations[70 * J * 4])[13];
    const auto hand_after = forward(s, &hips[70 * 3], &rotations[70 * J * 4])[13];
    CHECK(distance(hand_after, hand_target) < .01 && distance(hand_before, hand_target) > .1,
          "left hand moved from %g m to %g m off its target", distance(hand_before, hand_target), distance(hand_after, hand_target));

    const double root_miss = std::hypot(hips[60] - aside.smooth_root_2d[0], hips[62] - aside.smooth_root_2d[1]);
    CHECK(root_miss <= .04 + 1.e-3, "root waypoint missed by %g m (margin 0.04)", root_miss);
    std::printf("correction: keyframe %.4f m, hand %.4f m, root %.4f m\n", pose_error, distance(hand_after, hand_target), root_miss);
}

void contacts_stop_foot_skate() {
    const auto &s = kimodo::detail::soma30_spec;
    constexpr size_t frames = 60;
    const size_t J = s.joints(), foot = s.end_effectors[0];
    auto w = synthetic_walk(s, frames);
    // Hold the pose still while the hips drift 10 cm sideways, so the foot
    // skates, and flag the left foot and toe as planted for frames 20..39.
    for (size_t t = 0; t < frames; ++t) {
        std::copy_n(w.rotations.begin(), J * 4, w.rotations.begin() + static_cast<std::ptrdiff_t>(t * J * 4));
        w.hips[t * 3] = .005F * static_cast<float>(t);
        w.hips[t * 3 + 1] = .93F;
        w.hips[t * 3 + 2] = 0.F;
    }
    std::vector<float> contacts(frames * 4);
    for (size_t t = 20; t < 40; ++t) contacts[t * 4] = contacts[t * 4 + 1] = 1.F;
    const kimodo::detail::postprocess_targets none(frames, J);
    auto hips = w.hips, rotations = w.rotations;
    const auto corrected = kimodo::detail::correct_motion(s, hips, rotations, contacts, none, .04F);
    CHECK(corrected.has_value(), "correct_motion: %s", corrected ? "" : corrected.error().c_str());
    const auto slide = [&](const std::vector<float> &h, const std::vector<float> &r) {
        return distance(forward(s, &h[20 * 3], &r[20 * J * 4])[foot], forward(s, &h[39 * 3], &r[39 * J * 4])[foot]);
    };
    const double before = slide(w.hips, w.rotations), after = slide(hips, rotations);
    CHECK(before > .09 && after < .01, "planted foot slid %g m before, %g m after", before, after);
    std::printf("foot skate over a planted contact: %.3f m -> %.4f m\n", before, after);
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    smoother_matches_upstream();
    encoding_matches_conditioning();
    correction_pins_targets();
    contacts_stop_foot_skate();
    if (failures) std::fprintf(stderr, "%d post-processing check(s) failed\n", failures);
    else std::printf("post-processing checks passed\n");
    return failures ? 1 : 0;
}
