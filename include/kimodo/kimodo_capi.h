/*
 * kimodo_capi.h -- stable C ABI for kimodo.cpp.
 *
 * All entry points are implemented as an exception firewall.  Neural graph
 * execution is enabled only after its converted tensors pass reference tests.
 *
 * Versioning: structs carry their own size, which the caller sets with
 * sizeof.  Fields are only ever appended, so a struct from an older header
 * still works: the fields it lacks take their defaults.  Functions are only
 * ever added.
 *
 * Threads: calls on one model are serialised by the library, so one
 * generation runs at a time per model, from any thread.  Different models may
 * be used from different threads; they share the GPU.  Strings are UTF-8,
 * paths included, on every platform.
 */
#pragma once

#include <stddef.h>
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

/* 2: kinematic constraints.  3: the in-process engine -- runtime options
 * honoured, opening by data folder, capabilities, skeleton, limits,
 * sequences, progress and cancel, constraints JSON, text embeddings, logs. */
#define KIMODO_CAPI_ABI_VERSION 3

typedef struct kimodo_model kimodo_model;
typedef struct kimodo_motion kimodo_motion;

typedef enum kimodo_device {
    KIMODO_DEVICE_AUTO = 0,     /* the GPU when there is a usable one, else the CPU */
    KIMODO_DEVICE_CPU = 1,
    KIMODO_DEVICE_VULKAN = 2,
} kimodo_device;

typedef struct kimodo_runtime_options {
    uint32_t size;              /* caller sets sizeof(kimodo_runtime_options) */
    uint32_t threads;           /* CPU threads; 0 uses every hardware thread */
    kimodo_device device;
    const char *backend_dir;    /* unused: every backend is built into the library */
    /* ABI 3 */
    uint32_t gpu_index;         /* which GPU, 0..kimodo_gpu_count()-1 */
    /* Text encoder layers kept on the device at once: 0 decides (all 32 when
     * the device has room for them beside the motion model, else 8), 1..32
     * sets it; 32 keeps the whole encoder resident. */
    uint32_t text_layer_chunk;
    uint32_t text_resident_limit_mib; /* never keep more resident than this; 0 no limit */
    /* Raise or lower the active limits (kimodo_model_limits); 0 keeps the
     * default, and a value past kimodo_get_limit_ceilings is clamped to it. */
    uint32_t max_segment_frames;
    uint32_t max_segments;
} kimodo_runtime_options;

