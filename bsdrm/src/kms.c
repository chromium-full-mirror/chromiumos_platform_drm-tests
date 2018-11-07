/*
 * Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include "bs_drm.h"

#include <drm_fourcc.h>
#include <stdint.h>
#include <stdio.h>
#include <xf86drmMode.h>

struct modifier_table_t {
	char *name;
	uint64_t value;
};

#define MODIFIER_FORMAT(f) { #f, f }
static const struct modifier_table_t modifier_table[] = {
	MODIFIER_FORMAT(DRM_FORMAT_MOD_INVALID),
	MODIFIER_FORMAT(DRM_FORMAT_MOD_LINEAR),
	MODIFIER_FORMAT(I915_FORMAT_MOD_X_TILED),
	MODIFIER_FORMAT(I915_FORMAT_MOD_Y_TILED),
	MODIFIER_FORMAT(I915_FORMAT_MOD_Yf_TILED),
	MODIFIER_FORMAT(I915_FORMAT_MOD_Y_TILED_CCS),
	MODIFIER_FORMAT(I915_FORMAT_MOD_Yf_TILED_CCS),
	MODIFIER_FORMAT(DRM_FORMAT_MOD_SAMSUNG_64_32_TILE),
#ifdef DRM_FORMAT_MOD_QCOM_COMPRESSED
	MODIFIER_FORMAT(DRM_FORMAT_MOD_QCOM_COMPRESSED),
#endif
	MODIFIER_FORMAT(DRM_FORMAT_MOD_VIVANTE_TILED),
	MODIFIER_FORMAT(DRM_FORMAT_MOD_VIVANTE_SUPER_TILED),
	MODIFIER_FORMAT(DRM_FORMAT_MOD_VIVANTE_SPLIT_TILED),
	MODIFIER_FORMAT(DRM_FORMAT_MOD_VIVANTE_SPLIT_SUPER_TILED),
	MODIFIER_FORMAT(DRM_FORMAT_MOD_NVIDIA_TEGRA_TILED),
	MODIFIER_FORMAT(DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(0)),
	MODIFIER_FORMAT(DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(1)),
	MODIFIER_FORMAT(DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(2)),
	MODIFIER_FORMAT(DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(3)),
	MODIFIER_FORMAT(DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(4)),
	MODIFIER_FORMAT(DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(5)),
	MODIFIER_FORMAT(DRM_FORMAT_MOD_BROADCOM_VC4_T_TILED),
};
#undef MODIFIER_FORMAT

static char *modifier_to_string(uint64_t modifier)
{
	for (int i = 0; i < BS_ARRAY_LEN(modifier_table); i++) {
		if (modifier_table[i].value == modifier)
			return modifier_table[i].name;
	}

	return "UNKNOWN";
}

static void add_modifiers(int fd, uint32_t blob_id, uint64_t *modifier_list)
{
	drmModePropertyBlobPtr blob = drmModeGetPropertyBlob(fd, blob_id);
	if (blob) {
		struct drm_format_modifier_blob *header = blob->data;
		struct drm_format_modifier *modifiers;

		modifiers =
		    (struct drm_format_modifier *)((char *)header + header->modifiers_offset);

		for (int i = 0; i < header->count_modifiers; i++) {
			for (int j = 0; j < 64; j++) {
				if (modifier_list[j] == modifiers[i].modifier) {
					break;
				} else if (modifier_list[j] == 0) {
					modifier_list[j] = modifiers[i].modifier;
					break;
				}
			}
		}
	}

	drmModeFreePropertyBlob(blob);
}

void bs_print_supported_modifiers(int fd)
{
	uint64_t supported_modifier_list[64] = { 0 };

	drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);

	drmModePlaneResPtr plane_res = drmModeGetPlaneResources(fd);
	if (plane_res) {
		for (int i = 0; i < plane_res->count_planes; i++) {
			drmModePlanePtr plane = drmModeGetPlane(fd, plane_res->planes[i]);
			if (plane) {
				drmModeObjectPropertiesPtr properties = drmModeObjectGetProperties(
				    fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
				if (properties) {
					for (int j = 0; j < properties->count_props; j++) {
						drmModePropertyPtr prop =
						    drmModeGetProperty(fd, properties->props[j]);
						if (strcmp(prop->name, "IN_FORMATS") == 0)
							add_modifiers(fd,
								      properties->prop_values[j],
								      supported_modifier_list);

						drmModeFreeProperty(prop);
					}
				}
				drmModeFreeObjectProperties(properties);
			}
			drmModeFreePlane(plane);
		}
		drmModeFreePlaneResources(plane_res);
	}

	printf("[ ");
	for (int i = 0; i < 64; i++) {
		if (supported_modifier_list[i] == 0)
			break;
		printf("%s%s", i ? ", " : "", modifier_to_string(supported_modifier_list[i]));
	}
	printf(" ]\n");
}

bool bs_are_modifier_supported(int fd, int drm_plane_type)
{
	bool modifiers_supported = false;

	drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);

	drmModePlaneResPtr plane_res = drmModeGetPlaneResources(fd);
	for (int i = 0; plane_res && i < plane_res->count_planes && !modifiers_supported; i++) {
		drmModePlanePtr plane = drmModeGetPlane(fd, plane_res->planes[i]);
		if (plane) {
			drmModeObjectPropertiesPtr properties =
			    drmModeObjectGetProperties(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
			for (int j = 0; properties && j < properties->count_props; j++) {
				drmModePropertyPtr prop =
				    drmModeGetProperty(fd, properties->props[j]);
				if (strcmp(prop->name, "type") == 0 &&
				    properties->prop_values[0] != drm_plane_type) {
					drmModeFreeProperty(prop);
					break;
				}

				if (strcmp(prop->name, "IN_FORMATS") == 0)
					modifiers_supported = true;
				drmModeFreeProperty(prop);
			}
			drmModeFreeObjectProperties(properties);
		}
		drmModeFreePlane(plane);
	}
	drmModeFreePlaneResources(plane_res);

	return modifiers_supported;
}

uint64_t bs_string_to_modifier(char *modifier_str)
{
	for (int i = 0; i < BS_ARRAY_LEN(modifier_table); i++) {
		if (strcasecmp(modifier_str, modifier_table[i].name) == 0)
			return modifier_table[i].value;
	}

	return -1;
}
