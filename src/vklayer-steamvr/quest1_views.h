// Steam Link VR's foveated views, from vrlink to vrcompositor: libquest1_v4l2 in vrlink (src/v4l2shim/quest1_vrlink.c)
// writes them as vrlink submits each frame, the layer in vrcompositor (quest1_modifier.c) reads them as it copies it.
//
// vrlink's frame is an atlas of four views: per eye a sharp inset (the fixed foveation's centre), then the wide view
// it sits in. It submits the wide views (the bounds) and, after each eye's VRTextureWithPoseAndDepth_t, the inset's
// place over the wide view in the atlas and a view count of 2. vrcompositor here leaves the inset out (2026-11-12:
// only the wide view showed, its centre coded with few bits, as the inset was to cover it).
#pragma once
#include <stdint.h>

#define Q1_VIEWS_PATH "/dev/shm/quest1-vrlink-views"

struct q1_views
{
	uint32_t seq;      //!< odd while being written
	uint32_t eyes;     //!< 2 once vrlink submitted a frame with its insets
	float wide[2][4];  //!< each eye's wide view in the atlas: u0 v0 u1 v1
	float inset[2][4]; //!< the inset's place over it, in the atlas; the inset itself is the view just above
};
