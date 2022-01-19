/*
 * Copyright 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <getopt.h>

#include "bs_drm.h"

#define NUM_BUFFERS 2

typedef struct drm_property_value {
	char property_name[15];
	uint64_t value;
} drm_property_value_t;

struct bs_drm_pipe drm_pipe = { 0 };

struct bs_egl_fb *egl_fbs[NUM_BUFFERS];
EGLImageKHR egl_images[NUM_BUFFERS];
struct bs_egl *egl = NULL;
uint32_t ids[NUM_BUFFERS];

static uint32_t allowed_formats[] = {
	GBM_FORMAT_XRGB8888,
	GBM_FORMAT_XBGR8888,
	GBM_FORMAT_XRGB2101010,
	GBM_FORMAT_XBGR2101010,
	GBM_FORMAT_ARGB2101010,
	GBM_FORMAT_ABGR2101010,
};


static const struct option longopts[] = {
	{ "crtc_index", required_argument, NULL, 'c' },
	{ "format", required_argument, NULL, 'f' },
	{ "modifier", required_argument, NULL, 'm' },
	{ "test-page-flip-format-change", required_argument, NULL, 'p' },
	{ "banding", no_argument, NULL, 'b' },
	{ "help", no_argument, NULL, 'h' },
	{ "display", required_argument, NULL, 'd' },
	{ "renderer", required_argument, NULL, 'r' },
	{ 0, 0, 0, 0 },
};

const size_t allowed_formats_length = BS_ARRAY_LEN(allowed_formats);

static GLuint solid_shader_create()
{
	const GLchar *vert =
	    "attribute vec4 vPosition;\n"
	    "attribute vec4 vColor;\n"
	    "varying vec4 vFillColor;\n"
	    "void main() {\n"
	    "  gl_Position = vPosition;\n"
	    "  vFillColor = vColor;\n"
	    "}\n";

	const GLchar *frag =
	    "precision mediump float;\n"
	    "varying vec4 vFillColor;\n"
	    "void main() {\n"
	    "  gl_FragColor = vFillColor;\n"
	    "}\n";

	struct bs_gl_program_create_binding bindings[] = {
		{ 0, "vPosition" },
		{ 1, "vColor" },
		{ 0, NULL },
	};

	return bs_gl_program_create_vert_frag_bind(vert, frag, bindings);
}

static float f(int i)
{
	int a = i % 40;
	int b = (i / 40) % 6;
	switch (b) {
		case 0:
		case 1:
			return 0.0f;
		case 3:
		case 4:
			return 1.0f;
		case 2:
			return (a / 40.0f);
		case 5:
			return 1.0f - (a / 40.0f);
		default:
			return 0.0f;
	}
}

static uint32_t find_format(char *fourcc)
{
	if (!fourcc || strlen(fourcc) < 4)
		return 0;

	uint32_t format = fourcc_code(fourcc[0], fourcc[1], fourcc[2], fourcc[3]);
	for (int i = 0; i < allowed_formats_length; i++) {
		if (allowed_formats[i] == format)
			return format;
	}

	return 0;
}

static void print_help(const char *argv0, int drm_device_fd)
{
	char allowed_formats_string[allowed_formats_length * 6];
	int i;
	for (i = 0; i < allowed_formats_length; i++) {
		uint32_t format = allowed_formats[i];
		sprintf(allowed_formats_string + i * 6, "%.4s, ", (char *)&format);
	}

	allowed_formats_string[i * 6 - 2] = 0;

	// clang-format off
	printf("usage: %s [OPTIONS] [drm_device_path]\n", argv0);
	printf("  -f, --format <format>                        defines the fb format.\n");
	printf("  -m, --modifier <modifier>                    pass modifiers.\n");
	printf("  -p, --test-page-flip-format-change <format>  test page flips alternating formats.\n");
	printf("  -b, --banding                                show a pattern that makes banding easy to spot if present.\n");
	printf("  -c, --crtc_index <crtc_index>                CRTC index to use.\n");
	printf("  -h, --help                                   show help\n");
	printf("  -d, --display <drm_driver_name>              dri driver name for display.\n");
	printf("  -r, --renderer <drm_driver_name>             dri driver name for renderer.\n");
	printf("\n");
	printf(" <format> must be one of [ %s ].\n", allowed_formats_string);
	printf(" <modifier> must be one of ");
	bs_print_supported_modifiers(drm_device_fd);
	printf("\n");
	printf("\n");
	// clang-format on
}

static bool draw_gl(drmModeModeInfo *mode, int fb_idx, int color_idx, GLuint program,
		    bool banding_pattern)
{
	// clang-format off
	const GLfloat triangle_vertices[] = {
			0.0f, -0.5f, 0.0f,
			-0.5f, 0.5f, 0.0f,
			0.5f, 0.5f, 0.0f
		};
	const GLfloat triangle_colors[] = {
			1.0f, 0.0f, 0.0f, 1.0f,
			0.0f, 1.0f, 0.0f, 1.0f,
			0.0f, 0.0f, 1.0f, 1.0f
		};
	const GLfloat square_vertices[] = {
		-1.0f, -1.0f, 0.0f,
		 1.0f, -1.0f, 0.0f,
		-1.0f,  1.0f, 0.0f,
		 1.0f,  1.0f, 0.0f,
	};
	const GLfloat square_colors[] = {
		 0.05f, 0.05f, 0.05f, 1.0f,
		 0.05f, 0.05f, 0.05f, 1.0f,
		 0.45f, 0.45f, 0.45f, 1.0f,
		 0.45f, 0.45f, 0.45f, 1.0f,
	};

	glBindFramebuffer(GL_FRAMEBUFFER, bs_egl_fb_name(egl_fbs[fb_idx]));
	glViewport(0, 0, (GLint)mode->hdisplay, (GLint)mode->vdisplay);

	glClearColor(f(color_idx), f(color_idx + 80), f(color_idx + 160), 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(program);
	if (banding_pattern) {
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, square_vertices);
		glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, square_colors);
	} else {
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, triangle_vertices);
		glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, triangle_colors);
	}
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glDrawArrays(GL_TRIANGLES, 0, 3);

	usleep(1e6 / 120 /* 120 Hz */);
	glFinish();

	if (!bs_egl_image_flush_external(egl, egl_images[fb_idx])) {
		bs_debug_error("failed to call image_flush_external");
		return false;
	}

	return true;
}

