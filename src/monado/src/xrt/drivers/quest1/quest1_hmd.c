// Copyright 2026, Quest1-SteamOS contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Native Quest 1 HMD: 3DoF from the SyncBoss IMU, display for the 2880x1600 framebuffer.
 *
 * libsyncboss.so is Meta's bionic C library for the SyncBoss MCU; it is loaded
 * with libhybris (the process needs LD_PRELOAD=libbionictls.so). Records come
 * from a 32-entry ring at 1 kHz; layouts are in docs/syncboss-imu.md.
 *
 * @ingroup drv_quest1
 */

#include "quest1_interface.h"

#include "math/m_api.h"
#include "math/m_imu_3dof.h"
#include "math/m_mathinclude.h"
#include "math/m_relation_history.h"
#include "math/m_vec3.h"
#include "os/os_threading.h"
#include "os/os_time.h"
#include "util/u_debug.h"
#include "util/u_device.h"
#include "util/u_distortion_mesh.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "util/u_var.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern void *
android_dlopen(const char *filename, int flag);
extern void *
android_dlsym(void *handle, const char *symbol);

DEBUG_GET_ONCE_LOG_OPTION(quest1_log, "QUEST1_LOG", U_LOGGING_INFO)
// Head axis i = sign * IMU axis, e.g. "-y,x,z". Head frame: +X right, +Y up, +Z back.
// Calibrated on the device 2026-10-05: up = -x, look-down = +y, turn-left = -x, tilt-left = -z (IMU frame).
DEBUG_GET_ONCE_OPTION(quest1_imu_axes, "QUEST1_IMU_AXES", "-y,-x,-z")

#define Q1_TRACE(h, ...) U_LOG_XDEV_IFL_T(&h->base, h->log_level, __VA_ARGS__)
#define Q1_DEBUG(h, ...) U_LOG_XDEV_IFL_D(&h->base, h->log_level, __VA_ARGS__)
#define Q1_INFO(h, ...) U_LOG_XDEV_IFL_I(&h->base, h->log_level, __VA_ARGS__)
#define Q1_ERROR(h, ...) U_LOG_XDEV_IFL_E(&h->base, h->log_level, __VA_ARGS__)

#define Q1_PANEL_W 1440
#define Q1_PANEL_H 1600
#define Q1_REFRESH_HZ 72


/*
 *
 * libsyncboss.
 *
 */

struct sb_record
{
	uint64_t seq;
	uint32_t type; // 0 = headset IMU
	uint32_t pad;
	uint8_t data[96];
};

struct sb_imu_event
{
	uint64_t zero;
	uint64_t timestamp_us;
	float temperature;
	float accel[3]; // m/s²
	float gyro[3];  // rad/s
	uint32_t pad;
};

struct sb_api
{
	int (*init)(void **handle, const void *opts);
	int (*deinit)(void *handle);
	int (*imu_enable)(void *handle);
	int (*imu_disable)(void *handle);
	int (*wait)(void *handle, uint32_t timeout_ms, struct sb_record *out);
};

static bool
sb_load(struct sb_api *api)
{
	void *lib = android_dlopen("libsyncboss.so", RTLD_NOW);
	if (lib == NULL) {
		return false;
	}
	api->init = android_dlsym(lib, "syncboss_init");
	api->deinit = android_dlsym(lib, "syncboss_deinit");
	api->imu_enable = android_dlsym(lib, "syncboss_imu_enable");
	api->imu_disable = android_dlsym(lib, "syncboss_imu_disable");
	api->wait = android_dlsym(lib, "syncboss_wait_on_stream_data_exclusive");
	return api->init != NULL && api->imu_enable != NULL && api->wait != NULL;
}


/*
 *
 * Device.
 *
 */

struct quest1_hmd
{
	struct xrt_device base;

	struct sb_api sb;
	void *sb_handle;
	struct os_thread_helper oth;

	//! IMU axis remap: head[i] = axis_sign[i] * imu[axis_index[i]].
	int axis_index[3];
	float axis_sign[3];

