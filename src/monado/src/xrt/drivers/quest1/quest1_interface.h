// Copyright 2026, Quest1-SteamOS contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Native Oculus Quest 1 driver: headset IMU through Meta's libsyncboss (libhybris).
 * @ingroup drv_quest1
 */

#pragma once

#include "xrt/xrt_device.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @defgroup drv_quest1 Native Quest 1 driver
 * @ingroup drv
 *
 * Runs on the Quest 1 itself with no Android: the SyncBoss MCU is driven through
 * Meta's libsyncboss.so loaded with libhybris, giving the headset IMU at 1 kHz.
 */

//! True when the SyncBoss device node exists (we are on a Quest).
bool
quest1_detect(void);

//! 3DoF HMD, display set up for the 2880x1600 framebuffer that the panels show rotated by 180°.
struct xrt_device *
quest1_hmd_create(void);

#ifdef __cplusplus
}
#endif