bool check_if_fd_valid(int fd)
{
	if (fd < 0) {
		bs_debug_error("fd is not valid.");
		return false;
	}
	if (fcntl(fd, F_GETFD, 0) < 0) {
		bs_debug_error("Unable to return the fd flags.");
		return false;
	}
	return true;
}

static int test_atomic(int drm_device_fd, drmModeModeInfo *mode, GLuint program,
		       bool banding_pattern)
{
	uint32_t connector_id = drm_pipe.connector_id;
	uint32_t crtc_id = drm_pipe.crtc_id;
	uint32_t plane_id = bs_get_plane_id(drm_device_fd, crtc_id);
	if (plane_id < 1) {
		bs_debug_error("failed to get Plane ID.");
		return -1;
	}

	uint32_t mode_blob_id = 0;
	drmModeCreatePropertyBlob(drm_device_fd, mode, sizeof(drmModeModeInfo), &mode_blob_id);
	assert(mode_blob_id);

	int res = drmSetClientCap(drm_device_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	assert(!res);

	uint32_t flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_ALLOW_MODESET;
	drmModeAtomicReqPtr atomic_req = drmModeAtomicAlloc();
	int cursor = drmModeAtomicGetCursor(atomic_req);

	const drm_property_value_t connector_props[] = { { "CRTC_ID", crtc_id } };
	for (size_t i = 0; i < BS_ARRAY_LEN(connector_props); ++i) {
		uint32_t prop =
		    bs_drm_find_property_id(drm_device_fd, connector_id, DRM_MODE_OBJECT_CONNECTOR,
					    connector_props[i].property_name);
		drmModeAtomicAddProperty(atomic_req, connector_id, prop, connector_props[i].value);
	}

	int out_fence_fd = -1;
	const drm_property_value_t crtc_props[] = { { "OUT_FENCE_PTR", (uint64_t)&out_fence_fd },
						    { "MODE_ID", mode_blob_id },
						    { "ACTIVE", 1 } };
	for (size_t i = 0; i < BS_ARRAY_LEN(crtc_props); ++i) {
		uint32_t prop = bs_drm_find_property_id(
		    drm_device_fd, crtc_id, DRM_MODE_OBJECT_CRTC, crtc_props[i].property_name);
		drmModeAtomicAddProperty(atomic_req, crtc_id, prop, crtc_props[i].value);
	}

	const drm_property_value_t plane_props[] = {
		{ "CRTC_ID", crtc_id },
		{ "CRTC_X", 0 },
		{ "CRTC_Y", 0 },
		{ "CRTC_W", mode->hdisplay },
		{ "CRTC_H", mode->vdisplay },
		{ "SRC_X", 0 },
		{ "SRC_Y", 0 },
		{ "SRC_W", mode->hdisplay << 16 },
		{ "SRC_H", mode->vdisplay << 16 },
	};
	for (size_t i = 0; i < BS_ARRAY_LEN(plane_props); ++i) {
		uint32_t prop = bs_drm_find_property_id(
		    drm_device_fd, plane_id, DRM_MODE_OBJECT_PLANE, plane_props[i].property_name);
		drmModeAtomicAddProperty(atomic_req, plane_id, prop, plane_props[i].value);
	}

	uint32_t fb_propery_id =
	    bs_drm_find_property_id(drm_device_fd, plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");

	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(drm_device_fd, &fds);

	for (int i = 0; i <= 500; i++) {
		int fb_idx = i % 2;
		if (!draw_gl(mode, fb_idx, i, program, banding_pattern))
			return false;

		drmModeAtomicAddProperty(atomic_req, plane_id, fb_propery_id, ids[fb_idx]);
		res = drmModeAtomicCommit(drm_device_fd, atomic_req, flags, NULL);
		assert(!res);

		flags = DRM_MODE_PAGE_FLIP_EVENT;
		drmModeAtomicSetCursor(atomic_req, cursor);
		drmModeAtomicFree(atomic_req);
		atomic_req = drmModeAtomicAlloc();

		check_if_fd_valid(out_fence_fd);

		select(drm_device_fd + 1, &fds, NULL, NULL, NULL);
		char buffer[1024];
		int len = read(drm_device_fd, buffer, sizeof(buffer));
		assert(len > 0);
	}

	drmModeDestroyPropertyBlob(drm_device_fd, mode_blob_id);
	return true;
}

static void page_flip_handler(int drm_device_fd, unsigned int frame, unsigned int sec,
			      unsigned int usec, void *data)
{
	int *waiting_for_flip = data;
	*waiting_for_flip = 0;
}

static bool test_non_atomic(int drm_device_fd, drmModeModeInfo *mode, GLuint program,
			    bool banding_pattern)
{
	int ret = drmModeSetCrtc(drm_device_fd, drm_pipe.crtc_id, ids[0], 0 /* x */, 0 /* y */,
				 &drm_pipe.connector_id, 1 /* connector count */, mode);
	if (ret) {
		bs_debug_error("failed to set CRTC");
		return false;
	}

	// NOTE: Loop starts from 1 as `fb_idx` should not be 0. ids[0] is used in drmModeSetCrtc
	// which gets locked on some devices, so we have to draw to another buffer (ids[1]) and
	// commit it first to release [0].
	for (int i = 1; i <= 500; i++) {
		int waiting_for_flip = 1;
		int fb_idx = i % 2;
		if (!draw_gl(mode, i % 2, i, program, banding_pattern))
			return false;

		int ret = drmModePageFlip(drm_device_fd, drm_pipe.crtc_id, ids[fb_idx],
					  DRM_MODE_PAGE_FLIP_EVENT, &waiting_for_flip);
		if (ret) {
			bs_debug_error("failed page flip: %d", ret);
			return false;
		}

		while (waiting_for_flip) {
			drmEventContext evctx = {
				.version = DRM_EVENT_CONTEXT_VERSION,
				.page_flip_handler = page_flip_handler,
			};

			fd_set fds;
			FD_ZERO(&fds);
			FD_SET(drm_device_fd, &fds);

			ret = select(drm_device_fd + 1, &fds, NULL, NULL, NULL);
			if (ret < 0) {
				bs_debug_error("select err: %s", strerror(errno));
				return false;
			} else if (ret == 0) {
				bs_debug_error("select timeout");
				return false;
			}
			ret = drmHandleEvent(drm_device_fd, &evctx);
			if (ret) {
				bs_debug_error("failed to wait for page flip: %d", ret);
				return false;
			}
		}
	}
	return true;
}

struct check_driver_data {
	char *driver;
	int fd;
};

static bool check_driver(void *user, int fd)
{
	struct check_driver_data *data = (struct check_driver_data *)user;
	drmVersionPtr version;

	version = drmGetVersion(fd);
	if (!version)
		return false;

	if (strcmp(data->driver, version->name) != 0) {
		drmFreeVersion(version);
		return false;
	}

	drmFreeVersion(version);
	data->fd = dup(fd);
	return true;
}

int main(int argc, char **argv)
{
	bool help_flag = false;
	uint32_t format = GBM_FORMAT_XRGB8888;
	uint32_t test_page_flip_format_change = 0;
	uint64_t modifier = DRM_FORMAT_MOD_INVALID;
	bool banding_pattern = false;
	char *display_driver = NULL;
	char *render_driver = NULL;
	int32_t crtc_idx = -1;

	int c = -1;
	while ((c = getopt_long(argc, argv, "c:m:hp:f:bld:r:", longopts, NULL)) != -1) {
		switch (c) {
			case 'p':
				test_page_flip_format_change = find_format(optarg);
				if (!test_page_flip_format_change)
					help_flag = true;
				break;
			case 'f':
				format = find_format(optarg);
				if (!format) {
					bs_debug_error("unsupported format: %s", optarg);
					help_flag = true;
				}
				break;
			case 'm':
				modifier = bs_string_to_modifier(optarg);
				if (modifier == -1) {
					bs_debug_error("unsupported modifier: %s", optarg);
					help_flag = true;
				}
				break;
			case 'b':
				banding_pattern = true;
				break;
			case 'c':
				if (sscanf(optarg, "%d", &crtc_idx) != 1) {
					bs_debug_error("unsupported crtc index: %s", optarg);
					help_flag = true;
				}
				break;
			case 'h':
				help_flag = true;
				break;
			case 'd':
				if (display_driver) {
					free(display_driver);
					display_driver = NULL;
				}
				display_driver = strdup(optarg);
				break;
			case 'r':
				if (render_driver) {
					free(render_driver);
					render_driver = NULL;
				}
				render_driver = strdup(optarg);
				break;
		}
	}

	int drm_device_fd = -1;
	if (optind < argc) {
		drm_device_fd = open(argv[optind], O_RDWR);
		if (drm_device_fd < 0) {
			bs_debug_error("failed to open card %s", argv[optind]);
			return 1;
		}
	} else {
		if (display_driver) {
			struct check_driver_data data;
			data.driver = display_driver;
			data.fd = -1;
			bs_open_enumerate("/dev/dri/card%u", 0, DRM_MAX_MINOR,
					  check_driver, (void *)&data);
			drm_device_fd = data.fd;
		} else {
			drm_device_fd = bs_drm_open_main_display();
		}
		if (drm_device_fd < 0) {
			bs_debug_error("failed to open card for display");
			return 1;
		}
	}

	if (help_flag) {
		print_help(*argv, drm_device_fd);
		close(drm_device_fd);
		return 1;
	}

	struct gbm_device *gbm = gbm_create_device(drm_device_fd);
	if (!gbm) {
		bs_debug_error("failed to create gbm");
		return 1;
	}

	struct bs_drm_pipe_plumber *plumber = bs_drm_pipe_plumber_new();
	bs_drm_pipe_plumber_fd(plumber, drm_device_fd);
	if (crtc_idx >= 0)
		bs_drm_pipe_plumber_crtc_mask(plumber, 1 << crtc_idx);
	if (!bs_drm_pipe_plumber_make(plumber, &drm_pipe)) {
		bs_debug_error("failed to make drm_pipe");
		return 1;
	}
	bs_drm_pipe_plumber_destroy(&plumber);

	drmModeConnector *connector = drmModeGetConnector(drm_device_fd, drm_pipe.connector_id);
	assert(connector);
	drmModeModeInfo *mode = &connector->modes[0];

	egl = bs_egl_new();
	if (!bs_egl_setup(egl, render_driver)) {
		bs_debug_error("failed to setup egl context");
		return 1;
	}

	struct gbm_bo *bos[NUM_BUFFERS];
	for (size_t fb_index = 0; fb_index < NUM_BUFFERS; fb_index++) {
		if (test_page_flip_format_change && fb_index) {
			format = test_page_flip_format_change;
		}

		if (modifier != DRM_FORMAT_MOD_INVALID) {
			bos[fb_index] = gbm_bo_create_with_modifiers(
			    gbm, mode->hdisplay, mode->vdisplay, format, &modifier, 1);
		} else {
			bos[fb_index] = gbm_bo_create(gbm, mode->hdisplay, mode->vdisplay, format,
						      GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
		}

		if (bos[fb_index] == NULL) {
			bs_debug_error("failed to allocate BO %s modifiers",
				(modifier != DRM_FORMAT_MOD_INVALID ? "with" : "without"));
			return 1;
		}

		ids[fb_index] = bs_drm_fb_create_gbm(bos[fb_index]);
		if (ids[fb_index] == 0) {
			bs_debug_error("failed to create framebuffer id");
			return 1;
		}

		EGLImageKHR egl_image = bs_egl_image_create_gbm(egl, bos[fb_index]);
		if (egl_image == EGL_NO_IMAGE_KHR) {
			bs_debug_error("failed to create EGLImageKHR from framebuffer");
			return 1;
		}

		egl_fbs[fb_index] = bs_egl_fb_new(egl, egl_image);
		if (!egl_fbs[fb_index]) {
			bs_debug_error("failed to create framebuffer from EGLImageKHR");
			return 1;
		}
		egl_images[fb_index] = egl_image;
	}

	GLuint program = solid_shader_create();
	if (!program) {
		bs_debug_error("failed to create solid shader");
		return 1;
	}

	drmModeRes *res = drmModeGetResources(drm_device_fd);
	if (!res) {
		bs_debug_error("failed to get DRM resources");
		return 1;
	}
	/* Disable all CRTCs first, in case there are dangling connections. */
	for (int crtc_index = 0; crtc_index < res->count_crtcs; crtc_index++) {
		int ret = drmModeSetCrtc(drm_device_fd, res->crtcs[crtc_index],
					 0 /* bufferId */, 0 /* x */, 0 /* y */,
					 NULL, 0 /* connector count */, NULL);
		if (ret) {
			bs_debug_error("failed to disable CRTC:%d: %s",
				       res->crtcs[crtc_index], strerror(errno));
			return 1;
		}
	}
	drmModeFreeResources(res);

	struct drm_set_client_cap cap = { DRM_CLIENT_CAP_ATOMIC, 1 };
	bool has_atomic_capabilities = !drmIoctl(drm_device_fd, DRM_IOCTL_SET_CLIENT_CAP, &cap);
	if (has_atomic_capabilities) {
		if (!test_atomic(drm_device_fd, mode, program, banding_pattern))
			return 1;
	} else {
		printf("Running Legacy API. Atomic API is not supported.\n");
		if (!test_non_atomic(drm_device_fd, mode, program, banding_pattern))
			return 1;
	}

	for (size_t fb_index = 0; fb_index < NUM_BUFFERS; fb_index++) {
		bs_egl_fb_destroy(&egl_fbs[fb_index]);
		bs_egl_image_destroy(egl, &egl_images[fb_index]);
		drmModeRmFB(drm_device_fd, ids[fb_index]);
		gbm_bo_destroy(bos[fb_index]);
	}

	bs_egl_destroy(&egl);
	gbm_device_destroy(gbm);
	close(drm_device_fd);
	return 0;
}
