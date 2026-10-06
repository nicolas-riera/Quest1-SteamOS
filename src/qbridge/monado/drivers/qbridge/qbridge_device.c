// Copyright 2026, qbridge contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  qbridge HMD and Touch controller devices.
 *
 * Views are returned exactly as the app sampled them (latest sample) and the
 * sample number is remembered, so the frame goes back to the app tagged with
 * the pose it was rendered for; Meta's compositor then reprojects it.
 *
 * @ingroup drv_qbridge
 */

#include "qbridge_interface.h"

#include "math/m_api.h"
#include "os/os_time.h"
#include "util/u_device.h"
#include "util/u_distortion_mesh.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "util/u_visibility_mask.h"

#include <poll.h>
#include <stdio.h>
#include <string.h>


/*
 *
 * Bridge thread.
 *
 */

static struct qb_sys g_sys;
static bool g_sys_init;

static void *
qb_sys_thread(void *ptr)
{
	struct qb_sys *s = ptr;
	struct qb_link *l = &s->link;

	os_thread_helper_lock(&s->oth);
	while (os_thread_helper_is_running_locked(&s->oth)) {
		os_thread_helper_unlock(&s->oth);

		// Wait for activity without holding the mutex.
		struct pollfd p = {l->fd >= 0 ? l->fd : l->listen_fd, POLLIN, 0};
		poll(&p, 1, 100);

		os_mutex_lock(&s->mutex);
		if (l->fd < 0) {
			if (qb_link_accept(l, 0))
				U_LOG_I("qbridge: app connected (%ux%u per eye)", l->hello.eye_width, l->hello.eye_height);
		} else if (qb_link_poll(l) && l->have_pose && l->pose.seq != s->latest.seq) {
			s->latest = l->pose;
			s->have_pose = true;
		} else if (l->fd < 0) {
			s->have_pose = false;
			U_LOG_W("qbridge: app disconnected");
		}
		os_mutex_unlock(&s->mutex);

		os_thread_helper_lock(&s->oth);
	}
	os_thread_helper_unlock(&s->oth);
	return NULL;
}

struct qb_sys *
qb_sys_get(void)
{
	// Monado creates devices and the compositor target on one thread at startup.
	if (g_sys_init)
		return &g_sys;
	struct qb_sys *s = &g_sys;
	int r = qb_link_init(&s->link);
	if (r != 0) {
		U_LOG_E("qbridge: qb_link_init failed: %s", strerror(-r));
		return NULL;
	}
	os_mutex_init(&s->mutex);
	os_thread_helper_init(&s->oth);
	os_thread_helper_start(&s->oth, qb_sys_thread, s);
	g_sys_init = true;
	U_LOG_I("qbridge: listening, start the qbridge app on the headset");
	return s;
}

bool
qb_sys_wait_ready(struct qb_sys *s, int timeout_ms)
{
	for (int t = 0; t < timeout_ms; t += 20) {
		os_mutex_lock(&s->mutex);
		bool ready = s->link.fd >= 0 && s->have_pose;
		os_mutex_unlock(&s->mutex);
		if (ready)
			return true;
		os_nanosleep(20 * U_TIME_1MS_IN_NS);
	}
	return false;
}


/*
 *
 * Conversions.
 *
 */

static struct xrt_pose
to_pose(const struct qb_pose *p)
{
	struct xrt_pose o = {
	    {p->orientation.x, p->orientation.y, p->orientation.z, p->orientation.w},
	    {p->position.x, p->position.y, p->position.z},
	};
	return o;
}

static enum xrt_space_relation_flags
to_flags(uint32_t f)
{
	int o = 0;
	if (f & QB_POSE_ORIENTATION_VALID)
		o |= XRT_SPACE_RELATION_ORIENTATION_VALID_BIT;
	if (f & QB_POSE_POSITION_VALID)
		o |= XRT_SPACE_RELATION_POSITION_VALID_BIT;
	if (f & QB_POSE_ORIENTATION_TRACKED)
		o |= XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT;
	if (f & QB_POSE_POSITION_TRACKED)
		o |= XRT_SPACE_RELATION_POSITION_TRACKED_BIT;
	return (enum xrt_space_relation_flags)o;
}

