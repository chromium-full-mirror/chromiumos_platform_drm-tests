/*
 * Copyright 2019 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#define _GNU_SOURCE

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/udmabuf.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>

#include "bs_drm.h"

#define SEC_TO_NS 1000000000L
#define NS_TO_MS 1 / 1000000L

#define HANDLE_EINTR_AND_EAGAIN(x)                                             \
	({                                                                     \
		int result;                                                    \
		do {                                                           \
			result = (x);                                          \
		} while (result != -1 && (errno == EINTR || errno == EAGAIN)); \
		result;                                                        \
	})

// The purpose of this test is to assess the impact on the performance of
// compositing when using udmabuf to avoid copies. To accomplish this, we
// compare two paths:
//
// 1) Drawing the square to a shared memory buffer with the CPU, converting that
// to a dma-buf using udmabuf_create, and importing that dma-buf in GL to
// composite on to a scanout buffer.
//
// 2) Drawing the square to a shared memory buffer with the CPU, uploading that
// as a GL texture, and using that texture to composite onto a scanout buffer.
//
// For each path and for each frame, we time drawing the square with the CPU,
// and we time how long it takes GL to finish rendering.

// Duration to display frames for in seconds.
static const int kTestCaseDurationSeconds = 20;
// Name of memfd file created.
static const char* kMemFDCreateName = "dmabuf_test";
// Critical value for the standard normal distribution corresponding to a 95% confidence level.
static const double kZCriticalValue = 1.960;

// Represents a buffer that can be composited into and will be scanned out from.
struct Buffer {
	struct gbm_bo* bo;
	struct bs_egl_fb* gl_fb;
	uint32_t fb_id;
	EGLImageKHR egl_image;
};

// An implementation of double buffering: we composite into buffers[back_buffer] while
// the other buffer is being scanned out.
struct BufferQueue {
	struct Buffer buffers[2];
	size_t back_buffer;
};

// Position and velocity of the square.
struct MotionContext {
	int x;
	int y;
	int x_v;
	int y_v;
};

struct SharedMemoryBuffer {
	int memfd;
	uint32_t* mapped_rgba_data;
};

// Represents a shared-memory buffer imported into GL.
// |image_bo|, |image|, and |dmabuf_fd| are only used in the zero-copy path.
struct ImportedBuffer {
	GLuint image_texture;
	struct gbm_bo* image_bo;
	EGLImageKHR image;
	int dmabuf_fd;
};

// Context required for a page flip and memory cleanup when finished.
struct PageFlipContext {
	struct BufferQueue queue;
	struct MotionContext motion_context;

	struct SharedMemoryBuffer shm_buffer;
	struct ImportedBuffer imported_buffer;

	struct bs_egl* egl;
	GLuint vertex_attributes;
	bool use_zero_copy;
	int frames;
	uint32_t width;
	uint32_t height;
	uint32_t crtc_id;
	double sum_of_times;	      // Sum of timings on each frame.
	double sum_of_squared_times;  // Sum of squared timings on each frame.
};

double standard_error(double stddev, size_t n)
{
	return kZCriticalValue * (stddev / sqrt(n));
}

// Aligns num up to the nearest multiple of |multiple|.
uint32_t align(uint32_t num, int multiple)
{
	assert(multiple);
	return ((num + multiple - 1) / multiple) * multiple;
}

/*
 * Upload pixel data as a GL texture.
 */
void upload_texture(uint32_t* arr, size_t width, size_t height)
{
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, arr);
}

/*
 * Draw a randomly colored square, moving from top left to bottom right
 * behind a black background. Position of the square is set by |motion_context|.
 * |arr| points to the RGBA pixel data.
 */
void draw_square(size_t width, size_t height, struct MotionContext* motion_context, uint32_t* arr)
{
	size_t j_left_bound = motion_context->x;
	size_t j_right_bound = j_left_bound + 50;
	size_t i_top_bound = motion_context->y;
	size_t i_bottom_bound = i_top_bound + 50;
	uint32_t color = drand48() * 0xFFFFFFFF;

	if (i_bottom_bound >= height)
		motion_context->y_v = -16;

	if (j_right_bound >= width)
		motion_context->x_v = -16;

	if (j_left_bound <= 1)
		motion_context->x_v = 16;

	if (i_top_bound <= 1)
		motion_context->y_v = 16;

	uint32_t* dst = (uint32_t*)arr + i_top_bound * width;
	for (size_t row = i_top_bound; (row < i_bottom_bound) && (row < height); row++) {
		for (size_t col = j_left_bound; (col < j_right_bound) && (col < width); col++) {
			dst[col] = color;
		}
		dst += width;
	}
}

