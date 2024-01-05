/*
 * Copyright 2021 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

// As per https://www.kernel.org/doc/html/v5.4/media/uapi/v4l/dev-decoder.html
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <openssl/md5.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <unistd.h>

#include "bitstreams/bitstream_helper.h"
#include "bs_drm.h"
#include "pixel_formats/mt21_converter.h"
#include "pixel_formats/q08c_converter.h"
#include "v4l2_macros.h"

#ifndef V4L2_PIX_FMT_QC08C
#define V4L2_PIX_FMT_QC08C    v4l2_fourcc('Q', '0', '8', 'C') /* Qualcomm 8-bit compressed */
#endif

#ifndef V4L2_PIX_FMT_QC10C
#define V4L2_PIX_FMT_QC10C    v4l2_fourcc('Q', '1', '0', 'C') /* Qualcomm 10-bit compressed */
#endif

static const char* kDecodeDevice = "/dev/video-dec0";
static const int kInputbufferMaxSize = 4 * 1024 * 1024;
static const int kRequestBufferCount = 9;
static const uint32_t kInvalidFrameRate = 0;
// |kMaxRetryCount = 2^24| takes around 20 seconds to exhaust all retries on
// Trogdor when a decode stalls.
static const int kMaxRetryCount = 1 << 24;

static uint32_t kPreferredUncompressedFourCCs[] = {
    V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_QC08C, V4L2_PIX_FMT_MT21C,
    V4L2_PIX_FMT_P010};

static int log_run_level = DEFAULT_LOG_LEVEL;

struct per_frame_md5_hash_logs {
  bool enabled;
  bool output_file_enabled;
  FILE* output_fp;
};

struct mmap_buffers {
  void* start[VIDEO_MAX_PLANES];
  size_t length[VIDEO_MAX_PLANES];
  struct gbm_bo* bo;
};

struct queue {
  int v4lfd;
  enum v4l2_buf_type type;
  uint32_t fourcc;
  struct mmap_buffers* buffers;
  // |display_area| describes the bounds of the displayable image relative to
  // the image buffer. |left| and |top| are the offset from the top left corner
  // in pixels. |width| and |height| describe the displayable image's width and
  // height in pixels.
  struct v4l2_rect display_area;
  // |coded_width| x |coded_height|:
  // The size of the encoded frame.
  // Usually has an alignment of 16, 32 depending on codec.
  uint32_t coded_width;
  uint32_t coded_height;
  // Contains the number of bytes in every row of video (line_stride) in each
  // plane of an MMAP buffer. Copied from the V4L2 multi-planar format
  // structure.
  uint32_t bytes_per_line[VIDEO_MAX_PLANES];
  // Size of the plane in bytes. Copied from the V4L2 multi-planar format
  // structure.
  size_t size_image[VIDEO_MAX_PLANES];
  uint32_t cnt;
  uint32_t num_planes;
  enum v4l2_memory memory;  // V4L2_MEMORY_(MMAP|DMABUF) etc.
  uint32_t displayed_frames; // Not valid for OUTPUT queues.
  // Used to track the state of OUTPUT and CAPTURE queues. The CAPTURE queue
  // stops streaming when it dequeues a buffer with V4L2_BUF_FLAG_LAST set.
  // The OUTPUT queue should stop streaming when there are no remaining
  // compressed frames to enqueue to it.
  bool is_streaming;
  struct bitstream* bts;
};

struct md5_hash {
  uint8_t bytes[16];
};

struct data_buffer {
  uint8_t* data;
  size_t size;
};

enum file_output_type {
  FILE_NONE = 0,
  FILE_SINGLE = 1,
  FILE_MULTIPLE = 2
};

struct file_output {
  FILE* single_fp;
  char* file_prefix;
  enum file_output_type type;
  bool raw;
};

bool is_vp9(uint32_t fourcc) {
  // VP9 FourCC should match VP9X where 'X' is the version number. This check
  // ignores the last character, so VP90, VP91, etc. match.
  return (fourcc & 0xff) == 'V' &&
    (fourcc >> 8 & 0xff) == 'P' &&
    (fourcc >> 16 & 0xff) == '9';
}

char* fourcc_to_string(uint32_t fourcc, char* fourcc_string) {
  sprintf(fourcc_string, "%c%c%c%c", fourcc & 0xff, fourcc >> 8 & 0xff,
          fourcc >> 16 & 0xff, fourcc >> 24 & 0xff);
  // Return the pointer for conveniency.
  return fourcc_string;
}

// Verifies that |v4lfd| can be queried for capabilities, and prints them. If
// the ioctl() fails, a FATAL is issued and terminates the program.
void query_driver(int v4lfd) {
  struct v4l2_capability cap = {};
  const int ret = ioctl(v4lfd, VIDIOC_QUERYCAP, &cap);
  if (ret != 0)
    LOG_FATAL("VIDIOC_QUERYCAP failed: %s.", strerror(errno));

  LOG_INFO("Driver=\"%s\" bus_info=\"%s\" card=\"%s\" fd=0x%x", cap.driver,
           cap.bus_info, cap.card, v4lfd);
}

