package main

import (
	"encoding/base64"
	"encoding/binary"
	"fmt"
	"math"
)

// Kinematic constraints on POST /api/generate, in NVIDIA Kimodo's
// constraints JSON (upstream docs/source/user_guide/constraints.md), so a
// file saved from the upstream demo's "Save Constraints" posts unchanged:
//
//	root2d        smooth_root_2d [T][x, z], optional global_root_heading [T][cos, sin]
//	fullbody      root_positions [T][x, y, z] + local joint rotations
//	left-hand, right-hand, left-foot, right-foot
//	end-effector  the fullbody fields + joint_names from LeftHand, RightHand,
//	              LeftFoot, RightFoot, Hips
//
// Pose rotations are upstream's axis-angle local_joints_rot [T][J][3], or
// local_joints_rot_xyzw [T][J][4], the quaternion layout this server's
// rotations.f32 and GLB carry.  J is the model's joint count; the SOMA models
// also take upstream's 77-joint files.  Frames index the whole generated clip
// and positions are metres in the model's canonical space: Y up, the root at
// XZ (0, 0) on frame 0 facing +Z (or first_heading radians).
type motionConstraint struct {
	Type               string        `json:"type"`
	FrameIndices       []int         `json:"frame_indices"`
	SmoothRoot2D       [][]float64   `json:"smooth_root_2d,omitempty"`
	GlobalRootHeading  [][]float64   `json:"global_root_heading,omitempty"`
	RootPositions      [][]float64   `json:"root_positions,omitempty"`
	LocalJointsRot     [][][]float64 `json:"local_joints_rot,omitempty"`
	LocalJointsRotXYZW [][][]float64 `json:"local_joints_rot_xyzw,omitempty"`
	JointNames         []string      `json:"joint_names,omitempty"`
}

const maxConstraints = 256

// The native worker's constraint types and end-effector bits
// (include/kimodo/kimodo_capi.h).
const (
	constraintRoot2D      = 1
	constraintFullBody    = 2
	constraintEndEffector = 3
)

var endEffectorBits = map[string]uint32{"LeftFoot": 1, "RightFoot": 2, "LeftHand": 4, "RightHand": 8, "Hips": 16}
var endEffectorTypes = map[string]string{"left-hand": "LeftHand", "right-hand": "RightHand", "left-foot": "LeftFoot", "right-foot": "RightFoot"}

// Upstream SOMASkeleton30.from_SOMASkeleton77: the 30 joints' indices in the
// 77-joint skeleton (kimodo/skeleton/definitions.py bone orders).
var soma77To30 = [...]int{0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 18, 28, 39, 40, 41, 42, 46, 56, 67, 68, 69, 70, 72, 73, 74, 75}

type nativeConstraint struct {
	kind         uint32
	endEffectors uint32
	frames       []uint32
	roots        []float32 // [F*3]
	rotations    []float32 // [F*joints*4] XYZW
	smooth       []float32 // [F*2]
	heading      []float32 // [F*2]
}

// Checks the constraints against the model's skeleton and a clip of
// totalFrames frames and converts them for the native worker.
func parseConstraints(list []motionConstraint, skeletonKey string, joints, totalFrames int) ([]nativeConstraint, error) {
	if len(list) > maxConstraints {
		return nil, fmt.Errorf("at most %d constraints", maxConstraints)
	}
	result := make([]nativeConstraint, 0, len(list))
	for index, c := range list {
		native, err := parseConstraint(c, skeletonKey, joints, totalFrames)
		if err != nil {
			return nil, fmt.Errorf("constraints[%d] (%s): %w", index, c.Type, err)
		}
		result = append(result, native)
	}
	return result, nil
}

func parseConstraint(c motionConstraint, skeletonKey string, joints, totalFrames int) (nativeConstraint, error) {
	var native nativeConstraint
	count := len(c.FrameIndices)
	if count == 0 {
		return native, fmt.Errorf("frame_indices is empty")
	}
	for _, frame := range c.FrameIndices {
		if frame < 0 || frame >= totalFrames {
			return native, fmt.Errorf("frame %d is outside the %d-frame clip", frame, totalFrames)
		}
		native.frames = append(native.frames, uint32(frame))
	}
	var err error
	if c.SmoothRoot2D != nil {
		if native.smooth, err = flatten(c.SmoothRoot2D, count, 2, "smooth_root_2d"); err != nil {
			return native, err
		}
	}
	switch c.Type {
	case "root2d":
		native.kind = constraintRoot2D
		if native.smooth == nil {
			return native, fmt.Errorf("smooth_root_2d is required")
		}
		if c.RootPositions != nil || c.LocalJointsRot != nil || c.LocalJointsRotXYZW != nil || c.JointNames != nil {
			return native, fmt.Errorf("takes only smooth_root_2d and global_root_heading")
		}
		if c.GlobalRootHeading != nil {
			if native.heading, err = flatten(c.GlobalRootHeading, count, 2, "global_root_heading"); err != nil {
				return native, err
			}
		}
		return native, nil
	case "fullbody":
		native.kind = constraintFullBody
		if c.JointNames != nil {
			return native, fmt.Errorf("joint_names applies to end-effector constraints")
		}
	case "end-effector":
		native.kind = constraintEndEffector
		if len(c.JointNames) == 0 {
			return native, fmt.Errorf("joint_names is required (LeftHand, RightHand, LeftFoot, RightFoot, Hips)")
		}
		for _, name := range c.JointNames {
			bit, ok := endEffectorBits[name]
			if !ok {
				return native, fmt.Errorf("unknown end effector %q (LeftHand, RightHand, LeftFoot, RightFoot, Hips)", name)
			}
			native.endEffectors |= bit
		}
	default:
		name, ok := endEffectorTypes[c.Type]
		if !ok {
			return native, fmt.Errorf("unknown constraint type (root2d, fullbody, end-effector, left-hand, right-hand, left-foot, right-foot)")
		}
		if c.JointNames != nil {
			return native, fmt.Errorf("joint_names applies to end-effector constraints")
		}
		native.kind = constraintEndEffector
		native.endEffectors = endEffectorBits[name]
	}
	if c.GlobalRootHeading != nil {
		return native, fmt.Errorf("global_root_heading applies to root2d; a pose's heading comes from its hips")
	}
	if native.roots, err = flatten(c.RootPositions, count, 3, "root_positions"); err != nil {
		return native, err
	}
	native.rotations, err = poseRotations(c, count, skeletonKey, joints)
	return native, err
}