static struct xrt_fov
to_fov(const struct qb_fov *f)
{
	struct xrt_fov o = {f->left, f->right, f->up, f->down};
	return o;
}


/*
 *
 * HMD.
 *
 */

struct qbridge_hmd
{
	struct xrt_device base;
	struct qb_sys *sys;
};

static xrt_result_t
hmd_get_tracked_pose(struct xrt_device *xdev,
                     enum xrt_input_name name,
                     int64_t at_timestamp_ns,
                     struct xrt_space_relation *out_relation)
{
	struct qbridge_hmd *h = (struct qbridge_hmd *)xdev;
	if (name != XRT_INPUT_GENERIC_HEAD_POSE) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	struct xrt_space_relation rel = XRT_SPACE_RELATION_ZERO;
	os_mutex_lock(&h->sys->mutex);
	if (h->sys->have_pose) {
		rel.pose = to_pose(&h->sys->latest.head);
		rel.relation_flags = to_flags(h->sys->latest.head_flags);
	}
	os_mutex_unlock(&h->sys->mutex);
	*out_relation = rel;
	return XRT_SUCCESS;
}

static xrt_result_t
hmd_get_view_poses(struct xrt_device *xdev,
                   const struct xrt_vec3 *default_eye_relation,
                   int64_t at_timestamp_ns,
                   enum xrt_view_type view_type,
                   uint32_t view_count,
                   struct xrt_space_relation *out_head_relation,
                   struct xrt_fov *out_fovs,
                   struct xrt_pose *out_poses)
{
	struct qbridge_hmd *h = (struct qbridge_hmd *)xdev;
	if (view_count != 2 || !h->sys->have_pose) {
		return u_device_get_view_poses(xdev, default_eye_relation, at_timestamp_ns, view_type, view_count,
		                               out_head_relation, out_fovs, out_poses);
	}

	os_mutex_lock(&h->sys->mutex);
	const struct qb_pose_msg *s = &h->sys->latest;
	struct xrt_pose head = to_pose(&s->head);
	struct xrt_pose head_inv;
	math_pose_invert(&head, &head_inv);
	out_head_relation->pose = head;
	out_head_relation->relation_flags = to_flags(s->head_flags);
	for (uint32_t i = 0; i < 2; i++) {
		// Eye poses come in base space; Monado wants them relative to the head.
		struct xrt_pose eye = to_pose(&s->eye_pose[i]);
		math_pose_transform(&head_inv, &eye, &out_poses[i]);
		out_fovs[i] = to_fov(&s->eye_fov[i]);
	}
	h->sys->render_seq = s->seq;
	os_mutex_unlock(&h->sys->mutex);
	return XRT_SUCCESS;
}

static xrt_result_t
hmd_get_visibility_mask(struct xrt_device *xdev,
                        enum xrt_visibility_mask_type type,
                        uint32_t view_index,
                        struct xrt_visibility_mask **out_mask)
{
	struct xrt_fov fov = xdev->hmd->distortion.fov[view_index];
	u_visibility_mask_get_default(type, &fov, out_mask);
	return XRT_SUCCESS;
}

static void
hmd_destroy(struct xrt_device *xdev)
{
	u_device_free(xdev);
}

