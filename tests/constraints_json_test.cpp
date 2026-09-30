// NVIDIA Kimodo's constraints JSON in the library (src/constraints_json.cpp),
// held to the demo server's own cases (demo/constraints_test.go) so the two
// implementations accept and reject the same files.  Release builds drop
// assert, so failures are counted and reported.
#include "constraints.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>
#include <vector>

namespace {
using json = nlohmann::json;
using kimodo::detail::constraints_from_json;
int failures = 0;

void check(bool ok, const std::string &what) {
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++failures;
}

json identity_pose(int joints, int width) {
    json pose = json::array();
    for (int joint = 0; joint < joints; ++joint) {
        json value = json::array();
        for (int i = 0; i < width; ++i) value.push_back(width == 4 && i == 3 ? 1.0 : 0.0);
        pose.push_back(value);
    }
    return pose;
}

const auto &soma = kimodo::detail::soma30_spec;
const auto &g1 = kimodo::detail::g1skel34_spec;
constexpr int soma77_left_hand = 14; // SOMASkeleton30 joint 13 in the 77-joint order

// A file in upstream's format, as its demo saves it: SOMA constraints on the
// 77-joint skeleton with axis-angle rotations.
void upstream_file() {
    json pose = identity_pose(77, 3);
    pose[soma77_left_hand] = {0.0, std::numbers::pi / 2, 0.0}; // LeftHand, a quarter turn about +Y
    const json list = {
        {{"type", "root2d"}, {"frame_indices", {0, 30}}, {"smooth_root_2d", {{0, 0}, {.5, 1}}}, {"global_root_heading", {{1, 0}, {0, 1}}}},
        {{"type", "fullbody"}, {"frame_indices", {45}}, {"root_positions", {{0, .95, 1.2}}}, {"local_joints_rot", {pose}}, {"smooth_root_2d", {{0, 1.2}}}},
        {{"type", "left-hand"}, {"frame_indices", {60}}, {"root_positions", {{0, .95, 1.5}}}, {"local_joints_rot", {pose}}},
        {{"type", "end-effector"}, {"frame_indices", {70}}, {"root_positions", {{0, .95, 1.5}}}, {"local_joints_rot", {pose}}, {"joint_names", {"RightFoot", "Hips"}}},
        {{"type", "root2d"}, {"frame_indices", {1}}, {"smooth_root_2d", {{0, 0}}}, {"an_upstream_field_we_ignore", 7}},
    };
    auto parsed = constraints_from_json(list.dump(), soma, 150, 256);
    if (!parsed) { check(false, "upstream file: " + parsed.error()); return; }
    const auto &c = *parsed;
    check(c.size() == 5, "five constraints");
    check(c[0].type == kimodo::constraint_type::root2d && c[0].smooth_root_2d.size() == 4 && c[0].root_heading.size() == 4 &&
              c[0].local_rotations_xyzw.empty(), "root2d fields");
    check(c[1].type == kimodo::constraint_type::fullbody && c[1].local_rotations_xyzw.size() == 30 * 4 &&
              c[1].root_positions.size() == 3 && c[1].smooth_root_2d.size() == 2, "fullbody shapes");
    const float *hand = c[1].local_rotations_xyzw.data() + 13 * 4;
    const float half = std::sqrt(.5F);
    check(std::abs(hand[1] - half) < 1e-6F && std::abs(hand[3] - half) < 1e-6F && hand[0] == 0 && hand[2] == 0,
          "LeftHand is a quarter turn about +Y");
    check(c[1].local_rotations_xyzw[1 * 4 + 3] == 1, "an identity joint stays the identity");
    check(c[2].type == kimodo::constraint_type::end_effector && c[2].end_effectors == KIMODO_END_EFFECTOR_LEFT_HAND &&
              c[2].smooth_root_2d.empty(), "left-hand is the left-hand end effector");
    check(c[3].end_effectors == (KIMODO_END_EFFECTOR_RIGHT_FOOT | KIMODO_END_EFFECTOR_HIPS), "end-effector bits");

    // The struct path, axis-angle on 77 joints, gives the same rotations.
    kimodo::motion_constraint raw;
    raw.type = kimodo::constraint_type::fullbody;
    raw.frames = {45};
    raw.root_positions = {0, .95F, 1.2F};
    raw.pose_joints = 77;
    for (const auto &joint : pose)
        for (const auto &value : joint) raw.local_rotations_axis_angle.push_back(value.get<float>());
    auto canonical = kimodo::detail::canonical_constraints(soma, std::span(&raw, 1));
    if (!canonical) { check(false, "struct path: " + canonical.error()); return; }
    check((*canonical)[0].local_rotations_xyzw == c[1].local_rotations_xyzw && (*canonical)[0].local_rotations_axis_angle.empty(),
          "the struct path converts a 77-joint axis-angle pose as the JSON path does");
}

void xyzw() {
    json pose = identity_pose(34, 4);
    pose[5] = {0, 0, 2, 0}; // unnormalized: 180 degrees about Z
    const json list = {{{"type", "fullbody"}, {"frame_indices", {10}}, {"root_positions", {{0, .8, 0}}}, {"local_joints_rot_xyzw", {pose}}}};
    auto parsed = constraints_from_json(list.dump(), g1, 60, 256);
    if (!parsed) { check(false, "xyzw: " + parsed.error()); return; }
    const float *q = (*parsed)[0].local_rotations_xyzw.data() + 5 * 4;
    check(q[2] == 1 && q[3] == 0, "quaternions are normalized");
}

void rejects() {
    const json pose = json::array({identity_pose(30, 4)});
    const json root = {{0, .9, 0}};
    json zero_joint = identity_pose(30, 4);
    zero_joint[7] = {0, 0, 0, 0};
    const std::vector<std::pair<std::string, json>> cases = {
        {"frame 150 is outside", {{"type", "root2d"}, {"frame_indices", {150}}, {"smooth_root_2d", {{0, 0}}}}},
        {"frame_indices is empty", {{"type", "root2d"}, {"smooth_root_2d", json::array()}}},
        {"smooth_root_2d rows must hold", {{"type", "root2d"}, {"frame_indices", {1}}, {"smooth_root_2d", {{0}}}}},
        {"smooth_root_2d is required", {{"type", "root2d"}, {"frame_indices", {1}}}},
        {"takes only smooth_root_2d", {{"type", "root2d"}, {"frame_indices", {1}}, {"smooth_root_2d", {{0, 0}}}, {"root_positions", root}}},
        {"one row per frame index", {{"type", "fullbody"}, {"frame_indices", {1, 2}}, {"root_positions", root}, {"local_joints_rot_xyzw", pose}}},
        {"exactly one of", {{"type", "fullbody"}, {"frame_indices", {1}}, {"root_positions", root}, {"local_joints_rot_xyzw", pose},
                            {"local_joints_rot", json::array({identity_pose(30, 3)})}}},
        {"must have 30 or 77 joints", {{"type", "fullbody"}, {"frame_indices", {1}}, {"root_positions", root},
                                       {"local_joints_rot_xyzw", json::array({identity_pose(22, 4)})}}},
        {"zero or non-finite rotation", {{"type", "fullbody"}, {"frame_indices", {1}}, {"root_positions", root},
                                         {"local_joints_rot_xyzw", json::array({zero_joint})}}},
        {"joints must hold 4 values", {{"type", "fullbody"}, {"frame_indices", {1}}, {"root_positions", root},
                                       {"local_joints_rot_xyzw", json::array({identity_pose(30, 3)})}}},
        {"unknown constraint type", {{"type", "keyframe"}, {"frame_indices", {1}}}},
        {"unknown end effector", {{"type", "end-effector"}, {"frame_indices", {1}}, {"root_positions", root},
                                  {"local_joints_rot_xyzw", pose}, {"joint_names", {"Head"}}}},
        {"joint_names is required", {{"type", "end-effector"}, {"frame_indices", {1}}, {"root_positions", root}, {"local_joints_rot_xyzw", pose}}},
        {"joint_names applies", {{"type", "left-foot"}, {"frame_indices", {1}}, {"root_positions", root},
                                 {"local_joints_rot_xyzw", pose}, {"joint_names", {"LeftFoot"}}}},
        {"heading comes from its hips", {{"type", "fullbody"}, {"frame_indices", {1}}, {"root_positions", root},
                                         {"local_joints_rot_xyzw", pose}, {"global_root_heading", {{1, 0}}}}},
    };
    for (const auto &[want, c] : cases) {
        auto parsed = constraints_from_json(json::array({c}).dump(), soma, 150, 256);
        check(!parsed && parsed.error().find(want) != std::string::npos,
              "\"" + want + "\": got " + (parsed ? std::string("success") : parsed.error()));
    }
    // Beyond the server's cases: the file itself, and the count.
    auto bad = constraints_from_json("{\"type\": \"root2d\"}", soma, 150, 256);
    check(!bad && bad.error().find("must be a list") != std::string::npos, "a non-list file is rejected");
    bad = constraints_from_json("[{\"type\": ", soma, 150, 256);
    check(!bad && bad.error().find("invalid constraints JSON") != std::string::npos, "malformed JSON is rejected");
    json many = json::array();
    for (int i = 0; i < 3; ++i) many.push_back({{"type", "root2d"}, {"frame_indices", {i}}, {"smooth_root_2d", {{0, 0}}}});
    bad = constraints_from_json(many.dump(), soma, 150, 2);
    check(!bad && bad.error().find("at most 2 constraints") != std::string::npos, "the constraint count is limited");
    bad = constraints_from_json(json::array({{{"type", "root2d"}, {"frame_indices", {1.5}}, {"smooth_root_2d", {{0, 0}}}}}).dump(), soma, 150, 256);
    check(!bad && bad.error().find("whole frame numbers") != std::string::npos, "fractional frames are rejected");
    // Errors name the constraint set, as the server's do.
    bad = constraints_from_json(json::array({{{"type", "root2d"}, {"frame_indices", {0}}, {"smooth_root_2d", {{0, 0}}}},
                                             {{"type", "keyframe"}, {"frame_indices", {1}}}}).dump(), soma, 150, 256);
    check(!bad && bad.error().starts_with("constraints[1] (keyframe): "), "errors name the constraint set");
}
} // namespace

int main() {
    upstream_file();
    xyzw();
    rejects();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("constraints JSON: all checks passed\n");
    return 0;
}