bool is_fourcc_supported(int v4lfd, enum v4l2_buf_type type, uint32_t fourcc) {
  struct v4l2_fmtdesc fmtdesc = { .type = type };
  while (ioctl(v4lfd, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
    if (fourcc == fmtdesc.pixelformat)
      return true;
    fmtdesc.index++;
  }
  return false;
}

// Verifies that |fd| supports |compressed_format| and, if specified,
// |uncompressed_format|.
bool capabilities(int fd,
                  const char* fd_name,
                  uint32_t compressed_format,
                  uint32_t* uncompressed_format) {
  if (!is_fourcc_supported(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                           compressed_format)) {
    char fourcc_str[FOURCC_SIZE + 1];
    LOG_ERROR("%s is not supported for OUTPUT, try running `v4l2-ctl "
        "--list-formats-out-ext -d %s` for more info",
        fourcc_to_string(compressed_format, fourcc_str), fd_name);
    return false;
  }

  if (*uncompressed_format == V4L2_PIX_FMT_P010 ||
      *uncompressed_format == V4L2_PIX_FMT_QC10C) {
    char fourcc_str[FOURCC_SIZE + 1];
    LOG_INFO("Proceeding with CAPTURE format %s. Unable to test for high bit "
             "depth capabilities before bitstream is availible.",
        fourcc_to_string(*uncompressed_format, fourcc_str));
  } else if (*uncompressed_format != V4L2_PIX_FMT_INVALID) {
    if (!is_fourcc_supported(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                             *uncompressed_format)) {
      char fourcc_str[FOURCC_SIZE + 1];
      LOG_ERROR("%s is not supported for CAPTURE, try running `v4l2-ctl "
          "--list-formats-ext -d %s` for more info",
          fourcc_to_string(*uncompressed_format, fourcc_str), fd_name);
      return false;
    }
  }

  return true;
}

// The V4L2 device may emit events outside of the buffer data path. For event
// types, see https://www.kernel.org/doc/html/v5.4/media/uapi/v4l/vidioc-dqevent.html#id2
int subscribe_to_event(int v4lfd, uint32_t event_type, uint32_t id) {
  struct v4l2_event_subscription sub = {.type = event_type,
                                        .id = id,
                                        .flags = 0};

  const int ret = ioctl(v4lfd, VIDIOC_SUBSCRIBE_EVENT, &sub);
  if (ret != 0)
    LOG_ERROR("VIDIOC_SUBSCRIBE_EVENT failed: %s.", strerror(errno));

  return ret;
}

// Dequeues an event from the device. Only subscribed events can be dequeued.
// If there are no events, the return value is non-zero.
int dequeue_event(struct queue* queue, uint32_t* type) {
  // v4l2_event structure will be completely filled out by the driver.
  struct v4l2_event event;

  int ret = ioctl(queue->v4lfd, VIDIOC_DQEVENT, &event);
  if (!ret && type)
    *type = event.type;

  return ret;
}

bool apply_selection_to_queue(struct queue* queue,
                              struct v4l2_rect display_area) {
  struct v4l2_selection selection = {
     // |type| has a note in the header:"(do not use *_MPLANE types)".
     .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
     .target = V4L2_SEL_TGT_CROP,
     .flags = 0};
  memcpy(&selection.r, &display_area, sizeof(struct v4l2_rect));
  if (ioctl(queue->v4lfd, VIDIOC_S_SELECTION, &selection) != 0) {
    LOG_ERROR("VIDIOC_S_SELECTION failed: %s.", strerror(errno));
    return false;
  }
  assert(selection.r.left == 0);
  assert(selection.r.top == 0);
  LOG_INFO("Queue selection %dx%d", selection.r.width, selection.r.height);
  return true;
}

int request_mmap_buffers(struct queue* queue,
                         struct v4l2_requestbuffers* reqbuf) {
  const int v4lfd = queue->v4lfd;
  const uint32_t buffer_alloc = reqbuf->count * sizeof(struct mmap_buffers);
  struct mmap_buffers* buffers = (struct mmap_buffers*)malloc(buffer_alloc);
  assert(buffers);
  memset(buffers, 0, buffer_alloc);
  queue->buffers = buffers;
  queue->cnt = reqbuf->count;

  int ret;
  for (uint32_t i = 0; i < reqbuf->count; ++i) {
    struct v4l2_buffer buffer;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    memset(&buffer, 0, sizeof(buffer));
    buffer.type = reqbuf->type;
    buffer.memory = queue->memory;
    buffer.index = i;
    buffer.length = queue->num_planes;
    buffer.m.planes = planes;
    ret = ioctl(v4lfd, VIDIOC_QUERYBUF, &buffer);
    if (ret != 0) {
      LOG_ERROR("VIDIOC_QUERYBUF failed: %d.", ret);
      break;
    }

    for (uint32_t j = 0; j < queue->num_planes; ++j) {
      buffers[i].length[j] = buffer.m.planes[j].length;
      buffers[i].start[j] =
          mmap(NULL, buffer.m.planes[j].length, PROT_READ | PROT_WRITE,
               MAP_SHARED, v4lfd, buffer.m.planes[j].m.mem_offset);
      if (MAP_FAILED == buffers[i].start[j]) {
        LOG_ERROR("Failed to mmap buffer of length(%d) and offset(0x%x).",
                  buffer.m.planes[j].length, buffer.m.planes[j].m.mem_offset);
        ret = -1;
      }
    }
  }

  return ret;
}

int request_dmabuf_buffers(struct gbm_device* gbm,
                           struct queue* CAPTURE_queue,
                           uint64_t modifier, const int count) {
  int ret = 0;
  const uint32_t buffer_alloc = count * sizeof(struct mmap_buffers);
  struct mmap_buffers* buffers = (struct mmap_buffers*)malloc(buffer_alloc);
  assert(buffers);
  memset(buffers, 0, buffer_alloc);
  CAPTURE_queue->buffers = buffers;
  CAPTURE_queue->cnt = count;

  uint32_t gbm_format = GBM_FORMAT_NV12;
  if (CAPTURE_queue->fourcc == V4L2_PIX_FMT_P010)
    gbm_format = GBM_FORMAT_P010;

  for (uint32_t i = 0; i < CAPTURE_queue->cnt; ++i) {
    const uint32_t width = CAPTURE_queue->coded_width;
    const uint32_t height = CAPTURE_queue->coded_height;

    struct gbm_bo* bo = gbm_bo_create_with_modifiers(
        gbm, width, height, gbm_format, &modifier, /*count=*/1);
    assert(bo);
    CAPTURE_queue->buffers[i].bo = bo;
  }

  return ret;
}

// NV12 to I420 conversion.
// This function converts the NV12 |buffer_in| into an I420 buffers stored in a
// struct data_buffer. Ownership of the return structure's |.data| member is
// transferred to the caller. It must be freed elsewhere.
// |buffer_in| is padded, whereas the return buffer is tightly packed.
// Example: |display_area.left| = 0, |display_area.top| = 0,
// |display_area.width| = 8, |display_area.height| = 2, |buffer_width| = 10.
//
// NV12           I420
// YYYYYYYY00     YYYYYYYY
// YYYYYYYY00     YYYYYYYY
// UVUVUVUV00     UUUUVVVV
//
// HW pads 0s for |buffer_width - display_area.left - display_area.width|
// bytes after each row on Trogdor. But other platforms might leave the padding
// uninitialized, and in yet others accessing it might causes a crash of some
// sort (access violation).
struct data_buffer nv12_to_i420(struct v4l2_rect display_area,
                                uint32_t buffer_width,
                                uint32_t buffer_height,
                                uint8_t* buffer_in) {

  const size_t y_plane_size = display_area.width * display_area.height;

  // Both NV12 and I420 are 4:2:0, so the U and V planes are downsampled by a
  // factor of two in width and height respective to the Y plane.
  // When an image dimension is odd, round the downsampled dimension up.
  const size_t u_plane_left = display_area.left / 2;
  const size_t u_plane_top = display_area.top / 2;
  const size_t u_plane_width = (display_area.width + 1) / 2;
  const size_t u_plane_height = (display_area.height + 1) / 2;
  const size_t u_plane_size = u_plane_width * u_plane_height;

  struct data_buffer buffer_out = {.data = NULL,
                                   .size = y_plane_size + 2 * u_plane_size};
  buffer_out.data = malloc(sizeof(uint8_t) * buffer_out.size);
  assert(buffer_out.data);
  memset(buffer_out.data, 0, sizeof(uint8_t) * buffer_out.size);

  // Copies luma data from |buffer_in| one row at a time
  // to avoid touching the padding.
  for (int row = 0; row < display_area.height; ++row) {
    memcpy(buffer_out.data + row * display_area.width,
           buffer_in + display_area.left +
             (display_area.top + row) * buffer_width,
           display_area.width);
  }

  uint8_t* u_plane_out = &buffer_out.data[y_plane_size];
  uint8_t* v_plane_out = u_plane_out + u_plane_size;
  const size_t uv_plane_offset = buffer_width * buffer_height;

  for (int row = 0; row < u_plane_height; ++row) {
    for (int column = 0; column < u_plane_width; ++column) {
      *(u_plane_out + row * u_plane_width + column) =
          buffer_in[uv_plane_offset + (u_plane_top + row) * buffer_width +
          2 * (u_plane_left + column)];

      *(v_plane_out + row * u_plane_width + column) =
          buffer_in[uv_plane_offset + (u_plane_top + row) * buffer_width +
          2 * (u_plane_left + column) + 1];
    }
  }

  return buffer_out;
}

// P010 to I010 conversion.
// Does the same deinterleaving conversion that |nv12_to_i420| does.
// P010 use the msb of the 16 bits.  In order to convert to I010 each
// pixel needs to be downshifted 6 (16 - 10).
struct data_buffer p010_to_i010(struct v4l2_rect display_area,
                                uint32_t buffer_width_in_pixels,
                                uint32_t buffer_height,
                                uint8_t* buffer_in) {

  const size_t y_plane_size_in_pixels = display_area.width *
                                        display_area.height * 2;

  // Both P010 and I010 are 4:2:0, so the U and V planes are downsampled by a
  // factor of two in width and height respective to the Y plane.
  // When an image dimension is odd, round the downsampled dimension up.
  const size_t uv_plane_left = display_area.left / 2;
  const size_t uv_plane_top = display_area.top / 2;
  const size_t uv_plane_width = (display_area.width + 1) / 2;
  const size_t uv_plane_height = (display_area.height + 1) / 2;
  const size_t uv_plane_size_in_pixels = uv_plane_width * uv_plane_height * 2;

  struct data_buffer buffer_out = {.data = NULL,
                                   .size = y_plane_size_in_pixels +
                                           2 * uv_plane_size_in_pixels};
  buffer_out.data = malloc(sizeof(uint8_t) * buffer_out.size);
  assert(buffer_out.data);
  memset(buffer_out.data, 0, sizeof(uint8_t) * buffer_out.size);

  uint16_t* y_plane_out = (uint16_t*)buffer_out.data;
  uint16_t const* y_plane_in = (uint16_t*)buffer_in;
  y_plane_in += display_area.left;
  y_plane_in += display_area.top * buffer_width_in_pixels >> 1;
  for (int y = 0; y < display_area.height; ++y) {
    for (int x = 0 ; x < display_area.width; ++x) {
      y_plane_out[x] = y_plane_in[x] >> 6;
    }
    y_plane_out += display_area.width;
    y_plane_in += buffer_width_in_pixels >> 1;
  }

  uint16_t* u_plane_out = (uint16_t*)(buffer_out.data + y_plane_size_in_pixels);
  uint16_t* v_plane_out = u_plane_out + (uv_plane_size_in_pixels >> 1);
  const size_t uv_plane_offset = buffer_width_in_pixels * buffer_height;
  uint16_t const* uv_plane_in = (uint16_t*)(buffer_in + uv_plane_offset);

  uv_plane_in += uv_plane_left;
  uv_plane_in += uv_plane_top * buffer_width_in_pixels >> 1;

  for (int row = 0; row < uv_plane_height; ++row) {
    for (int column = 0; column < uv_plane_width; ++column) {
      u_plane_out[column] = uv_plane_in[2 * column] >> 6;
      v_plane_out[column] = uv_plane_in[2 * column + 1] >> 6;
    }
    u_plane_out += uv_plane_width;
    v_plane_out += uv_plane_width;
    uv_plane_in += buffer_width_in_pixels >> 1;
  }

  return buffer_out;
}

void compute_and_print_md5hash(const void *data,
                               size_t len,
                               uint32_t frame_index,
                               const struct per_frame_md5_hash_logs*
                               md5_hash_logs) {
  struct md5_hash hash;
  MD5_CTX ctx;

  MD5_Init(&ctx);
  MD5_Update(&ctx, data, len);
  MD5_Final(hash.bytes, &ctx);

  // This printout follows the ways of Tast PlatformDecoding. Don't change !
  LOG_INFO("frame # %d - ", frame_index);

  char md5Str[33] = {};
  for (int n = 0; n < 16; ++n) {
    sprintf(&md5Str[n * 2], "%02x", hash.bytes[n]);
  }

  LOG_INFO("MD5 checksum hash: %s", md5Str);
  if (md5_hash_logs->output_file_enabled) {
    fprintf(md5_hash_logs->output_fp, "%s\n", md5Str);
  }
}

// Handles cropping a frame to the displayable area. The returned struct
// data_buffer member |data| must be freed by the caller.
struct data_buffer map_buffers_and_convert_to_i420(struct queue* CAPTURE_queue,
                                                   uint32_t queue_index) {
  assert(CAPTURE_queue->memory == V4L2_MEMORY_DMABUF ||
         CAPTURE_queue->memory == V4L2_MEMORY_MMAP);

  // Maps the frame buffer into |y_buffer| and get the buffer's stride and
  // size metadata for cropping and deinterleaving the image.
  uint32_t buffer_bytes_per_line = 0;
  uint32_t buffer_height = 0;
  size_t buffer_size = 0;
  uint8_t* plane_0_buffer = NULL;
  uint8_t* plane_1_buffer = NULL;
  int bo_fd = 0;

  if (CAPTURE_queue->memory == V4L2_MEMORY_DMABUF) {
    struct gbm_bo* bo = CAPTURE_queue->buffers[queue_index].bo;

    assert(CAPTURE_queue->fourcc != V4L2_PIX_FMT_MT21C);

    bo_fd = gbm_bo_get_fd(bo);
    assert(bo_fd > 0);
    buffer_size = lseek(bo_fd, 0, SEEK_END);
    lseek(bo_fd, 0, SEEK_SET);

    assert(gbm_bo_get_stride_for_plane(bo, 0) ==
           gbm_bo_get_stride_for_plane(bo, 1));

    buffer_bytes_per_line = gbm_bo_get_stride_for_plane(bo, 0);
    buffer_height = gbm_bo_get_height(bo);

    plane_0_buffer = mmap(0, buffer_size, PROT_READ, MAP_SHARED, bo_fd, 0);
  } else {
    assert(CAPTURE_queue->memory == V4L2_MEMORY_MMAP);

    buffer_bytes_per_line = CAPTURE_queue->bytes_per_line[0];
    buffer_height = CAPTURE_queue->coded_height;

    buffer_size = CAPTURE_queue->buffers[queue_index].length[0];
    plane_0_buffer = CAPTURE_queue->buffers[queue_index].start[0];
    if (CAPTURE_queue->fourcc == V4L2_PIX_FMT_MT21C) {
      plane_1_buffer = CAPTURE_queue->buffers[queue_index].start[1];
      buffer_size += CAPTURE_queue->buffers[queue_index].length[1];
    }
  }

  assert(buffer_bytes_per_line * buffer_height * 3 / 2 <= buffer_size);

  // Libvpx golden md5 hashes are calculated in I420 format.
  // Deinterleave |buffer_nv12| and apply the display area.
  struct data_buffer buffer_i420 = {};

  if (CAPTURE_queue->fourcc == V4L2_PIX_FMT_MT21C) {
    size_t tmp_buf_size = CAPTURE_queue->coded_width * buffer_height * 3 / 2;
    uint8_t* nv12_buffer = (uint8_t*)malloc(tmp_buf_size);
    convert_mt21_to_nv12(
        nv12_buffer, nv12_buffer + CAPTURE_queue->coded_width * buffer_height,
        plane_0_buffer, plane_1_buffer, CAPTURE_queue->coded_width,
        CAPTURE_queue->coded_height,
        CAPTURE_queue->buffers[queue_index].length[0],
        CAPTURE_queue->buffers[queue_index].length[1]);
    buffer_i420 =
        nv12_to_i420(CAPTURE_queue->display_area, CAPTURE_queue->coded_width,
                     buffer_height, nv12_buffer);
    free(nv12_buffer);
  } else if (CAPTURE_queue->fourcc == V4L2_PIX_FMT_QC08C) {
    uint8_t* nv12_buffer = (uint8_t*)malloc(buffer_size);
    convert_q08c_to_nv12(nv12_buffer,
                         nv12_buffer + buffer_bytes_per_line * buffer_height,
                         plane_0_buffer, CAPTURE_queue->coded_width,
                         CAPTURE_queue->coded_height);
    buffer_i420 =
        nv12_to_i420(CAPTURE_queue->display_area, buffer_bytes_per_line,
                     buffer_height, nv12_buffer);
    free(nv12_buffer);
  } else if (CAPTURE_queue->fourcc == V4L2_PIX_FMT_NV12) {
    buffer_i420 =
        nv12_to_i420(CAPTURE_queue->display_area, buffer_bytes_per_line,
                     buffer_height, plane_0_buffer);
  } else {
    assert(CAPTURE_queue->fourcc == V4L2_PIX_FMT_P010);
    buffer_i420 =
        p010_to_i010(CAPTURE_queue->display_area, buffer_bytes_per_line,
                     buffer_height, plane_0_buffer);
  }

  if (CAPTURE_queue->memory == V4L2_MEMORY_DMABUF) {
    assert(plane_1_buffer == NULL);
    munmap(plane_0_buffer, buffer_size);
    close(bo_fd);
  }

  return buffer_i420;
}

// Maps |CAPTURE_queue|'s buffer at |queue_index| and calculates its MD5 sum.
void map_buffers_and_calculate_md5hash(struct queue* CAPTURE_queue,
                                       uint32_t queue_index,
                                       const struct per_frame_md5_hash_logs*
                                       md5_hash_logs) {

  struct data_buffer buffer_i420 =
    map_buffers_and_convert_to_i420(CAPTURE_queue, queue_index);
  assert(buffer_i420.data);

  compute_and_print_md5hash(buffer_i420.data, buffer_i420.size,
                            CAPTURE_queue->displayed_frames, md5_hash_logs);

  free(buffer_i420.data);
}

// This is the input queue that will take compressed data.
// 4.5.1.5
int setup_OUTPUT(struct queue* OUTPUT_queue,
                 const uint32_t* optional_width,
                 const uint32_t* optional_height) {
  int ret = 0;

  // 1. Sets the coded format on OUTPUT via VIDIOC_S_FMT().
  if (!ret) {
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));

    fmt.type = OUTPUT_queue->type;
    if (optional_width)
      fmt.fmt.pix_mp.width = *optional_width;
    if (optional_height)
      fmt.fmt.pix_mp.height = *optional_height;
    fmt.fmt.pix_mp.pixelformat = OUTPUT_queue->fourcc;
    if (OUTPUT_queue->num_planes == 1)
      fmt.fmt.pix_mp.plane_fmt[0].sizeimage = kInputbufferMaxSize;
    fmt.fmt.pix_mp.num_planes = OUTPUT_queue->num_planes;

    int ret = ioctl(OUTPUT_queue->v4lfd, VIDIOC_S_FMT, &fmt);
    if (ret != 0)
      LOG_ERROR("OUTPUT queue: VIDIOC_S_FMT failed: %s.", strerror(errno));
  }

  // 2. Allocates source (bytestream) buffers via VIDIOC_REQBUFS() on OUTPUT.
  if (!ret) {
    struct v4l2_requestbuffers reqbuf = {.count = kRequestBufferCount,
                                         .type = OUTPUT_queue->type,
                                         .memory = OUTPUT_queue->memory};

    ret = ioctl(OUTPUT_queue->v4lfd, VIDIOC_REQBUFS, &reqbuf);
    if (ret != 0)
      LOG_ERROR("OUTPUT queue: VIDIOC_REQBUFS failed: %s.", strerror(errno));

    char fourcc[FOURCC_SIZE + 1];
    LOG_INFO("OUTPUT queue: %d buffers requested, %d buffers for compressed "
        "data (%s) returned.", kRequestBufferCount, reqbuf.count,
        fourcc_to_string(OUTPUT_queue->fourcc, fourcc));

    if (OUTPUT_queue->memory == V4L2_MEMORY_MMAP)
      ret = request_mmap_buffers(OUTPUT_queue, &reqbuf);
  }

  // 3. Starts streaming on the OUTPUT queue via VIDIOC_STREAMON().
  if (!ret) {
    ret = ioctl(OUTPUT_queue->v4lfd, VIDIOC_STREAMON, &OUTPUT_queue->type);
    if (ret != 0)
      LOG_ERROR("OUTPUT queue: VIDIOC_STREAMON failed: %s.", strerror(errno));
    else
      OUTPUT_queue->is_streaming = true;
  }

  return ret;
}