// Rows of `width` finite values, one per frame index, flattened.
func flatten(rows [][]float64, count, width int, name string) ([]float32, error) {
	if len(rows) != count {
		return nil, fmt.Errorf("%s needs one row per frame index (%d), got %d", name, count, len(rows))
	}
	values := make([]float32, 0, count*width)
	for _, row := range rows {
		if len(row) != width {
			return nil, fmt.Errorf("%s rows must hold %d values", name, width)
		}
		for _, v := range row {
			if math.IsNaN(v) || math.IsInf(v, 0) {
				return nil, fmt.Errorf("%s values must be finite", name)
			}
			values = append(values, float32(v))
		}
	}
	return values, nil
}

// The pose's local rotations as XYZW quaternions in the model's joint order.
func poseRotations(c motionConstraint, count int, skeletonKey string, joints int) ([]float32, error) {
	axisAngle := c.LocalJointsRot != nil
	if axisAngle == (c.LocalJointsRotXYZW != nil) {
		return nil, fmt.Errorf("a pose needs exactly one of local_joints_rot (axis-angle) or local_joints_rot_xyzw")
	}
	frames, name, width := c.LocalJointsRotXYZW, "local_joints_rot_xyzw", 4
	if axisAngle {
		frames, name, width = c.LocalJointsRot, "local_joints_rot", 3
	}
	if len(frames) != count {
		return nil, fmt.Errorf("%s needs one pose per frame index (%d), got %d", name, count, len(frames))
	}
	rotations := make([]float32, 0, count*joints*4)
	for _, pose := range frames {
		var selected []int
		switch {
		case len(pose) == joints:
		case skeletonKey == "soma30" && len(pose) == 77:
			selected = soma77To30[:]
		default:
			accepted := fmt.Sprint(joints)
			if skeletonKey == "soma30" {
				accepted += " or 77"
			}
			return nil, fmt.Errorf("%s poses must have %s joints, got %d", name, accepted, len(pose))
		}
		for joint := 0; joint < joints; joint++ {
			source := joint
			if selected != nil {
				source = selected[joint]
			}
			value := pose[source]
			if len(value) != width {
				return nil, fmt.Errorf("%s joints must hold %d values", name, width)
			}
			q := [4]float64{}
			if axisAngle {
				q = axisAngleQuaternion(value[0], value[1], value[2])
			} else {
				copy(q[:], value)
			}
			norm := math.Sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3])
			if math.IsNaN(norm) || math.IsInf(norm, 0) || norm < 1e-6 {
				return nil, fmt.Errorf("%s holds a zero or non-finite rotation", name)
			}
			for _, v := range q {
				rotations = append(rotations, float32(v/norm))
			}
		}
	}
	return rotations, nil
}

func axisAngleQuaternion(x, y, z float64) [4]float64 {
	angle := math.Sqrt(x*x + y*y + z*z)
	if angle < 1e-12 {
		return [4]float64{0, 0, 0, 1}
	}
	s := math.Sin(angle/2) / angle
	return [4]float64{x * s, y * s, z * s, math.Cos(angle / 2)}
}

// The worker's constraint field: "-" for none, else the base64 of the block
// src/generate.cpp decode_constraints reads.
func encodeConstraints(joints int, list []nativeConstraint) string {
	if len(list) == 0 {
		return "-"
	}
	var b []byte
	u32 := func(v uint32) { b = binary.LittleEndian.AppendUint32(b, v) }
	f32 := func(values []float32) {
		for _, v := range values {
			u32(math.Float32bits(v))
		}
	}
	u32(uint32(joints))
	u32(uint32(len(list)))
	for _, c := range list {
		flags := uint32(0)
		if c.rotations != nil {
			flags |= 1
		}
		if c.smooth != nil {
			flags |= 2
		}
		if c.heading != nil {
			flags |= 4
		}
		u32(c.kind)
		u32(c.endEffectors)
		u32(uint32(len(c.frames)))
		u32(flags)
		for _, frame := range c.frames {
			u32(frame)
		}
		if c.rotations != nil {
			f32(c.roots)
			f32(c.rotations)
		}
		f32(c.smooth)
		f32(c.heading)
	}
	return base64.StdEncoding.EncodeToString(b)
}
