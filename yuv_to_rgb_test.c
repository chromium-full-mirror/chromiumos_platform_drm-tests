/*
 * Copyright 2023 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/*
 * The yuv_to_rgb_test checks if the GPU (through OpenGL) can correctly convert
 * YUV samples to RGB samples. The tests consists of:
 *
 * - Importing a minigbm NV12 buffer as an EGL image and binding it to a GL
 *   texture.
 * - Drawing a triangle strip and sampling from the GL texture using the GPU.
 * - Reading the rendered RGB pixel data from the GL framebuffer.
 * - Comparing each rendered pixel against expected values.
 *
 * This is done for different YUV color spaces and ranges.
 */

#include <gbm.h>
#include <getopt.h>

#include "bs_drm.h"

#define ARRAY_SIZE(A) (sizeof(A) / sizeof(*(A)))
#define NUM_BYTES_PER_RGBA_PIXEL 4
#define NUM_PLANES_NV12 2
#define STRINGIFY(x) \
	case x:      \
		return #x

static const uint32_t width = 4;
static const uint32_t height = 4;

static const int num_color_components = 4;
static const int rgb_value_tolerance = 3;

// clang-format off
static uint8_t nv12_y[] = {
	/* Y */
	50,  70,  90, 110,
	50,  70,  90, 110,
	50,  70,  90, 110,
	50,  70,  90, 110,
};

static uint8_t nv12_uv[] = {
	/* UV */
	120, 130, 140, 130,
	120, 160, 140, 160,
};

static uint8_t expected_rec601_narrow[] = {
	 43,  41,  23, 255,
	 66,  64,  47, 255,
	 89,  80, 110, 255,
	113, 103, 134, 255,

	 43,  41,  23, 255,
	 66,  64,  47, 255,
	 89,  80, 110, 255,
	113, 103, 134, 255,

	 91,  17,  23, 255,
	114,  40,  47, 255,
	137,  55, 110, 255,
	161,  79, 134, 255,

	 91,  17,  23, 255,
	114,  40,  47, 255,
	137,  55, 110, 255,
	161,  79, 134, 255,
};

static uint8_t expected_rec601_full[] = {
	 54,  51,  37, 255,
	 74,  71,  57, 255,
	 94,  84, 112, 255,
	114, 104, 132, 255,

	 54,  51,  37, 255,
	 74,  71,  57, 255,
	 94,  84, 112, 255,
	114, 104, 132, 255,

	 96,  29,  37, 255,
	116,  49,  57, 255,
	136,  62, 112, 255,
	156,  82, 132, 255,

	 96,  29,  37, 255,
	116,  49,  57, 255,
	136,  62, 112, 255,
	156,  82, 132, 255,
};

static uint8_t expected_rec709_narrow[] = {
	 43,  40,  23, 255,
	 66,  64,  46, 255,
	 90,  83, 112, 255,
	113, 106, 135, 255,

	 43,  40,  23, 255,
	 66,  64,  46, 255,
	 90,  83, 112, 255,
	113, 106, 135, 255,

	 97,  24,  23, 255,
	120,  48,  46, 255,
	144,  67, 112, 255,
	167,  90, 135, 255,

	 97,  24,  23, 255,
	120,  48,  46, 255,
	144,  67, 112, 255,
	167,  90, 135, 255,
};

static uint8_t expected_rec709_full[] = {
	 54,  50,  36, 255,
	 74,  70,  56, 255,
	 94,  86, 113, 255,
	114, 106, 133, 255,

	 54,  50,  36, 255,
	 74,  70,  56, 255,
	 94,  86, 113, 255,
	114, 106, 133, 255,

	101,  36,  36, 255,
	121,  56,  56, 255,
	141,  72, 113, 255,
	161,  92, 133, 255,

	101,  36,  36, 255,
	121,  56,  56, 255,
	141,  72, 113, 255,
	161,  92, 133, 255,
};

static uint8_t expected_rec2020_narrow[] = {
	 43,  40,  22, 255,
	 66,  63,  46, 255,
	 90,  83, 112, 255,
	113, 106, 135, 255,

	 43,  40,  22, 255,
	 66,  63,  46, 255,
	 90,  83, 112, 255,
	113,  106, 135, 255,

	 93,  20,  22, 255,
	117,  44,  46, 255,
	140,  63, 112, 255,
	163,  86, 135, 255,

	 93,  20,  22, 255,
	117,  44,  46, 255,
	140,  63, 112, 255,
	163,  86, 135, 255,
};

