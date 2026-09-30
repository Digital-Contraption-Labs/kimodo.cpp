// NVIDIA Kimodo's constraints JSON (upstream docs/source/user_guide/
// constraints.md), as its demo's "Save Constraints" writes it: a list of
// constraint sets.
//
//   root2d        smooth_root_2d [T][x, z], optional global_root_heading [T][cos, sin]
//   fullbody      root_positions [T][x, y, z] + local joint rotations
//   left-hand, right-hand, left-foot, right-foot
//   end-effector  the fullbody fields + joint_names from LeftHand, RightHand,
//                 LeftFoot, RightFoot, Hips
//
// Pose rotations are axis-angle local_joints_rot [T][J][3] or
// local_joints_rot_xyzw [T][J][4]; J is the model's joint count, and the SOMA
// model also takes upstream's 77-joint poses.  A port of the demo server's
// parseConstraints (demo/constraints.go), checks and messages included, so a
// file behaves the same through the server and through the library; unknown
// fields are ignored, as Go's decoder ignores them.
#include "constraints.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <utility>

namespace kimodo::detail {
namespace {
using json = nlohmann::json;
using result = std::expected<void, std::string>;

constexpr std::array<std::pair<std::string_view, std::uint32_t>, 5> end_effector_bits{{
    {"LeftFoot", KIMODO_END_EFFECTOR_LEFT_FOOT}, {"RightFoot", KIMODO_END_EFFECTOR_RIGHT_FOOT},
    {"LeftHand", KIMODO_END_EFFECTOR_LEFT_HAND}, {"RightHand", KIMODO_END_EFFECTOR_RIGHT_HAND},
    {"Hips", KIMODO_END_EFFECTOR_HIPS}}};
constexpr std::array<std::pair<std::string_view, std::uint32_t>, 4> limb_types{{
    {"left-hand", KIMODO_END_EFFECTOR_LEFT_HAND}, {"right-hand", KIMODO_END_EFFECTOR_RIGHT_HAND},
    {"left-foot", KIMODO_END_EFFECTOR_LEFT_FOOT}, {"right-foot", KIMODO_END_EFFECTOR_RIGHT_FOOT}}};

// A field that is there and not null, as a Go slice is non-nil.
const json *field(const json &object, const char *name) {
    const auto found = object.find(name);
    return found == object.end() || found->is_null() ? nullptr : &*found;
}

// Rows of `width` finite values, one per frame index, flattened (Go's flatten).
std::expected<std::vector<float>, std::string> flatten(const json *rows, size_t count, size_t width, const char *name) {
    const size_t found = rows && rows->is_array() ? rows->size() : 0;
    if (rows && !rows->is_array()) return std::unexpected(std::string(name) + " must be a list of rows");
    if (found != count)
        return std::unexpected(std::string(name) + " needs one row per frame index (" + std::to_string(count) + "), got " + std::to_string(found));
    std::vector<float> values;
    values.reserve(count * width);
    for (size_t i = 0; i < found; ++i) {
        const json &row = (*rows)[i];
        if (!row.is_array() || row.size() != width)
            return std::unexpected(std::string(name) + " rows must hold " + std::to_string(width) + " values");
        for (const json &value : row) {
            if (!value.is_number()) return std::unexpected(std::string(name) + " values must be numbers");
            const auto v = static_cast<float>(value.get<double>());
            if (!std::isfinite(v)) return std::unexpected(std::string(name) + " values must be finite");
            values.push_back(v);
        }
    }
    return values;
}

// The pose's local rotations as XYZW quaternions in the model's joint order
// (Go's poseRotations).
std::expected<std::vector<float>, std::string> pose_rotations(const json &c, size_t count, const skeleton_spec &skeleton) {
    const json *axis_angle = field(c, "local_joints_rot"), *xyzw = field(c, "local_joints_rot_xyzw");
    if ((axis_angle != nullptr) == (xyzw != nullptr))
        return std::unexpected("a pose needs exactly one of local_joints_rot (axis-angle) or local_joints_rot_xyzw");
    const json &frames = axis_angle ? *axis_angle : *xyzw;
    const char *name = axis_angle ? "local_joints_rot" : "local_joints_rot_xyzw";
    const size_t width = axis_angle ? 3 : 4;
    const size_t found = frames.is_array() ? frames.size() : 0;
    if (found != count)
        return std::unexpected(std::string(name) + " needs one pose per frame index (" + std::to_string(count) + "), got " + std::to_string(found));
    std::vector<float> rotations;
    rotations.reserve(count * skeleton.joints() * 4);
    std::vector<double> values;
    for (const json &pose : frames) {
        if (!pose.is_array()) return std::unexpected(std::string(name) + " poses must be lists of joints");
        const size_t joints = pose.size();
        // Checked here, before the joints' values, in the order Go checks.
        if (joints != skeleton.joints() && !(skeleton.key == "soma30" && joints == 77))
            return std::unexpected(std::string(name) + " poses must have " + std::to_string(skeleton.joints()) +
                                   (skeleton.key == "soma30" ? " or 77" : "") + " joints, got " + std::to_string(joints));
        values.clear();
        for (const json &joint : pose) {
            if (!joint.is_array() || joint.size() != width)
                return std::unexpected(std::string(name) + " joints must hold " + std::to_string(width) + " values");
            for (const json &value : joint) {
                if (!value.is_number()) return std::unexpected(std::string(name) + " values must be numbers");
                values.push_back(value.get<double>());
            }
        }
        auto converted = pose_rotations_xyzw(skeleton, std::span<const double>(values), 1, joints, static_cast<int>(width), name);
        if (!converted) return std::unexpected(converted.error());
        rotations.insert(rotations.end(), converted->begin(), converted->end());
    }
    return rotations;
}

std::expected<motion_constraint, std::string> parse_constraint(const json &c, const skeleton_spec &skeleton, size_t total_frames) {
    motion_constraint native;
    if (!c.is_object()) return std::unexpected("a constraint set must be an object");
    const json *indices = field(c, "frame_indices");
    if (indices && !indices->is_array()) return std::unexpected("frame_indices must be a list of frames");
    const size_t count = indices ? indices->size() : 0;
    if (count == 0) return std::unexpected("frame_indices is empty");
    for (const json &frame : *indices) {
        if (!frame.is_number_integer()) return std::unexpected("frame_indices must hold whole frame numbers");
        const auto value = frame.get<std::int64_t>();
        if (value < 0 || static_cast<std::uint64_t>(value) >= total_frames)
            return std::unexpected("frame " + std::to_string(value) + " is outside the " + std::to_string(total_frames) + "-frame clip");
        native.frames.push_back(static_cast<unsigned>(value));
    }
    if (const json *smooth = field(c, "smooth_root_2d")) {
        auto values = flatten(smooth, count, 2, "smooth_root_2d");
        if (!values) return std::unexpected(values.error());
        native.smooth_root_2d = std::move(*values);
    }
    const json *type_field = field(c, "type");
    const std::string type = type_field && type_field->is_string() ? type_field->get<std::string>() : std::string();
    const json *names = field(c, "joint_names");
    const json *heading = field(c, "global_root_heading");
    if (type == "root2d") {
        native.type = constraint_type::root2d;
        if (native.smooth_root_2d.empty() && !field(c, "smooth_root_2d")) return std::unexpected("smooth_root_2d is required");
        if (field(c, "root_positions") || field(c, "local_joints_rot") || field(c, "local_joints_rot_xyzw") || names)
            return std::unexpected("takes only smooth_root_2d and global_root_heading");
        if (heading) {
            auto values = flatten(heading, count, 2, "global_root_heading");
            if (!values) return std::unexpected(values.error());
            native.root_heading = std::move(*values);
        }
        return native;
    }
    if (type == "fullbody") {
        native.type = constraint_type::fullbody;
        if (names) return std::unexpected("joint_names applies to end-effector constraints");
    } else if (type == "end-effector") {
        native.type = constraint_type::end_effector;
        if (!names || !names->is_array() || names->empty())
            return std::unexpected("joint_names is required (LeftHand, RightHand, LeftFoot, RightFoot, Hips)");
        for (const json &name : *names) {
            const std::string text = name.is_string() ? name.get<std::string>() : name.dump();
            std::uint32_t bit = 0;
            for (const auto &[known, value] : end_effector_bits)
                if (text == known) bit = value;
            if (!bit) return std::unexpected("unknown end effector \"" + text + "\" (LeftHand, RightHand, LeftFoot, RightFoot, Hips)");
            native.end_effectors |= bit;
        }
    } else {
        std::uint32_t bit = 0;
        for (const auto &[known, value] : limb_types)
            if (type == known) bit = value;
        if (!bit)
            return std::unexpected("unknown constraint type (root2d, fullbody, end-effector, left-hand, right-hand, left-foot, right-foot)");
        if (names) return std::unexpected("joint_names applies to end-effector constraints");
        native.type = constraint_type::end_effector;
        native.end_effectors = bit;
    }
    if (heading) return std::unexpected("global_root_heading applies to root2d; a pose's heading comes from its hips");
    auto roots = flatten(field(c, "root_positions"), count, 3, "root_positions");
    if (!roots) return std::unexpected(roots.error());
    native.root_positions = std::move(*roots);
    auto rotations = pose_rotations(c, count, skeleton);
    if (!rotations) return std::unexpected(rotations.error());
    native.local_rotations_xyzw = std::move(*rotations);
    return native;
}
} // namespace

std::expected<std::vector<motion_constraint>, std::string> constraints_from_json(
    std::string_view text, const skeleton_spec &skeleton, std::size_t total_frames, std::size_t max_constraints) {
    json list;
    try {
        list = json::parse(text.begin(), text.end());
    } catch (const json::exception &error) {
        return std::unexpected(std::string("invalid constraints JSON: ") + error.what());
    }
    if (!list.is_array()) return std::unexpected("constraints JSON must be a list of constraint sets");
    if (list.size() > max_constraints) return std::unexpected("at most " + std::to_string(max_constraints) + " constraints");
    std::vector<motion_constraint> result;
    result.reserve(list.size());
    for (size_t index = 0; index < list.size(); ++index) {
        const json &c = list[index];
        auto parsed = parse_constraint(c, skeleton, total_frames);
        if (!parsed) {
            const json *type = c.is_object() ? field(c, "type") : nullptr;
            const std::string name = type && type->is_string() ? type->get<std::string>() : std::string();
            return std::unexpected("constraints[" + std::to_string(index) + "] (" + name + "): " + parsed.error());
        }
        result.push_back(std::move(*parsed));
    }
    return result;
}

} // namespace kimodo::detail