struct xrt_device *
qbridge_hmd_create(struct qb_sys *s)
{
	enum u_device_alloc_flags flags = (enum u_device_alloc_flags)(U_DEVICE_ALLOC_HMD | U_DEVICE_ALLOC_TRACKING_NONE);
	struct qbridge_hmd *h = U_DEVICE_ALLOCATE(struct qbridge_hmd, flags, 1, 0);
	h->sys = s;

	h->base.update_inputs = u_device_noop_update_inputs;
	h->base.get_tracked_pose = hmd_get_tracked_pose;
	h->base.get_view_poses = hmd_get_view_poses;
	h->base.get_visibility_mask = hmd_get_visibility_mask;
	h->base.destroy = hmd_destroy;

	h->base.name = XRT_DEVICE_GENERIC_HMD;
	h->base.device_type = XRT_DEVICE_TYPE_HMD;
	h->base.inputs[0].name = XRT_INPUT_GENERIC_HEAD_POSE;
	h->base.supported.orientation_tracking = true;
	h->base.supported.position_tracking = true;
	snprintf(h->base.str, XRT_DEVICE_NAME_LEN, "Quest (qbridge)");
	snprintf(h->base.serial, XRT_DEVICE_NAME_LEN, "qbridge-hmd");
	h->base.tracking_origin->type = XRT_TRACKING_TYPE_OTHER;
	snprintf(h->base.tracking_origin->name, XRT_TRACKING_NAME_LEN, "Meta stage space");

	size_t idx = 0;
	h->base.hmd->blend_modes[idx++] = XRT_BLEND_MODE_OPAQUE;
	h->base.hmd->blend_mode_count = idx;

	os_mutex_lock(&s->mutex);
	uint32_t w = s->link.hello.eye_width, hgt = s->link.hello.eye_height;
	float hz = s->link.hello.refresh_rate > 0 ? s->link.hello.refresh_rate : 72.0f;
	for (int e = 0; e < 2; e++) {
		h->base.hmd->distortion.fov[e] = to_fov(&s->latest.eye_fov[e]);
	}
	os_mutex_unlock(&s->mutex);

	// Side by side, exactly the layout of the shared buffers. Meta's compositor
	// owns lens distortion, so none here.
	h->base.hmd->screens[0].w_pixels = w * 2;
	h->base.hmd->screens[0].h_pixels = hgt;
	h->base.hmd->screens[0].nominal_frame_interval_ns = (int64_t)(1e9 / hz);
	for (int e = 0; e < 2; e++) {
		h->base.hmd->views[e].display.w_pixels = w;
		h->base.hmd->views[e].display.h_pixels = hgt;
		h->base.hmd->views[e].viewport.x_pixels = e * w;
		h->base.hmd->views[e].viewport.y_pixels = 0;
		h->base.hmd->views[e].viewport.w_pixels = w;
		h->base.hmd->views[e].viewport.h_pixels = hgt;
		h->base.hmd->views[e].rot = u_device_rotation_ident;
	}
	u_distortion_mesh_set_none(&h->base);
	return &h->base;
}


/*
 *
 * Touch controllers.
 *
 */

enum qb_touch_input
{
	// Hand specific: X/Y/menu on the left, A/B/system on the right.
	QB_IN_PRIMARY_CLICK = 0,
	QB_IN_PRIMARY_TOUCH,
	QB_IN_SECONDARY_CLICK,
	QB_IN_SECONDARY_TOUCH,
	QB_IN_MENU_CLICK,
	QB_IN_SQUEEZE_VALUE,
	QB_IN_TRIGGER_TOUCH,
	QB_IN_TRIGGER_VALUE,
	QB_IN_THUMBSTICK_CLICK,
	QB_IN_THUMBSTICK_TOUCH,
	QB_IN_THUMBSTICK,
	QB_IN_GRIP_POSE,
	QB_IN_AIM_POSE,
	QB_IN_COUNT
};

static struct xrt_binding_input_pair simple_inputs_touch[4] = {
    {XRT_INPUT_SIMPLE_SELECT_CLICK, XRT_INPUT_TOUCH_TRIGGER_VALUE},
    {XRT_INPUT_SIMPLE_MENU_CLICK, XRT_INPUT_TOUCH_MENU_CLICK},
    {XRT_INPUT_SIMPLE_GRIP_POSE, XRT_INPUT_TOUCH_GRIP_POSE},
    {XRT_INPUT_SIMPLE_AIM_POSE, XRT_INPUT_TOUCH_AIM_POSE},
};

static struct xrt_binding_output_pair simple_outputs_touch[1] = {
    {XRT_OUTPUT_NAME_SIMPLE_VIBRATION, XRT_OUTPUT_NAME_TOUCH_HAPTIC},
};