// Reads parsed compressed frame data from |file_buf| and submits it to
// |OUTPUT_queue|.
int submit_compressed_data(struct queue* OUTPUT_queue, uint32_t queue_index) {
  struct mmap_buffers* buffers = OUTPUT_queue->buffers;
  const size_t buf_size = fill_compressed_buffer(buffers[queue_index].start[0],
                                                 buffers[queue_index].length[0],
                                                 OUTPUT_queue->bts);

  if (!buf_size)
    return 0;

  struct v4l2_buffer v4l2_buffer;
  struct v4l2_plane planes[VIDEO_MAX_PLANES];

  memset(&v4l2_buffer, 0, sizeof(v4l2_buffer));
  v4l2_buffer.index = queue_index;
  v4l2_buffer.type = OUTPUT_queue->type;
  v4l2_buffer.memory = OUTPUT_queue->memory;
  v4l2_buffer.length = 1;
  v4l2_buffer.m.planes = planes;
  v4l2_buffer.m.planes[0].length = buffers[queue_index].length[0];
  v4l2_buffer.m.planes[0].bytesused = buf_size;
  v4l2_buffer.m.planes[0].data_offset = 0;
  int ret = ioctl(OUTPUT_queue->v4lfd, VIDIOC_QBUF, &v4l2_buffer);
  if (ret != 0) {
    LOG_ERROR("OUTPUT queue: VIDIOC_QBUF failed: %s.", strerror(errno));
    return -1;
  }

  return 0;
}

