/*
 * qbridge wire protocol, shared by the Android app (bionic, client) and the
 * Linux side (glibc, server) running in the Holo chroot on the same Quest.
 *
 * Transport: SOCK_SEQPACKET on the abstract unix address QB_SOCKET_NAME
 * (abstract sockets are per network namespace, which the chroot shares).
 * Every packet starts with struct qb_header. AHardwareBuffers travel with
 * AHardwareBuffer_sendHandleToUnixSocket right after a QB_MSG_BUFFERS packet.
 *
 * Frame flow (images are side-by-side stereo, left eye on the left half):
 *   app  -> linux  HELLO     display/eye configuration
 *   app  -> linux  BUFFERS   + QB_MAX_SLOTS AHardwareBuffers
 *   app  -> linux  POSE      every app frame, for its predicted display time
 *   linux-> app    FRAME     slot `slot` now holds an image rendered with
 *                            the views of POSE `pose_seq`; slot belongs to app
 *   app  -> linux  RELEASE   slot is no longer displayed, linux may reuse it
 *   linux-> app    HAPTIC    controller vibration
 */
#ifndef QBRIDGE_PROTO_H
#define QBRIDGE_PROTO_H

#include <stdint.h>

#define QB_SOCKET_NAME "qbridge"   /* abstract: "\0qbridge" */
#define QB_PROTO_VERSION 1
#define QB_MAX_SLOTS 3
#define QB_HAND_JOINTS 26          /* XR_HAND_JOINT_COUNT_EXT */

enum qb_msg_type {
	QB_MSG_HELLO = 1,
	QB_MSG_BUFFERS = 2,
	QB_MSG_POSE = 3,
	QB_MSG_FRAME = 4,
	QB_MSG_RELEASE = 5,
	QB_MSG_HAPTIC = 6,
};

struct qb_header {
	uint32_t type;
	uint32_t size; /* whole packet, header included */
};

struct qb_quat { float x, y, z, w; };
struct qb_vec3 { float x, y, z; };
struct qb_pose { struct qb_quat orientation; struct qb_vec3 position; };
struct qb_fov { float left, right, up, down; }; /* radians, OpenXR convention */

/* Pose validity bits (mirror XrSpaceLocationFlags semantics). */
#define QB_POSE_ORIENTATION_VALID   (1u << 0)
#define QB_POSE_POSITION_VALID      (1u << 1)
#define QB_POSE_ORIENTATION_TRACKED (1u << 2)
#define QB_POSE_POSITION_TRACKED    (1u << 3)

struct qb_hello {
	struct qb_header hdr;
	uint32_t version;
	uint32_t eye_width, eye_height; /* recommended per-eye size */
	float refresh_rate;
	uint32_t has_hand_tracking;
};

struct qb_buffers {
	struct qb_header hdr;
	uint32_t count;               /* AHardwareBuffers following on the socket */
	uint32_t width, height;       /* full side-by-side image */
	uint32_t format;              /* AHARDWAREBUFFER_FORMAT_* */
};

/* Touch controller inputs, one per hand. */
#define QB_BTN_A          (1u << 0)  /* right hand */
#define QB_BTN_B          (1u << 1)
#define QB_BTN_X          (1u << 2)  /* left hand */
#define QB_BTN_Y          (1u << 3)
#define QB_BTN_MENU       (1u << 4)
#define QB_BTN_THUMBSTICK (1u << 5)
#define QB_TOUCH_A        (1u << 8)
#define QB_TOUCH_B        (1u << 9)
#define QB_TOUCH_X        (1u << 10)
#define QB_TOUCH_Y        (1u << 11)
#define QB_TOUCH_THUMBSTICK (1u << 12)
#define QB_TOUCH_TRIGGER  (1u << 13)

struct qb_controller {
	uint32_t active;              /* controller connected and tracked */
	uint32_t grip_flags, aim_flags;
	struct qb_pose grip, aim;
	struct qb_vec3 linear_velocity, angular_velocity; /* grip, base space */
	uint32_t buttons;             /* QB_BTN_* | QB_TOUCH_* */
	float trigger, squeeze;
	float thumbstick_x, thumbstick_y;
};

struct qb_hand {
	uint32_t active;
	struct qb_pose joints[QB_HAND_JOINTS];
	float radii[QB_HAND_JOINTS];
	uint32_t joint_flags[QB_HAND_JOINTS];
};

struct qb_pose_msg {
	struct qb_header hdr;
	uint64_t seq;
	int64_t display_time_ns;      /* CLOCK_MONOTONIC, predicted display time */
	uint32_t head_flags;
	struct qb_pose head;          /* XR_REFERENCE_SPACE_TYPE_STAGE (fallback LOCAL) */
	struct qb_pose eye_pose[2];
	struct qb_fov eye_fov[2];
	struct qb_controller controller[2]; /* 0 = left, 1 = right */
	struct qb_hand hand[2];
};

struct qb_frame {
	struct qb_header hdr;
	uint32_t slot;
	uint32_t _pad;
	uint64_t pose_seq;            /* POSE whose eye views were used to render */
};

struct qb_release {
	struct qb_header hdr;
	uint32_t slot;
};

struct qb_haptic {
	struct qb_header hdr;
	uint32_t hand;                /* 0 = left, 1 = right */
	float amplitude;              /* 0..1, 0 stops */
	float frequency;              /* Hz, 0 = runtime default */
	int64_t duration_ns;
};

#endif
