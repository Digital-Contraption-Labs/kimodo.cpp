package main

import (
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"math"
	"strings"
	"testing"
)

func identityPose(joints, width int) [][]float64 {
	pose := make([][]float64, joints)
	for joint := range pose {
		pose[joint] = make([]float64, width)
		if width == 4 {
			pose[joint][3] = 1
		}
	}
	return pose
}

// A file in upstream's format, as its demo saves it: SOMA constraints on the
// 77-joint skeleton with axis-angle rotations.
func TestParseUpstreamConstraints(t *testing.T) {
	pose := identityPose(77, 3)
	pose[soma77To30[13]] = []float64{0, math.Pi / 2, 0} // LeftHand, a quarter turn about +Y
	body, err := json.Marshal([]map[string]any{
		{"type": "root2d", "frame_indices": []int{0, 30}, "smooth_root_2d": [][]float64{{0, 0}, {.5, 1}}, "global_root_heading": [][]float64{{1, 0}, {0, 1}}},
		{"type": "fullbody", "frame_indices": []int{45}, "root_positions": [][]float64{{0, .95, 1.2}}, "local_joints_rot": [][][]float64{pose}, "smooth_root_2d": [][]float64{{0, 1.2}}},
		{"type": "left-hand", "frame_indices": []int{60}, "root_positions": [][]float64{{0, .95, 1.5}}, "local_joints_rot": [][][]float64{pose}},
		{"type": "end-effector", "frame_indices": []int{70}, "root_positions": [][]float64{{0, .95, 1.5}}, "local_joints_rot": [][][]float64{pose}, "joint_names": []string{"RightFoot", "Hips"}},
	})
	if err != nil {
		t.Fatal(err)
	}
	var list []motionConstraint
	if err := json.Unmarshal(body, &list); err != nil {
		t.Fatal(err)
	}
	native, err := parseConstraints(list, "soma30", 30, 150)
	if err != nil {
		t.Fatal(err)
	}
	if native[0].kind != constraintRoot2D || len(native[0].smooth) != 4 || len(native[0].heading) != 4 || native[0].rotations != nil {
		t.Fatalf("root2d: %+v", native[0])
	}
	full := native[1]
	if full.kind != constraintFullBody || len(full.rotations) != 30*4 || len(full.roots) != 3 || len(full.smooth) != 2 {
		t.Fatalf("fullbody shapes: %d rotations, %d roots, %d smooth", len(full.rotations), len(full.roots), len(full.smooth))
	}
	hand := full.rotations[13*4 : 14*4]
	if want := float32(math.Sqrt(.5)); math.Abs(float64(hand[1]-want)) > 1e-6 || math.Abs(float64(hand[3]-want)) > 1e-6 || hand[0] != 0 || hand[2] != 0 {
		t.Fatalf("LeftHand quaternion %v, want a quarter turn about +Y", hand)
	}
	if spine := full.rotations[1*4 : 2*4]; spine[3] != 1 {
		t.Fatalf("identity joint %v", spine)
	}
	if native[2].kind != constraintEndEffector || native[2].endEffectors != 4 || native[2].smooth != nil {
		t.Fatalf("left-hand: %+v", native[2])
	}
	if native[3].endEffectors != 2|16 {
		t.Fatalf("end-effector bits %d", native[3].endEffectors)
	}
}

func TestParseConstraintsXYZW(t *testing.T) {
	pose := identityPose(34, 4)
	pose[5] = []float64{0, 0, 2, 0} // unnormalized: 180 degrees about Z
	list := []motionConstraint{{Type: "fullbody", FrameIndices: []int{10}, RootPositions: [][]float64{{0, .8, 0}}, LocalJointsRotXYZW: [][][]float64{pose}}}
	native, err := parseConstraints(list, "g1skel34", 34, 60)
	if err != nil {
		t.Fatal(err)
	}
	if got := native[0].rotations[5*4 : 6*4]; got[2] != 1 || got[3] != 0 {
		t.Fatalf("quaternions are normalized, got %v", got)
	}
}