int prime_OUTPUT(struct queue* OUTPUT_queue) {
  int ret = 0;

  for (uint32_t i = 0; i < OUTPUT_queue->cnt; ++i) {
    ret = submit_compressed_data(OUTPUT_queue, i);
    if (ret || is_end_of_stream(OUTPUT_queue->bts))
      break;
  }
  return ret;
}

void cleanup_queue(struct queue* queue) {
  if (queue->cnt) {
    struct mmap_buffers* buffers = queue->buffers;

    for (uint32_t i = 0; i < queue->cnt; ++i) {
      for (uint32_t j = 0; j < queue->num_planes; ++j) {
        if (buffers[i].length[j])
          munmap(buffers[i].start[j], buffers[i].length[j]);
      }
      if (buffers[i].bo)
        gbm_bo_destroy(buffers[i].bo);
    }

    free(queue->buffers);
    queue->cnt = 0;
  }
}

int queue_buffer_CAPTURE(struct queue* queue, uint32_t index) {
  struct v4l2_buffer v4l2_buffer;
  struct v4l2_plane planes[VIDEO_MAX_PLANES];
  memset(&v4l2_buffer, 0, sizeof v4l2_buffer);
  memset(&planes, 0, sizeof planes);

  v4l2_buffer.type = queue->type;
  v4l2_buffer.memory = queue->memory;
  v4l2_buffer.index = index;
  v4l2_buffer.m.planes = planes;
  v4l2_buffer.length = queue->num_planes;

  struct gbm_bo* bo = queue->buffers[index].bo;
  for (uint32_t i = 0; i < queue->num_planes; ++i) {
    if (queue->memory == V4L2_MEMORY_DMABUF) {
      v4l2_buffer.m.planes[i].m.fd = gbm_bo_get_fd_for_plane(bo, i);
      assert(v4l2_buffer.m.planes[i].m.fd);
    } else if (queue->memory == V4L2_MEMORY_MMAP) {
      struct mmap_buffers* buffers = queue->buffers;

      v4l2_buffer.m.planes[i].length = buffers[index].length[i];
      v4l2_buffer.m.planes[i].bytesused = buffers[index].length[i];
      v4l2_buffer.m.planes[i].data_offset = 0;
    }
  }

  int ret = ioctl(queue->v4lfd, VIDIOC_QBUF, &v4l2_buffer);
  if (ret != 0)
    LOG_ERROR("CAPTURE queue: VIDIOC_QBUF failed: %s.", strerror(errno));

  if (queue->memory == V4L2_MEMORY_DMABUF) {
    for (uint32_t i = 0; i < queue->num_planes; ++i) {
      int ret_close = close(v4l2_buffer.m.planes[i].m.fd);
      if (ret_close != 0) {
        LOG_ERROR("close failed with v4l2_buffer.m.planes[%d].m.fd: %s.", i,
                  strerror(errno));
        ret = ret_close;
      }
    }
  }

  return ret;
}

