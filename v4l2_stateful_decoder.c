/*
 * Copyright 2021 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

// As per https://www.kernel.org/doc/html/v5.4/media/uapi/v4l/dev-decoder.html
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <openssl/md5.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "bs_drm.h"

#define FOURCC_SIZE 4

enum logging_levels {
  kLoggingInfo = 0,
  kLoggingError,
  kLoggingFatal,
  kLoggingLevelMax
};

#define DEFAULT_LOG_LEVEL kLoggingInfo

#define LOG(level, stream, fmt, ...)       \
  do {                                     \
    if (level >= log_run_level) {          \
      fprintf(stream, fmt, ##__VA_ARGS__); \
      fprintf(stream, "\n");               \
    }                                      \
  } while (0)

#define LOG_INFO(fmt, ...) LOG(kLoggingInfo, stdout, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) LOG(kLoggingError, stderr, fmt, ##__VA_ARGS__)
#define LOG_FATAL(fmt, ...)                         \
  do {                                              \
    LOG(kLoggingFatal, stderr, fmt, ##__VA_ARGS__); \
    exit(EXIT_FAILURE);                             \
  } while (0)

// TODO(frkoenig):
// P010 had not landed as an official V4L2 format yet. Once it has remove this.
#ifndef V4L2_PIX_FMT_P010
#define V4L2_PIX_FMT_P010    v4l2_fourcc('P', '0', '1', '0') /* 15  Y/CbCr 4:2:0 10-bit per pixel*/
#endif
// Only available in kernels 5.4 and later.
#ifndef V4L2_PIX_FMT_NV12_UBWC
#define V4L2_PIX_FMT_NV12_UBWC v4l2_fourcc('Q', '1', '2', '8') /* UBWC 8-bit Y/CbCr 4:2:0  */

#endif

static const char* kDecodeDevice = "/dev/video-dec0";
static const int kInputbufferMaxSize = 4 * 1024 * 1024;
static const int kRequestBufferCount = 8;
static const int kTimestampMarkerForShowFrames = 1;
static const uint32_t kIVFHeaderSignature = v4l2_fourcc('D', 'K', 'I', 'F');

static int log_run_level = DEFAULT_LOG_LEVEL;

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
  // |display_width| x |display_height|:
  // The size of the image on the screen.
  uint32_t display_width;
  uint32_t display_height;
  // |coded_width| x |coded_height|:
  // The size of the encoded frame.
  // Usually has an alignment of 16, 32 depending on codec.
  uint32_t coded_width;
  uint32_t coded_height;
  uint32_t cnt;
  uint32_t num_planes;
  uint32_t memory;
  uint32_t processed_frames;
  uint32_t displayed_frames;
  // Set to true when a frame is recieved with V4L2_BUF_FLAG_LAST set. This
  // happens when a decoder returns the last frame from its stream. Any
  // subsequent attempts to dequeue will fail with EPIPE.
  bool buf_last_flag_has_been_set;
};

struct ivf_file_header {
  uint32_t signature;
  uint16_t version;
  uint16_t header_length;
  uint32_t fourcc;
  uint16_t width;
  uint16_t height;
  uint32_t denominator;
  uint32_t numerator;
  uint32_t frame_cnt;
  uint32_t unused;
} __attribute__((packed));

struct ivf_frame_header {
  uint32_t size;
  uint64_t timestamp;
} __attribute__((packed));

struct compressed_file {
  FILE* fp;
  struct ivf_file_header header;
};

struct md5_hash {
  uint8_t bytes[16];
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

struct compressed_file open_file(const char* file_name) {
  struct compressed_file file = {0};

  FILE* fp = fopen(file_name, "rb");
  if (fp) {
    if (fread(&file.header, sizeof(struct ivf_file_header), 1, fp) != 1) {
      fclose(fp);
      LOG_ERROR("Unable to read ivf file header.");
    }

    if (file.header.signature != kIVFHeaderSignature) {
      fclose(fp);
      LOG_ERROR("Incorrect header signature : 0x%0x != 0x%0x",
                file.header.signature, kIVFHeaderSignature);
    }

    file.fp = fp;

    char fourcc[FOURCC_SIZE + 1];
    LOG_INFO("OUTPUT format: %s", fourcc_to_string(file.header.fourcc, fourcc));

    LOG_INFO("Ivf file header: %d x %d", file.header.width, file.header.height);
    // |width| and |height| should be even numbers.
    // This is because nv12 to i420 conversion (needed for md5hash computation)
    // assumes |width| and |height| to be even numbers.
    assert((file.header.width % 2) == 0);
    assert((file.header.height % 2) == 0);
    LOG_INFO("Ivf file header: frame_cnt = %d", file.header.frame_cnt);
  } else {
    LOG_ERROR("Unable to open file: %s.", file_name);
  }