typedef struct kimodo_generation_options {
    uint32_t size;              /* caller sets sizeof(kimodo_generation_options) */
    uint64_t seed;
    uint32_t frames;            /* kimodo_generate*: the clip; ignored by sequences */
    uint32_t diffusion_steps;
    float text_cfg_weight;
    float constraint_cfg_weight;
    /* ABI 2; a version-1 caller's smaller size leaves them at their defaults. */
    float first_heading;        /* initial facing, radians; 0 faces +Z */
    uint32_t post_process;      /* nonzero: foot-skate cleanup and IK onto constraints */
    float root_margin;          /* metres a root target may be missed by; 0.04 upstream */
    /* ABI 3.  (The reserved word keeps this struct's size apart from ABI 2's,
     * whose last four bytes are alignment padding.) */
    uint32_t reserved;
    uint32_t transition_frames; /* frames blended between segments; 0 selects 5 */
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
 *
 * ABI 3: a pose may instead be given as local_rotations_axis_angle [F,J,3]
 * (NVIDIA's local_joints_rot), and on the SOMA models in NVIDIA's 77-joint
 * order (pose_joints = 77), which is mapped onto the model's 30 joints.
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
    /* ABI 3 */
    const float *local_rotations_axis_angle; /* [frame_count,joints,3]; instead of xyzw */
    uint32_t pose_joints;       /* the pose arrays' J: 0 is the model's joints; SOMA also takes 77 */
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
 * Weights reach the device at the first generation; kimodo_open loads them at
 * once.
 */
KIMODO_API kimodo_model *kimodo_model_load(
    const char *motion_gguf,
    const char *text_gguf,
    const char *text_adapter_gguf,
    const kimodo_runtime_options *options,
    char *err,
    int err_len);

/* Releases the model and every buffer it holds, host and device. */
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

/* ------------------------------------------------------------------ ABI 3 */

typedef enum kimodo_status {
    KIMODO_OK = 0,
    KIMODO_ERROR = 1,           /* the reason is in `err` and kimodo_model_last_error */
    KIMODO_CANCELLED = 2,       /* the progress callback asked to stop */
} kimodo_status;

/* What this build of the library can do.  Strings are static. */
typedef struct kimodo_capabilities {
    uint32_t size;              /* caller sets sizeof(kimodo_capabilities) */
    int abi;                    /* KIMODO_CAPI_ABI_VERSION of the library */
    const char *version;        /* the library's version, "0.1.0" */
    const char *commit;         /* the source commit it was built from */
    uint32_t devices;           /* bits (1u << KIMODO_DEVICE_*) of the backends built in */
    uint32_t post_processing;   /* nonzero: foot-skate cleanup and IK are built in */
} kimodo_capabilities;

/* Fills `out` up to out->size.  Returns 0, or -1 for a NULL or undersized struct. */
KIMODO_API int kimodo_get_capabilities(kimodo_capabilities *out);

/* The library's log.  Levels match ggml's, whose messages come here too. */
typedef enum kimodo_log_level {
    KIMODO_LOG_DEBUG = 1,       /* timings and detail, for measurement */
    KIMODO_LOG_INFO = 2,
    KIMODO_LOG_WARNING = 3,
    KIMODO_LOG_ERROR = 4,
} kimodo_log_level;

/* One line, without its newline.  May be called from any thread. */
typedef void (*kimodo_log_fn)(void *user_data, kimodo_log_level level, const char *message);

/*
 * Sends the library's messages at `min_level` and above to `callback`, for
 * every model.  With no callback -- the default -- the library prints
 * nothing.  NULL stops the messages.
 */
KIMODO_API void kimodo_set_log_callback(kimodo_log_fn callback, void *user_data, kimodo_log_level min_level);

/* GPUs the Vulkan backend can use, in gpu_index order; 0 without one. */
KIMODO_API int kimodo_gpu_count(void);

typedef struct kimodo_gpu_info {
    uint32_t size;              /* caller sets sizeof(kimodo_gpu_info) */
    char name[256];             /* UTF-8, NUL-terminated */
    uint64_t memory_total;      /* bytes of device memory */
    uint64_t memory_free;       /* bytes free now, as the driver reports it */
} kimodo_gpu_info;

/* Returns 0, or -1 for an index out of range or a bad struct. */
KIMODO_API int kimodo_gpu_info_get(int gpu_index, kimodo_gpu_info *out);

/*
 * Starts the engine: opens the models in `data_dir` (for example CF's
 * Assets/kimodo) and loads every weight onto the device now, so the first
 * generation starts at once and a shortage surfaces here.  kimodo_model_free
 * stops it and releases everything.
 *
 *   motion_model  a file name in data_dir; NULL selects
 *                 kimodo-soma-seed-v1.1-f32.gguf
 *   text_model    a file name in data_dir, with tokenizer.gguf beside it;
 *                 NULL selects Llama-3-Kimodo-Q8_0.gguf, "" loads none (then
 *                 only kimodo_generate_embedding can generate)
 *   options       NULL for the defaults
 */
KIMODO_API kimodo_model *kimodo_open(
    const char *data_dir,
    const char *motion_model,
    const char *text_model,
    const kimodo_runtime_options *options,
    char *err,
    int err_len);

/*
 * The motion models in `data_dir` (kimodo-*.gguf), one file name per line,
 * written into `buffer` and NUL-terminated.  Returns the length of the whole
 * list without its NUL -- call with a larger buffer if it did not fit -- or
 * -1 when the folder cannot be read.
 */
KIMODO_API int kimodo_list_motion_models(const char *data_dir, char *buffer, int buffer_len);

/* The skeleton: its key ("soma30", "g1skel34", "smplx22") and joints. */
KIMODO_API const char *kimodo_model_skeleton(const kimodo_model *model);
KIMODO_API const char *kimodo_model_joint_name(const kimodo_model *model, int joint);
/* The parent joint, or -1 for the root (and for a bad joint). */
KIMODO_API int kimodo_model_joint_parent(const kimodo_model *model, int joint);
/* The joint's rest offset from its parent, metres, into out_xyz[3].  Returns 0 or -1. */
KIMODO_API int kimodo_model_joint_offset(const kimodo_model *model, int joint, float *out_xyz);

/*
 * What a model accepts.  Generations outside these fail with a reason.  The
 * defaults are the demo server's: 2..360 frames (12 s) per segment, 16
 * segments.  Motion past 12 s per segment has not been judged for quality.
 */
typedef struct kimodo_limits {
    uint32_t size;              /* caller sets sizeof(kimodo_limits) */
    uint32_t min_segment_frames;
    uint32_t max_segment_frames;
    uint32_t max_segments;
    uint32_t max_transition_frames;
    uint32_t max_diffusion_steps;
    uint32_t max_constraints;
    uint32_t max_prompt_bytes;  /* UTF-8 bytes per prompt */
    uint32_t frame_rate;        /* frames per second of every motion */
} kimodo_limits;

/* The active limits of `model`.  Returns 0 or -1. */
KIMODO_API int kimodo_model_limits(const kimodo_model *model, kimodo_limits *out);
/* The most kimodo_runtime_options may raise the limits to.  Returns 0 or -1. */
KIMODO_API int kimodo_get_limit_ceilings(kimodo_limits *out);

/*
 * The server's defaults for `model` (NULL for a model-independent answer):
 * 150 frames, 100 steps, seed 0, text and constraint guidance 2.0, first
 * heading 0, transition 5 frames, root margin 0.04 m, and post-processing on
 * where the build has it, except for the G1 robot.  Sets options->size.
 */
KIMODO_API void kimodo_generation_options_init(const kimodo_model *model, kimodo_generation_options *options);
/* Zeroes `options` and sets its size: every field at its default. */
KIMODO_API void kimodo_runtime_options_init(kimodo_runtime_options *options);

typedef enum kimodo_stage {
    KIMODO_STAGE_LOADING = 1,   /* weights onto the device (done by kimodo_open) */
    KIMODO_STAGE_ENCODING_TEXT = 2,  /* done, total: prompts */
    KIMODO_STAGE_SAMPLING = 3,  /* done, total: diffusion steps over every segment */
} kimodo_stage;

/* Called on the generating thread.  Return nonzero to cancel. */
typedef int (*kimodo_progress_fn)(void *user_data, kimodo_stage stage, uint32_t done, uint32_t total);

typedef struct kimodo_segment {
    const char *prompt;         /* UTF-8 */
    uint32_t frames;
} kimodo_segment;

typedef struct kimodo_request {
    uint32_t size;              /* caller sets sizeof(kimodo_request) */
    const kimodo_segment *segments;  /* played in order, blended over transition_frames */
    uint32_t segment_count;
    const kimodo_constraints *constraints;  /* optional */
    /* Optional: constraints in NVIDIA Kimodo's JSON, a list of constraint
     * sets as its demo saves them (types root2d, fullbody, end-effector,
     * left-hand, right-hand, left-foot, right-foot).  Joined with
     * `constraints`.  Frames index the whole clip. */
    const char *constraints_json;
    kimodo_progress_fn progress;     /* optional */
    void *progress_user_data;
} kimodo_request;

/*
 * The general generation: one or more prompt segments, constraints, progress
 * and cancel.  On KIMODO_OK `*out` owns the motion; otherwise it is NULL.
 * `options` supplies seed, steps, guidance, heading, post-processing and the
 * transition; its `frames` is ignored.
 */
KIMODO_API kimodo_status kimodo_generate_sequence(
    kimodo_model *model,
    const kimodo_request *request,
    const kimodo_generation_options *options,
    kimodo_motion **out,
    char *err,
    int err_len);

/*
 * A prompt's 4096-value LLM2Vec embedding into out[4096]: what
 * kimodo_generate_embedding takes, so a machine with the text encoder can
 * prepare embeddings for one without.
 */
KIMODO_API kimodo_status kimodo_encode_text(
    kimodo_model *model,
    const char *prompt,
    float *out,
    char *err,
    int err_len);

/*
 * The entry points' types, for a host that loads the library at run time
 * (LoadLibrary, dlopen) instead of linking it: look each one up by its name
 * and cast it to the matching type.  The library checks at compile time that
 * these match the declarations above.
 */
typedef int (*kimodo_abi_version_fn)(void);
typedef kimodo_model *(*kimodo_model_load_fn)(const char *motion_gguf, const char *text_gguf,
    const char *text_adapter_gguf, const kimodo_runtime_options *options, char *err, int err_len);
typedef void (*kimodo_model_free_fn)(kimodo_model *model);
typedef const char *(*kimodo_model_last_error_fn)(const kimodo_model *model);
typedef int (*kimodo_model_joints_fn)(const kimodo_model *model);
typedef kimodo_motion *(*kimodo_generate_fn)(kimodo_model *model, const char *prompt,
    const kimodo_generation_options *options, char *err, int err_len);
typedef kimodo_motion *(*kimodo_generate_constrained_fn)(kimodo_model *model, const char *prompt,
    const kimodo_generation_options *options, const kimodo_constraints *constraints, char *err, int err_len);
typedef kimodo_motion *(*kimodo_generate_embedding_fn)(kimodo_model *model, const kimodo_embedding *embedding,
    const kimodo_generation_options *options, char *err, int err_len);
typedef void (*kimodo_motion_free_fn)(kimodo_motion *motion);
typedef int (*kimodo_motion_frames_fn)(const kimodo_motion *motion);
typedef int (*kimodo_motion_joints_fn)(const kimodo_motion *motion);
typedef const float *(*kimodo_motion_local_rotations_xyzw_fn)(const kimodo_motion *motion);
typedef const float *(*kimodo_motion_root_positions_fn)(const kimodo_motion *motion);
typedef int (*kimodo_get_capabilities_fn)(kimodo_capabilities *out);
typedef void (*kimodo_set_log_callback_fn)(kimodo_log_fn callback, void *user_data, kimodo_log_level min_level);
typedef int (*kimodo_gpu_count_fn)(void);
typedef int (*kimodo_gpu_info_get_fn)(int gpu_index, kimodo_gpu_info *out);
typedef kimodo_model *(*kimodo_open_fn)(const char *data_dir, const char *motion_model, const char *text_model,
    const kimodo_runtime_options *options, char *err, int err_len);
typedef int (*kimodo_list_motion_models_fn)(const char *data_dir, char *buffer, int buffer_len);
typedef const char *(*kimodo_model_skeleton_fn)(const kimodo_model *model);
typedef const char *(*kimodo_model_joint_name_fn)(const kimodo_model *model, int joint);
typedef int (*kimodo_model_joint_parent_fn)(const kimodo_model *model, int joint);
typedef int (*kimodo_model_joint_offset_fn)(const kimodo_model *model, int joint, float *out_xyz);
typedef int (*kimodo_model_limits_fn)(const kimodo_model *model, kimodo_limits *out);
typedef int (*kimodo_get_limit_ceilings_fn)(kimodo_limits *out);
typedef void (*kimodo_generation_options_init_fn)(const kimodo_model *model, kimodo_generation_options *options);
typedef void (*kimodo_runtime_options_init_fn)(kimodo_runtime_options *options);
typedef kimodo_status (*kimodo_generate_sequence_fn)(kimodo_model *model, const kimodo_request *request,
    const kimodo_generation_options *options, kimodo_motion **out, char *err, int err_len);
typedef kimodo_status (*kimodo_encode_text_fn)(kimodo_model *model, const char *prompt, float *out,
    char *err, int err_len);

#ifdef __cplusplus
}
#endif