// This is the output queue that will produce uncompressed frames, see 4.5.1.6
// Capture Setup
// https://www.kernel.org/doc/html/v5.4/media/uapi/v4l/dev-decoder.html#capture-setup
int setup_CAPTURE(struct gbm_device* gbm,
                  struct queue* CAPTURE_queue,
                  uint64_t modifier,
                  uint32_t coded_frame_rate) {
  int ret = 0;

  // 1. Call VIDIOC_G_FMT() on the CAPTURE queue to get format for the
  // destination buffers parsed/decoded from the bytestream.
  if (!ret) {
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));

    fmt.type = CAPTURE_queue->type;

    ret = ioctl(CAPTURE_queue->v4lfd, VIDIOC_G_FMT, &fmt);
    if (ret != 0)
      LOG_ERROR("CAPTURE queue: VIDIOC_G_FMT failed: %s.", strerror(errno));

    // By default the CAPTURE format is not known at initialization because
    // it could be 8bit or 10bit and that won't be known until the file is
    // parsed. The format provided by VIDIOC_G_FMT may not be a preferred
    // format, in which case one should be choosen from the preferred list.
    if (CAPTURE_queue->fourcc == V4L2_PIX_FMT_INVALID) {
      const size_t num_fourccs =
          sizeof(kPreferredUncompressedFourCCs) / sizeof(uint32_t);

      // Pick from a list of supported formats in order of their preference.
      for (size_t i = 0; i < num_fourccs; ++i) {
        if (is_fourcc_supported(CAPTURE_queue->v4lfd,
                                V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                                kPreferredUncompressedFourCCs[i])) {
          CAPTURE_queue->fourcc = kPreferredUncompressedFourCCs[i];
          break;
        }
      }
    }
    assert(CAPTURE_queue->fourcc != V4L2_PIX_FMT_INVALID);
    if (CAPTURE_queue->fourcc == V4L2_PIX_FMT_QC08C) {
      modifier = DRM_FORMAT_MOD_QCOM_COMPRESSED;
      LOG_INFO("CAPTURE format is compressed, setting modifier.");
    }

    // Oftentimes the driver will give us back the appropriately aligned sizes.
    // Pick them up here.
    if (CAPTURE_queue->coded_width != fmt.fmt.pix_mp.width ||
        CAPTURE_queue->coded_height != fmt.fmt.pix_mp.height) {
      LOG_DEBUG("CAPTURE queue provided adjusted coded dimensions: %dx%d -->"
          "%dx%d",CAPTURE_queue->coded_width, CAPTURE_queue->coded_height,
          fmt.fmt.pix_mp.width , fmt.fmt.pix_mp.height);
    }
    CAPTURE_queue->coded_width = fmt.fmt.pix_mp.width;
    CAPTURE_queue->coded_height = fmt.fmt.pix_mp.height;

    if (CAPTURE_queue->num_planes != fmt.fmt.pix_mp.num_planes) {
      LOG_DEBUG("CAPTURE queue provided adjusted num planes: %d --> %d",
        CAPTURE_queue->num_planes, fmt.fmt.pix_mp.num_planes);
    }

    char fourcc[FOURCC_SIZE + 1];
    LOG_INFO("CAPTURE queue: width=%d, height=%d, format=%s, num_planes=%d",
      CAPTURE_queue->coded_width, CAPTURE_queue->coded_height,
      fourcc_to_string(CAPTURE_queue->fourcc, fourcc),
      CAPTURE_queue->num_planes);
  }

  // 2. Optional. Acquire the visible resolution via VIDIOC_G_SELECTION.
  if (!ret) {
    struct v4l2_selection selection;
    memset(&selection, 0, sizeof(selection));
    selection.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    selection.target = V4L2_SEL_TGT_COMPOSE;

    ret = ioctl(CAPTURE_queue->v4lfd, VIDIOC_G_SELECTION, &selection);
    if (ret != 0)
      LOG_ERROR("VIDIOC_G_SELECTION failed: %s.", strerror(errno));

    if (CAPTURE_queue->display_area.left != selection.r.left ||
        CAPTURE_queue->display_area.top != selection.r.top ||
        CAPTURE_queue->display_area.width != selection.r.width ||
        CAPTURE_queue->display_area.height != selection.r.height) {
      LOG_DEBUG("CAPTURE queue visible area changed from %dx%d+%d+%d to "
        "%dx%d+%d+%d",
        CAPTURE_queue->display_area.width, CAPTURE_queue->display_area.height,
        CAPTURE_queue->display_area.left, CAPTURE_queue->display_area.top,
        selection.r.width , selection.r.height,
        selection.r.left , selection.r.top);
    }
    CAPTURE_queue->display_area = selection.r;

    LOG_INFO("DisplayLeft=%d, DisplayTop=%d, DisplayWidth=%d, DisplayHeight=%d",
              selection.r.left, selection.r.top,
              selection.r.width, selection.r.height);
  }


  // 4. Optional. Set the CAPTURE format via VIDIOC_S_FMT() on the CAPTURE
  // queue. The client may choose a different format than selected/suggested by
  // the decoder in VIDIOC_G_FMT().
  if (!ret) {
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = CAPTURE_queue->type;
    fmt.fmt.pix_mp.pixelformat = CAPTURE_queue->fourcc;

    fmt.fmt.pix_mp.width = CAPTURE_queue->coded_width;
    fmt.fmt.pix_mp.height = CAPTURE_queue->coded_height;

    if (CAPTURE_queue->fourcc == V4L2_PIX_FMT_MT21C) {
      // Factor in header when allocating buffers.
      fmt.fmt.pix_mp.plane_fmt[0].sizeimage = compute_mt21_min_y_size(
          CAPTURE_queue->coded_width, CAPTURE_queue->coded_height);
      fmt.fmt.pix_mp.plane_fmt[1].sizeimage = compute_mt21_min_uv_size(
          CAPTURE_queue->coded_width, CAPTURE_queue->coded_height);
    }

    ret = ioctl(CAPTURE_queue->v4lfd, VIDIOC_S_FMT, &fmt);
    if (ret != 0)
      LOG_ERROR("CAPTURE queue: VIDIOC_S_FMT failed: %s.", strerror(errno));

    // 7.31.4.
    // When the application calls the VIDIOC_S_FMT ioctl with a pointer to a
    // struct v4l2_format structure the driver checks and adjusts the
    // parameters against hardware abilities. Drivers should not return an
    // error code unless the type field is invalid, this is a mechanism to
    // fathom device capabilities and to approach parameters acceptable for
    // both the application and driver.
    assert(fmt.fmt.pix_mp.pixelformat == CAPTURE_queue->fourcc);
    CAPTURE_queue->num_planes = fmt.fmt.pix_mp.num_planes;
    if (CAPTURE_queue->fourcc == V4L2_PIX_FMT_MT21C) {
      assert(CAPTURE_queue->num_planes == 2);
    }

    for (int i = 0; i < CAPTURE_queue->num_planes; ++i) {
      CAPTURE_queue->bytes_per_line[i] =
        fmt.fmt.pix_mp.plane_fmt[i].bytesperline;
      CAPTURE_queue->size_image[i] =
        fmt.fmt.pix_mp.plane_fmt[i].sizeimage;
    }
  }

  // 5. (Optional) Sets the coded frame interval on the CAPTURE queue via
  // VIDIOC_S_PARM. Support for this feature is signaled by the
  // V4L2_FMT_FLAG_ENC_CAP_FRAME_INTERVAL format flag.
  if (!ret && coded_frame_rate != kInvalidFrameRate) {
    struct v4l2_streamparm parms;
    memset(&parms, 0, sizeof(parms));
    parms.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    // TODO(b/202758590): Handle non-integral coded_frame_rate
    parms.parm.output.timeperframe.numerator = 1;
    parms.parm.output.timeperframe.denominator = coded_frame_rate;

    LOG_INFO("Time per frame set to %d/%d seconds",
             parms.parm.output.timeperframe.numerator,
             parms.parm.output.timeperframe.denominator);

    ret = ioctl(CAPTURE_queue->v4lfd, VIDIOC_S_PARM, &parms);
    if (ret != 0)
      LOG_ERROR("CAPTURE queue: VIDIOC_S_PARM failed: %s.", strerror(errno));
  }

  // 10. Allocates CAPTURE buffers via VIDIOC_REQBUFS() on the CAPTURE queue.
  if (!ret) {
    struct v4l2_requestbuffers reqbuf;
    memset(&reqbuf, 0, sizeof(reqbuf));
    reqbuf.count = kRequestBufferCount;
    reqbuf.type = CAPTURE_queue->type;
    reqbuf.memory = CAPTURE_queue->memory;

    ret = ioctl(CAPTURE_queue->v4lfd, VIDIOC_REQBUFS, &reqbuf);
    if (ret != 0)
      LOG_ERROR("CAPTURE queue: VIDIOC_REQBUFS failed: %s.", strerror(errno));

    char fourcc[FOURCC_SIZE + 1];
    LOG_INFO("CAPTURE queue: %d buffers requested, %d buffers for decoded data "
        " (%s) returned.", kRequestBufferCount, reqbuf.count,
        fourcc_to_string(CAPTURE_queue->fourcc, fourcc));

    if (CAPTURE_queue->memory == V4L2_MEMORY_DMABUF)
      ret = request_dmabuf_buffers(gbm, CAPTURE_queue, modifier, reqbuf.count);
    else if (CAPTURE_queue->memory == V4L2_MEMORY_MMAP)
      ret = request_mmap_buffers(CAPTURE_queue, &reqbuf);
    else
      ret = -1;

    if (!ret) {
      for (uint32_t i = 0; i < reqbuf.count; ++i) {
        ret = queue_buffer_CAPTURE(CAPTURE_queue, i);
        if (ret != 0)
          break;
      }
    }
  }

  // 11. Calls VIDIOC_STREAMON() on the CAPTURE queue to start decoding frames.
  if (!ret) {
    ret = ioctl(CAPTURE_queue->v4lfd, VIDIOC_STREAMON, &CAPTURE_queue->type);
    if (ret != 0)
      LOG_ERROR("VIDIOC_STREAMON failed: %s.", strerror(errno));
    else
      CAPTURE_queue->is_streaming = true;
  }

  return ret;
}

