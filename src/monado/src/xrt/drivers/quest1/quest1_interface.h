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

//! The Touch controllers the HMD created (QUEST1_CONTROLLERS=0 disables them); false if none.
bool
quest1_hmd_get_controllers(struct xrt_device *hmd, struct xrt_device **out_left, struct xrt_device **out_right);


/*
 *
 * Touch controllers (quest1_controller.c), fed by the HMD driver's SyncBoss thread.
 *
 */

enum quest1_controller_buttons
{
	QUEST1_BUTTON_AX = 1 << 0,
	QUEST1_BUTTON_BY = 1 << 1,
	QUEST1_BUTTON_MENU = 1 << 2, //!< Menu (left) or Oculus (right).
	QUEST1_BUTTON_STICK = 1 << 3,
};

enum quest1_controller_touches
{
	QUEST1_TOUCH_AX = 1 << 0,
	QUEST1_TOUCH_BY = 1 << 1,
	QUEST1_TOUCH_STICK = 1 << 2,
	QUEST1_TOUCH_TRIGGER = 1 << 3,
	QUEST1_TOUCH_THUMBREST = 1 << 4,
};

struct quest1_controller_state
{
	uint32_t buttons; //!< quest1_controller_buttons
	uint32_t touches; //!< quest1_controller_touches
	float trigger, grip;     //!< 0..1
	float stick_x, stick_y;  //!< -1..1, +y up
};

struct quest1_controller;

//! Amplitude 0..1; duration_ns 0 = minimal pulse (OpenXR XR_MIN_HAPTIC_DURATION).
typedef void (*quest1_haptic_fn)(void *data, bool left, float amplitude, int64_t duration_ns);

struct quest1_controller *
quest1_controller_create(struct xrt_device *hmd, bool left);

struct xrt_device *
quest1_controller_xdev(struct quest1_controller *c);

//! IMU sample in m/s² and rad/s, raw controller axes, monotonic time.
void
quest1_controller_push_imu(struct quest1_controller *c,
                           int64_t when_ns,
                           const struct xrt_vec3 *accel,
                           const struct xrt_vec3 *gyro);

void
quest1_controller_push_state(struct quest1_controller *c, int64_t when_ns, const struct quest1_controller_state *s);

void
quest1_controller_set_disconnected(struct quest1_controller *c);

void
quest1_controller_set_haptic_fn(struct quest1_controller *c, quest1_haptic_fn fn, void *data);

#ifdef __cplusplus
}
#endif