static uint8_t expected_rec2020_full[] = {
	 54,  50,  36, 255,
	 74,  70,  56, 255,
	 94,  87, 114, 255,
	114, 107, 134, 255,

	 54,  50,  36, 255,
	 74,  70,  56, 255,
	 94,  87, 114, 255,
	114, 107, 134, 255,

	 98,  33,  36, 255,
	118,  53,  56, 255,
	138,  69, 114, 255,
	158,  89, 134, 255,

	 98,  33,  36, 255,
	118,  53,  56, 255,
	138,  69, 114, 255,
	158,  89, 134, 255,
};

struct yuv_sampling_options {
	/* One of:
	* - EGL_ITU_REC601_EXT.
	* - EGL_ITU_REC709_EXT.
	* - EGL_ITU_REC2020_EXT.
	*/
	EGLint yuv_color_space;

	/* One of:
	* - EGL_YUV_FULL_RANGE_EXT.
	* - EGL_YUV_NARROW_RANGE_EXT.
	*/
	EGLint yuv_range;
};

// clang-format on

static const EGLint yuv_color_space_list[] = {
	EGL_ITU_REC601_EXT,
	EGL_ITU_REC709_EXT,
	EGL_ITU_REC2020_EXT,
};

static const EGLint yuv_range_list[] = {
	EGL_YUV_FULL_RANGE_EXT,
	EGL_YUV_NARROW_RANGE_EXT,
};

static const char *get_egl_attr_string(EGLint attr)
{
	switch (attr) {
		STRINGIFY(EGL_ITU_REC601_EXT);
		STRINGIFY(EGL_ITU_REC709_EXT);
		STRINGIFY(EGL_ITU_REC2020_EXT);
		STRINGIFY(EGL_YUV_FULL_RANGE_EXT);
		STRINGIFY(EGL_YUV_NARROW_RANGE_EXT);
		default:
			assert(false && "Unsupported EGL attribute.");
			return "";
	}
}

const char *get_gl_error_string(GLenum error)
{
	switch (error) {
		case GL_INVALID_ENUM:
			return "[GL_INVALID_ENUM]: An unacceptable value is specified for an "
			       "enumerated argument.";
		case GL_INVALID_VALUE:
			return "[GL_INVALID_VALUE]: A numeric argument is out of range.";
		case GL_INVALID_OPERATION:
			return "[GL_INVALID_OPERATION]: The specified operation is not allowed "
			       "in the current state.";
		case GL_INVALID_FRAMEBUFFER_OPERATION:
			return "[GL_INVALID_FRAMEBUFFER_OPERATION]: The framebuffer object is "
			       "not complete.";
		case GL_OUT_OF_MEMORY:
			return "[GL_OUT_OF_MEMORY]: There is not enough memory left to execute "
			       "the command.";
	}
	return "[UNKNOWN_GL_ERROR]";
}

static void check_gl_error(const char *op)
{
	for (GLenum error = glGetError(); error != GL_NO_ERROR; error = glGetError()) {
		bs_debug_error("after %s() glError (0x%x): %s", op, error,
			       get_gl_error_string(error));
	}
}

EGLImageKHR create_egl_image_with_yuv_sampling_options(
    struct gbm_bo *bo, const struct yuv_sampling_options *yuv_sampling_options)
{
	assert(bo);
	assert(gbm_bo_get_plane_count(bo) == NUM_PLANES_NV12);

	int fds[NUM_PLANES_NV12];

	for (size_t plane = 0; plane < NUM_PLANES_NV12; plane++) {
		fds[plane] = gbm_bo_get_fd_for_plane(bo, plane);
		if (fds[plane] < 0) {
			bs_debug_error("failed to get fb for bo: %d", fds[plane]);
			return EGL_NO_IMAGE_KHR;
		}
	}

	// When the bo has 2 planes with modifier support, it requires 31 components.
	EGLint khr_image_attrs[31] = {
		EGL_WIDTH,
		gbm_bo_get_width(bo),
		EGL_HEIGHT,
		gbm_bo_get_height(bo),
		EGL_LINUX_DRM_FOURCC_EXT,
		(EGLint)gbm_bo_get_format(bo),
		EGL_NONE,
	};