	//! Only touched by the IMU thread.
	struct
	{
		struct m_imu_3dof fusion;
		struct xrt_vec3 gyro_bias;
		uint64_t still_since_ns;
		bool bias_valid;
		int64_t clock_offset_ns; //!< monotonic - IMU clock
		bool have_offset;
		uint64_t samples;
	} imu;

	struct m_relation_history *history;
	enum u_logging_level log_level;
};

static inline struct quest1_hmd *
q1(struct xrt_device *xdev)
{
	return (struct quest1_hmd *)xdev;
}

static bool
parse_axes(struct quest1_hmd *h, const char *spec)
{
	const char *p = spec;
	for (int i = 0; i < 3; i++) {
		float sign = 1.0f;
		if (*p == '-' || *p == '+') {
			sign = *p == '-' ? -1.0f : 1.0f;
			p++;
		}
		if (*p < 'x' || *p > 'z') {
			return false;
		}
		h->axis_index[i] = *p - 'x';
		h->axis_sign[i] = sign;
		p++;
		if (i < 2 && *p++ != ',') {
			return false;
		}
	}
	return *p == '\0';
}

static struct xrt_vec3
remap(const struct quest1_hmd *h, const float v[3])
{
	return (struct xrt_vec3){
	    h->axis_sign[0] * v[h->axis_index[0]],
	    h->axis_sign[1] * v[h->axis_index[1]],
	    h->axis_sign[2] * v[h->axis_index[2]],
	};
}

/*!
 * The fusion only removes gyro bias on request: estimate it whenever the
 * headset has been still for a second (bias at rest is ~0.01 rad/s).
 */
static void
update_gyro_bias(struct quest1_hmd *h, uint64_t ts_ns, const struct xrt_vec3 *accel, const struct xrt_vec3 *gyro)
{
	struct xrt_vec3 d = m_vec3_sub(*gyro, h->imu.gyro_bias);
	bool still = m_vec3_len(d) < (h->imu.bias_valid ? 0.03f : 0.1f) &&
	             fabsf(m_vec3_len(*accel) - (float)MATH_GRAVITY_M_S2) < 0.5f;
	if (!still) {
		h->imu.still_since_ns = 0;
		return;
	}
	if (h->imu.still_since_ns == 0) {
		h->imu.still_since_ns = ts_ns;
	}
	if (ts_ns - h->imu.still_since_ns < U_TIME_1S_IN_NS) {
		return;
	}
	float alpha = h->imu.bias_valid ? 0.001f : 0.05f;
	h->imu.gyro_bias = m_vec3_add(h->imu.gyro_bias, m_vec3_mul_scalar(d, alpha));
	h->imu.bias_valid = true;
}

static void
handle_imu(struct quest1_hmd *h, const struct sb_imu_event *e)
{
	uint64_t imu_ns = e->timestamp_us * U_TIME_1US_IN_NS;
	int64_t now = os_monotonic_get_ns();

	// The IMU clock is the MCU's: map it to monotonic with the smallest observed
	// delivery delay, letting it creep up slowly to follow drift.
	int64_t offset = now - (int64_t)imu_ns;
	if (!h->imu.have_offset || offset < h->imu.clock_offset_ns) {
		h->imu.clock_offset_ns = offset;
		h->imu.have_offset = true;
	} else {
		h->imu.clock_offset_ns += (offset - h->imu.clock_offset_ns) / 4096;
	}

	struct xrt_vec3 accel = remap(h, e->accel);
	struct xrt_vec3 gyro = remap(h, e->gyro);
	update_gyro_bias(h, imu_ns, &accel, &gyro);
	struct xrt_vec3 gyro_unbiased = m_vec3_sub(gyro, h->imu.gyro_bias);

	m_imu_3dof_update(&h->imu.fusion, imu_ns, &accel, &gyro_unbiased);
	h->imu.samples++;

	struct xrt_space_relation rel = XRT_SPACE_RELATION_ZERO;
	rel.pose.orientation = h->imu.fusion.rot;
	// Angular velocity is in the base space.
	math_quat_rotate_vec3(&rel.pose.orientation, &gyro_unbiased, &rel.angular_velocity);
	rel.relation_flags = (enum xrt_space_relation_flags)(
	    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
	    XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT);
	m_relation_history_push(h->history, &rel, (int64_t)imu_ns + h->imu.clock_offset_ns);

	if (h->imu.samples % 5000 == 0) {
		Q1_DEBUG(h, "imu %llu samples, bias [%.4f %.4f %.4f], %.1f °C",
		         (unsigned long long)h->imu.samples, h->imu.gyro_bias.x, h->imu.gyro_bias.y,
		         h->imu.gyro_bias.z, e->temperature);
	}
}

