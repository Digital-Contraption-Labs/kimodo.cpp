// Post-processing: a port of upstream kimodo/postprocess.py and of
// MotionCorrection's Python binding (BindingsPython.cpp correct_motion) around
// the vendored MotionCorrection library in third_party/motion_correction.
#include "postprocess.hpp"

#include "AnimProcessing/Utility.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace kimodo::detail {
namespace {
// The constrained pose's hips: its root joint's position (FK roots it there).
std::array<float, 3> pose_hips(const motion_constraint &c, size_t i) {
    return {c.root_positions[i * 3], c.root_positions[i * 3 + 1], c.root_positions[i * 3 + 2]};
}

// strip_nan_inf, as the binding applies to every value it reads.
float finite(float v) { return std::isfinite(v) ? v : 0.F; }

std::vector<Math::Transform> working_rig(const skeleton_spec &s) {
    // create_working_rig_from_skeleton: rest offsets and identity rotations,
    // the root lifted so the lowest rest joint sits just above the ground
    // (upstream lifts SOMA more, as its model generates lower to the ground).
    const size_t J = s.joints();
    std::vector<std::array<double, 3>> rest(J);
    double lowest = 0;
    for (size_t j = 0; j < J; ++j) {
        if (s.parents[j] >= 0)
            for (size_t k = 0; k < 3; ++k) rest[j][k] = rest[static_cast<size_t>(s.parents[j])][k] + s.offsets[j][k];
        lowest = std::min(lowest, rest[j][1]);
    }
    const double lift = s.key == soma30_spec.key ? .02 : .007;
    std::vector<Math::Transform> pose(J);
    for (size_t j = 0; j < J; ++j) {
        const auto &o = s.offsets[j];
        pose[j].SetTranslation(s.parents[j] < 0 ? Math::Vector(0.F, static_cast<float>(-lowest + lift), 0.F) : Math::Vector(o[0], o[1], o[2]));
        pose[j].SetRotation(Math::Quaternion(0.F, 0.F, 0.F, 1.F));
    }
    return pose;
}

std::vector<std::vector<Math::Transform>> poses(const std::vector<Math::Transform> &rig, std::span<const float> hips,
                                                std::span<const float> xyzw, size_t frames) {
    const size_t J = rig.size();
    std::vector<std::vector<Math::Transform>> out(frames, rig);
    for (size_t f = 0; f < frames; ++f) {
        out[f][0].SetTranslation(Math::Vector(finite(hips[f * 3]), finite(hips[f * 3 + 1]), finite(hips[f * 3 + 2])));
        for (size_t j = 0; j < J; ++j) {
            const float *q = xyzw.data() + (f * J + j) * 4;
            Math::Quaternion rotation(finite(q[0]), finite(q[1]), finite(q[2]), finite(q[3]));
            rotation.Normalize();
            out[f][j].SetRotation(rotation);
        }
    }
    return out;
}
} // namespace

postprocess_targets::postprocess_targets(std::size_t frames, std::size_t joints)
    : hips(frames * 3), rotations_xyzw(frames * joints * 4), full_body(frames), left_hand(frames), right_hand(frames),
      left_foot(frames), right_foot(frames), root(frames), full_body_pose(frames) {
    for (size_t i = 3; i < rotations_xyzw.size(); i += 4) rotations_xyzw[i] = 1.F;
}

postprocess_targets postprocess_targets::slice(std::ptrdiff_t first, std::size_t count, std::size_t joints) const {
    postprocess_targets out(count, joints);
    for (size_t i = 0; i < count; ++i) {
        const std::ptrdiff_t source = first + static_cast<std::ptrdiff_t>(i);
        if (source < 0 || static_cast<size_t>(source) >= frames()) continue;
        const auto t = static_cast<size_t>(source);
        std::copy_n(hips.begin() + static_cast<std::ptrdiff_t>(t * 3), 3, out.hips.begin() + static_cast<std::ptrdiff_t>(i * 3));
        std::copy_n(rotations_xyzw.begin() + static_cast<std::ptrdiff_t>(t * joints * 4), joints * 4, out.rotations_xyzw.begin() + static_cast<std::ptrdiff_t>(i * joints * 4));
        out.full_body[i] = full_body[t]; out.left_hand[i] = left_hand[t]; out.right_hand[i] = right_hand[t];
        out.left_foot[i] = left_foot[t]; out.right_foot[i] = right_foot[t]; out.root[i] = root[t];
        out.full_body_pose[i] = full_body_pose[t];
    }
    return out;
}

