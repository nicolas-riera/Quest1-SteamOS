// Copyright 2026, Quest1-SteamOS contributors
// SPDX-License-Identifier: BSL-1.0
// Protocol between the Quest 1 Vulkan layer inside vrcompositor (src/vklayer-steamvr/quest1_scanout.c)
// and Monado's MDP target (comp_window_mdp.c): the layer hands over the gralloc buffers vrcompositor
// renders into and announces every finished frame; Monado puts them on the panels, re-projecting
// them to the head pose of the display time. SOCK_SEQPACKET on Q1D_SOCKET, one struct q1d_msg per
// packet. Both sides are built from this header.
#pragma once
#include <stdint.h>

#define Q1D_SOCKET "/run/monado/quest1_direct"
#define Q1D_MAX_IMAGES 4

enum q1d_msg_type
{
	//! layer -> Monado: describes the buffer (image `index`, half `half`); the next packet is the
	//! AHardwareBuffer itself (AHardwareBuffer_sendHandleToUnixSocket)
	Q1D_BUFFER = 1,
	//! layer -> Monado: image `index` holds the finished frame `frame`, rendered with `pose`
	Q1D_FRAME = 2,
	//! Monado -> layer: Monado no longer reads image `index`
	Q1D_RELEASE = 3,
	//! Monado -> layer: frame `frame` is on the panels (time_ns, CLOCK_MONOTONIC)
	Q1D_DISPLAYED = 4,
};

struct q1d_msg
{
	uint32_t type;
	uint32_t index;
	uint32_t half; //!< 0: panel at x = 0 of the display image, 1: panel at x = 1440
	uint32_t count;
	uint32_t width, height, stride_px;
	uint32_t bgra; //!< the bytes are B, G, R, A (vrcompositor writes B8G8R8A8 views)
	uint32_t ubwc;
	uint32_t pose_valid;
	uint64_t frame;
	int64_t time_ns;  //!< Q1D_FRAME: time the render pose was predicted for; Q1D_DISPLAYED: when
	float pose[4];    //!< Q1D_FRAME: render orientation x, y, z, w (OpenXR LOCAL space)
};