int create_udmabuf(int fd, size_t length)
{
	int udmabuf_dev_fd = HANDLE_EINTR_AND_EAGAIN(open("/dev/udmabuf", O_RDWR));

	struct udmabuf_create create;
	create.memfd = fd;
	create.flags = UDMABUF_FLAGS_CLOEXEC;
	create.offset = 0;
	create.size = length;

	int dmabuf_fd = HANDLE_EINTR_AND_EAGAIN(ioctl(udmabuf_dev_fd, UDMABUF_CREATE, &create));
	if (dmabuf_fd < 0) {
		bs_debug_error("error creating udmabuf");
		exit(EXIT_FAILURE);
	}

	close(udmabuf_dev_fd);
	return dmabuf_fd;
}

/*
 * Create a region of shared memory of size |length|.
 * The region is sealed with F_SEAL_SHRINK.
 */
int create_memfd(size_t length)
{
	int fd = memfd_create(kMemFDCreateName, MFD_ALLOW_SEALING);
	if (fd == -1) {
		bs_debug_error("memfd_create() error: %s", strerror(errno));
		exit(EXIT_FAILURE);
	}

	int res = HANDLE_EINTR_AND_EAGAIN(ftruncate(fd, length));
	if (res == -1) {
		bs_debug_error("ftruncate() error: %s", strerror(errno));
		exit(EXIT_FAILURE);
	}

	// udmabuf_create requires that file descriptors be sealed with
	// F_SEAL_SHRINK.
	if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK) < 0) {
		bs_debug_error("fcntl() error: %s", strerror(errno));
		exit(EXIT_FAILURE);
	}

	return fd;
}