static struct xrt_binding_profile binding_profiles_touch[1] = {
    {
        .name = XRT_DEVICE_SIMPLE_CONTROLLER,
        .inputs = simple_inputs_touch,
        .input_count = ARRAY_SIZE(simple_inputs_touch),
        .outputs = simple_outputs_touch,
        .output_count = ARRAY_SIZE(simple_outputs_touch),
    },
};

struct qbridge_controller
{
	struct xrt_device base;
	struct qb_sys *sys;
	int hand;
};

static xrt_result_t
ctrl_get_tracked_pose(struct xrt_device *xdev,
                      enum xrt_input_name name,
                      int64_t at_timestamp_ns,
                      struct xrt_space_relation *out_relation)
{
	struct qbridge_controller *c = (struct qbridge_controller *)xdev;
	if (name != XRT_INPUT_TOUCH_GRIP_POSE && name != XRT_INPUT_TOUCH_AIM_POSE) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	struct xrt_space_relation rel = XRT_SPACE_RELATION_ZERO;
	os_mutex_lock(&c->sys->mutex);
	const struct qb_controller *q = &c->sys->latest.controller[c->hand];
	if (c->sys->have_pose && q->active) {
		bool grip = name == XRT_INPUT_TOUCH_GRIP_POSE;
		rel.pose = to_pose(grip ? &q->grip : &q->aim);
		rel.relation_flags = to_flags(grip ? q->grip_flags : q->aim_flags);
		if (grip) {
			rel.linear_velocity = (struct xrt_vec3){q->linear_velocity.x, q->linear_velocity.y,
			                                        q->linear_velocity.z};
			rel.angular_velocity = (struct xrt_vec3){q->angular_velocity.x, q->angular_velocity.y,
			                                         q->angular_velocity.z};
			rel.relation_flags |= XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT |
			                      XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT;
		}
	}
	os_mutex_unlock(&c->sys->mutex);
	*out_relation = rel;
	return XRT_SUCCESS;
}

static xrt_result_t
ctrl_update_inputs(struct xrt_device *xdev)
{
	struct qbridge_controller *c = (struct qbridge_controller *)xdev;
	struct xrt_input *in = c->base.inputs;
	int64_t now = os_monotonic_get_ns();

	os_mutex_lock(&c->sys->mutex);
	struct qb_controller q = c->sys->latest.controller[c->hand];
	os_mutex_unlock(&c->sys->mutex);

	bool left = c->hand == 0;
	uint32_t b = q.buttons;
	in[QB_IN_PRIMARY_CLICK].value.boolean = b & (left ? QB_BTN_X : QB_BTN_A);
	in[QB_IN_PRIMARY_TOUCH].value.boolean = b & (left ? QB_TOUCH_X : QB_TOUCH_A);
	in[QB_IN_SECONDARY_CLICK].value.boolean = b & (left ? QB_BTN_Y : QB_BTN_B);
	in[QB_IN_SECONDARY_TOUCH].value.boolean = b & (left ? QB_TOUCH_Y : QB_TOUCH_B);
	in[QB_IN_MENU_CLICK].value.boolean = left && (b & QB_BTN_MENU);
	in[QB_IN_SQUEEZE_VALUE].value.vec1.x = q.squeeze;
	in[QB_IN_TRIGGER_TOUCH].value.boolean = b & QB_TOUCH_TRIGGER;
	in[QB_IN_TRIGGER_VALUE].value.vec1.x = q.trigger;
	in[QB_IN_THUMBSTICK_CLICK].value.boolean = b & QB_BTN_THUMBSTICK;
	in[QB_IN_THUMBSTICK_TOUCH].value.boolean = b & QB_TOUCH_THUMBSTICK;
	in[QB_IN_THUMBSTICK].value.vec2.x = q.thumbstick_x;
	in[QB_IN_THUMBSTICK].value.vec2.y = q.thumbstick_y;
	for (int i = 0; i < QB_IN_COUNT; i++) {
		in[i].timestamp = now;
		in[i].active = q.active != 0;
	}
	return XRT_SUCCESS;
}

