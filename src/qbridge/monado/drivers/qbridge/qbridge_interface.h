// Copyright 2026, qbridge contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  qbridge: Monado devices and compositor target backed by Meta's
 *         runtime on the same Quest, through the qbridge Android app.
 * @ingroup drv_qbridge
 */

#pragma once

#include "xrt/xrt_device.h"
#include "os/os_threading.h"

#include "qb_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Process-wide bridge state, shared by the devices and the compositor target
 * (both live in monado-service). A thread keeps the link fed with tracking.
 */
struct qb_sys
{
	struct os_mutex mutex;
	struct os_thread_helper oth;
	struct qb_link link;

	//! Latest tracking sample from the app.
	struct qb_pose_msg latest;
	bool have_pose;

	//! Sample the compositor last rendered with (sent back with the frame).
	uint64_t render_seq;
};

//! Get (and on first call start) the bridge. NULL if the socket can't be set up.
struct qb_sys *
qb_sys_get(void);

//! Wait until the app is connected and has sent a first pose.
bool
qb_sys_wait_ready(struct qb_sys *s, int timeout_ms);

struct xrt_device *
qbridge_hmd_create(struct qb_sys *s);

//! hand: 0 = left, 1 = right. Shares the HMD's tracking origin.
struct xrt_device *
qbridge_controller_create(struct qb_sys *s, int hand, struct xrt_tracking_origin *origin);

#ifdef __cplusplus
}
#endif