func TestParseConstraintsRejects(t *testing.T) {
	pose := [][][]float64{identityPose(30, 4)}
	root := [][]float64{{0, .9, 0}}
	zeroJoint := identityPose(30, 4)
	zeroJoint[7] = []float64{0, 0, 0, 0}
	cases := map[string]motionConstraint{
		"frame 150 is outside":          {Type: "root2d", FrameIndices: []int{150}, SmoothRoot2D: [][]float64{{0, 0}}},
		"frame_indices is empty":        {Type: "root2d", SmoothRoot2D: [][]float64{}},
		"smooth_root_2d rows must hold": {Type: "root2d", FrameIndices: []int{1}, SmoothRoot2D: [][]float64{{0}}},
		"smooth_root_2d is required":    {Type: "root2d", FrameIndices: []int{1}},
		"takes only smooth_root_2d":     {Type: "root2d", FrameIndices: []int{1}, SmoothRoot2D: [][]float64{{0, 0}}, RootPositions: root},
		"one row per frame index":       {Type: "fullbody", FrameIndices: []int{1, 2}, RootPositions: root, LocalJointsRotXYZW: pose},
		"exactly one of":                {Type: "fullbody", FrameIndices: []int{1}, RootPositions: root, LocalJointsRotXYZW: pose, LocalJointsRot: [][][]float64{identityPose(30, 3)}},
		"must have 30 or 77 joints":     {Type: "fullbody", FrameIndices: []int{1}, RootPositions: root, LocalJointsRotXYZW: [][][]float64{identityPose(22, 4)}},
		"zero or non-finite rotation":   {Type: "fullbody", FrameIndices: []int{1}, RootPositions: root, LocalJointsRotXYZW: [][][]float64{zeroJoint}},
		"joints must hold 4 values":     {Type: "fullbody", FrameIndices: []int{1}, RootPositions: root, LocalJointsRotXYZW: [][][]float64{identityPose(30, 3)}},
		"unknown constraint type":       {Type: "keyframe", FrameIndices: []int{1}},
		"unknown end effector":          {Type: "end-effector", FrameIndices: []int{1}, RootPositions: root, LocalJointsRotXYZW: pose, JointNames: []string{"Head"}},
		"joint_names is required":       {Type: "end-effector", FrameIndices: []int{1}, RootPositions: root, LocalJointsRotXYZW: pose},
		"joint_names applies":           {Type: "left-foot", FrameIndices: []int{1}, RootPositions: root, LocalJointsRotXYZW: pose, JointNames: []string{"LeftFoot"}},
		"heading comes from its hips":   {Type: "fullbody", FrameIndices: []int{1}, RootPositions: root, LocalJointsRotXYZW: pose, GlobalRootHeading: [][]float64{{1, 0}}},
	}
	for want, c := range cases {
		if _, err := parseConstraints([]motionConstraint{c}, "soma30", 30, 150); err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%q: got %v", want, err)
		}
	}
}

// The block layout src/generate.cpp decode_constraints reads.
func TestEncodeConstraints(t *testing.T) {
	if encodeConstraints(30, nil) != "-" {
		t.Fatal("no constraints must encode as -")
	}
	list := []nativeConstraint{
		{kind: constraintRoot2D, frames: []uint32{0, 9}, smooth: []float32{0, 0, 1, 2}},
		{kind: constraintEndEffector, endEffectors: 8, frames: []uint32{5}, roots: []float32{0, .9, 0}, rotations: make([]float32, 2*4)},
	}
	b, err := base64.StdEncoding.DecodeString(encodeConstraints(2, list))
	if err != nil {
		t.Fatal(err)
	}
	u32 := func(i int) uint32 { return binary.LittleEndian.Uint32(b[i*4:]) }
	want := []uint32{2, 2, constraintRoot2D, 0, 2, 2, 0, 9}
	for i, v := range want {
		if u32(i) != v {
			t.Fatalf("word %d = %d, want %d", i, u32(i), v)
		}
	}
	if math.Float32frombits(u32(len(want)+3)) != 2 {
		t.Fatal("root2d smooth_root_2d follows its frames")
	}
	second := len(want) + 4
	if u32(second) != constraintEndEffector || u32(second+1) != 8 || u32(second+2) != 1 || u32(second+3) != 1 || u32(second+4) != 5 {
		t.Fatalf("end-effector header %v", []uint32{u32(second), u32(second + 1), u32(second + 2), u32(second + 3), u32(second + 4)})
	}
	if words := len(b) / 4; words != second+5+3+8 {
		t.Fatalf("block holds %d words", words)
	}
}
