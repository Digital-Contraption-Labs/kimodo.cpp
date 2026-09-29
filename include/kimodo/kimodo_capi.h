/*
 * kimodo_capi.h -- stable C ABI for kimodo.cpp.
 *
 * All entry points are implemented as an exception firewall.  Neural graph
 * execution is enabled only after its converted tensors pass reference tests.
 */
#pragma once

#include <stdint.h>

#if defined(KIMODO_SHARED)
#  if defined(_WIN32) && !defined(__MINGW32__)
#    if defined(KIMODO_BUILD)
#      define KIMODO_API __declspec(dllexport)
#    else
#      define KIMODO_API __declspec(dllimport)
#    endif
#  else
#    define KIMODO_API __attribute__((visibility("default")))
#  endif
#else
#  define KIMODO_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define KIMODO_CAPI_ABI_VERSION 2 /* 2: kinematic constraints */

typedef struct kimodo_model kimodo_model;
typedef struct kimodo_motion kimodo_motion;

typedef enum kimodo_device {
    KIMODO_DEVICE_AUTO = 0,
    KIMODO_DEVICE_CPU = 1,
    KIMODO_DEVICE_VULKAN = 2,
} kimodo_device;

typedef struct kimodo_runtime_options {
    uint32_t size;              /* caller sets sizeof(kimodo_runtime_options) */
    uint32_t threads;           /* 0 selects the runtime default */
    kimodo_device device;
    const char *backend_dir;    /* NULL selects the executable/library directory */
} kimodo_runtime_options;

typedef struct kimodo_generation_options {
    uint32_t size;              /* caller sets sizeof(kimodo_generation_options) */
    uint64_t seed;
    uint32_t frames;
    uint32_t diffusion_steps;
    float text_cfg_weight;
    float constraint_cfg_weight;
    /* ABI 2; a version-1 caller's smaller size leaves them at their defaults. */
    float first_heading;        /* initial facing, radians; 0 faces +Z */
    uint32_t post_process;      /* nonzero: foot-skate cleanup and IK onto constraints */
    float root_margin;          /* metres a root target may be missed by; 0.04 upstream */
} kimodo_generation_options;

/*
 * Kinematic constraints, NVIDIA Kimodo's constraint sets.  Space is the
 * model's canonical frame: Y up, metres, the smoothed root at XZ (0, 0) on
 * frame 0 facing the options' `first_heading` (0 faces +Z).  Frame indices count from 0
 * across the whole generated clip.
 *
 *   ROOT2D        smooth_root_2d [F,2] (x, z) waypoints or a dense path, and
 *                 optionally root_heading [F,2] (cos, sin).
 *   FULLBODY      a keyframe pose: root_positions [F,3] (hips, Y above the
 *                 ground) and local_rotations_xyzw [F,J,4] in the model's
 *                 joint order, the same layout kimodo_motion returns.  Every
 *                 joint's position is constrained; smooth_root_2d defaults to
 *                 the hips' XZ.
 *   END_EFFECTOR  the same pose fields, constraining only the chains named
 *                 by the end_effectors bits (positions of the chain, rotation
 *                 of its first joint) and the root.
 */
typedef enum kimodo_constraint_type {
    KIMODO_CONSTRAINT_ROOT2D = 1,
    KIMODO_CONSTRAINT_FULLBODY = 2,
    KIMODO_CONSTRAINT_END_EFFECTOR = 3,
} kimodo_constraint_type;

enum {
    KIMODO_END_EFFECTOR_LEFT_FOOT = 1u,
    KIMODO_END_EFFECTOR_RIGHT_FOOT = 2u,
    KIMODO_END_EFFECTOR_LEFT_HAND = 4u,
    KIMODO_END_EFFECTOR_RIGHT_HAND = 8u,
    KIMODO_END_EFFECTOR_HIPS = 16u,
};

typedef struct kimodo_constraint {
    uint32_t size;              /* caller sets sizeof(kimodo_constraint) */
    kimodo_constraint_type type;
    uint32_t end_effectors;     /* KIMODO_END_EFFECTOR_* bits; END_EFFECTOR only */
    uint32_t frame_count;
    const uint32_t *frames;               /* [frame_count] */
    const float *root_positions;          /* [frame_count,3]; pose types */
    const float *local_rotations_xyzw;    /* [frame_count,joints,4]; pose types */
    const float *smooth_root_2d;          /* [frame_count,2]; ROOT2D, else optional */
    const float *root_heading;            /* [frame_count,2]; ROOT2D only, optional */
} kimodo_constraint;

typedef struct kimodo_constraints {
    uint32_t size;              /* caller sets sizeof(kimodo_constraints) */
    const kimodo_constraint *items;
    uint32_t count;
} kimodo_constraints;

/* A borrowed, row-major [1, 1, 4096] F32 LLM2Vec embedding. */
typedef struct kimodo_embedding {
    const float *data;
    uint32_t values;            /* must be exactly 4096 */
} kimodo_embedding;

/* Returns KIMODO_CAPI_ABI_VERSION. */
KIMODO_API int kimodo_abi_version(void);

/*
 * Load a converted motion model. `text_gguf` is optional only for a
 * precomputed-embedding workflow; ordinary prompt generation requires it.
 * `text_adapter_gguf` is the merged or separately converted LLM2Vec adapter.
 * On failure returns NULL and writes a NUL-terminated reason if `err` permits.
 */
KIMODO_API kimodo_model *kimodo_model_load(
    const char *motion_gguf,
    const char *text_gguf,
    const char *text_adapter_gguf,
    const kimodo_runtime_options *options,
    char *err,
    int err_len);

KIMODO_API void kimodo_model_free(kimodo_model *model);
KIMODO_API const char *kimodo_model_last_error(const kimodo_model *model);
/* Joints of the model's skeleton: the J of constraint and motion rotations. */
KIMODO_API int kimodo_model_joints(const kimodo_model *model);

/* Prompt is UTF-8. Returns an owning motion or NULL on failure. */
KIMODO_API kimodo_motion *kimodo_generate(
    kimodo_model *model,
    const char *prompt,
    const kimodo_generation_options *options,
    char *err,
    int err_len);

/*
 * kimodo_generate conditioned on kinematic constraints.  `constraints` may be
 * NULL or empty, which is the same as kimodo_generate; constraint_cfg_weight
 * in `options` scales how strongly they are followed, and post_process pins
 * them with IK afterwards.
 */
KIMODO_API kimodo_motion *kimodo_generate_constrained(
    kimodo_model *model,
    const char *prompt,
    const kimodo_generation_options *options,
    const kimodo_constraints *constraints,
    char *err,
    int err_len);

/*
 * Denoiser-only entry point.  This is the first supported integration
 * boundary and deliberately does not start Python or load a text runtime.
 */
KIMODO_API kimodo_motion *kimodo_generate_embedding(
    kimodo_model *model,
    const kimodo_embedding *embedding,
    const kimodo_generation_options *options,
    char *err,
    int err_len);

KIMODO_API void kimodo_motion_free(kimodo_motion *motion);
KIMODO_API int kimodo_motion_frames(const kimodo_motion *motion);
KIMODO_API int kimodo_motion_joints(const kimodo_motion *motion);
/* Borrowed row-major buffers, valid until kimodo_motion_free: [T,J,4], [T,3]. */
KIMODO_API const float *kimodo_motion_local_rotations_xyzw(const kimodo_motion *motion);
KIMODO_API const float *kimodo_motion_root_positions(const kimodo_motion *motion);

#ifdef __cplusplus
}
#endif