postprocess_targets build_postprocess_targets(const skeleton_spec &s, std::span<const motion_constraint> constraints, std::size_t frames) {
    const size_t J = s.joints();
    postprocess_targets out(frames, J);
    // extract_input_motion_from_constraints sorts fullbody constraints last,
    // so their exact hips win over smoothed or end-effector roots.
    for (const bool fullbody_pass : {false, true})
        for (const auto &c : constraints) {
            if ((c.type == constraint_type::fullbody) != fullbody_pass) continue;
            for (size_t i = 0; i < c.frames.size(); ++i) {
                const size_t t = c.frames[i];
                if (c.type == constraint_type::root2d) {
                    out.root[t] = 1.F;
                    out.hips[t * 3] = c.smooth_root_2d[i * 2];
                    out.hips[t * 3 + 2] = c.smooth_root_2d[i * 2 + 1];
                    continue;
                }
                auto hips = pose_hips(c, i);
                if (c.type == constraint_type::end_effector && !(c.end_effectors & KIMODO_END_EFFECTOR_HIPS) && !c.smooth_root_2d.empty()) {
                    // The model conditions an end effector's root on its
                    // smoothed root, not the hips.
                    hips[0] = c.smooth_root_2d[i * 2];
                    hips[2] = c.smooth_root_2d[i * 2 + 1];
                }
                std::copy(hips.begin(), hips.end(), out.hips.begin() + static_cast<std::ptrdiff_t>(t * 3));
                std::copy_n(c.local_rotations_xyzw.begin() + static_cast<std::ptrdiff_t>(i * J * 4), J * 4,
                            out.rotations_xyzw.begin() + static_cast<std::ptrdiff_t>(t * J * 4));
                if (c.type == constraint_type::fullbody) {
                    out.full_body[t] = 1.F;
                    out.full_body_pose[t] = 1;
                    continue;
                }
                // Upstream masks only its named left-hand ... right-foot
                // sets; an end-effector set naming the same joints pins the
                // same way here.
                if (c.end_effectors & KIMODO_END_EFFECTOR_LEFT_FOOT) out.left_foot[t] = 1.F;
                if (c.end_effectors & KIMODO_END_EFFECTOR_RIGHT_FOOT) out.right_foot[t] = 1.F;
                if (c.end_effectors & KIMODO_END_EFFECTOR_LEFT_HAND) out.left_hand[t] = 1.F;
                if (c.end_effectors & KIMODO_END_EFFECTOR_RIGHT_HAND) out.right_hand[t] = 1.F;
            }
        }
    return out;
}