  return file;
}

int query_format(int v4lfd, enum v4l2_buf_type type, uint32_t fourcc) {
  struct v4l2_fmtdesc fmtdesc;
  memset(&fmtdesc, 0, sizeof(fmtdesc));

  fmtdesc.type = type;
  while (ioctl(v4lfd, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
    if (fourcc == 0) {
      char fourcc_str[FOURCC_SIZE + 1];
      LOG_INFO("%s", fourcc_to_string(fmtdesc.pixelformat, fourcc_str));
    } else if (fourcc == fmtdesc.pixelformat)
      return 1;
    fmtdesc.index++;
  }

  return 0;
}

int capabilities(int v4lfd,
                 uint32_t compressed_format,
                 uint32_t uncompressed_format) {
  struct v4l2_capability cap;
  memset(&cap, 0, sizeof(cap));
  int ret = ioctl(v4lfd, VIDIOC_QUERYCAP, &cap);
  if (ret != 0)
    LOG_ERROR("VIDIOC_QUERYCAP failed: %s.", strerror(errno));

  LOG_INFO("Driver=\"%s\" bus_info=\"%s\" card=\"%s\" fd=0x%x", cap.driver,
           cap.bus_info, cap.card, v4lfd);

  if (!query_format(v4lfd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                    compressed_format)) {
    char fourcc_str[FOURCC_SIZE + 1];
    LOG_ERROR("%s is not supported, supported formats are...",
        fourcc_to_string(compressed_format, fourcc_str));
    query_format(v4lfd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, 0);
    ret = 1;
  }

  if (!query_format(v4lfd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                    uncompressed_format)) {
    char fourcc_str[FOURCC_SIZE + 1];
    LOG_ERROR("%s is not supported, supported formats are...",
        fourcc_to_string(uncompressed_format, fourcc_str));
    query_format(v4lfd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 0);
    ret = 1;
  }

  return ret;
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

  uint32_t gbm_format;
  if (CAPTURE_queue->fourcc == V4L2_PIX_FMT_NV12 ||
      CAPTURE_queue->fourcc == V4L2_PIX_FMT_MT21C) {
    gbm_format = GBM_FORMAT_NV12;
  } else if (CAPTURE_queue->fourcc == V4L2_PIX_FMT_P010) {
    gbm_format = GBM_FORMAT_P010;
  } else {
    char fourcc_str[FOURCC_SIZE + 1];
    LOG_ERROR("%s format not supported for the CAPTURE_queue",
        fourcc_to_string(CAPTURE_queue->fourcc, fourcc_str));
    return -1;
  }

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
// This function converts the NV12 |buffer_in| into an I420 |buffer_out|.
// |buffer_in| is padded, whereas |buffer_out| is tightly packed.
// Example: |display_width| = 8, |display_height| = 2, |buffer_width| = 10.
//
// NV12           I420
// YYYYYYYY00     YYYYYYYY
// YYYYYYYY00     YYYYYYYY
// UVUVUVUV00     UUUUVVVV
//
// HW pads 0s for |buffer_width - display_width| bytes after each row on
// Trogdor. But other platforms might leave the padding uninitialized, and in
// yet others accessing it might causes a crash of some sort (access violation).
void nv12_to_i420(uint32_t display_width,
                  uint32_t display_height,
                  uint32_t buffer_width,
                  uint32_t buffer_height,
                  uint8_t* buffer_in,
                  uint8_t* buffer_out) {
  // Copies luma data from |buffer_in| one row at a time
  // to avoid touching the padding.
  for (int row = 0; row < display_height; ++row)
    memcpy(buffer_out + row * display_width, buffer_in + row * buffer_width,
           display_width);

  const size_t y_plane_size = display_width * display_height;
  const size_t u_plane_size = y_plane_size / 4;
  uint8_t* u_plane_out = &buffer_out[y_plane_size];
  uint8_t* v_plane_out = u_plane_out + u_plane_size;
  const size_t uv_plane_offset = buffer_width * buffer_height;

  for (int row = 0; row < display_height / 2; ++row) {
    for (int column = 0; column < display_width / 2; ++column) {
      *(u_plane_out + row * display_width / 2 + column) =
          buffer_in[uv_plane_offset + row * buffer_width + 2 * column];

      *(v_plane_out + row * display_width / 2 + column) =
          buffer_in[uv_plane_offset + row * buffer_width + 2 * column + 1];
    }
  }
}

// Computes md5 hash for each frame.
struct md5_hash compute_md5hash(struct queue* CAPTURE_queue,
                                uint32_t queue_index) {
  // TODO: needs to handle V4L2_MEMORY_MMAP case
  assert(CAPTURE_queue->memory == V4L2_MEMORY_DMABUF);
  // TODO: only handles 8 bit pixels
  assert(CAPTURE_queue->fourcc == V4L2_PIX_FMT_NV12);

  struct gbm_bo* bo = CAPTURE_queue->buffers[queue_index].bo;
  int bo_fd = gbm_bo_get_fd(bo);
  size_t buffer_size = lseek(bo_fd, 0, SEEK_END);
  lseek(bo_fd, 0, SEEK_SET);

  assert(gbm_bo_get_stride_for_plane(bo, 0) ==
         gbm_bo_get_stride_for_plane(bo, 1));

  uint8_t* buffer_nv12 = mmap(0, buffer_size, PROT_READ, MAP_SHARED, bo_fd, 0);

  // Libvpx golden md5 hashes are calculated in I420 format.
  // Uses |buffer_i420| to deinterleave |buffer_nv12| for this purpose.
  const uint32_t display_width = CAPTURE_queue->display_width;
  const uint32_t display_height = CAPTURE_queue->display_height;
  // buffer w x h :
  // The size of the buffer that the frame will be stored in.
  // Usually the driver has limitations on alignment depending on HW.
  const uint32_t buffer_width = gbm_bo_get_stride_for_plane(bo, 0);
  const uint32_t buffer_height = gbm_bo_get_height(bo);

  assert(buffer_width * buffer_height <= buffer_size);

  const uint32_t buffer_i420_size_in_bytes =
      (display_width * display_height * 3) / 2;

  uint8_t* buffer_i420 = malloc(sizeof(size_t) * buffer_i420_size_in_bytes);
  assert(buffer_i420);
  memset(buffer_i420, 0, sizeof(size_t) * buffer_i420_size_in_bytes);

  nv12_to_i420(display_width, display_height, buffer_width, buffer_height,
               buffer_nv12, buffer_i420);

  struct md5_hash hash;

  MD5_CTX ctx;
  MD5_Init(&ctx);
  MD5_Update(&ctx, buffer_i420, buffer_i420_size_in_bytes);
  MD5_Final(hash.bytes, &ctx);

  free(buffer_i420);

  munmap(buffer_nv12, buffer_size);

  return hash;
}

// This is the input queue that will take compressed data.
// 4.5.1.5
int setup_OUTPUT(struct queue* OUTPUT_queue) {
  int ret = 0;

  // 1. Sets the coded format on OUTPUT via VIDIOC_S_FMT().
  if (!ret) {
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));

    fmt.type = OUTPUT_queue->type;
    fmt.fmt.pix_mp.pixelformat = OUTPUT_queue->fourcc;
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = kInputbufferMaxSize;
    fmt.fmt.pix_mp.num_planes = 1;

    int ret = ioctl(OUTPUT_queue->v4lfd, VIDIOC_S_FMT, &fmt);
    if (ret != 0)
      LOG_ERROR("VIDIOC_S_FMT failed: %s.", strerror(errno));
  }

  // 2. Allocates source (bytestream) buffers via VIDIOC_REQBUFS() on OUTPUT.
  if (!ret) {
    struct v4l2_requestbuffers reqbuf;
    memset(&reqbuf, 0, sizeof(reqbuf));
    reqbuf.count = kRequestBufferCount;
    reqbuf.type = OUTPUT_queue->type;
    reqbuf.memory = OUTPUT_queue->memory;

    ret = ioctl(OUTPUT_queue->v4lfd, VIDIOC_REQBUFS, &reqbuf);
    if (ret != 0)
      LOG_ERROR("VIDIOC_REQBUFS failed: %s.", strerror(errno));

    char fourcc[FOURCC_SIZE + 1];
    LOG_INFO("%d buffers requested, %d buffers for compressed data (%s) "
        "returned.", kRequestBufferCount, reqbuf.count,
        fourcc_to_string(OUTPUT_queue->fourcc, fourcc));

    ret = request_mmap_buffers(OUTPUT_queue, &reqbuf);
  }

  // 3. Starts streaming on the OUTPUT queue via VIDIOC_STREAMON().
  if (!ret) {
    ret = ioctl(OUTPUT_queue->v4lfd, VIDIOC_STREAMON, &OUTPUT_queue->type);
    if (ret != 0)
      LOG_ERROR("VIDIOC_STREAMON failed: %s.", strerror(errno));
  }

  return ret;
}

int submit_compressed_frame(struct compressed_file* file,
                            struct queue* OUTPUT_queue,
                            uint32_t queue_index,
                            uint32_t frames_to_decode) {
  const uint32_t num = file->header.numerator;
  const uint32_t den = file->header.denominator;

  if (OUTPUT_queue->displayed_frames >= frames_to_decode) {
    LOG_INFO(
        "Already prepared requested number of frames |frames_to_decode|"
        "from OUTPUT_queue.");

    return 0;
  }

  struct ivf_frame_header frame_header = {0};

  if (fread(&frame_header, sizeof(struct ivf_frame_header), 1, file->fp) != 1) {
    if (!feof(file->fp)) {
      LOG_ERROR("Unable to read ivf frame header.");
      return -1;
    } else {
      LOG_INFO("Unable to read ivf frame header, reached the end of ivf file.");
      return 0;
    }
  }

  struct mmap_buffers* buffers = OUTPUT_queue->buffers;
  if (fread(buffers[queue_index].start[0], sizeof(uint8_t), frame_header.size,
            file->fp) != frame_header.size) {
    LOG_ERROR("Unable to read ivf frame data.");
    return -1;
  }

  OUTPUT_queue->processed_frames++;

  if (is_vp9(file->header.fourcc)) {
    // VP9 Bitstream and Decoding Process Specification
    // Sec 6.2 "Uncompressed header syntax", pp.28.
    // https://storage.googleapis.com/downloads.webmproject.org/docs/vp9/vp9-bitstream-specification-v0.6-20160331-draft.pdf
    //
    // |show_frame| is the second bit (counting from the least
    // significant one) of the VP9 frame header's first byte.
    const bool show_frame =
        (*(uint32_t*)buffers[queue_index].start[0] & (1 << 1));

    // Assumption here is that an non-displayed frame is always followed by a
    // displayed frame. Please reference super-frame for more details.
    // http://downloads.webmproject.org/docs/vp9/vp9-bitstream_superframe-and-uncompressed-header_v1.0.pdf
    if (!show_frame)
      OUTPUT_queue->processed_frames++;
  }

  struct v4l2_buffer v4l2_buffer;
  struct v4l2_plane planes[VIDEO_MAX_PLANES];

  memset(&v4l2_buffer, 0, sizeof(v4l2_buffer));
  v4l2_buffer.index = queue_index;
  v4l2_buffer.type = OUTPUT_queue->type;
  v4l2_buffer.memory = OUTPUT_queue->memory;
  v4l2_buffer.length = 1;
  // Intentionally starting timestamp |kTimestampMarkerForShowFrames| at 1
  // so that we can detect non-displayed frames with simpler logic.
  v4l2_buffer.timestamp.tv_sec = kTimestampMarkerForShowFrames;
  v4l2_buffer.timestamp.tv_usec = ((frame_header.timestamp * den) / num) * 100;
  v4l2_buffer.m.planes = planes;
  v4l2_buffer.m.planes[0].length = buffers[queue_index].length[0];
  v4l2_buffer.m.planes[0].bytesused = frame_header.size;
  v4l2_buffer.m.planes[0].data_offset = 0;
  int ret = ioctl(OUTPUT_queue->v4lfd, VIDIOC_QBUF, &v4l2_buffer);
  if (ret != 0) {
    LOG_ERROR("VIDIOC_QBUF failed: %s.", strerror(errno));
    return -1;
  }

  // Counts processed displayed frames in OUTPUT_queue. Non-displayed frames
  // are not counted as non-displayed frame is a part of a super-frame.
  OUTPUT_queue->displayed_frames++;

  return 0;
}

int prime_OUTPUT(struct compressed_file* file,
                 struct queue* OUTPUT_queue,
                 uint32_t frames_to_decode) {
  int ret = 0;

  for (uint32_t i = 0; i < OUTPUT_queue->cnt; ++i) {
    ret = submit_compressed_frame(file, OUTPUT_queue, i, frames_to_decode);
    if (ret)
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
    LOG_ERROR("VIDIOC_QBUF failed: %s.", strerror(errno));

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

// This is the output queue that will produce uncompressed frames.
// 4.5.1.6
int setup_CAPTURE(struct gbm_device* gbm,
                  struct queue* CAPTURE_queue,
                  uint64_t modifier) {
  int ret = 0;

  // 1. Calls VIDIOC_G_FMT() on the CAPTURE queue to get format for the
  //    destination buffers parsed/decoded from the bytestream.
  if (!ret) {
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = CAPTURE_queue->type;

    int ret = ioctl(CAPTURE_queue->v4lfd, VIDIOC_G_FMT, &fmt);
    if (ret != 0)
      LOG_ERROR("VIDIOC_G_FMT failed: %s.", strerror(errno));

    CAPTURE_queue->coded_width = fmt.fmt.pix_mp.width;
    CAPTURE_queue->coded_height = fmt.fmt.pix_mp.height;
    CAPTURE_queue->num_planes = fmt.fmt.pix_mp.num_planes;

    LOG_INFO("CAPTURE: %d x %d", fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height);
    LOG_INFO("num_planes = %d", fmt.fmt.pix_mp.num_planes);
  }

  // 4. Optional. Sets the CAPTURE format via VIDIOC_S_FMT() on the CAPTURE
  // queue.
  //    The client may choose a different format than selected/suggested by the
  //    decoder in VIDIOC_G_FMT().
  if (!ret) {
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = CAPTURE_queue->type;
    fmt.fmt.pix_mp.pixelformat = CAPTURE_queue->fourcc;

    fmt.fmt.pix_mp.width = CAPTURE_queue->coded_width;
    fmt.fmt.pix_mp.height = CAPTURE_queue->coded_height;

    ret = ioctl(CAPTURE_queue->v4lfd, VIDIOC_S_FMT, &fmt);
    if (ret != 0)
      LOG_ERROR("VIDIOC_S_FMT failed: %s.", strerror(errno));
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
      LOG_ERROR("VIDIOC_REQBUFS failed: %s.", strerror(errno));

    char fourcc[FOURCC_SIZE + 1];
    LOG_INFO("%d buffers requested, %d buffers for decoded data (%s) "
        "returned.", kRequestBufferCount, reqbuf.count,
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
  }

  return ret;
}

void write_file_to_disk(FILE* fp,
                        struct queue* CAPTURE_queue,
                        uint32_t queue_index,
                        uint32_t frame_index,
                        bool show_frame) {
  if (V4L2_MEMORY_DMABUF == CAPTURE_queue->memory) {
    struct gbm_bo* bo = CAPTURE_queue->buffers[queue_index].bo;
    int bo_fd = gbm_bo_get_fd(bo);
    assert(bo_fd);

    assert(gbm_bo_get_stride_for_plane(bo, 0) ==
           gbm_bo_get_stride_for_plane(bo, 1));

    const uint32_t buffer_width = gbm_bo_get_stride_for_plane(bo, 0);
    const uint32_t buffer_height = gbm_bo_get_height(bo);

    const uint32_t buffer_size = (buffer_width * buffer_height * 3) / 2;

    uint8_t* buffer_nv12 =
        mmap(0, buffer_size, PROT_READ, MAP_SHARED, bo_fd, 0);

    fwrite(buffer_nv12, buffer_size, 1, fp);

    munmap(buffer_nv12, buffer_size);

    close(bo_fd);
  } else {
    if (CAPTURE_queue->num_planes == 1) {
      size_t buffer_size =
          (3 * CAPTURE_queue->coded_width * CAPTURE_queue->coded_height) >> 1;
      uint8_t* buffer = CAPTURE_queue->buffers[queue_index].start[0];

      fwrite(buffer, buffer_size, 1, fp);
    } else {
      for (uint32_t i = 0; i < CAPTURE_queue->num_planes; ++i) {
        size_t buffer_size =
            (CAPTURE_queue->coded_width * CAPTURE_queue->coded_height) >> i;
        uint8_t* buffer = CAPTURE_queue->buffers[queue_index].start[i];

        fwrite(buffer, buffer_size, 1, fp);
      }
    }
  }
}

int dequeue_buffer(struct queue* queue,
                   uint32_t* index,
                   uint32_t* bytesused,
                   uint32_t* flags,
                   bool* show_frame) {
  struct v4l2_buffer v4l2_buffer;
  struct v4l2_plane planes[VIDEO_MAX_PLANES] = {0};
  memset(&v4l2_buffer, 0, sizeof(v4l2_buffer));
  v4l2_buffer.type = queue->type;
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

  // For displayed frames, timestamp values will be increasing for each
  // frame starting from 1 (which can be configured differently for the
  // first frame). For non-displayed frames, timestamp values will be 0.
  // Thus, this info can be used to distinguish non-displayed frames
  // from displayed frames.
  if (show_frame) {
    *show_frame =
        !(v4l2_buffer.timestamp.tv_sec < kTimestampMarkerForShowFrames);
  }

  if (v4l2_buffer.flags & V4L2_BUF_FLAG_LAST) {
    queue->buf_last_flag_has_been_set = true;
  }

  return ret;
}

// 4.5.1.10. Drain
// https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/dev-decoder.html#drain
int drain(struct queue* OUTPUT_queue, struct queue* CAPTURE_queue) {
  // 1. Begin the drain sequence by issuing VIDIOC_DECODER_CMD().
  struct v4l2_decoder_cmd cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.cmd = V4L2_DEC_CMD_STOP;

  // V4L2_DEC_CMD_STOP may not be supported, but we haven't run into
  // a driver that doesn't support V4L2_DEC_CMD_STOP cmd.
  int ret = ioctl(OUTPUT_queue->v4lfd, VIDIOC_DECODER_CMD, &cmd);
  assert(ret == 0);

  // 2. Dequeue buffers
  // The way the decode loop is set up, there shouldn't be any buffers
  // left to dequeue. Furthermore, if the CAPTURE_queue has already set
  // V4L2_BUF_FLAG_LAST, then there is no need to dequeue.
  if (!CAPTURE_queue->buf_last_flag_has_been_set ) {
    uint32_t bytesused = 0;
    uint32_t flags = 0;
    const int max_drain_delay = 2048;
    // Checks to make sure the queue is empty.
    // It takes time to execute V4L2_DEC_CMD_STOP cmd.
    // From experiments, iterating |kMaxDrainDelay| times
    // was good enough to get V4L2_BUF_FLAG_LAST flag.
    int counter;
    for (counter = max_drain_delay; counter > 0; --counter) {
      ret = dequeue_buffer(CAPTURE_queue, NULL, &bytesused, &flags, NULL);

      if (ret == 0) {
        break;
      } else {
        if (errno != EAGAIN) {
          LOG_ERROR("VIDIOC_DQBUF failed: %s", strerror(errno));
          break;
        }
      }
    }

    if (counter == 0) {
      LOG_ERROR(
          "V4L2_DEC_CMD_STOP cmd execution was not completed within %d "
          "iterations.\n",
          max_drain_delay);
      ret = 1;
    }

    if (!CAPTURE_queue->buf_last_flag_has_been_set && (bytesused != 0)) {
      LOG_ERROR("WARNING: CAPTURE queue did not clean up.\n");
      ret = 1;
    }
  }

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

int decode(struct compressed_file* file,
           struct queue* CAPTURE_queue,
           struct queue* OUTPUT_queue,
           uint64_t modifier,
           FILE* output_file,
           uint32_t frames_to_decode,
           bool print_md5hash) {
  int ret = 0;

  while (CAPTURE_queue->processed_frames < OUTPUT_queue->processed_frames) {
    {
      uint32_t index = 0;
      uint32_t flags = 0;
      bool show_frame;
      const int ret_dequeue = dequeue_buffer(CAPTURE_queue, &index, NULL,
                                             &flags, &show_frame);
      if (ret_dequeue != 0) {
        if (errno != EAGAIN) {
          LOG_ERROR("VIDIOC_DQBUF failed for CAPTURE queue: %s.",
                    strerror(errno));
          ret = ret_dequeue;
          break;
        }
        continue;
      }

      if (show_frame) {
        CAPTURE_queue->displayed_frames++;
        if (output_file) {
          write_file_to_disk(output_file, CAPTURE_queue, index,
                             CAPTURE_queue->processed_frames, show_frame);
        }
      }

      CAPTURE_queue->processed_frames++;

      if (print_md5hash && show_frame) {
        LOG_INFO("frame # %d - ", CAPTURE_queue->displayed_frames);

        struct md5_hash hash = compute_md5hash(CAPTURE_queue, index);

        for (int n = 0; n < 16; ++n)
          printf("%02x", hash.bytes[n]);
        printf("\n");
      }

      // Break out of the decode loop if V4L2_BUF_FLAG_LAST is set.
      if (CAPTURE_queue->buf_last_flag_has_been_set) {
        LOG_INFO("CAPTURE_queue produced last buffer.");
        break;
      }

      // Done with buffer, queue it back up.
      ret = queue_buffer_CAPTURE(CAPTURE_queue, index);
    }

    // A frame was recieved on the CAPTURE queue, that means there should
    // now be a free OUTPUT buffer.
    {
      uint32_t index = 0;
      const int ret_dequeue = dequeue_buffer(OUTPUT_queue, &index, NULL,
                                             NULL, NULL);
      if (ret_dequeue != 0) {
        if (errno != EAGAIN) {
          LOG_ERROR("VIDIOC_DQBUF failed for OUTPUT queue: %s.",
                    strerror(errno));
          ret = ret_dequeue;
          break;
        }
        continue;
      }

      submit_compressed_frame(file, OUTPUT_queue, index, frames_to_decode);
    }
  }

  drain(OUTPUT_queue, CAPTURE_queue);

  LOG_INFO("%d frames decoded, %d displayable, %d non-visible.",
           CAPTURE_queue->processed_frames, CAPTURE_queue->displayed_frames,
           CAPTURE_queue->processed_frames - CAPTURE_queue->displayed_frames);

  return ret;
}

static void print_help(const char* argv0) {
  printf("usage: %s [OPTIONS]\n", argv0);
  printf("  -f, --file        ivf file to decode\n");
  printf("  -w, --write       write out decompressed frames to a file\n");
  printf("  -m, --max         max number of visible frames to decode\n");
  printf("  -b, --buffer      use mmap instead of dmabuf\n");
  printf("  -o, --output_fmt  fourcc of output format\n");
  printf("  -l, --log_level   specifies log level, 0:info 1:error 2:fatal \n");
  printf(
      "  -d, --md5         compute md5 hash for each decoded visible frame "
      "in I420 format\n");
}

static const struct option longopts[] = {
    {"file", required_argument, NULL, 'f'},
    {"write", no_argument, NULL, 'w'},
    {"max", required_argument, NULL, 'm'},
    {"buffer", no_argument, NULL, 'b'},
    {"output_fmt", required_argument, NULL, 'o'},
    {"log_level", required_argument, NULL, 'l'},
    {"md5", no_argument, NULL, 'd'},
    {0, 0, 0, 0},
};

int main(int argc, char* argv[]) {
  int c;
  char* file_name = NULL;
  bool write_out = false;
  bool print_md5hash = false;
  uint32_t frames_to_decode = UINT_MAX;
  uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
  uint32_t uncompressed_fourcc = V4L2_PIX_FMT_NV12;
  uint32_t CAPTURE_memory = V4L2_MEMORY_DMABUF;

  while ((c = getopt_long(argc, argv, "wbdm:f:o:l:", longopts, NULL)) != -1) {
    switch (c) {
      case 'f':
        file_name = strdup(optarg);
        break;
      case 'm':
        frames_to_decode = atoi(optarg);
        break;
      case 'w':
        write_out = true;
        break;
      case 'b':
        CAPTURE_memory = V4L2_MEMORY_MMAP;
        break;
      case 'o':
        if (strlen(optarg) == 4) {
          uncompressed_fourcc =
              v4l2_fourcc(toupper(optarg[0]), toupper(optarg[1]),
                          toupper(optarg[2]), toupper(optarg[3]));
          if (uncompressed_fourcc == V4L2_PIX_FMT_NV12_UBWC)
            modifier = DRM_FORMAT_MOD_QCOM_COMPRESSED;
        }
        break;
      case 'l': {
        const uint32_t specified_log_run_level = atoi(optarg);
        if (specified_log_run_level >= kLoggingLevelMax) {
          LOG_INFO("Undefined log level %d, using default log level instead.",
                   specified_log_run_level);
        } else {
          log_run_level = specified_log_run_level;
        }
        break;
      }
      case 'd':
        print_md5hash = true;
        break;
      default:
        break;
    }
  }

  LOG_INFO("Simple v4l2 decode.");

  if (frames_to_decode != UINT_MAX)
    LOG_INFO("Only decoding a max of %d frames.", frames_to_decode);

  char fourcc[FOURCC_SIZE + 1];
  LOG_INFO("CAPTURE format: %s", fourcc_to_string(uncompressed_fourcc, fourcc));

  if (uncompressed_fourcc == V4L2_PIX_FMT_NV12_UBWC)
    LOG_INFO("Compressed format, setting modifier.");

  if (!file_name) {
    print_help(argv[0]);
    exit(1);
  }

  int drm_device_fd = bs_drm_open_main_display();
  if (drm_device_fd < 0) {
    LOG_FATAL("Failed to open card for display.");
  }

  struct gbm_device* gbm = gbm_create_device(drm_device_fd);
  if (!gbm) {
    close(drm_device_fd);
    LOG_FATAL("Failed to create gbm device.");
  }

  struct compressed_file compressed_file = open_file(file_name);
  if (!compressed_file.fp)
    LOG_FATAL("Unable to open ivf file: %s.", file_name);

  FILE* output_file = NULL;

  if (write_out) {
    // Replaces |.ivf| with |.yuv| in |file_name|.
    strcpy(strrchr(file_name, '.'), ".yuv");

    output_file = fopen(file_name, "wb");

    if (!output_file) {
      LOG_ERROR("Unable to open output yuv file: %s.", file_name);
      return 1;
    }
  }

  int v4lfd = open(kDecodeDevice, O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (v4lfd < 0)
    LOG_FATAL("Unable to open device file: %s.", kDecodeDevice);

  if (capabilities(v4lfd, compressed_file.header.fourcc, uncompressed_fourcc) !=
      0)
    LOG_FATAL("Not enough capabilities present for decoding.");

  struct queue OUTPUT_queue = {.v4lfd = v4lfd,
                               .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                               .fourcc = compressed_file.header.fourcc,
                               .num_planes = 1,
                               .memory = V4L2_MEMORY_MMAP,
                               .processed_frames = 0,
                               .displayed_frames = 0};
  int ret = setup_OUTPUT(&OUTPUT_queue);

  if (!ret)
    ret = prime_OUTPUT(&compressed_file, &OUTPUT_queue, frames_to_decode);

  struct queue CAPTURE_queue = {.v4lfd = v4lfd,
                                .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                                .fourcc = uncompressed_fourcc,
                                .num_planes = 1,
                                .memory = CAPTURE_memory,
                                .display_width = compressed_file.header.width,
                                .display_height = compressed_file.header.height,
                                .processed_frames = 0,
                                .displayed_frames = 0};
  if (!ret)
    ret = setup_CAPTURE(gbm, &CAPTURE_queue, modifier);

  if (!ret) {
    ret = decode(&compressed_file, &CAPTURE_queue, &OUTPUT_queue, modifier,
                 output_file, frames_to_decode, print_md5hash);
  }

  if (write_out)
    fclose(output_file);

  cleanup_queue(&OUTPUT_queue);
  cleanup_queue(&CAPTURE_queue);
  close(v4lfd);
  fclose(compressed_file.fp);
  close(drm_device_fd);
  free(file_name);

  return ret;
}