static void *
imu_thread(void *ptr)
{
	struct quest1_hmd *h = ptr;
	os_thread_helper_name(&h->oth, "Quest1 IMU");

	os_thread_helper_lock(&h->oth);
	while (os_thread_helper_is_running_locked(&h->oth)) {
		os_thread_helper_unlock(&h->oth);

		struct sb_record rec;
		int r = h->sb.wait(h->sb_handle, 100, &rec);
		if (r == 0 && rec.type == 0) {
			struct sb_imu_event e;
			memcpy(&e, rec.data, sizeof(e));
			handle_imu(h, &e);
		} else if (r != 0 && r != -11) {
			Q1_ERROR(h, "syncboss_wait_on_stream_data_exclusive: %d", r);
			os_nanosleep(100 * U_TIME_1MS_IN_NS);
		}

		os_thread_helper_lock(&h->oth);
	}
	os_thread_helper_unlock(&h->oth);
	return NULL;
}

static xrt_result_t
hmd_get_tracked_pose(struct xrt_device *xdev,
                     enum xrt_input_name name,
                     int64_t at_timestamp_ns,
                     struct xrt_space_relation *out_relation)
{
	struct quest1_hmd *h = q1(xdev);
	if (name != XRT_INPUT_GENERIC_HEAD_POSE) {
		U_LOG_XDEV_UNSUPPORTED_INPUT(&h->base, h->log_level, name);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	struct xrt_space_relation rel = XRT_SPACE_RELATION_ZERO;
	if (m_relation_history_get(h->history, at_timestamp_ns, &rel) == M_RELATION_HISTORY_RESULT_INVALID) {
		rel.pose.orientation.w = 1.0f;
		rel.relation_flags = (enum xrt_space_relation_flags)(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
		                                                     XRT_SPACE_RELATION_POSITION_VALID_BIT);
	}
	*out_relation = rel;
	return XRT_SUCCESS;
}

static void
hmd_destroy(struct xrt_device *xdev)
{
	struct quest1_hmd *h = q1(xdev);
	os_thread_helper_destroy(&h->oth);
	if (h->sb_handle != NULL) {
		if (h->sb.imu_disable != NULL) {
			h->sb.imu_disable(h->sb_handle);
		}
		if (h->sb.deinit != NULL) {
			h->sb.deinit(h->sb_handle);
		}
	}
	u_var_remove_root(h);
	m_imu_3dof_close(&h->imu.fusion);
	m_relation_history_destroy(&h->history);
	u_device_free(&h->base);
}

/*
 *
 * Lens distortion: Meta's runtime defaults for the Quest 1 (Oculus SDK
 * LensConfig, CatmullRom10), see docs/quest1-lens.md.
 *
 */

#define Q1_PANEL_W_M 0.0594f
#define Q1_PANEL_H_M 0.0660f
#define Q1_METERS_PER_TAN_ANGLE 0.0389f
#define Q1_LENS_TO_SCREEN_X 0.002986f // panel centre - lens centre, mirrored per eye
#define Q1_LENS_TO_SCREEN_Y -0.00272f

static const float q1_k[11] = {1.0f,    1.0374f, 1.0810f, 1.1330f, 1.1970f, 1.2754f,
                               1.3771f, 1.5133f, 1.7018f, 1.9732f, 2.3000f};

//! Scale from screen radius² to tan-angle, exactly as the runtime evaluates it (MaxR = 1).
static float
q1_catmull_rom10(float rsq)
{
	const float *K = q1_k;
	float f = 10.0f * rsq;
	int k = (int)floorf(f);
	k = k < 0 ? 0 : (k > 10 ? 10 : k);
	float t = f - (float)k;
	float p0, m0, p1, m1;
	if (k == 0) {
		p0 = 1.0f;
		m0 = K[1] - K[0];
		p1 = K[1];
		m1 = 0.5f * (K[2] - K[0]);
	} else if (k < 9) {
		p0 = K[k];
		m0 = 0.5f * (K[k + 1] - K[k - 1]);
		p1 = K[k + 1];
		m1 = 0.5f * (K[k + 2] - K[k]);
	} else if (k == 9) {
		p0 = K[9];
		m0 = 0.5f * (K[10] - K[8]);
		p1 = K[10];
		m1 = K[10] - K[9];
	} else {
		p0 = K[10];
		m0 = K[10] - K[9];
		p1 = p0 + m0;
		m1 = m0;
	}
	float omt = 1.0f - t;
	return (p0 * (1.0f + 2.0f * t) + m0 * t) * omt * omt + (p1 * (1.0f + 2.0f * omt) - m1 * omt) * t * t;
}

static xrt_result_t
hmd_compute_distortion(struct xrt_device *xdev, uint32_t view, float u, float v, struct xrt_uv_triplet *out)
{
	const struct xrt_fov *fov = &xdev->hmd->distortion.fov[view];

	// Point on this eye's panel relative to the lens centre, y up, in tan units at the centre.
	float sx = view == 0 ? -1.0f : 1.0f;
	float x = ((u - 0.5f) * Q1_PANEL_W_M + sx * Q1_LENS_TO_SCREEN_X) / Q1_METERS_PER_TAN_ANGLE;
	float y = ((0.5f - v) * Q1_PANEL_H_M + Q1_LENS_TO_SCREEN_Y) / Q1_METERS_PER_TAN_ANGLE;
	float rsq = x * x + y * y;
	float s = q1_catmull_rom10(rsq);

	float tl = tanf(fov->angle_left), tr = tanf(fov->angle_right);
	float tu = tanf(fov->angle_up), td = tanf(fov->angle_down);
	const float chroma[3] = {0.996f - 0.007f * rsq, 1.0f, 1.006f + 0.020f * rsq};
	struct xrt_vec2 *uv[3] = {&out->r, &out->g, &out->b};
	for (int c = 0; c < 3; c++) {
		float tx = x * s * chroma[c], ty = y * s * chroma[c];
		uv[c]->x = (tx - tl) / (tr - tl);
		uv[c]->y = (tu - ty) / (tu - td);
	}
	return XRT_SUCCESS;
}

static void
setup_display(struct quest1_hmd *h)
{
	struct xrt_hmd_parts *hmd = h->base.hmd;

	hmd->screens[0].w_pixels = 2 * Q1_PANEL_W;
	hmd->screens[0].h_pixels = Q1_PANEL_H;
	hmd->screens[0].nominal_frame_interval_ns = U_TIME_1S_IN_NS / Q1_REFRESH_HZ;
	hmd->view_count = 2;

	// Meta's runtime: up 47°, down 53°, outer 52°, inner 42°.
	const struct xrt_fov fov_left = {.angle_left = DEG_TO_RAD(-52.0), .angle_right = DEG_TO_RAD(42.0),
	                                 .angle_up = DEG_TO_RAD(47.0), .angle_down = DEG_TO_RAD(-53.0)};
	for (int e = 0; e < 2; e++) {
		hmd->distortion.fov[e] = fov_left;
		if (e == 1) { // mirror for the right eye
			hmd->distortion.fov[e].angle_left = -fov_left.angle_right;
			hmd->distortion.fov[e].angle_right = -fov_left.angle_left;
		}

		// The panels show the framebuffer rotated by 180°: the left eye sees the
		// right half, upside down.
		hmd->views[e].display.w_pixels = Q1_PANEL_W;
		hmd->views[e].display.h_pixels = Q1_PANEL_H;
		hmd->views[e].viewport.x_pixels = e == 0 ? Q1_PANEL_W : 0;
		hmd->views[e].viewport.y_pixels = 0;
		hmd->views[e].viewport.w_pixels = Q1_PANEL_W;
		hmd->views[e].viewport.h_pixels = Q1_PANEL_H;
		hmd->views[e].rot = u_device_rotation_180;
	}

	size_t idx = 0;
	hmd->blend_modes[idx++] = XRT_BLEND_MODE_OPAQUE;
	hmd->blend_mode_count = idx;

	hmd->distortion.models = XRT_DISTORTION_MODEL_COMPUTE;
	hmd->distortion.preferred = XRT_DISTORTION_MODEL_COMPUTE;
	h->base.compute_distortion = hmd_compute_distortion;
	u_distortion_mesh_fill_in_compute(&h->base);
}

bool
quest1_detect(void)
{
	return access("/dev/syncboss0", F_OK) == 0;
}

struct xrt_device *
quest1_hmd_create(void)
{
	enum u_device_alloc_flags flags = (enum u_device_alloc_flags)(U_DEVICE_ALLOC_HMD | U_DEVICE_ALLOC_TRACKING_NONE);
	struct quest1_hmd *h = U_DEVICE_ALLOCATE(struct quest1_hmd, flags, 1, 0);
	h->log_level = debug_get_log_option_quest1_log();

	h->base.update_inputs = u_device_noop_update_inputs;
	h->base.get_tracked_pose = hmd_get_tracked_pose;
	h->base.get_view_poses = u_device_get_view_poses;
	h->base.get_visibility_mask = u_device_get_visibility_mask;
	h->base.destroy = hmd_destroy;
	h->base.name = XRT_DEVICE_GENERIC_HMD;
	h->base.device_type = XRT_DEVICE_TYPE_HMD;
	h->base.inputs[0].name = XRT_INPUT_GENERIC_HEAD_POSE;
	h->base.supported.orientation_tracking = true;
	h->base.supported.position_tracking = false;
	snprintf(h->base.str, XRT_DEVICE_NAME_LEN, "Oculus Quest (native)");
	snprintf(h->base.serial, XRT_DEVICE_NAME_LEN, "quest1-hmd");
	h->base.tracking_origin->type = XRT_TRACKING_TYPE_OTHER;
	snprintf(h->base.tracking_origin->name, XRT_TRACKING_NAME_LEN, "Quest 1 IMU");

	m_imu_3dof_init(&h->imu.fusion, M_IMU_3DOF_USE_GRAVITY_DUR_20MS);
	m_relation_history_create(&h->history);
	os_thread_helper_init(&h->oth);
	setup_display(h);

	const char *axes = debug_get_option_quest1_imu_axes();
	if (!parse_axes(h, axes)) {
		Q1_ERROR(h, "bad QUEST1_IMU_AXES '%s', expected e.g. '-y,x,z'", axes);
		goto err;
	}

	if (!sb_load(&h->sb)) {
		Q1_ERROR(h, "cannot load libsyncboss.so through libhybris (Android blobs mounted? LD_PRELOAD=libbionictls.so?)");
		goto err;
	}
	// syncboss_init_options_t: [2] interface version (must be 1), [3] disable
	// telemetry (it waits forever for Android's hwservicemanager).
	uint8_t opts[16] = {0, 0, 1, 1};
	int r = h->sb.init(&h->sb_handle, opts);
	if (r != 0) {
		Q1_ERROR(h, "syncboss_init: %d", r);
		h->sb_handle = NULL;
		goto err;
	}
	r = h->sb.imu_enable(h->sb_handle);
	if (r != 0) {
		Q1_ERROR(h, "syncboss_imu_enable: %d", r);
		goto err;
	}
	if (os_thread_helper_start(&h->oth, imu_thread, h) != 0) {
		Q1_ERROR(h, "cannot start the IMU thread");
		goto err;
	}

	u_var_add_root(h, "Quest 1 HMD", true);
	u_var_add_log_level(h, &h->log_level, "log_level");
	u_var_add_ro_vec3_f32(h, &h->imu.gyro_bias, "gyro_bias");
	m_imu_3dof_add_vars(&h->imu.fusion, h, "fusion.");

	Q1_INFO(h, "IMU running (axes %s)", axes);
	return &h->base;

err:
	hmd_destroy(&h->base);
	return NULL;
}