GLuint setup_shaders_and_geometry(int width, int height)
{
	const GLchar* vert =
	    "attribute vec2 pos;\n"
	    "varying vec2 tex_pos;\n"
	    "void main() {\n"
	    "  gl_Position = vec4(pos, 0, 1);\n"
	    "  tex_pos = vec2((pos.x + 1.0) / 2.0, (pos.y + 1.0) / 2.0);\n"
	    "}\n";

	const GLchar* frag =
	    "precision mediump float;\n"
	    "uniform sampler2D tex;\n"
	    "varying vec2 tex_pos;\n"
	    "void main() {\n"
	    "  gl_FragColor =  texture2D(tex, tex_pos);\n"
	    "}\n";

	const GLfloat verts[] = {
		-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f,
	};

	struct bs_gl_program_create_binding bindings[] = {
		{ 0, "pos" },
		{ 0, NULL },
	};

	// Compile and link GL program.
	GLuint program = bs_gl_program_create_vert_frag_bind(vert, frag, bindings);
	if (!program) {
		bs_debug_error("failed to compile shader program");
		exit(EXIT_FAILURE);
	}

	glUseProgram(program);
	glViewport(0, 0, width, height);

	GLuint buffer = 0;
	glGenBuffers(1, &buffer);
	glBindBuffer(GL_ARRAY_BUFFER, buffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

	glUniform1i(glGetUniformLocation(program, "tex"), 0);
	GLint pos_attrib_index = glGetAttribLocation(program, "pos");
	glEnableVertexAttribArray(pos_attrib_index);
	glVertexAttribPointer(pos_attrib_index, 2, GL_FLOAT, GL_FALSE, 0, 0);
	glDeleteProgram(program);
	return buffer;
}

struct bs_egl_fb* create_gl_framebuffer(struct bs_egl* egl, EGLImageKHR egl_image)
{
	struct bs_egl_fb* fb = bs_egl_fb_new(egl, egl_image);
	if (!fb) {
		bs_egl_image_destroy(egl, &egl_image);
		bs_debug_error("failed to make rendering framebuffer for buffer object");
		exit(EXIT_FAILURE);
	}
	return fb;
}

EGLImageKHR import_source_buffer(struct bs_egl* egl, struct gbm_bo* bo, GLuint image_texture)
{
	EGLImageKHR image = bs_egl_image_create_gbm(egl, bo);
	if (image == EGL_NO_IMAGE_KHR) {
		bs_debug_error("failed to make image from buffer object");
		exit(EXIT_FAILURE);
	}

	glBindTexture(GL_TEXTURE_2D, image_texture);

	if (!bs_egl_target_texture2D(egl, image)) {
		bs_debug_error("failed to import egl color_image as a texture");
		exit(EXIT_FAILURE);
	}
	return image;
}

/*
 * Initialize GL pipeline.
 *   width: width of display
 *   height: height of display
 *
 */
GLuint init_gl(struct PageFlipContext* context, uint32_t width, uint32_t height)
{
	context->vertex_attributes = setup_shaders_and_geometry(width, height);

	GLuint image_texture = 0;
	glGenTextures(1, &image_texture);
	glBindTexture(GL_TEXTURE_2D, image_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	return image_texture;
}

/*
 * Call on each frame.
 * This function is called with alternating fb's.
 */
void draw_gl(GLuint fb)
{
	// Bind the screen framebuffer to GL.
	glBindFramebuffer(GL_FRAMEBUFFER, fb);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	// Block until rendering is complete.
	// We can easily measure how long rendering takes if this function
	// blocks.
	glFinish();
}

/*
 * Called at the end of each page flip.
 * Schedules a new page flip alternating between
 * the two buffers.
 */
static void draw_and_swap_frame(int display_fd, unsigned int frame, unsigned int sec,
				unsigned int usec, void* data)
{
	struct PageFlipContext* context = data;
	struct BufferQueue* queue = &context->queue;
	struct Buffer buf = queue->buffers[queue->back_buffer];
	struct bs_egl* egl = context->egl;
	struct SharedMemoryBuffer shm_buffer = context->shm_buffer;
	int crtc_id = context->crtc_id;
	uint32_t width = context->width;
	uint32_t height = context->height;
	int err;
	struct timespec start, finish;
	clock_gettime(CLOCK_MONOTONIC, &start);
	if (context->use_zero_copy) {
		int dmabuf_fd = context->imported_buffer.dmabuf_fd;
		struct dma_buf_sync sync_start = { 0 };
		sync_start.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW;
		int rv = HANDLE_EINTR_AND_EAGAIN(ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync_start));
		if (rv != 0) {
			bs_debug_error("error with dma_buf start sync");
			exit(EXIT_FAILURE);
		}

		draw_square(width, height, &context->motion_context, shm_buffer.mapped_rgba_data);

		struct dma_buf_sync sync_end = { 0 };
		sync_end.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
		rv = HANDLE_EINTR_AND_EAGAIN(ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync_end));
		if (rv != 0) {
			bs_debug_error("error with dma_buf end sync");
			exit(EXIT_FAILURE);
		}
	} else {
		draw_square(width, height, &context->motion_context, shm_buffer.mapped_rgba_data);
		// TODO(crbug.com/1069612): Experiment a third path which uses
		// glTexSubImage2D instead of glTexImage2D() on each frame. It
		// should be faster.
		upload_texture(shm_buffer.mapped_rgba_data, width, height);
	}
	draw_gl(bs_egl_fb_name(buf.gl_fb));
	clock_gettime(CLOCK_MONOTONIC, &finish);

	double ns_diff =
	    (SEC_TO_NS * (finish.tv_sec - start.tv_sec)) + finish.tv_nsec - start.tv_nsec;
	double ms_to_draw_and_render = (ns_diff)*NS_TO_MS;

	context->sum_of_times += ms_to_draw_and_render;
	context->sum_of_squared_times += ms_to_draw_and_render * ms_to_draw_and_render;

	bs_egl_image_flush_external(egl, buf.egl_image);
	err = drmModePageFlip(display_fd, crtc_id, buf.fb_id, DRM_MODE_PAGE_FLIP_EVENT, context);

	if (err) {
		bs_debug_error("failed page flip: %s", strerror(errno));
		exit(EXIT_FAILURE);
	}

	queue->back_buffer = (queue->back_buffer + 1) % 2;
	context->motion_context.x += context->motion_context.x_v;
	context->motion_context.y += context->motion_context.y_v;
	context->frames++;
}