static xrt_result_t
ctrl_set_output(struct xrt_device *xdev, enum xrt_output_name name, const struct xrt_output_value *value)
{
	struct qbridge_controller *c = (struct qbridge_controller *)xdev;
	if (name != XRT_OUTPUT_NAME_TOUCH_HAPTIC) {
		return XRT_ERROR_OUTPUT_UNSUPPORTED;
	}
	os_mutex_lock(&c->sys->mutex);
	qb_link_haptic(&c->sys->link, c->hand, value->vibration.amplitude, value->vibration.frequency,
	               value->vibration.duration_ns);
	os_mutex_unlock(&c->sys->mutex);
	return XRT_SUCCESS;
}

static void
ctrl_destroy(struct xrt_device *xdev)
{
	u_device_free(xdev);
}

struct xrt_device *
qbridge_controller_create(struct qb_sys *s, int hand, struct xrt_tracking_origin *origin)
{
	enum u_device_alloc_flags flags = U_DEVICE_ALLOC_TRACKING_NONE;
	struct qbridge_controller *c = U_DEVICE_ALLOCATE(struct qbridge_controller, flags, QB_IN_COUNT, 1);
	c->sys = s;
	c->hand = hand;
	bool left = hand == 0;

	u_device_populate_function_pointers(&c->base, ctrl_get_tracked_pose, ctrl_destroy);
	c->base.update_inputs = ctrl_update_inputs;
	c->base.set_output = ctrl_set_output;
	c->base.name = XRT_DEVICE_TOUCH_CONTROLLER;
	c->base.device_type = left ? XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER : XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER;
	c->base.tracking_origin = origin;
	c->base.supported.orientation_tracking = true;
	c->base.supported.position_tracking = true;
	snprintf(c->base.str, XRT_DEVICE_NAME_LEN, "Quest %s Touch (qbridge)", left ? "Left" : "Right");
	snprintf(c->base.serial, XRT_DEVICE_NAME_LEN, "qbridge-%s", left ? "left" : "right");

	struct xrt_input *in = c->base.inputs;
	in[QB_IN_PRIMARY_CLICK].name = left ? XRT_INPUT_TOUCH_X_CLICK : XRT_INPUT_TOUCH_A_CLICK;
	in[QB_IN_PRIMARY_TOUCH].name = left ? XRT_INPUT_TOUCH_X_TOUCH : XRT_INPUT_TOUCH_A_TOUCH;
	in[QB_IN_SECONDARY_CLICK].name = left ? XRT_INPUT_TOUCH_Y_CLICK : XRT_INPUT_TOUCH_B_CLICK;
	in[QB_IN_SECONDARY_TOUCH].name = left ? XRT_INPUT_TOUCH_Y_TOUCH : XRT_INPUT_TOUCH_B_TOUCH;
	in[QB_IN_MENU_CLICK].name = left ? XRT_INPUT_TOUCH_MENU_CLICK : XRT_INPUT_TOUCH_SYSTEM_CLICK;
	in[QB_IN_SQUEEZE_VALUE].name = XRT_INPUT_TOUCH_SQUEEZE_VALUE;
	in[QB_IN_TRIGGER_TOUCH].name = XRT_INPUT_TOUCH_TRIGGER_TOUCH;
	in[QB_IN_TRIGGER_VALUE].name = XRT_INPUT_TOUCH_TRIGGER_VALUE;
	in[QB_IN_THUMBSTICK_CLICK].name = XRT_INPUT_TOUCH_THUMBSTICK_CLICK;
	in[QB_IN_THUMBSTICK_TOUCH].name = XRT_INPUT_TOUCH_THUMBSTICK_TOUCH;
	in[QB_IN_THUMBSTICK].name = XRT_INPUT_TOUCH_THUMBSTICK;
	in[QB_IN_GRIP_POSE].name = XRT_INPUT_TOUCH_GRIP_POSE;
	in[QB_IN_AIM_POSE].name = XRT_INPUT_TOUCH_AIM_POSE;
	c->base.outputs[0].name = XRT_OUTPUT_NAME_TOUCH_HAPTIC;

	c->base.binding_profiles = binding_profiles_touch;
	c->base.binding_profile_count = ARRAY_SIZE(binding_profiles_touch);
	return &c->base;
}
