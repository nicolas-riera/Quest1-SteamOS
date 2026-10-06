// Copyright 2026, qbridge contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Builder for the qbridge devices (Quest tracking through Meta's runtime).
 * @ingroup xrt_iface
 */

#include "xrt/xrt_config_drivers.h"
#include "xrt/xrt_prober.h"
#include "xrt/xrt_system.h"

#include "util/u_debug.h"
#include "util/u_logging.h"
#include "util/u_misc.h"

#include "target_builder_helpers.h"
#include "target_builder_interface.h"

#include "qbridge/qbridge_interface.h"

DEBUG_GET_ONCE_BOOL_OPTION(qbridge_enable, "QBRIDGE_ENABLE", false)
DEBUG_GET_ONCE_NUM_OPTION(qbridge_wait_ms, "QBRIDGE_WAIT_MS", 120000)

static const char *driver_list[] = {
    "qbridge",
};

static xrt_result_t
qbridge_estimate_system(struct xrt_builder *xb,
                        cJSON *config,
                        struct xrt_prober *xp,
                        struct xrt_builder_estimate *estimate)
{
	estimate->certain.head = true;
	estimate->certain.left = true;
	estimate->certain.right = true;
	estimate->priority = 100;
	return XRT_SUCCESS;
}

static xrt_result_t
qbridge_open_system_impl(struct xrt_builder *xb,
                         cJSON *config,
                         struct xrt_prober *xp,
                         struct xrt_tracking_origin *origin,
                         struct xrt_system_devices *xsysd,
                         struct xrt_frame_context *xfctx,
                         struct t_builder_options *tbo)
{
	struct qb_sys *s = qb_sys_get();
	if (s == NULL) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	U_LOG_I("qbridge: waiting for the qbridge app on the headset...");
	if (!qb_sys_wait_ready(s, (int)debug_get_num_option_qbridge_wait_ms())) {
		U_LOG_E("qbridge: the app did not connect");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct xrt_device *head = qbridge_hmd_create(s);
	struct xrt_device *left = qbridge_controller_create(s, 0, head->tracking_origin);
	struct xrt_device *right = qbridge_controller_create(s, 1, head->tracking_origin);

	xsysd->static_xdevs[xsysd->static_xdev_count++] = head;
	xsysd->static_xdevs[xsysd->static_xdev_count++] = left;
	xsysd->static_xdevs[xsysd->static_xdev_count++] = right;
	tbo->head = head;
	tbo->left = left;
	tbo->right = right;
	return XRT_SUCCESS;
}

static void
qbridge_destroy(struct xrt_builder *xb)
{
	free(xb);
}

struct xrt_builder *
t_builder_qbridge_create(void)
{
	struct t_builder *ub = U_TYPED_CALLOC(struct t_builder);
	ub->base.estimate_system = qbridge_estimate_system;
	ub->base.open_system = t_builder_open_system_static_roles;
	ub->base.destroy = qbridge_destroy;
	ub->base.identifier = "qbridge";
	ub->base.name = "Quest via Meta runtime (qbridge)";
	ub->base.driver_identifiers = driver_list;
	ub->base.driver_identifier_count = ARRAY_SIZE(driver_list);
	ub->base.exclude_from_automatic_discovery = !debug_get_bool_option_qbridge_enable();
	ub->open_system_static_roles = qbridge_open_system_impl;
	return &ub->base;
}