// Writes a single frame to disk. The file path is based on the input file name
// and the frame characteristics. For example:
// Input file: /path/file.ivf
// Frame number: 12
// Image width: 800
// Image height: 450
// This results in a file written to: /path/file_frame_0012_800x450_I420.yuv
void write_frame_to_disk(const struct file_output* file_output,
                         struct queue* CAPTURE_queue,
                         uint32_t queue_index) {

  FILE* fp = 0;
  if (FILE_MULTIPLE == file_output->type) {
    #define FILE_NAME_FORMAT "%s_frame_%04d_%dx%d_I420.yuv"
    // Update FILE_NAME_FORMAT if this assert is triggered.
    assert(CAPTURE_queue->displayed_frames <= 9999);
    const int sz = snprintf(NULL, 0, FILE_NAME_FORMAT,
                            file_output->file_prefix,
                            CAPTURE_queue->displayed_frames,
                            CAPTURE_queue->display_area.width,
                            CAPTURE_queue->display_area.height);
    char file_name[sz + 1]; // note +1 for terminating null byte
    snprintf(file_name, sizeof file_name, FILE_NAME_FORMAT,
            file_output->file_prefix,
            CAPTURE_queue->displayed_frames,
            CAPTURE_queue->display_area.width,
            CAPTURE_queue->display_area.height);
    #undef FILE_NAME_FORMAT

    fp = fopen(file_name, "wb");

    if (!fp)
      LOG_ERROR("Unable to open output yuv file: %s.", file_name);
  } else if (FILE_SINGLE == file_output->type) {
    fp = file_output->single_fp;
  } else {
    return;
  }

  if (file_output->raw) {
    if (CAPTURE_queue->memory == V4L2_MEMORY_DMABUF) {
      struct gbm_bo* bo = CAPTURE_queue->buffers[queue_index].bo;
      int bo_fd = gbm_bo_get_fd(bo);
      assert(bo_fd > 0);
      off_t buffer_size = lseek(bo_fd, 0, SEEK_END);
      lseek(bo_fd, 0, SEEK_SET);

      void* buffer = mmap(0, buffer_size, PROT_READ, MAP_SHARED, bo_fd, 0);

      fwrite(buffer, buffer_size, 1, fp);

      munmap(buffer, buffer_size);
      close(bo_fd);
    } else {
      for (uint32_t i = 0; i < CAPTURE_queue->num_planes; ++i) {
        fwrite(CAPTURE_queue->buffers[queue_index].start[i],
          CAPTURE_queue->size_image[i], 1, fp);
      }
    }
  } else if (CAPTURE_queue->fourcc == V4L2_PIX_FMT_NV12 ||
             CAPTURE_queue->fourcc == V4L2_PIX_FMT_P010 ||
             CAPTURE_queue->fourcc == V4L2_PIX_FMT_MT21C ||
             CAPTURE_queue->fourcc == V4L2_PIX_FMT_QC08C) {
    struct data_buffer buffer_i420 =
      map_buffers_and_convert_to_i420(CAPTURE_queue, queue_index);

    fwrite(buffer_i420.data, buffer_i420.size, 1, fp);
    free(buffer_i420.data);
  } else {
    LOG_ERROR("No conversion to I420 for this file. Unable to write to disk.");
  }

  if (FILE_MULTIPLE == file_output->type)
    fclose(fp);
}

int dequeue_buffer(struct queue* queue,
                   uint32_t* index,
                   uint32_t* bytesused,
                   uint32_t* flags) {
  struct v4l2_buffer v4l2_buffer;
  struct v4l2_plane planes[VIDEO_MAX_PLANES] = {0};
  memset(&v4l2_buffer, 0, sizeof(v4l2_buffer));
  v4l2_buffer.type = queue->type;
  v4l2_buffer.memory = queue->memory;
  // "Applications call the VIDIOC_DQBUF ioctl to dequeue [...]. They just
  // set the type, memory and reserved fields of a struct v4l2_buffer as
  // above, when VIDIOC_DQBUF is called with a pointer to this structure the
  // driver fills the remaining fields or returns an error code."
  // https://www.kernel.org/doc/html/v4.19/media/uapi/v4l/vidioc-qbuf.html
  // Mediatek 8173 however ,needs the |length| field filed in.
  v4l2_buffer.length = queue->num_planes;

  v4l2_buffer.m.planes = planes;
  v4l2_buffer.m.planes[0].bytesused = 0;
  int ret = ioctl(queue->v4lfd, VIDIOC_DQBUF, &v4l2_buffer);

  if (index)
    *index = v4l2_buffer.index;
  if (bytesused)
    *bytesused = v4l2_buffer.m.planes[0].bytesused;
  if (flags)
    *flags = v4l2_buffer.flags;

  return ret;
}

// 4.5.1.10. Drain
// https://www.kernel.org/doc/html/v5.4/media/uapi/v4l/dev-decoder.html#drain
// initiate_drain should be called when the source ends or when the requested
// number of frames has been met.
int initiate_drain(struct queue* OUTPUT_queue) {
  //  1. Begin the drain sequence by issuing VIDIOC_DECODER_CMD().
  struct v4l2_decoder_cmd cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.cmd = V4L2_DEC_CMD_STOP;

  // V4L2_DEC_CMD_STOP may not be supported, but we haven't run into
  // a driver that doesn't support V4L2_DEC_CMD_STOP cmd.
  int ret = ioctl(OUTPUT_queue->v4lfd, VIDIOC_DECODER_CMD, &cmd);
  if (!ret)
    OUTPUT_queue->is_streaming = false;

  // 2. The decode loop needs to proceed normally as long as the CAPTURE
  // queue has buffers that can be dequeued.
  // This step occurs in the main |decode| loop.
  return ret;
}

int finish_drain(struct queue* OUTPUT_queue, struct queue* CAPTURE_queue) {
  // Steps 1 and 2 in the drain sequence have already occurred when this is
  // called.
  int ret = 0;

  // 3. Reset by issuing VIDIOC_STREAMOFF
  int ret_streamoff =
      ioctl(OUTPUT_queue->v4lfd, VIDIOC_STREAMOFF, &OUTPUT_queue->type);
  if (ret_streamoff != 0) {
    LOG_ERROR("VIDIOC_STREAMOFF failed on OUTPUT: %s", strerror(errno));
    ret = ret_streamoff;
  }

  ret_streamoff =
      ioctl(CAPTURE_queue->v4lfd, VIDIOC_STREAMOFF, &CAPTURE_queue->type);
  if (ret_streamoff != 0) {
    LOG_ERROR("VIDIOC_STREAMOFF failed on CAPTURE: %s", strerror(errno));
    ret = ret_streamoff;
  }

  return ret;
}

