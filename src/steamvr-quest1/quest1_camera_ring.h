// Shared-memory ring of tracking-camera framesets, written by one producer process and read by any
// number of consumers (driver_quest1's camera component, debugging tools). See docs/passthrough.md.
//
// Producers: `qcam --ring` today (owns SyncBoss, so only while Monado and Meta's sensors HAL are
// stopped); later a helper that is a second client of Meta's sensors HAL camera stream
// (ICameraProvider::getStream(HEADSET)) so frames come from the same source as Meta's tracking.
//
// Layout: one file (default /dev/shm/quest1-cameras), a q1cam_ring_header followed by slot_count
// pixel areas of slot_bytes each. Each slot holds up to Q1CAM_MAX_CAMS 8-bit grayscale images,
// raw sensor orientation, no metadata line, one after the other at q1cam_ring_slot.cam[i].offset
// (relative to the start of the slot's pixel area).
//
// Protocol (single writer):
//   writer: pick slot (write_count % slot_count); slot.seq++ (odd = writing); fill pixels and
//           metadata; slot.seq++ (even = stable); write_count++ (release); FUTEX_WAKE(&write_count).
//   reader: n = write_count (acquire); if n == last seen: FUTEX_WAIT(&write_count, n, timeout);
//           slot = (n - 1) % slot_count; s0 = slot.seq; copy; s1 = slot.seq; valid if s0 == s1, even.
// With 4 slots and a reader that copies within one frame period (33 ms) torn reads cannot happen in
// practice; the sequence check catches them anyway.
#pragma once

#include <stdint.h>

#define Q1CAM_RING_MAGIC 0x52433151u /* "Q1CR" */
#define Q1CAM_RING_VERSION 1u
#define Q1CAM_RING_DEFAULT_PATH "/dev/shm/quest1-cameras"
#define Q1CAM_MAX_CAMS 4
#define Q1CAM_RING_SLOTS 4

/* frame types, as Meta's sensors HAL FrameType (vendor.oculus.hardware.sensors@1.0) */
#define Q1CAM_FRAME_HEADSET 2u    /* SLAM exposure (the one passthrough uses) */
#define Q1CAM_FRAME_CONTROLLER 3u /* ~40 us exposure for the controller LEDs: useless for viewing */
#define Q1CAM_FRAME_HAND 4u

struct q1cam_ring_cam
{
	uint32_t present; /* 1 if this camera has an image in the slot */
	uint32_t cam_id;  /* calibration Id (camera_calibration_v2.json): 0/2 bottom L/R, 1/3 top L/R */
	uint32_t width;   /* 640 */
	uint32_t height;  /* 480 */
	uint32_t stride;  /* bytes per row */
	uint32_t offset;  /* from the start of the slot's pixel area */
	float exposure_s; /* as reported by the sensor metadata (0 = unknown) */
	float gain;       /* analog gain (0 = unknown) */
};

struct q1cam_ring_slot
{
	volatile uint32_t seq;    /* odd while the writer fills the slot */
	uint32_t frame_type;      /* Q1CAM_FRAME_* */
	uint64_t frameset;        /* producer's frameset counter */
	int64_t capture_mono_ns;  /* mid-exposure, CLOCK_MONOTONIC (0 = unknown, use arrival) */
	int64_t arrival_mono_ns;  /* when the producer got the frameset, CLOCK_MONOTONIC */
	uint32_t ncams;           /* number of cam[] entries filled (present or not) */
	uint32_t _pad;
	struct q1cam_ring_cam cam[Q1CAM_MAX_CAMS];
};

struct q1cam_ring_header
{
	uint32_t magic;       /* Q1CAM_RING_MAGIC, written last at creation */
	uint32_t version;     /* Q1CAM_RING_VERSION */
	uint32_t header_size; /* sizeof(struct q1cam_ring_header): the pixel areas start here */
	uint32_t slot_count;  /* Q1CAM_RING_SLOTS */
	uint32_t slot_bytes;  /* size of each slot's pixel area */
	int32_t producer_pid; /* 0 when the producer exited cleanly */
	volatile uint32_t write_count; /* completed slots since creation (futex word) */
	uint32_t _pad;
	struct q1cam_ring_slot slot[Q1CAM_RING_SLOTS];
};

static inline uint64_t q1cam_ring_file_size(uint32_t slot_bytes)
{
	return (uint64_t)sizeof(struct q1cam_ring_header) + (uint64_t)Q1CAM_RING_SLOTS * slot_bytes;
}