std::expected<void, std::string> correct_motion(const skeleton_spec &s, std::vector<float> &hips, std::vector<float> &local_xyzw,
                                                std::span<const float> contacts, const postprocess_targets &targets, float root_margin) {
    const size_t J = s.joints(), T = targets.frames();
    if (hips.size() != T * 3 || local_xyzw.size() != T * J * 4 || contacts.size() != T * 4 || targets.rotations_xyzw.size() != T * J * 4)
        return std::unexpected("post-processing inputs disagree on their frame count");
    if (!std::isfinite(root_margin) || root_margin < 0.F) return std::unexpected("root margin must be a non-negative distance");
    constexpr float contact_threshold = .5F;
    const auto contact = [&](size_t f, size_t c) { return contacts[f * 4 + c] > contact_threshold ? 1.F : 0.F; };

    const auto rig = working_rig(s);
    std::vector<int> parents(s.parents.begin(), s.parents.end());
    const auto left_foot = static_cast<int>(s.end_effectors[0]), right_foot = static_cast<int>(s.end_effectors[1]);
    const auto left_hand = static_cast<int>(s.end_effectors[2]), right_hand = static_cast<int>(s.end_effectors[3]);

    // The end effectors a keyframe pins, unless a full-body keyframe already does.
    std::vector<Animation::ContactInfo> pins(4);
    const std::array<std::pair<int, const std::vector<float> *>, 4> pinned{{{left_hand, &targets.left_hand}, {right_hand, &targets.right_hand},
                                                                           {left_foot, &targets.left_foot}, {right_foot, &targets.right_foot}}};
    for (size_t p = 0; p < 4; ++p) {
        pins[p].jointIndex = pinned[p].first;
        pins[p].hintOffset = Math::Vector(0.F, 0.F, p < 2 ? -.1F : .1F);
        for (size_t f = 0; f < T; ++f) pins[p].contactMask.push_back((1.F - targets.full_body[f]) * (*pinned[p].second)[f]);
    }

    // Foot contacts from the model, heel flagged when its toe is; none where
    // a foot is pinned by a keyframe.
    std::vector<Animation::ContactInfo> feet(2);
    feet[0].jointIndex = right_foot;
    feet[0].hintOffset = Math::Vector(0.F, 0.F, .1F);
    feet[0].minHeight = Animation::JointLocalToGlobal(parents, right_foot, rig).GetTranslation().GetY();
    feet[1].jointIndex = left_foot;
    feet[1].hintOffset = Math::Vector(0.F, 0.F, .1F);
    feet[1].minHeight = Animation::JointLocalToGlobal(parents, left_foot, rig).GetTranslation().GetY();
    feet[0].contactMask.resize(T);
    feet[1].contactMask.resize(T);
    for (size_t f = 0; f < T; ++f) {
        const bool right_pinned = targets.right_foot[f] != 0.F, left_pinned = targets.left_foot[f] != 0.F;
        feet[0].contactMask[f] = std::min((right_pinned ? 0.F : contact(f, 3)) + (right_pinned ? 0.F : contact(f, 2)), 1.F);
        feet[1].contactMask[f] = std::min((left_pinned ? 0.F : contact(f, 1)) + (left_pinned ? 0.F : contact(f, 0)), 1.F);
    }
    int left_toe = -1, right_toe = -1;
    for (size_t j = 0; j < J; ++j) {
        if (parents[j] == left_foot) left_toe = static_cast<int>(j);
        if (parents[j] == right_foot) right_toe = static_cast<int>(j);
    }
    if (left_toe != -1 && right_toe != -1) {
        // Upstream gives both toes the right toe's rest height.
        const float toe_height = Animation::JointLocalToGlobal(parents, right_toe, rig).GetTranslation().GetY();
        feet.resize(4);
        feet[2].jointIndex = right_toe;
        feet[3].jointIndex = left_toe;
        for (size_t i = 2; i < 4; ++i) {
            feet[i].contactType = Animation::kOneBone;
            feet[i].minHeight = toe_height;
            feet[i].contactMask.resize(T);
        }
        for (size_t f = 0; f < T; ++f) {
            feet[2].contactMask[f] = targets.right_foot[f] != 0.F ? 0.F : contact(f, 3);
            feet[3].contactMask[f] = targets.left_foot[f] != 0.F ? 0.F : contact(f, 1);
        }
    }

    auto corrected = poses(rig, hips, local_xyzw, T);
    const auto goal = poses(rig, targets.hips, targets.rotations_xyzw, T);
    Animation::CorrectMotion(corrected, goal, targets.full_body, targets.root, feet, pins, parents, rig,
                             contact_threshold, root_margin, s.key == g1skel34_spec.key);

    for (size_t f = 0; f < T; ++f) {
        const auto &t = corrected[f][0].GetTranslation();
        hips[f * 3] = t.GetX(); hips[f * 3 + 1] = t.GetY(); hips[f * 3 + 2] = t.GetZ();
        for (size_t j = 0; j < J; ++j) {
            const auto &q = corrected[f][j].GetRotation();
            std::copy_n(reinterpret_cast<const float *>(&q), 4, local_xyzw.begin() + static_cast<std::ptrdiff_t>((f * J + j) * 4)); // x y z w
        }
    }
    return {};
}

} // namespace kimodo::detail
