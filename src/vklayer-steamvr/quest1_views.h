// Steam Link VR's foveated views, from vrlink to vrcompositor: libquest1_v4l2 in vrlink (src/v4l2shim/quest1_vrlink.c)
// writes them as vrlink submits each frame, the layer in vrcompositor (quest1_modifier.c) reads them as it copies it.
//
// vrlink's frame is an atlas of four views: per eye a sharp inset (the fixed foveation's centre), then the wide view
// it sits in. It submits the wide views (the bounds) and, after each eye's VRTextureWithPoseAndDepth_t, the inset's
// place over the wide view in the atlas and a view count of 2. vrcompositor here leaves the inset out (2026-11-12:
// only the wide view showed, its centre coded with few bits, as the inset was to cover it).
// It also says that vrlink runs (alive_ns), for direct scanout's stream mode (quest1_scanout.c: qs_streaming).
#pragma once
#include <stdint.h>

#define Q1_VIEWS_PATH "/dev/shm/quest1-vrlink-views"

struct q1_views
{
	uint32_t seq;      //!< odd while being written
	uint32_t eyes;     //!< 2 once vrlink submitted a frame with its insets
	float wide[2][4];  //!< each eye's wide view in the atlas: u0 v0 u1 v1
	float inset[2][4]; //!< the inset's place over it, in the atlas; the inset itself is the view just above
	uint64_t alive_ns; //!< CLOCK_MONOTONIC, written every second while vrlink runs
};

// The head pose of each frame vrlink submits, for vrcompositor's distortion (quest1_modifier_pose.c): the layer pairs
// the picture it copies with the pose of its submit, found by its frame buffer. A file of its own, so that an older
// shim or layer on the other side reads the views above as before.
#define Q1_POSES_PATH "/dev/shm/quest1-vrlink-poses"
#define Q1_POSES 16

struct q1_poses
{
	uint32_t n; //!< submits published: the last is p[(n - 1) % Q1_POSES]
	uint32_t pid; //!< vrlink's process, whose fds fd[] are (0 from a shim older than fd[])
	struct
	{
		uint32_t seq;   //!< its submit's number (n after it), 0 while being written
		int32_t frame;  //!< which of vrlink's 16 frame buffers (its texture set's index) the picture is in
		uint64_t t_ns;   //!< CLOCK_MONOTONIC, just before vrlink's submit reaches vrcompositor
		float pose[12];  //!< the submit's head pose (HmdMatrix34_t: row-major 3x4, the position in its last column)
	} p[Q1_POSES];
	//! vrlink's own fd of each of its frame buffers (p[].frame), the dma-buf it decodes into, -1 if not known: the
	//! layer finds which buffer an image it imports is by comparing files (kcmp). After p[] so that a layer built
	//! before it reads the rest as before.
	int32_t fd[Q1_POSES];
};
