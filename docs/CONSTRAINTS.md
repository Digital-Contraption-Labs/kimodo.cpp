# Kinematic constraints

Kimodo's denoiser takes a condition beside the text: for each frame, observed
values for some motion features and a mask saying which ones are known. The
model fills in everything else consistently with them. NVIDIA packages this as
constraint sets, and this port implements the same ones:

| Type | Constrains, at each listed frame |
| --- | --- |
| `root2d` | the smoothed root's ground position `(x, z)`, and optionally its heading |
| `fullbody` | every joint's position, the hips height and the body's heading |
| `left-hand`, `right-hand`, `left-foot`, `right-foot` | that chain's joint positions (hand + hand end, foot + toe), the rotation of its first joint, the root position and heading |
| `end-effector` | the same for any of `LeftHand`, `RightHand`, `LeftFoot`, `RightFoot`, `Hips` |

A full-body keyframe constrains joint *positions* computed from the pose you
give; the joint rotations themselves are left to the model. Constraints are
applied through the separated classifier-free guidance's constraint branch,
weighted by `constraint_cfg` (default 2.0, upstream's value).

`src/constraints.cpp` is a port of upstream's `kimodo/constraints.py` and
`KimodoMotionRep.create_conditions`. `tests/constraints_test.cpp` checks it
against the multi-prompt hand-off conditioning, which is itself verified
against upstream, on all three skeletons.

## Space

All positions are metres in the model's canonical space: Y up, the smoothed
root at XZ `(0, 0)` on frame 0, facing +Z (or `first_heading` radians). A
motion this server generated is already in that space. To reuse its pose at
frame `t` at the same place in a new clip, send its root position and
rotations for frame `t` unchanged. Frame indices count from 0 across the whole
generated clip, including every prompt segment.

## HTTP

`POST /api/generate` accepts five more fields:

```json
{
  "prompt": "A person walks forward.",
  "frames": 90,
  "seed": 22,
  "model": "soma-rp-v1.1",
  "constraint_cfg": 2.0,
  "first_heading": 0.0,
  "post_processing": true,
  "root_margin": 0.04,
  "constraints": [
    {
      "type": "fullbody",
      "frame_indices": [45],
      "root_positions": [[0.02, 1.31, -0.48]],
      "local_joints_rot_xyzw": [[[0, 0, 0, 1], "... one XYZW quaternion per joint ..."]]
    },
    {
      "type": "root2d",
      "frame_indices": [89],
      "smooth_root_2d": [[1.5, 2.0]]
    }
  ]
}
```

- `constraints` uses NVIDIA's constraints JSON
  (`docs/source/user_guide/constraints.md` upstream). A file saved with the
  upstream demo's *Save Constraints* posts unchanged: its axis-angle
  `local_joints_rot` is accepted, and so are its 77-joint SOMA poses on the SOMA
  models.
- `local_joints_rot_xyzw` is this server's own convention: parent-local XYZW
  quaternions in `joint_names` order, the layout of `rotations.f32` and the
  GLB. Give each pose one of the two rotation forms, not both.
- `root_positions` is the hips' position with Y above the ground (about 0.9 m
  standing). `smooth_root_2d` defaults to the hips' XZ.
- `global_root_heading` (`[cos, sin]` per frame) is accepted on `root2d` only.
  A pose's heading always comes from its hips.
- `post_processing` runs upstream's post-processing (see below). It defaults
  to true except on the G1 models, as in NVIDIA's demo. `root_margin` (metres,
  default 0.04) is how far a corrected root may stay from a `root2d` target.
- `GET /api/models` lists each model's `joint_names`, `parents` and `offsets`.
- An invalid constraint is answered with 400 and a message naming it, e.g.
  `constraints[0] (fullbody): local_joints_rot_xyzw poses must have 30 or 77 joints, got 22`.
- The animation record keeps `constraints`, `constraint_cfg`,
  `first_heading`, `post_processing` and `root_margin`.

To take frame `t` of an animation as a keyframe, read
`/api/animations/<id>/root.f32` (`[frames, 3]`) and `rotations.f32`
(`[frames, joints, 4]`), both little-endian float32. Row `t` of each is the
keyframe's `root_positions` and `local_joints_rot_xyzw`.

On the demo page, select an animation, pause, scrub to a pose and press
*Pin current pose*. The next generation passes through that pose at the listed
frame, which you can edit. Pins stay while you look at other animations. An
animation that was generated with constraints offers to reuse them. The
*Post-process* checkbox is the `post_processing` switch.