// From 4.5.1.9. Dynamic Resolution Change
// This is called after a source change event is caught, dequeued,
// and all pending buffers in the |CAPTURE_queue| are drained.
int handle_dynamic_resolution_change(struct gbm_device* gbm,
                                     struct queue* CAPTURE_queue,
                                     uint64_t modifier) {
  // Calls VIDIOC_STREAMOFF to stop the CAPTURE queue stream.
  int ret = ioctl(CAPTURE_queue->v4lfd, VIDIOC_STREAMOFF, &CAPTURE_queue->type);
  if (ret != 0)
    LOG_ERROR("VIDIOC_STREAMOFF failed: %s.", strerror(errno));

  // Deallocates |CAPTURE_queue| buffers via VIDIOC_REQBUFS. Calling
  // VIDIOC_REQBUFS with |count == 0| tells the device to deallocate the
  // existing buffers.
  if (!ret) {
    cleanup_queue(CAPTURE_queue);

    struct v4l2_requestbuffers reqbuf = {.count = 0,
                                         .type = CAPTURE_queue->type,
                                         .memory = CAPTURE_queue->memory};

    ret = ioctl(CAPTURE_queue->v4lfd, VIDIOC_REQBUFS, &reqbuf);
    if (ret != 0) {
      LOG_ERROR("CAPTURE queue: VIDIOC_REQBUFS failed while deallocating "
                "buffers: %s.", strerror(errno));
    }
  }

  // setup_CAPTURE will call VIDIOC_STREAMON as part of the capture setup
  // sequence, which starts capture queue streaming.
  if (!ret) {
    ret = setup_CAPTURE(gbm, CAPTURE_queue, modifier, kInvalidFrameRate);
  }

  return ret;
}

int decode(struct gbm_device* gbm,
           struct queue* CAPTURE_queue,
           struct queue* OUTPUT_queue,
           uint64_t modifier,
           const struct file_output *file_output,
           uint32_t frames_to_decode,
           const struct per_frame_md5_hash_logs* md5_hash_logs) {
  int ret = 0;

  // If no buffers have been dequeued for more than |kMaxRetryCount| retries, we
  // should exit the program. Something is wrong in the decoder or with how we
  // are controlling it.
  int num_tries = kMaxRetryCount;

  // TODO(nhebert) See if it is possible to have two events dequeue before
  // the first is handled. Assume "no" for now.
  bool pending_dynamic_resolution_change = false;

  while (ret == 0 && CAPTURE_queue->is_streaming && num_tries != 0) {
    {
      uint32_t event_type = 0;
      if (!dequeue_event(CAPTURE_queue, &event_type) &&
          event_type == V4L2_EVENT_SOURCE_CHANGE) {
        LOG_INFO("CAPTURE queue experienced a source change.");
        pending_dynamic_resolution_change = true;
      }

      uint32_t index = 0;
      uint32_t flags = 0;
      uint32_t bytesused = 0;
      const int ret_dequeue = dequeue_buffer(CAPTURE_queue, &index, &bytesused,
                                             &flags);
      if (ret_dequeue != 0) {
        if (errno != EAGAIN) {
          LOG_ERROR("VIDIOC_DQBUF failed for CAPTURE queue: %s.",
                    strerror(errno));
          ret = ret_dequeue;
          break;
        }
        num_tries--;
      } else {
        // Successfully dequeued a buffer. Reset the |num_tries| counter.
        num_tries = kMaxRetryCount;

        const bool is_flag_error_set = (flags & V4L2_BUF_FLAG_ERROR) != 0;
        const bool is_flag_last_set = (flags & V4L2_BUF_FLAG_LAST) != 0;
        if (is_flag_last_set)
          CAPTURE_queue->is_streaming = false;

        // Don't use a buffer flagged with V4L2_BUF_FLAG_ERROR regardless of
        // |bytesused| or with V4L2_BUF_FLAG_LAST and |bytesused| == 0
        const bool ignore_buffer = is_flag_error_set ||
          (is_flag_last_set && (bytesused == 0));
        if (!ignore_buffer &&
            CAPTURE_queue->displayed_frames < frames_to_decode) {
          CAPTURE_queue->displayed_frames++;

          if (FILE_NONE != file_output->type)
            write_frame_to_disk(file_output, CAPTURE_queue, index);

          if (md5_hash_logs->enabled)
            map_buffers_and_calculate_md5hash(CAPTURE_queue, index,
                                              md5_hash_logs);
        } else {
          LOG_DEBUG("Buffer set %s%s%s.",
                    is_flag_error_set ? "V4L2_BUF_FLAG_ERROR" : "",
                    (is_flag_error_set && is_flag_last_set) ? " and " : "",
                    is_flag_last_set ? "V4L2_BUF_FLAG_LAST" : "");
        }

        // When the device has decoded |frames_to_decode| displayable frames,
        // start the drain sequence.
        if (!ret && OUTPUT_queue->is_streaming &&
            CAPTURE_queue->displayed_frames >= frames_to_decode) {
          ret = initiate_drain(OUTPUT_queue);
        }

        // Done with buffer, queue it back up unless we got V4L2_BUF_FLAG_LAST
        if (!ret && !is_flag_last_set)
          ret = queue_buffer_CAPTURE(CAPTURE_queue, index);

        if (!ret && pending_dynamic_resolution_change && is_flag_last_set) {
          LOG_DEBUG("Handling dynamic resolution change.");
          pending_dynamic_resolution_change = false;
          ret = handle_dynamic_resolution_change(gbm, CAPTURE_queue, modifier);
        }
      }
    }

    // Check the OUTPUT queue for free buffers and fill accordingly.
    if (!ret) {
      uint32_t index = 0;
      const int ret_dequeue = dequeue_buffer(OUTPUT_queue, &index, NULL,
                                             NULL);
      if (ret_dequeue != 0) {
        if (errno != EAGAIN) {
          LOG_ERROR("VIDIOC_DQBUF failed for OUTPUT queue: %s.",
                    strerror(errno));
          ret = ret_dequeue;
          break;
        }
        continue;
      }

      if (OUTPUT_queue->is_streaming) {
        ret = submit_compressed_data(OUTPUT_queue, index);

        // If there are no remaining frames, we should stop the OUTPUT queue.
        // Doing so, lets the decoder know it should process the remaining
        // buffers in the output queue, then dequeue a buffer with
        // V4L2_BUF_FLAG_LAST set. After that happens, the decode loop will
        // stop.
        if (!ret && is_end_of_stream(OUTPUT_queue->bts))
          ret = initiate_drain(OUTPUT_queue);
      }
    }
  }

  if (num_tries == 0) {
    LOG_FATAL("Decoder appeared to stall after decoding %d frames.",
              CAPTURE_queue->displayed_frames);
  }

  finish_drain(OUTPUT_queue, CAPTURE_queue);

  LOG_INFO("%d displayable frames decoded.", CAPTURE_queue->displayed_frames);

  return ret;
}

