package main

import (
	"encoding/binary"
	"encoding/json"
	"testing"
)

func TestBuildSkeletonGLB(t *testing.T) {
	for _, skeletonKey := range []string{"smplx22", "soma30", "g1skel34"} {
		t.Run(skeletonKey, func(t *testing.T) {
			skeleton := skeletonDefinitions[skeletonKey]
			roots := []float32{0, 0, 0, 1, 0, 0}
			rotations := make([]float32, 2*len(skeleton.parents)*4)
			for frame := 0; frame < 2; frame++ {
				for joint := range skeleton.parents {
					rotations[(frame*len(skeleton.parents)+joint)*4+3] = 1
				}
			}
			glb, err := buildSkeletonGLB(roots, rotations, skeleton)
			if err != nil {
				t.Fatal(err)
			}
			assertSkeletonGLB(t, glb, len(skeleton.parents))
		})
	}
}

func TestSafePathPart(t *testing.T) {
	for _, value := range []string{"q8_0", "run-jump.seed42", "BF16"} {
		if !safePathPart(value) {
			t.Fatalf("expected safe path part: %q", value)
		}
	}
	for _, value := range []string{"", ".", "..", "../escape", "a/b", "space here"} {
		if safePathPart(value) {
			t.Fatalf("expected unsafe path part: %q", value)
		}
	}
}

func assertSkeletonGLB(t *testing.T, b []byte, expectedJoints int) {
	t.Helper()
	if len(b) < 20 || binary.LittleEndian.Uint32(b) != 0x46546c67 || binary.LittleEndian.Uint32(b[4:]) != 2 || int(binary.LittleEndian.Uint32(b[8:])) != len(b) {
		t.Fatalf("invalid GLB header")
	}
	jsonLength := int(binary.LittleEndian.Uint32(b[12:]))
	if binary.LittleEndian.Uint32(b[16:]) != 0x4e4f534a || 20+jsonLength > len(b) {
		t.Fatalf("invalid GLB JSON chunk")
	}
	var document struct {
		Asset      map[string]string `json:"asset"`
		Nodes      []json.RawMessage `json:"nodes"`
		Animations []json.RawMessage `json:"animations"`
	}
	if err := json.Unmarshal(b[20:20+jsonLength], &document); err != nil {
		t.Fatal(err)
	}
	if document.Asset["version"] != "2.0" || len(document.Nodes) != expectedJoints || len(document.Animations) != 1 {
		t.Fatalf("unexpected GLB document: %s", b[20:20+jsonLength])
	}
}