struct BufferQueue init_buffers(struct gbm_device* gbm, struct bs_egl* egl, uint32_t width,
				uint32_t height)
{
	struct BufferQueue queue;
	memset(&queue, 0, sizeof(struct BufferQueue));
	for (size_t i = 0; i < 2; i++) {
		struct gbm_bo* screen_bo = gbm_bo_create(gbm, width, height, GBM_FORMAT_ARGB8888,
							 GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
		if (!screen_bo) {
			bs_debug_error("failed to create screen bo");
			exit(EXIT_FAILURE);
		}

		EGLImageKHR egl_image = bs_egl_image_create_gbm(egl, screen_bo);
		if (egl_image == EGL_NO_IMAGE_KHR) {
			bs_debug_error("failed to make image from buffer object");
			exit(EXIT_FAILURE);
		}

		uint32_t fb_id = bs_drm_fb_create_gbm(screen_bo);
		if (!fb_id) {
			bs_debug_error("failed to make drm fb from image");
			exit(EXIT_FAILURE);
		}

		queue.buffers[i].egl_image = egl_image;
		queue.buffers[i].bo = screen_bo;
		queue.buffers[i].fb_id = fb_id;
		queue.buffers[i].gl_fb = create_gl_framebuffer(egl, egl_image);
	}
	queue.back_buffer = 1;
	return queue;
}

struct PageFlipContext init_page_flip_context(struct gbm_device* gbm, struct bs_egl* egl,
					      int display_fd)
{
	struct bs_drm_pipe pipe = { 0 };
	if (!bs_drm_pipe_make(display_fd, &pipe)) {
		bs_debug_error("failed to make pipe: %s", strerror(errno));
		exit(EXIT_FAILURE);
	}

	drmModeConnector* connector = drmModeGetConnector(display_fd, pipe.connector_id);
	drmModeModeInfo* mode = &connector->modes[0];

	struct PageFlipContext context;
	memset(&context, 0, sizeof(struct PageFlipContext));

	context.crtc_id = pipe.crtc_id;
	context.height = mode->vdisplay;
	context.width = mode->hdisplay;
	context.egl = egl;
	context.motion_context = (struct MotionContext){ 1, 1, 16, 16 };
	context.queue = init_buffers(gbm, egl, mode->hdisplay, mode->vdisplay);
	context.sum_of_times = 0;
	context.sum_of_squared_times = 0;
	context.frames = 0;

	// Set display mode which also flips the page.
	int ret_display =
	    drmModeSetCrtc(display_fd, pipe.crtc_id, context.queue.buffers[0].fb_id, 0 /* x */,
			   0 /* y */, &pipe.connector_id, 1 /* connector count */, mode);

	if (ret_display) {
		bs_debug_error("failed to set crtc: %s", strerror(errno));
		exit(EXIT_FAILURE);
	}
	return context;
}

struct gbm_bo* import_dmabuf(struct gbm_device* gbm, int dmabuf_fd, uint32_t width, uint32_t height)
{
	// Import buffer object from shared dma_buf.
	struct gbm_import_fd_modifier_data gbm_import_data;
	gbm_import_data.width = width;
	gbm_import_data.height = height;
	gbm_import_data.format = GBM_FORMAT_ARGB8888;
	gbm_import_data.num_fds = 1;
	gbm_import_data.fds[0] = dmabuf_fd;
	gbm_import_data.strides[0] = width * 4;
	gbm_import_data.offsets[0] = 0;
	gbm_import_data.modifier = 0;

	struct gbm_bo* image_bo =
	    gbm_bo_import(gbm, GBM_BO_IMPORT_FD_MODIFIER, &gbm_import_data, GBM_BO_USE_RENDERING);

	if (!image_bo) {
		bs_debug_error("failed to make image bo");
		exit(EXIT_FAILURE);
	}
	return image_bo;
}

void destroy_shm_buffer(struct SharedMemoryBuffer buf, uint32_t length)
{
	munmap(buf.mapped_rgba_data, length);
	close(buf.memfd);
}

void destroy_imported_buffer(struct ImportedBuffer buf, struct bs_egl* egl)
{
	glDeleteTextures(1, &buf.image_texture);
	if (buf.image != EGL_NO_IMAGE_KHR)
		bs_egl_image_destroy(egl, &buf.image);
	if (buf.image_bo)
		gbm_bo_destroy(buf.image_bo);
	if (buf.dmabuf_fd >= 0)
		close(buf.dmabuf_fd);
}

void destroy_buffers(struct BufferQueue queue, struct bs_egl* egl)
{
	for (size_t i = 0; i < 2; i++) {
		bs_egl_image_destroy(egl, &queue.buffers[i].egl_image);
		bs_egl_fb_destroy(&queue.buffers[i].gl_fb);
		gbm_bo_destroy(queue.buffers[i].bo);
	}
}

void print_results(double sum_of_squares, double sum, int frames, bool use_zero_copy)
{
	double avg = sum / frames;
	double stddev = sqrt((sum_of_squares - (frames * (avg * avg))) / (frames - 1));
	double std_err = standard_error(stddev, frames);

	double begin_range = avg - std_err;
	double end_range = avg + std_err;
	if (use_zero_copy)
		printf("Using udmabuf (zero-copy path):\n");
	else
		printf("Using glTexImage2D (one-copy path):\n");

	printf("  n       = %d frames\n", frames);
	printf("  CI(t)   = (%.2f ms, %.2f ms)\n", begin_range, end_range);
	printf("  Sum(t)  = %.2f ms\n", sum);
}

int main(int argc, char** argv)
{
	struct timespec clock_resolution;
	clock_getres(CLOCK_MONOTONIC, &clock_resolution);
	// Make sure that the clock resolution is at least 1ms.
	assert(clock_resolution.tv_sec == 0 && clock_resolution.tv_nsec <= 1000000);

	int display_fd = bs_drm_open_main_display();
	if (display_fd < 0) {
		bs_debug_error("failed to open card for display");
		exit(EXIT_FAILURE);
	}
	struct gbm_device* gbm = gbm_create_device(display_fd);
	if (!gbm) {
		bs_debug_error("failed to create gbm device");
		exit(EXIT_FAILURE);
	}
	struct bs_egl* egl = bs_egl_new();
	if (!bs_egl_setup(egl, NULL)) {
		bs_debug_error("failed to setup egl context");
		exit(EXIT_FAILURE);
	}

	struct PageFlipContext context = init_page_flip_context(gbm, egl, display_fd);

	const uint32_t width = context.width;
	const uint32_t height = context.height;
	uint32_t length = align(width * height * 4, getpagesize());
	int memfd = create_memfd(length);

	context.imported_buffer.image_texture = init_gl(&context, width, height);
	context.shm_buffer.memfd = memfd;

	context.shm_buffer.mapped_rgba_data =
	    mmap(NULL, length, PROT_WRITE | PROT_READ, MAP_SHARED, context.shm_buffer.memfd, 0);

	draw_and_swap_frame(display_fd, 0, 0, 0, &context);

	int ret;
	fd_set fds;
	time_t start, cur;
	struct timeval v;
	drmEventContext ev;

	printf("n         = Number of frames\n");
	printf(
	    "CI(t)     = 95%% Z confidence interval for the mean time to draw and composite a "
	    "frame\n");
	printf("Sum(t)    = Total drawing and compositing time\n\n");

	for (size_t i = 0; i < 2; i++) {
		context.use_zero_copy = i;
		context.frames = 0;
		context.sum_of_times = 0;
		context.sum_of_squared_times = 0;

		if (context.use_zero_copy) {
			context.imported_buffer.dmabuf_fd = create_udmabuf(memfd, length);
			context.imported_buffer.image_bo =
			    import_dmabuf(gbm, context.imported_buffer.dmabuf_fd, width, height);

			context.imported_buffer.image =
			    import_source_buffer(context.egl, context.imported_buffer.image_bo,
						 context.imported_buffer.image_texture);
		}
		srand(time(&start));
		FD_ZERO(&fds);
		memset(&v, 0, sizeof(v));
		memset(&ev, 0, sizeof(ev));
		ev.version = 2;
		ev.page_flip_handler = draw_and_swap_frame;

		// Display for kTestCaseDurationSeconds seconds.
		while (time(&cur) < start + kTestCaseDurationSeconds) {
			FD_SET(0, &fds);
			FD_SET(display_fd, &fds);
			v.tv_sec = start + kTestCaseDurationSeconds - cur;

			ret = HANDLE_EINTR_AND_EAGAIN(select(display_fd + 1, &fds, NULL, NULL, &v));
			if (ret < 0) {
				bs_debug_error("select() failed on page flip: %s", strerror(errno));
				exit(EXIT_FAILURE);
			} else if (FD_ISSET(0, &fds)) {
				fprintf(stderr, "exit due to user-input\n");
				break;
			} else if (FD_ISSET(display_fd, &fds)) {
				drmHandleEvent(display_fd, &ev);
			}
		}

		print_results(context.sum_of_squared_times, context.sum_of_times, context.frames,
			      context.use_zero_copy);
	}

	destroy_imported_buffer(context.imported_buffer, egl);
	destroy_shm_buffer(context.shm_buffer, length);
	glDeleteBuffers(1, &context.vertex_attributes);
	destroy_buffers(context.queue, egl);
	bs_egl_destroy(&egl);
	gbm_device_destroy(gbm);
	close(display_fd);
}