static void print_help(const char* argv0) {
  printf("usage: %s [OPTIONS]\n", argv0);
  printf("  -f, --file        ivf file to decode\n");
  printf("  -w, --write       output visible frames in I420 format "
                             "to individual files (one frame per file)\n");
  printf("  -b, --raw         output visible frames in original format "
                             "one frame per file. The entire buffer is written "
                             "as is without any cropping or conversion.\n");
  printf("  -s, --single      file name to output all visible frames in I420 "
                             "format to\n");
  printf("  -m, --max         max number of visible frames to decode\n");
  printf("  -a, --mmap        use mmap instead of dmabuf\n");
  printf("  -c, --capture_fmt fourcc of the CAPTURE queue, i.e. the "
                             "decoded video format\n");
  printf("  -l, --log_level   specifies log level, 0:debug 1:info 2:error "
                             "3:fatal (default: %d)\n", DEFAULT_LOG_LEVEL);
  printf("  -r, --frame_rate  (optional) specify a frame rate (Hz)\n");
  printf("  -d, --md5        compute md5 hash for each decoded visible frame "
                             "in I420 format and displays in an INFO log. If "
                             "specified with argument will also write "
                             "checksums to specified file.\n");
}

static const struct option longopts[] = {
    {"file", required_argument, NULL, 'f'},
    {"write", no_argument, NULL, 'w'},
    {"max", required_argument, NULL, 'm'},
    {"mmap", no_argument, NULL, 'a'},
    {"capture_fmt", required_argument, NULL, 'c'},
    {"log_level", required_argument, NULL, 'l'},
    {"md5", optional_argument, NULL, 'd'},
    {"frame_rate", required_argument, NULL, 'r'},
    {"single", required_argument, NULL, 's'},
    {"raw", no_argument, NULL, 'b'},
    {0, 0, 0, 0},
};

int main(int argc, char* argv[]) {
  int c;
  char* file_name = NULL;
  uint32_t frames_to_decode = UINT_MAX;
  uint32_t frame_rate = kInvalidFrameRate;
  uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
  uint32_t uncompressed_fourcc = V4L2_PIX_FMT_INVALID;
  enum v4l2_memory CAPTURE_memory = V4L2_MEMORY_DMABUF;
  struct file_output file_output = {.raw = false};
  struct per_frame_md5_hash_logs md5_hash_logs = {.enabled = false,
                                                  .output_file_enabled = false,
                                                  .output_fp = NULL};

  while ((c = getopt_long(argc, argv, "f:m:wbac:l:d::r:s:", longopts, NULL))
          != -1) {
    switch (c) {
      case 'f':
        file_name = strdup(optarg);
        break;
      case 'm':
        frames_to_decode = atoi(optarg);
        break;
      case 's':
        if (file_output.type != FILE_NONE) {
          LOG_FATAL("Can only write out single OR multiple files, not both");
        } else {
          char* path_str = strdup(optarg);
          char* dir_str = dirname(path_str);
          mkdir(dir_str, 0777);
          free(path_str);
          file_output.single_fp = fopen(optarg, "wb");
          file_output.type = FILE_SINGLE;
          if (!file_output.single_fp)
            LOG_FATAL("Unable to create output yuv file: %s.", optarg);
        }
        break;
      case 'b':
        file_output.raw = true;
      case 'w':
        if (file_output.type != FILE_NONE) {
          LOG_FATAL("Can only write out single OR multiple files, not both");
        } else {
          file_output.type = FILE_MULTIPLE;
        }
        break;
      case 'a':
        CAPTURE_memory = V4L2_MEMORY_MMAP;
        break;
      case 'c':
        if (strlen(optarg) == 4) {
          uncompressed_fourcc =
              v4l2_fourcc(toupper(optarg[0]), toupper(optarg[1]),
                          toupper(optarg[2]), toupper(optarg[3]));

          char fourcc[FOURCC_SIZE + 1];
          LOG_INFO("User-provided CAPTURE format: %s",
              fourcc_to_string(uncompressed_fourcc, fourcc));

          if (uncompressed_fourcc == V4L2_PIX_FMT_QC08C) {
            modifier = DRM_FORMAT_MOD_QCOM_COMPRESSED;
            LOG_INFO("CAPTURE format is compressed, setting modifier.");
          }
        }
        break;
      case 'l': {
        const int specified_log_run_level = atoi(optarg);
        if (specified_log_run_level >= kLoggingLevelMax) {
          LOG_INFO("Undefined log level %d, using default log level instead.",
                   specified_log_run_level);
        } else {
          log_run_level = specified_log_run_level;
        }
        break;
      }
      case 'd':
        md5_hash_logs.enabled = true;
        if(!optarg)
          break;

        md5_hash_logs.output_fp = fopen(optarg, "w");
        if(!md5_hash_logs.output_fp)
          LOG_FATAL("Unable to create md5 checksum log file.");

        md5_hash_logs.output_file_enabled = true;
        break;
      case 'r':
        frame_rate = atoi(optarg);
        if (frame_rate == kInvalidFrameRate)
        {
          LOG_FATAL("Invalid frame rate provided - %s. --frame_rate requires "
                    "positive integer less than 2^32.", optarg);
        }
        break;
      default:
        LOG_FATAL("Invalid argument syntax");
        break;
    }
  }

  LOG_INFO("Simple v4l2 decode.");

  if (frames_to_decode != UINT_MAX)
    LOG_INFO("Only decoding a max of %d frames.", frames_to_decode);

  if (!file_name) {
    print_help(argv[0]);
    exit(1);
  }

  int drm_device_fd = -1;
  struct gbm_device* gbm = NULL;
  const bool use_gbm = V4L2_MEMORY_DMABUF == CAPTURE_memory;
  if (use_gbm) {
    drm_device_fd = bs_drm_open_main_display();
    if (drm_device_fd < 0) {
      LOG_FATAL("Failed to open card for display.");
    }

    gbm = gbm_create_device(drm_device_fd);
    if (!gbm) {
      close(drm_device_fd);
      LOG_FATAL("Failed to create gbm device.");
    }
  }

  struct bitstream bts;
  init_bitstream(file_name, &bts);

  if (FILE_MULTIPLE == file_output.type) {
    const size_t len_prefix = strrchr(file_name, '.') - file_name;
    file_output.file_prefix = strndup(file_name, len_prefix);
  }

  int v4lfd = open(kDecodeDevice, O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (v4lfd < 0)
    LOG_FATAL("Unable to open device file: %s.", kDecodeDevice);
  query_driver(v4lfd);

  if (!capabilities(v4lfd, kDecodeDevice, bts.fourcc, &uncompressed_fourcc))
    LOG_FATAL("Not enough capabilities present for decoding.");

  struct queue OUTPUT_queue = {.v4lfd = v4lfd,
                               .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                               .fourcc = bts.fourcc,
                               .num_planes = 1,
                               .memory = V4L2_MEMORY_MMAP,
                               .displayed_frames = 0,
                               .is_streaming = false,
                               .bts = &bts};
  int ret = setup_OUTPUT(&OUTPUT_queue, /*optional_width=*/NULL,
       /*optional_height=*/NULL);

  if (!ret)
    ret = prime_OUTPUT(&OUTPUT_queue);

  struct queue CAPTURE_queue = {.v4lfd = v4lfd,
                                .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                                .fourcc = uncompressed_fourcc,
                                .num_planes = 1,
                                .memory = CAPTURE_memory,
                                .displayed_frames = 0,
                                .is_streaming = false};
  if (!ret) {
    ret = setup_CAPTURE(gbm, &CAPTURE_queue, modifier, frame_rate);
  }

  if (subscribe_to_event(v4lfd, V4L2_EVENT_SOURCE_CHANGE, 0 /* id */))
    LOG_FATAL("Unable to subscribe to source change event.");

  if (!ret) {
    ret = decode(gbm, &CAPTURE_queue, &OUTPUT_queue, modifier,
                 &file_output, frames_to_decode, &md5_hash_logs);
  }

  if (file_output.file_prefix)
    free(file_output.file_prefix);
  if (file_output.single_fp)
    fclose(file_output.single_fp);

  cleanup_queue(&OUTPUT_queue);
  cleanup_queue(&CAPTURE_queue);
  close(v4lfd);
  if (use_gbm)
    close(drm_device_fd);
  free(file_name);
  cleanup_bitstream(&bts);

  // Closes checksum log file if specified
  if (md5_hash_logs.output_file_enabled)
    fclose(md5_hash_logs.output_fp);

  return ret;
}
