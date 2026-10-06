// Socket protocol between the simulated display (quest1_display.c, in vrcompositor) and the
// receiver in driver_quest1 (vrserver). SOCK_SEQPACKET, one struct qd_msg per packet.
#pragma once
#include <stdint.h>

#define QD_MAX_IMAGES 4

enum qd_msg_type
{
	QD_SWAPCHAIN = 1, //!< layer -> driver: image description, one OPAQUE_FD per image (SCM_RIGHTS)
	QD_PRESENT = 2,   //!< layer -> driver: image `index` is finished, in TRANSFER_SRC_OPTIMAL
	QD_RELEASE = 3,   //!< driver -> layer: done reading image `index`
	QD_POSED = 4,     //!< driver -> layer: the pose of the frame after `frame` is published
	//! layer -> driver: frame `frame` is submitted (GPU still rendering): publish the pose of the
	//! next frame now, answered by QD_POSED. Its QD_PRESENT follows once rendered, with
	//! QD_PRESENT_POSED in flags. Lets vrcompositor work on the next frame while the GPU renders.
	QD_POSE = 5,
};

//! QD_PRESENT flags: the pose was already handled by a QD_POSE for this frame
#define QD_PRESENT_POSED 1u

struct qd_msg
{
	uint32_t type;
	uint32_t index;
	uint32_t count;
	uint32_t width, height;
	uint32_t format; //!< VkFormat
	uint32_t usage;  //!< VkImageUsageFlags the images were created with
	uint32_t flags;  //!< VkImageCreateFlags
	uint32_t view_format_count;
	uint32_t view_formats[4];
	uint64_t size[QD_MAX_IMAGES]; //!< allocation sizes (dedicated allocations)
	uint64_t frame;
	//! QD_PRESENT: the vblank (CLOCK_MONOTONIC ns, on the shared grid: multiples of the display
	//! period) that started this frame; the driver submits the frame with the pose it published then
	uint64_t vblank_ns;
};
