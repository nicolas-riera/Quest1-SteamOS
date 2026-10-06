// Copyright 2026, Quest1-SteamOS contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Quest 1 Touch controllers: 3DoF orientation from their IMU, arm model, buttons.
 *
 * The SyncBoss stream (read by the HMD driver's thread) carries the controllers' IMU and
 * button records; the HMD driver decodes them and pushes them here. Without camera tracking
 * the hand position comes from a simple arm model hung off the head pose. Each controller's
 * yaw is aligned with the head's on the first sample and whenever the Oculus/menu button is
 * held for a second (as on the Quest).
 *
 * @ingroup drv_quest1
 */

#include "math/m_api.h"
#include "math/m_imu_3dof.h"
#include "math/m_mathinclude.h"
#include "math/m_space.h"
#include "os/os_threading.h"
#include "os/os_time.h"
#include "util/u_debug.h"
#include "util/u_device.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "util/u_var.h"

#include "quest1_interface.h"

#include <stdio.h>
#include <string.h>

DEBUG_GET_ONCE_OPTION(quest1_ctrl_axes, "QUEST1_CTRL_AXES", "x,y,z")

//! Held this long, the Oculus (right) or menu (left) button recenters the controller's yaw.
#define RECENTER_HOLD_NS (1 * U_TIME_1S_IN_NS)

static struct xrt_binding_input_pair simple_inputs[4] = {
    {XRT_INPUT_SIMPLE_SELECT_CLICK, XRT_INPUT_TOUCH_TRIGGER_VALUE},
    {XRT_INPUT_SIMPLE_MENU_CLICK, XRT_INPUT_TOUCH_MENU_CLICK},
    {XRT_INPUT_SIMPLE_GRIP_POSE, XRT_INPUT_TOUCH_GRIP_POSE},
    {XRT_INPUT_SIMPLE_AIM_POSE, XRT_INPUT_TOUCH_AIM_POSE},
};

static struct xrt_binding_output_pair simple_outputs[1] = {
    {XRT_OUTPUT_NAME_SIMPLE_VIBRATION, XRT_OUTPUT_NAME_TOUCH_HAPTIC},
};

static struct xrt_binding_profile binding_profiles[1] = {
    {
        .name = XRT_DEVICE_SIMPLE_CONTROLLER,
        .inputs = simple_inputs,
        .input_count = ARRAY_SIZE(simple_inputs),
        .outputs = simple_outputs,
        .output_count = ARRAY_SIZE(simple_outputs),
    },
};

enum touch_input_index
{
	// left: X, Y, menu / right: A, B, Oculus (system)
	IN_AX_CLICK = 0,
	IN_AX_TOUCH,
	IN_BY_CLICK,
	IN_BY_TOUCH,
	IN_MENU_CLICK,

	IN_SQUEEZE_VALUE,
	IN_TRIGGER_TOUCH,
	IN_TRIGGER_VALUE,
	IN_THUMBSTICK_CLICK,
	IN_THUMBSTICK_TOUCH,
	IN_THUMBSTICK,
	IN_THUMBREST_TOUCH,
	IN_GRIP_POSE,
	IN_AIM_POSE,

	IN_COUNT
};

struct quest1_controller
{
	struct xrt_device base;
	struct xrt_device *hmd;
	bool left;

	struct os_mutex mutex;

	//! Controller IMU axis remap: ctrl[i] = axis_sign[i] * imu[axis_index[i]].
	int axis_index[3];
	float axis_sign[3];

	struct m_imu_3dof fusion;
	struct xrt_quat yaw_offset; //!< Applied before the fusion orientation to align with the head.
	bool aligned;
	int64_t last_imu_ns;

	struct quest1_controller_state state;
	int64_t state_ns;
	int64_t menu_down_since_ns;
	bool connected;

	quest1_haptic_fn haptic_fn;
	void *haptic_data;

	enum u_logging_level log_level;
};

static inline struct quest1_controller *
qc(struct xrt_device *xdev)
{
	return (struct quest1_controller *)xdev;
}

static void
parse_axes(struct quest1_controller *c, const char *spec)
{
	const char *p = spec;
	for (int i = 0; i < 3; i++) {
		float sign = 1.0f;
		if (*p == '-' || *p == '+') {
			sign = *p == '-' ? -1.0f : 1.0f;
			p++;
		}
		if (*p < 'x' || *p > 'z') {
			goto fail;
		}
		c->axis_index[i] = *p - 'x';
		c->axis_sign[i] = sign;
		p++;
		if (i < 2 && *p++ != ',') {
			goto fail;
		}
	}
	return;
fail:
	U_LOG_W("quest1: bad QUEST1_CTRL_AXES '%s', using x,y,z", spec);
	for (int i = 0; i < 3; i++) {
		c->axis_index[i] = i;
		c->axis_sign[i] = 1.0f;
	}
}

static struct xrt_vec3
remap(const struct quest1_controller *c, const struct xrt_vec3 *v)
{
	const float in[3] = {v->x, v->y, v->z};
	return (struct xrt_vec3){
	    c->axis_sign[0] * in[c->axis_index[0]],
	    c->axis_sign[1] * in[c->axis_index[1]],
	    c->axis_sign[2] * in[c->axis_index[2]],
	};
}

//! Rotation about the vertical axis only (heading) of q.
static struct xrt_quat
yaw_of(const struct xrt_quat *q)
{
	// forward = q * (0,0,-1), flattened
	float fx = -2.0f * (q->x * q->z + q->w * q->y);
	float fz = -(1.0f - 2.0f * (q->x * q->x + q->y * q->y));
	float yaw = atan2f(-fx, -fz);
	return (struct xrt_quat){0, sinf(yaw / 2), 0, cosf(yaw / 2)};
}

static bool
head_pose(struct quest1_controller *c, int64_t at_ns, struct xrt_pose *out)
{
	struct xrt_space_relation rel = XRT_SPACE_RELATION_ZERO;
	if (xrt_device_get_tracked_pose(c->hmd, XRT_INPUT_GENERIC_HEAD_POSE, at_ns, &rel) != XRT_SUCCESS ||
	    !(rel.relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT)) {
		return false;
	}
	*out = rel.pose;
	return true;
}

//! Make the controller point where the head looks. Call with the mutex held.
static void
align_yaw_locked(struct quest1_controller *c, int64_t at_ns)
{
	struct xrt_pose head;
	if (!head_pose(c, at_ns, &head)) {
		return;
	}
	struct xrt_quat head_yaw = yaw_of(&head.orientation);
	struct xrt_quat ctrl_yaw = yaw_of(&c->fusion.rot);
	struct xrt_quat inv;
	math_quat_invert(&ctrl_yaw, &inv);
	math_quat_rotate(&head_yaw, &inv, &c->yaw_offset);
	c->aligned = true;
	U_LOG_I("quest1: %s controller recentered", c->left ? "left" : "right");
}


/*
 *
 * Feeding from the HMD driver's SyncBoss thread.
 *
 */

void
quest1_controller_push_imu(struct quest1_controller *c,
                           int64_t when_ns,
                           const struct xrt_vec3 *accel,
                           const struct xrt_vec3 *gyro)
{
	struct xrt_vec3 a = remap(c, accel), g = remap(c, gyro);
	os_mutex_lock(&c->mutex);
	m_imu_3dof_update(&c->fusion, when_ns, &a, &g);
	c->last_imu_ns = when_ns;
	c->connected = true;
	// once gravity has settled pitch and roll
	if (!c->aligned && c->fusion.state == M_IMU_3DOF_STATE_RUNNING) {
		align_yaw_locked(c, when_ns);
	}
	os_mutex_unlock(&c->mutex);
}

void
quest1_controller_push_state(struct quest1_controller *c, int64_t when_ns, const struct quest1_controller_state *s)
{
	os_mutex_lock(&c->mutex);
	c->state = *s;
	c->state_ns = when_ns;
	c->connected = true;
	bool menu = (s->buttons & QUEST1_BUTTON_MENU) != 0;
	if (!menu) {
		c->menu_down_since_ns = 0;
	} else if (c->menu_down_since_ns == 0) {
		c->menu_down_since_ns = when_ns;
	} else if (c->menu_down_since_ns > 0 && when_ns - c->menu_down_since_ns > RECENTER_HOLD_NS) {
		align_yaw_locked(c, when_ns);
		c->menu_down_since_ns = -1; // once per hold
	}
	os_mutex_unlock(&c->mutex);
}

void
quest1_controller_set_disconnected(struct quest1_controller *c)
{
	os_mutex_lock(&c->mutex);
	c->connected = false;
	c->state = (struct quest1_controller_state){0};
	os_mutex_unlock(&c->mutex);
}

void
quest1_controller_set_haptic_fn(struct quest1_controller *c, quest1_haptic_fn fn, void *data)
{
	c->haptic_fn = fn;
	c->haptic_data = data;
}


/*
 *
 * xrt_device.
 *
 */

static void
set_bool(struct quest1_controller *c, int i, int64_t ns, bool v)
{
	c->base.inputs[i].timestamp = ns;
	c->base.inputs[i].value.boolean = v;
}

static void
set_f32(struct quest1_controller *c, int i, int64_t ns, float v)
{
	c->base.inputs[i].timestamp = ns;
	c->base.inputs[i].value.vec1.x = v;
}

static xrt_result_t
controller_update_inputs(struct xrt_device *xdev)
{
	struct quest1_controller *c = qc(xdev);
	os_mutex_lock(&c->mutex);
	const struct quest1_controller_state *s = &c->state;
	int64_t ns = c->state_ns;
	set_bool(c, IN_AX_CLICK, ns, s->buttons & QUEST1_BUTTON_AX);
	set_bool(c, IN_AX_TOUCH, ns, s->touches & QUEST1_TOUCH_AX);
	set_bool(c, IN_BY_CLICK, ns, s->buttons & QUEST1_BUTTON_BY);
	set_bool(c, IN_BY_TOUCH, ns, s->touches & QUEST1_TOUCH_BY);
	set_bool(c, IN_MENU_CLICK, ns, s->buttons & QUEST1_BUTTON_MENU);
	set_f32(c, IN_SQUEEZE_VALUE, ns, s->grip);
	set_bool(c, IN_TRIGGER_TOUCH, ns, s->touches & QUEST1_TOUCH_TRIGGER);
	set_f32(c, IN_TRIGGER_VALUE, ns, s->trigger);
	set_bool(c, IN_THUMBSTICK_CLICK, ns, s->buttons & QUEST1_BUTTON_STICK);
	set_bool(c, IN_THUMBSTICK_TOUCH, ns, s->touches & QUEST1_TOUCH_STICK);
	c->base.inputs[IN_THUMBSTICK].timestamp = ns;
	c->base.inputs[IN_THUMBSTICK].value.vec2.x = s->stick_x;
	c->base.inputs[IN_THUMBSTICK].value.vec2.y = s->stick_y;
	set_bool(c, IN_THUMBREST_TOUCH, ns, s->touches & QUEST1_TOUCH_THUMBREST);
	os_mutex_unlock(&c->mutex);
	return XRT_SUCCESS;
}

/*!
 * Arm model: the elbow hangs at a fixed offset below and in front of the head, turned with the
 * head's heading; the forearm points along the controller's orientation.
 */
static xrt_result_t
controller_get_tracked_pose(struct xrt_device *xdev,
                            enum xrt_input_name name,
                            int64_t at_ns,
                            struct xrt_space_relation *out)
{
	struct quest1_controller *c = qc(xdev);
	if (name != XRT_INPUT_TOUCH_AIM_POSE && name != XRT_INPUT_TOUCH_GRIP_POSE) {
		U_LOG_XDEV_UNSUPPORTED_INPUT(&c->base, c->log_level, name);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	os_mutex_lock(&c->mutex);
	bool ok = c->connected && c->fusion.state == M_IMU_3DOF_STATE_RUNNING;
	struct xrt_quat rot;
	math_quat_rotate(&c->yaw_offset, &c->fusion.rot, &rot);
	struct xrt_vec3 gyro = c->fusion.last.gyro;
	os_mutex_unlock(&c->mutex);

	*out = (struct xrt_space_relation)XRT_SPACE_RELATION_ZERO;
	if (!ok) {
		return XRT_SUCCESS; // no flags: not tracked
	}

	struct xrt_pose head = {XRT_QUAT_IDENTITY, {0, 0, 0}};
	head_pose(c, at_ns, &head);
	struct xrt_quat head_yaw = yaw_of(&head.orientation);

	float side = c->left ? -1.0f : 1.0f;
	struct xrt_vec3 elbow_local = {side * 0.17f, -0.45f, -0.05f}, elbow, forearm;
	struct xrt_vec3 forearm_local = {0.0f, 0.0f, -0.30f};
	math_quat_rotate_vec3(&head_yaw, &elbow_local, &elbow);
	math_quat_rotate_vec3(&rot, &forearm_local, &forearm);

	out->pose.orientation = rot;
	out->pose.position = (struct xrt_vec3){
	    head.position.x + elbow.x + forearm.x,
	    head.position.y + elbow.y + forearm.y,
	    head.position.z + elbow.z + forearm.z,
	};
	math_quat_rotate_derivative(&rot, &gyro, &out->angular_velocity);
	out->relation_flags = XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
	                      XRT_SPACE_RELATION_POSITION_VALID_BIT | XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT;
	return XRT_SUCCESS;
}

static xrt_result_t
controller_set_output(struct xrt_device *xdev, enum xrt_output_name name, const struct xrt_output_value *value)
{
	struct quest1_controller *c = qc(xdev);
	if (name != XRT_OUTPUT_NAME_TOUCH_HAPTIC || c->haptic_fn == NULL) {
		return XRT_SUCCESS;
	}
	c->haptic_fn(c->haptic_data, c->left, value->vibration.amplitude, value->vibration.duration_ns);
	return XRT_SUCCESS;
}

static void
controller_destroy(struct xrt_device *xdev)
{
	struct quest1_controller *c = qc(xdev);
	u_var_remove_root(c);
	m_imu_3dof_close(&c->fusion);
	os_mutex_destroy(&c->mutex);
	u_device_free(&c->base);
}

struct quest1_controller *
quest1_controller_create(struct xrt_device *hmd, bool left)
{
	struct quest1_controller *c =
	    U_DEVICE_ALLOCATE(struct quest1_controller, U_DEVICE_ALLOC_TRACKING_NONE, IN_COUNT, 1);
	if (c == NULL) {
		return NULL;
	}
	os_mutex_init(&c->mutex);
	c->hmd = hmd;
	// The arm model is relative to the head: share its tracking origin. With the default one
	// (type NONE) the builder would add its 3DoF controller offset (±0.2, 1.3, -0.5) on top.
	c->base.tracking_origin = hmd->tracking_origin;
	c->left = left;
	c->log_level = U_LOGGING_INFO;
	c->yaw_offset = (struct xrt_quat)XRT_QUAT_IDENTITY;
	parse_axes(c, debug_get_option_quest1_ctrl_axes());
	m_imu_3dof_init(&c->fusion, M_IMU_3DOF_USE_GRAVITY_DUR_20MS);

	u_device_populate_function_pointers(&c->base, controller_get_tracked_pose, controller_destroy);
	c->base.update_inputs = controller_update_inputs;
	c->base.set_output = controller_set_output;
	c->base.name = XRT_DEVICE_TOUCH_CONTROLLER;
	c->base.device_type = left ? XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER : XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER;
	c->base.supported.orientation_tracking = true;
	c->base.supported.position_tracking = false; // arm model only

	snprintf(c->base.str, XRT_DEVICE_NAME_LEN, "Oculus Quest %s Touch Controller", left ? "Left" : "Right");
	snprintf(c->base.serial, XRT_DEVICE_NAME_LEN, "Quest1 %s Controller", left ? "Left" : "Right");

	struct xrt_input *in = c->base.inputs;
	in[IN_AX_CLICK].name = left ? XRT_INPUT_TOUCH_X_CLICK : XRT_INPUT_TOUCH_A_CLICK;
	in[IN_AX_TOUCH].name = left ? XRT_INPUT_TOUCH_X_TOUCH : XRT_INPUT_TOUCH_A_TOUCH;
	in[IN_BY_CLICK].name = left ? XRT_INPUT_TOUCH_Y_CLICK : XRT_INPUT_TOUCH_B_CLICK;
	in[IN_BY_TOUCH].name = left ? XRT_INPUT_TOUCH_Y_TOUCH : XRT_INPUT_TOUCH_B_TOUCH;
	in[IN_MENU_CLICK].name = left ? XRT_INPUT_TOUCH_MENU_CLICK : XRT_INPUT_TOUCH_SYSTEM_CLICK;
	in[IN_SQUEEZE_VALUE].name = XRT_INPUT_TOUCH_SQUEEZE_VALUE;
	in[IN_TRIGGER_TOUCH].name = XRT_INPUT_TOUCH_TRIGGER_TOUCH;
	in[IN_TRIGGER_VALUE].name = XRT_INPUT_TOUCH_TRIGGER_VALUE;
	in[IN_THUMBSTICK_CLICK].name = XRT_INPUT_TOUCH_THUMBSTICK_CLICK;
	in[IN_THUMBSTICK_TOUCH].name = XRT_INPUT_TOUCH_THUMBSTICK_TOUCH;
	in[IN_THUMBSTICK].name = XRT_INPUT_TOUCH_THUMBSTICK;
	in[IN_THUMBREST_TOUCH].name = XRT_INPUT_TOUCH_THUMBREST_TOUCH;
	in[IN_GRIP_POSE].name = XRT_INPUT_TOUCH_GRIP_POSE;
	in[IN_AIM_POSE].name = XRT_INPUT_TOUCH_AIM_POSE;
	c->base.outputs[0].name = XRT_OUTPUT_NAME_TOUCH_HAPTIC;

	c->base.binding_profiles = binding_profiles;
	c->base.binding_profile_count = ARRAY_SIZE(binding_profiles);

	u_var_add_root(c, c->base.str, true);
	m_imu_3dof_add_vars(&c->fusion, c, "");
	return c;
}

struct xrt_device *
quest1_controller_xdev(struct quest1_controller *c)
{
	return &c->base;
}
