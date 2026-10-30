// Copy of src/q1track/q1track_shm.h for Monado's quest1 driver: keep both in sync.
// SPDX-License-Identifier: MIT
/*
 * Shared memory between q1trackd (Meta's tracking through libossdk, a bionic process) and its
 * glibc clients (Monado's quest1 driver). One writer, any number of readers; seqlock: the writer
 * makes `seq` odd, writes, then makes it even again; a reader retries while `seq` is odd or
 * changed during its copy. Plain C, the same layout for bionic and glibc on aarch64.
 *
 * Poses are in libossdk's tracking space (meters, +Y up, -Z forward, quaternions x,y,z,w) and
 * timestamps are CLOCK_MONOTONIC nanoseconds, as libossdk reports them.
 */
#pragma once

#include <stdint.h>

#define Q1TRACK_SHM_PATH "/run/q1track/poses"
#define Q1TRACK_MAGIC 0x4b525431u /* "1TRK" */
#define Q1TRACK_VERSION 1

/* RigidBodyTrackingData status bits (VrApi ovrTrackingStatus values) */
#define Q1TRACK_ORI_TRACKED 0x1u
#define Q1TRACK_POS_TRACKED 0x2u
#define Q1TRACK_ORI_VALID 0x4u
#define Q1TRACK_POS_VALID 0x8u

struct q1track_pose {
	uint32_t valid;     /* libossdk returned a state */
	uint32_t status;    /* Q1TRACK_* bits */
	int64_t sample_ns;  /* tracking sample time */
	int64_t predicted_ns; /* time the pose is predicted for */
	float orientation[4]; /* x, y, z, w */
	float position[3];
	float angular_velocity[3]; /* rad/s, tracking space */
	float linear_velocity[3];  /* m/s */
	float angular_acceleration[3];
	float linear_acceleration[3];
};

struct q1track_controller {
	uint64_t id; /* libossdk DeviceId, 0 = none */
	uint32_t connected;
	uint32_t buttons; /* OSSDK::Input::v1::Button bits */
	float trigger, grip; /* 0..1 */
	float stick_x, stick_y; /* -1..1, +y up */
	float battery; /* 0..1, < 0 = unknown */
	uint32_t pad;
	struct q1track_pose pose;
};

struct q1track_shm {
	uint32_t magic, version;
	volatile uint32_t seq; /* odd while the writer updates the fields below */
	uint32_t writer_pid;
	int64_t update_ns; /* CLOCK_MONOTONIC time of the last update (staleness check) */
	uint64_t updates;
	float ipd; /* meters, 0 = unknown */
	uint32_t pad;
	struct q1track_pose head;
	struct q1track_controller controller[2]; /* [0] left, [1] right */

	/* client -> q1trackd: haptics (amplitude 0..1 until the given time) */
	volatile uint32_t haptic_seq[2];
	float haptic_amplitude[2];
	int64_t haptic_until_ns[2];
};