	size_t attrs_index = 6;
	const uint64_t modifier = gbm_bo_get_modifier(bo);
	for (size_t plane = 0; plane < NUM_PLANES_NV12; plane++) {
		khr_image_attrs[attrs_index++] = EGL_DMA_BUF_PLANE0_FD_EXT + plane * 3;
		khr_image_attrs[attrs_index++] = fds[plane];
		khr_image_attrs[attrs_index++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT + plane * 3;
		khr_image_attrs[attrs_index++] = gbm_bo_get_offset(bo, plane);
		khr_image_attrs[attrs_index++] = EGL_DMA_BUF_PLANE0_PITCH_EXT + plane * 3;
		khr_image_attrs[attrs_index++] = gbm_bo_get_stride_for_plane(bo, plane);

		if (modifier != DRM_FORMAT_MOD_INVALID) {
			khr_image_attrs[attrs_index++] =
			    EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT + plane * 2;
			khr_image_attrs[attrs_index++] = modifier & 0xfffffffful;
			khr_image_attrs[attrs_index++] =
			    EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT + plane * 2;
			khr_image_attrs[attrs_index++] = modifier >> 32;
		}
	}

	khr_image_attrs[attrs_index++] = EGL_YUV_COLOR_SPACE_HINT_EXT;
	khr_image_attrs[attrs_index++] = yuv_sampling_options->yuv_color_space;
	khr_image_attrs[attrs_index++] = EGL_SAMPLE_RANGE_HINT_EXT;
	khr_image_attrs[attrs_index++] = yuv_sampling_options->yuv_range;

	khr_image_attrs[attrs_index++] = EGL_NONE;

	PFNEGLCREATEIMAGEKHRPROC egl_create_image_khr_fn =
	    (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	if (!egl_create_image_khr_fn) {
		bs_debug_error(
		    "eglGetProcAddress returned NULL for a required extension entry point.");
		return EGL_NO_IMAGE_KHR;
	}

	// Creates EGL image from attribute list.
	EGLImageKHR image = egl_create_image_khr_fn(eglGetDisplay(EGL_DEFAULT_DISPLAY),
						    EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
						    NULL /* no client buffer */, khr_image_attrs);

	if (image == EGL_NO_IMAGE_KHR) {
		bs_debug_error("failed to make image from target buffer");
		return EGL_NO_IMAGE_KHR;
	}

	for (size_t plane = 0; plane < NUM_PLANES_NV12; plane++) {
		close(fds[plane]);
	}

	return image;
}

static bool bind_egl_image_to_gl_texture(EGLImageKHR image)
{
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC gl_egl_image_target_texture_2d_oes_fn =
	    eglGetProcAddress("glEGLImageTargetTexture2DOES");
	if (!gl_egl_image_target_texture_2d_oes_fn) {
		bs_debug_error(
		    "eglGetProcAddress returned NULL for a required extension entry point.");
		return false;
	}
	gl_egl_image_target_texture_2d_oes_fn(GL_TEXTURE_EXTERNAL_OES, image);

	return glGetError() == GL_NO_ERROR;
}

// Creates and returns a destination rendering frame buffer for OpenGL to draw onto.
static struct bs_egl_fb *create_rendering_frame_buffer(struct gbm_device *gbm, struct bs_egl *egl,
						       uint32_t width, uint32_t height)
{
	struct gbm_bo *rendering_bo =
	    gbm_bo_create(gbm, width, height, GBM_FORMAT_ARGB8888, GBM_BO_USE_RENDERING);
	if (!rendering_bo) {
		bs_debug_error("failed to create a gbm buffer for rendering.");
		return NULL;
	}

	EGLImageKHR rendering_egl_image = bs_egl_image_create_gbm(egl, rendering_bo);
	if (rendering_egl_image == EGL_NO_IMAGE_KHR) {
		bs_debug_error("failed to make image from buffer object");
		return NULL;
	}

	struct bs_egl_fb *fb = bs_egl_fb_new(egl, rendering_egl_image);
	if (!fb) {
		bs_debug_error("failed to make rendering framebuffer for buffer object");
		return NULL;
	}

	assert(fb);
	return fb;
}

// Creates an OpenGL texture and binds the EGL image to it.
static bool create_texture(struct gbm_bo *bo, uint32_t width, uint32_t height,
			   const struct yuv_sampling_options yuv_sampling_options)
{
	EGLImageKHR image = create_egl_image_with_yuv_sampling_options(bo, &yuv_sampling_options);
	if (image == EGL_NO_IMAGE_KHR) {
		bs_debug_error("failed to create egl image for NV12 bo");
		return false;
	}

	glActiveTexture(GL_TEXTURE0);
	check_gl_error("glActiveTexture");

	GLuint tex;
	glGenTextures(1, &tex);
	check_gl_error("glGenTextures");

	glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
	check_gl_error("glBindTexture");

	if (!bind_egl_image_to_gl_texture(image)) {
		bs_debug_error("failed to bind egl image to gl texture.");
		return false;
	}

	// Sets the texture filtering parameters.
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	// Sets the texture wrapping parameters.
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	check_gl_error("glTexParameteri");

	return true;
}

// Builds and compiles vertex and shader program.
static bool set_up_graphics(struct bs_egl_fb *fb, uint32_t width, uint32_t height, GLuint *program)
{
	const GLchar *vert =
	    "#version 300 es\n"
	    "out vec2 texPos;\n"
	    "void main() {\n"
	    "  vec2 pos[4];\n"
	    "  pos[0] = vec2(-1.0, -1.0);\n"
	    "  pos[1] = vec2(1.0, -1.0);\n"
	    "  pos[2] = vec2(-1.0, 1.0);\n"
	    "  pos[3] = vec2(1.0, 1.0);\n"
	    "  gl_Position.xy = pos[gl_VertexID];\n"
	    "  gl_Position.zw = vec2(0.0, 1.0);\n"
	    "  vec2 uvs[4];\n"
	    "  uvs[0] = vec2(0.0, 0.0);\n"
	    "  uvs[1] = vec2(1.0, 0.0);\n"
	    "  uvs[2] = vec2(0.0, 1.0);\n"
	    "  uvs[3] = vec2(1.0, 1.0);\n"
	    "  texPos = uvs[gl_VertexID];\n"
	    "}\n";

	const GLchar *frag =
	    "#version 300 es\n"
	    "#extension GL_OES_EGL_image_external_essl3 : require\n"
	    "precision mediump float;\n"
	    "uniform samplerExternalOES texSampler;\n"
	    "in vec2 texPos;\n"
	    "out vec4 fragColor;\n"
	    "void main() {\n"
	    "  fragColor = texture(texSampler, texPos);\n"
	    "}\n";

	program = bs_gl_program_create_vert_frag_bind(vert, frag, NULL);
	if (!program) {
		bs_debug_error("failed to compile shader program");
		return false;
	};

	glBindFramebuffer(GL_FRAMEBUFFER, bs_egl_fb_name(fb));
	check_gl_error("glBindFramebuffer");

	glViewport(0, 0, (GLint)width, (GLint)height);
	check_gl_error("glViewport");

	glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
	check_gl_error("glClearColor");
	glClear(GL_COLOR_BUFFER_BIT);
	check_gl_error("glClear");

	glUseProgram(program);
	check_gl_error("glUseProgram");

	return true;
}

static GLubyte *get_expected_rgb_values(uint32_t width, uint32_t height,
					const struct yuv_sampling_options yuv_sampling_options)
{
	EGLint yuv_color_space = yuv_sampling_options.yuv_color_space;
	EGLint yuv_range = yuv_sampling_options.yuv_range;

	unsigned char *rgb_values = NULL;
	if (yuv_color_space == EGL_ITU_REC601_EXT && yuv_range == EGL_YUV_NARROW_RANGE_EXT) {
		rgb_values = expected_rec601_narrow;
	} else if (yuv_color_space == EGL_ITU_REC601_EXT && yuv_range == EGL_YUV_FULL_RANGE_EXT) {
		rgb_values = expected_rec601_full;
	} else if (yuv_color_space == EGL_ITU_REC709_EXT && yuv_range == EGL_YUV_NARROW_RANGE_EXT) {
		rgb_values = expected_rec709_narrow;
	} else if (yuv_color_space == EGL_ITU_REC709_EXT && yuv_range == EGL_YUV_FULL_RANGE_EXT) {
		rgb_values = expected_rec709_full;
	} else if (yuv_color_space == EGL_ITU_REC2020_EXT &&
		   yuv_range == EGL_YUV_NARROW_RANGE_EXT) {
		rgb_values = expected_rec2020_narrow;
	} else if (yuv_color_space == EGL_ITU_REC2020_EXT && yuv_range == EGL_YUV_FULL_RANGE_EXT) {
		rgb_values = expected_rec2020_full;
	};

	return rgb_values;
}

static void print_rgba_values_as_errors(const GLubyte *pixels)
{
	assert(num_color_components == 4);
	bs_debug_error("%3hhu %3hhu %3hhu %3hhu", pixels[0], pixels[1], pixels[2], pixels[3]);
}

static bool examine_rbg_values_by_component(const GLubyte *actual_values,
					    const GLubyte *expected_values,
					    int *first_mismatched_component_index)
{
	for (int i = 0; i < num_color_components; i++) {
		if (abs((int)expected_values[i] - (int)actual_values[i]) > rgb_value_tolerance) {
			*first_mismatched_component_index = i;
			return false;
		}
	}
	return true;
}

static bool examine_rbg_values(uint32_t width, uint32_t height, GLubyte *expected_values)
{
	GLubyte *pixels =
	    (GLubyte *)malloc(width * height * NUM_BYTES_PER_RGBA_PIXEL * sizeof(GLubyte));
	glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	check_gl_error("glReadPixels");

	for (int j = 0; j < height; j++) {
		for (int i = 0; i < width; i++) {
			const size_t pixel_offset = j * width + i;
			const GLubyte *actual_values_by_component =
			    &pixels[pixel_offset * num_color_components];
			const GLubyte *expected_values_by_component =
			    &expected_values[pixel_offset * num_color_components];

			int first_mismatched_component_index = 0;
			if (examine_rbg_values_by_component(actual_values_by_component,
							    expected_values_by_component,
							    &first_mismatched_component_index)) {
				continue;
			}

			bs_debug_error("Mismatch at pixel (%i, %i), component %d", j, i,
				       first_mismatched_component_index);
			bs_debug_error("Expected RGBA: ");
			print_rgba_values_as_errors(expected_values_by_component);
			bs_debug_error("Actual RGBA:   ");
			print_rgba_values_as_errors(actual_values_by_component);
			return false;
		}
	}
	return true;
}

int main(int argc, char **argv)
{
	int display_fd = bs_drm_open_main_display();
	if (display_fd < 0) {
		bs_debug_error("failed to open device for display");
		exit(EXIT_FAILURE);
	}

	struct bs_mapper *mapper = bs_mapper_dma_buf_new();
	if (!mapper) {
		bs_debug_error("failed to create mapper object");
		exit(EXIT_FAILURE);
	}

	struct gbm_device *gbm = gbm_create_device(display_fd);
	if (!gbm) {
		bs_debug_error("failed to create gbm device");
		exit(EXIT_FAILURE);
	}

	struct bs_egl *egl = bs_egl_new();
	if (!bs_egl_setup(egl, NULL)) {
		bs_debug_error("failed to setup egl context");
		exit(EXIT_FAILURE);
	}

	struct bs_egl_fb *fb = create_rendering_frame_buffer(gbm, egl, width, height);
	if (!fb) {
		bs_debug_error("failed to create rendering framebuffer");
		exit(EXIT_FAILURE);
	}

	// Allocates a GBM buffer to store NV12 data and imports it into OpenGL.
	uint32_t flags = GBM_BO_USE_TEXTURING | GBM_BO_USE_LINEAR | GBM_BO_USE_SW_WRITE_OFTEN;
	if (!gbm_device_is_format_supported(gbm, GBM_FORMAT_NV12, flags)) {
		bs_debug_error("GBM_FORMAT_NV12 is not supported");
		exit(EXIT_FAILURE);
	}

	struct gbm_bo *bo = gbm_bo_create(gbm, width, height, GBM_FORMAT_NV12, flags);
	if (!bo) {
		bs_debug_error("failed to allocate NV12 buffer object");
		exit(EXIT_FAILURE);
	}

	uint32_t dst_stride_y;
	uint32_t dst_stride_uv;

	void *map_data_y;
	void *map_data_uv;
	uint8_t *bo_y_plane = bs_mapper_map(mapper, bo, 0, &map_data_y, &dst_stride_y);
	if (bo_y_plane == MAP_FAILED) {
		bs_debug_error("failed to mmap gbm bo plane 0 (Y)");
		exit(EXIT_FAILURE);
	}

	uint8_t *bo_uv_plane = bs_mapper_map(mapper, bo, 1, &map_data_uv, &dst_stride_uv);
	if (bo_uv_plane == MAP_FAILED) {
		bs_debug_error("failed to mmap gbm bo plane 1 (UV)");
		exit(EXIT_FAILURE);
	}

	assert(dst_stride_y >= width);
	assert(dst_stride_uv >= width);

	uint8_t *tmp_dst_y = bo_y_plane;
	uint8_t *tmp_src_y = nv12_y;
	for (int row = 0; row < height; row++) {
		for (int col = 0; col < width; col++) {
			tmp_dst_y[col] = tmp_src_y[col];
		}
		tmp_dst_y += dst_stride_y;
		tmp_src_y += width;
	}

	uint8_t *tmp_dst_uv = bo_uv_plane;
	uint8_t *tmp_src_uv = nv12_uv;
	for (int row = 0; row < height / 2; row++) {
		for (int col = 0; col < width; col++) {
			tmp_dst_uv[col] = tmp_src_uv[col];
		}
		tmp_dst_uv += dst_stride_uv;
		tmp_src_uv += width;
	}

	bs_mapper_unmap(mapper, bo, map_data_y);
	bs_mapper_unmap(mapper, bo, map_data_uv);

	GLuint program;
	if (!set_up_graphics(fb, width, height, &program)) {
		bs_debug_error("failed to set up graphics");
		exit(EXIT_FAILURE);
	}

	bool are_all_conversions_correct = true;

	for (int i = 0; i < ARRAY_SIZE(yuv_color_space_list); i++) {
		for (int j = 0; j < ARRAY_SIZE(yuv_range_list); j++) {
			struct yuv_sampling_options yuv_sampling_options;
			yuv_sampling_options.yuv_color_space = yuv_color_space_list[i];
			yuv_sampling_options.yuv_range = yuv_range_list[j];

			if (create_texture(bo, width, height, yuv_sampling_options)) {
				bs_debug_info(
				    "created texture from buffer object with color space: "
				    "%s, yuv range: %s",
				    get_egl_attr_string(yuv_color_space_list[i]),
				    get_egl_attr_string(yuv_range_list[j]));
			} else {
				bs_debug_error(
				    "failed to create texture from buffer object with color space: "
				    "%s, yuv range: %s",
				    get_egl_attr_string(yuv_color_space_list[i]),
				    get_egl_attr_string(yuv_range_list[j]));
				exit(EXIT_FAILURE);
			}

			// Draws to framebuffer.
			GLuint indices[4] = { 0, 1, 2, 3 };
			glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_INT, indices);
			check_gl_error("glDrawElements");
			glFinish();

			GLubyte *expected =
			    get_expected_rgb_values(width, height, yuv_sampling_options);
			if (examine_rbg_values(width, height, expected)) {
				bs_debug_info(
				    "color conversion from color space: "
				    "%s, yuv range: %s is correct",
				    get_egl_attr_string(yuv_color_space_list[i]),
				    get_egl_attr_string(yuv_range_list[j]));
			} else {
				are_all_conversions_correct = false;
				bs_debug_error(
				    "color conversion from color space: "
				    "%s, yuv range: %s failed",
				    get_egl_attr_string(yuv_color_space_list[i]),
				    get_egl_attr_string(yuv_range_list[j]));
			}
		}
	}

	// De-allocates all graphics resources after the test.
	glUseProgram(0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glDeleteProgram(program);
	bs_egl_fb_destroy(&fb);
	bs_egl_destroy(&egl);
	gbm_bo_destroy(bo);
	gbm_device_destroy(gbm);
	bs_mapper_destroy(mapper);
	close(display_fd);

	if (are_all_conversions_correct) {
		bs_debug_info("[  PASSED  ] yuv_to_rgb_test succeeded");
		exit(EXIT_SUCCESS);
	}

	bs_debug_info("[  FAILED  ] yuv_to_rgb_test failed");
	exit(EXIT_FAILURE);
}
