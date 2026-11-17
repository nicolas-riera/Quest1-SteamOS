// SPDX-License-Identifier: MIT
/*
 * Shared memory between q1handsd (Meta's hand tracking through libossdk, a bionic process) and its glibc
 * clients (driver_quest1). One writer, any number of readers; seqlock as in q1track_shm.h: the writer makes
 * `seq` odd, writes, then makes it even again; a reader retries while `seq` is odd or changed during its copy.
 * Plain C, the same layout for bionic and glibc on aarch64. docs/hand-tracking.md.
 *
 * Poses are in libossdk's tracking space (meters, +Y up, -Z forward, quaternions x,y,z,w), as in q1track_shm.h;
 * `head` is the head pose at the same instant, so a client can express a hand in the head's frame whatever
 * space its own head pose is in. Timestamps are CLOCK_MONOTONIC nanoseconds.
 */
#pragma once

#include <stdint.h>

#define Q1HANDS_SHM_PATH "/run/q1track/hands"
#define Q1HANDS_MAGIC 0x53444e48u /* "HNDS" */
#define Q1HANDS_VERSION 1

#define Q1HANDS_BONES 19 /* WristRoot, ForearmStub, Thumb0-3, Index1-3, Middle1-3, Ring1-3, Pinky0-3 */
#define Q1HANDS_SKEL_BONES 24 /* the 19 plus the five finger tips (thumb, index, middle, ring, pinky) */

enum q1hands_mode { /* what the cameras and trackingservice are doing now */
	Q1HANDS_MODE_CONTROLLERS = 0, /* camera mux mode "default": Touch controllers tracked, no hands */
	Q1HANDS_MODE_HANDS = 1,       /* camera mux mode "HandTracking": hands tracked, controllers on their IMU only */
};

struct q1hands_hand {
	uint32_t valid;   /* libossdk returned a hand state */
	uint32_t status;  /* HandTrackingStatus as libossdk reports it */
	uint32_t tracked; /* q1handsd's verdict: this hand has a usable pose */
	uint32_t pad;
	int64_t sample_ns;
	int64_t output_ns;
	float root_rot[4]; /* wrist root pose, x,y,z,w (tracking space) */
	float root_pos[3];
	float pad2;
	float bone_rot[Q1HANDS_BONES][4]; /* local rotations, x,y,z,w; Meta's bone order */
	float confidence[5]; /* thumb, index, middle, ring, pinky; -1 = none */
	float pinch_occlusion[5];
	float pinch_confidence;
	uint32_t version; /* hand tracking version */
};

struct q1hands_shm {
	uint32_t magic, version;
	volatile uint32_t seq; /* odd while the writer updates the fields below */
	uint32_t writer_pid;
	int64_t update_ns; /* CLOCK_MONOTONIC time of the last update (staleness check) */
	uint64_t updates;
	uint32_t mode;           /* q1hands_mode, the one the system is in (read back from trackingservice) */
	uint32_t hands_enabled;  /* libossdk's getHandTrackingEnabled */
	uint32_t controller_in_hand[2]; /* libossdk isInHand, [0] left [1] right */
	uint32_t controller_connected[2];
	float hand_scale; /* 0 = unknown */
	uint32_t skeleton_valid;
	/* head pose at the time of the hand sample (libossdk tracking space) */
	float head_rot[4];
	float head_pos[3];
	uint32_t head_valid;
	struct q1hands_hand hand[2]; /* [0] left [1] right */
	/* Meta's bind-pose skeleton per hand: local rotation and translation of each of the 24 bones, and the parents */
	float bind_rot[2][Q1HANDS_SKEL_BONES][4];
	float bind_pos[2][Q1HANDS_SKEL_BONES][3];
	int16_t bind_parent[Q1HANDS_SKEL_BONES];
	uint8_t raw[2048]; /* the first bytes of libossdk's hand state, for diagnostics */
};