## Post-processing

This is upstream's `post_processing=True`. After diffusion it:

- cleans up foot skating where the model's own foot-contact channels say a
  foot is planted;
- corrects the root toward `root2d` targets, within `root_margin`;
- pins full-body keyframes and hand or foot targets exactly with IK.

It is NVIDIA's MotionCorrection library, vendored unmodified in
`third_party/motion_correction` (Apache-2.0), with Eigen as a submodule.
`src/postprocess.cpp` ports the Python glue: the working rig, the per-type
masks and the constraint targets. As upstream does, every prompt segment is
corrected and then re-encoded into the motion representation
(`src/motion_encode.cpp`: smoothed root, velocities, foot contacts) before
the next segment's hand-off. Its first frames are pinned to the corrected
previous segment and replace them rather than blending.

It is verified in three ways:

- The root smoother matches upstream's `smooth_root.py` to 5e-7 m
  (`tests/postprocess_test.cpp`, values from
  `reference/dump_smooth_root_reference.py`).
- On a real generated walk with foot contacts, a keyframe, a hand target and a
  root waypoint, `correct_motion` produced output bit-identical to NVIDIA's own
  compiled `motion_correction` extension given the same inputs.
- `tests/postprocess_test.cpp` checks that a keyframe and a hand are pinned, a
  root waypoint lands within its margin, and a planted foot stops skating.

Differences from upstream:

- Upstream builds post-processing masks only for its named `left-hand` ...
  `right-foot` sets. A generic `end-effector` set naming the same joints is
  ignored there; here it pins the same way.
- Post-processing needs x86 (its math library is SSE code). Other platforms
  build without it (`model::post_processing_available()`), and requests for it
  fail.
- Every file that includes Eigen is built in the one `kimodo-postprocess`
  target with upstream's AVX flags. Eigen's allocation alignment depends on the
  instruction set, so mixing flags between files corrupts the heap.

## C and C++

`kimodo_generate_constrained` takes a `kimodo_constraints` array of
`kimodo_constraint` (`include/kimodo/kimodo_capi.h`). ABI 2 adds
`first_heading`, `post_process` and `root_margin` to the end of
`kimodo_generation_options`. A caller built against version 1 passes the
smaller `size` and gets the defaults. `kimodo_model_joints` gives the `J` of
the rotations. The C++ `model::generate_text`, `generate_embedding` and
`generate_text_sequence` take a `generation_options`, whose post-processing is
off by default as in upstream's Python API.

## What to expect

These measurements used NVIDIA's own SOMA RP example constraint files, posted
unchanged with their prompts and three seeds. They show the mean distance of
the constrained joints, or of the hips' XZ for root paths, from the
constraint's target. The upstream column is the result shipped with each
example, which upstream's demo post-processed.

| Example | Raw diffusion | Post-processed | Upstream |
| --- | ---: | ---: | ---: |
| full-body keyframes | 3.0 cm | 0.0 cm | 0.4 cm |
| hands and feet | 1.1–3.4 cm | 0.0–0.1 cm | 0.0 cm |
| dense root path | 8.6 cm | 3.1 cm | 10.6 cm |
| root waypoints | 6.1 cm | 3.8 cm | 4.0 cm |
| keyframe + path | 2.7 / 4.0 cm | 0.0 / 2.7 cm | 0.0 / 2.8 cm |

Keyframes in later prompt segments are also pinned exactly once
post-processed, on their frame. Without post-processing they landed within
5–15 cm, typically a frame early. Post-processing adds well under a second to
a clip.

Upstream's `FootSkateFromHeight` metric (toe speed while a toe is below 5 cm)
rates post-processed output in the same range as upstream's shipped examples,
for example 2.2 against 2.3 cm/s and 39.6 against 40.4 cm/s. It rises slightly
over raw output because the correction lowers feet onto the ground, so more
frames count.

Limitations:

- Poses far from the training data (the model has little acrobatics) are
  generated less naturally, and IK then forces them the rest of the way.
- Upstream's guidance still applies: keep each constraint type to about 20
  keyframes or fewer (dense root paths excepted). Don't contradict the prompt.
  Raise `constraint_cfg` if the raw motion misses keyframes badly, since IK can
  only bend what diffusion produced.
- Upstream considers post-processing poor on G1, so it is off there by
  default.
