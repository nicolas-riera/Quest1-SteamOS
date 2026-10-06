// Copyright 2026, Quest1-SteamOS contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Builder for the native Quest 1 devices.
 * @ingroup xrt_iface
 */

#include "xrt/xrt_config_drivers.h"
#include "xrt/xrt_prober.h"
#include "xrt/xrt_system.h"

#include "util/u_logging.h"
#include "util/u_misc.h"

#include "target_builder_helpers.h"
#include "target_builder_interface.h"

#include "quest1/quest1_interface.h"

static const char *driver_list[] = {
    "quest1",
};

static xrt_result_t
quest1_estimate_system(struct xrt_builder *xb,
                       cJSON *config,
                       struct xrt_prober *xp,
                       struct xrt_builder_estimate *estimate)
{
	if (quest1_detect()) {
		estimate->certain.head = true;
		estimate->priority = 50;
	}
	return XRT_SUCCESS;
}

static xrt_result_t
quest1_open_system_impl(struct xrt_builder *xb,
                        cJSON *config,
                        struct xrt_prober *xp,
                        struct xrt_tracking_origin *origin,
                        struct xrt_system_devices *xsysd,
                        struct xrt_frame_context *xfctx,
                        struct t_builder_options *tbo)
{
	struct xrt_device *head = quest1_hmd_create();
	if (head == NULL) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	// The HMD first: it owns the SyncBoss thread that feeds the controllers, and the system
	// destroys static devices in order.
	xsysd->static_xdevs[xsysd->static_xdev_count++] = head;
	tbo->head = head;

	struct xrt_device *left = NULL, *right = NULL;
	if (quest1_hmd_get_controllers(head, &left, &right)) {
		xsysd->static_xdevs[xsysd->static_xdev_count++] = left;
		xsysd->static_xdevs[xsysd->static_xdev_count++] = right;
		tbo->left = left;
		tbo->right = right;
	}
	return XRT_SUCCESS;
}

static void
quest1_destroy(struct xrt_builder *xb)
{
	free(xb);
}

struct xrt_builder *
t_builder_quest1_create(void)
{
	struct t_builder *ub = U_TYPED_CALLOC(struct t_builder);
	ub->base.estimate_system = quest1_estimate_system;
	ub->base.open_system = t_builder_open_system_static_roles;
	ub->base.destroy = quest1_destroy;
	ub->base.identifier = "quest1";
	ub->base.name = "Oculus Quest 1 (native)";
	ub->base.driver_identifiers = driver_list;
	ub->base.driver_identifier_count = ARRAY_SIZE(driver_list);
	ub->base.exclude_from_automatic_discovery = false;
	ub->open_system_static_roles = quest1_open_system_impl;
	return &ub->base;
}
