/*
 * Copyright 2021 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
// https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/dev-encoder.html
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(USE_V4LPLUGIN)
// Some devices (e.g. Rockchip) have a userspace "libv4l" library to bridge the
// stateful V4L2 encoder API with a stateless encoder hardware.
#include <libv4l2.h>
#endif

#define MAX_HIERARCHICAL_LAYERS 6

// TODO(mcasas): Consider trying all /dev/video-enc* device files if a platform
// has more than two.
static const char* kEncodeDeviceFiles[] = {"/dev/video-enc0",
                                           "/dev/video-enc1"};
static const int kInputbufferMaxSize = 4 * 1024 * 1024;
static const int kRequestBufferCount = 8;
static const uint32_t kIVFHeaderSignature = v4l2_fourcc('D', 'K', 'I', 'F');

int do_ioctl(int fd, int request, void* data) {
#if defined(USE_V4LPLUGIN)
  return v4l2_ioctl(fd, request, data);
#else
  return ioctl(fd, request, data);
#endif
}

struct file_buffer {
  uint8_t* buffer;
  uint32_t frame_size;
};

struct file_info {
  FILE* fp;
  uint32_t format;
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
  uint32_t raw_width;
  uint32_t raw_height;
  uint32_t encoded_width;
  uint32_t encoded_height;
  uint32_t cnt;
  uint32_t frame_cnt;
  uint32_t num_planes;
  uint32_t framerate;
  uint32_t strides[VIDEO_MAX_PLANES];
};

struct encoder_control {
  uint32_t id;
  uint32_t value;
  uint32_t enabled;
  char* name;
};

#define ENCODER_CTL_DEFAULT(ID, VALUE) \
  { .id = ID, .value = VALUE, .enabled = 1, .name = #ID }

#define ENCODER_CTL(ID) \
  { .id = ID, .value = 0, .enabled = 0, .name = #ID }

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

void print_fourcc(uint32_t fourcc) {
  printf("%c%c%c%c\n", fourcc & 0xff, fourcc >> 8 & 0xff, fourcc >> 16 & 0xff,
         fourcc >> 24 & 0xff);
}

int query_format(int v4lfd, enum v4l2_buf_type type, uint32_t fourcc) {
  struct v4l2_fmtdesc fmtdesc;
  memset(&fmtdesc, 0, sizeof(fmtdesc));

  fmtdesc.type = type;
  while (do_ioctl(v4lfd, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
    if (fourcc == 0)
      print_fourcc(fmtdesc.pixelformat);
    else if (fourcc == fmtdesc.pixelformat)
      return 1;
    fmtdesc.index++;
  }

  return 0;
}

void enumerate_menu(int v4lfd, uint32_t id, uint32_t min, uint32_t max) {
  struct v4l2_querymenu querymenu;
  memset(&querymenu, 0, sizeof(querymenu));

  querymenu.id = id;
  for (querymenu.index = min; querymenu.index <= max; querymenu.index++) {
    if (0 == do_ioctl(v4lfd, VIDIOC_QUERYMENU, &querymenu))
      fprintf(stderr, " %s\n", querymenu.name);
  }
}

int verify_capabilities(int v4lfd,
                        uint32_t OUTPUT_format,
                        uint32_t CAPTURE_format) {
  struct v4l2_capability cap;
  memset(&cap, 0, sizeof(cap));
  int ret = do_ioctl(v4lfd, VIDIOC_QUERYCAP, &cap);
  if (ret != 0)
    perror("VIDIOC_QUERYCAP failed");

  printf("driver=\"%s\" bus_info=\"%s\" card=\"%s\" fd=0x%x\n", cap.driver,
         cap.bus_info, cap.card, v4lfd);

  if (!query_format(v4lfd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, OUTPUT_format)) {
    printf("Insufficient supported OUTPUT formats:\n");
    query_format(v4lfd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, 0);
    printf("Wanted: ");
    print_fourcc(OUTPUT_format);
    ret = 1;
  }

  if (!query_format(v4lfd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                    CAPTURE_format)) {
    printf("Insufficient supported CAPTURE formats:\n");
    query_format(v4lfd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 0);
    printf("Wanted: ");
    print_fourcc(CAPTURE_format);
    ret = 1;
  }

  return ret;
}

int queue_OUTPUT_buffer(struct queue* queue,
                        struct mmap_buffers* buffers,
                        uint32_t index) {
  // compute frame timestamp
  const float usec_per_frame = (1.0 / queue->framerate) * 1000000;
  const uint64_t usec_time_stamp = usec_per_frame * queue->frame_cnt;
  const uint64_t tv_sec = usec_time_stamp / 1000000;

  struct v4l2_buffer v4l2_buffer;
  struct v4l2_plane planes[VIDEO_MAX_PLANES];
  memset(&v4l2_buffer, 0, sizeof(v4l2_buffer));

  v4l2_buffer.index = index;
  v4l2_buffer.type = queue->type;
  v4l2_buffer.memory = V4L2_MEMORY_MMAP;
  v4l2_buffer.length = queue->num_planes;
  v4l2_buffer.timestamp.tv_sec = tv_sec;
  v4l2_buffer.timestamp.tv_usec = usec_time_stamp - tv_sec;
  v4l2_buffer.sequence = queue->frame_cnt;
  v4l2_buffer.m.planes = planes;
  for (uint32_t i = 0; i < queue->num_planes; ++i) {
    v4l2_buffer.m.planes[i].length = buffers[index].length[i];
    v4l2_buffer.m.planes[i].bytesused = buffers[index].length[i];
    v4l2_buffer.m.planes[i].data_offset = 0;
  }

  int ret = do_ioctl(queue->v4lfd, VIDIOC_QBUF, &v4l2_buffer);
  if (ret != 0) {
    perror("VIDIOC_QBUF failed");
    return -1;
  }

  queue->frame_cnt++;

  return 0;
}

// This function copies the contents pointed by |frame_buffer|
// to |queue|s |index| buffer.  Handle the case where the format
// of the file on disk is the same as the encoding format.
int submit_raw_frame_in_bulk(const uint8_t* frame_buffer,
                             struct queue* queue,
                             uint32_t index) {
  assert(queue->num_planes == 1 || queue->num_planes == 2);
  assert(queue->raw_width == queue->encoded_width);
  // TODO: the code below assumes NV12 because the Chroma planes are copied in
  // one call. Extend to YV12 if ever the need arises.
  assert(queue->fourcc == v4l2_fourcc('N', 'V', '1', '2'));

  struct mmap_buffers* buffers = queue->buffers;

  // Read luma plane first.
  const size_t luma_plane_size = queue->raw_width * queue->raw_height;
  uint8_t* buffer = buffers[index].start[0];
  memcpy(buffer, frame_buffer, luma_plane_size);

  // Now read both chroma planes together.
  if (queue->num_planes == 2)
    buffer = buffers[index].start[1];
  else
    buffer += queue->encoded_width * queue->encoded_height;

  const size_t chroma_planes_size = luma_plane_size / 2;
  memcpy(buffer, frame_buffer + luma_plane_size, chroma_planes_size);

  return queue_OUTPUT_buffer(queue, buffers, index);
}

// This function copies the contents pointed by |frame_buffer|
// to |queue|s |index| buffer.  Copy row by row for the situations
// where the width/height of the v4l2 buffer is different than
// that of the file on disk, or when conversion between formats is necessary.
int submit_raw_frame_row_by_row(const uint8_t* frame_buffer,
                                uint32_t file_format,
                                struct queue* queue,
                                uint32_t index) {
  assert(queue->num_planes == 1 || queue->num_planes == 3);
  assert(queue->fourcc == v4l2_fourcc('Y', 'V', '1', '2') ||
         queue->fourcc == v4l2_fourcc('Y', 'M', '1', '2') ||
         queue->fourcc == v4l2_fourcc('N', 'V', '1', '2'));
  assert(file_format == v4l2_fourcc('Y', 'V', '1', '2'));

  struct mmap_buffers* buffers = queue->buffers;
  const size_t luma_raw_width = queue->raw_width;

  // Read luma plane first, row by row.
  uint8_t* buffer = buffers[index].start[0];
  for (int row = 0; row < queue->raw_height; ++row) {
    memcpy(buffer, frame_buffer, luma_raw_width);
    frame_buffer += luma_raw_width;
    buffer += queue->encoded_width;
  }

  const size_t chroma_raw_width = luma_raw_width / 2;
  // Now read the chroma planes.
  if ((queue->fourcc == v4l2_fourcc('Y', 'V', '1', '2') ||
       queue->fourcc == v4l2_fourcc('Y', 'M', '1', '2')) &&
      file_format == v4l2_fourcc('Y', 'V', '1', '2')) {
    assert((queue->fourcc == v4l2_fourcc('Y', 'M', '1', '2') &&
            queue->num_planes == 3) ||
           (queue->fourcc == v4l2_fourcc('Y', 'V', '1', '2') &&
            queue->num_planes == 1));

    buffer = buffers[index].start[1];
    const uint32_t stride_u = queue->strides[1];

    for (int row = 0; row < queue->raw_height / 2; ++row) {
      memcpy(buffer, frame_buffer, chroma_raw_width);
      frame_buffer += chroma_raw_width;
      buffer += stride_u;
    }

    uint32_t stride_v = stride_u;
    if (queue->num_planes == 3) {
      buffer = buffers[index].start[2];
      stride_v = queue->strides[2];
    } else {
      assert(queue->num_planes == 1);
      buffer = buffers[index].start[0] +
               5 * queue->encoded_width * queue->encoded_height / 4;
    }

    for (int row = 0; row < queue->raw_height / 2; ++row) {
      memcpy(buffer, frame_buffer, chroma_raw_width);
      frame_buffer += chroma_raw_width;
      buffer += stride_v;
    }

  } else if (queue->fourcc == v4l2_fourcc('N', 'V', '1', '2') &&
             file_format == v4l2_fourcc('Y', 'V', '1', '2') &&
             queue->num_planes == 1) {
    assert(queue->num_planes == 1);
    buffer =
        buffers[index].start[0] + queue->encoded_width * queue->encoded_height;
    const uint8_t* u_ptr = frame_buffer;
    const uint8_t* v_ptr =
        frame_buffer + ((queue->raw_width * queue->raw_height) / 4);
    for (int row = 0; row < queue->raw_height / 2; ++row) {
      for (int col = 0; col < chroma_raw_width; ++col) {
        buffer[col * 2] = u_ptr[col];
        buffer[col * 2 + 1] = v_ptr[col];
      }
      buffer += queue->encoded_width;
      u_ptr += chroma_raw_width;
      v_ptr += chroma_raw_width;
    }
  } else {
    fprintf(stderr,
            "combination of queue format, number of planes, and file format "
            "unsupported\n");
    return -1;
  }

  return queue_OUTPUT_buffer(queue, buffers, index);
}

// Read the contents of a frame from the file on disk to a memory buffer.
// This makes later format conversions quicker as there is no need to
// read a byte at a time from the disk.
// The frame is then copied to a |queue| buffer and submitted to the driver
// by the leaf functions.
int submit_raw_frame(const struct file_buffer* fb,
                     const struct file_info* fi,
                     struct queue* queue,
                     uint32_t index) {
  if (fread(fb->buffer, fb->frame_size, 1, fi->fp) != 1) {
    fprintf(stderr, "unable to read frame into memory\n");
    return -1;
  }

  if (queue->raw_width == queue->encoded_width && queue->fourcc == fi->format &&
      queue->fourcc == v4l2_fourcc('N', 'V', '1', '2')) {
    return submit_raw_frame_in_bulk(fb->buffer, queue, index);
  }

  return submit_raw_frame_row_by_row(fb->buffer, fi->format, queue, index);
}

void cleanup_queue(struct queue* queue) {
  if (queue->cnt) {
    struct mmap_buffers* buffers = queue->buffers;

    for (uint32_t i = 0; i < queue->cnt; i++)
      for (uint32_t j = 0; j < queue->num_planes; j++) {
        munmap(buffers[i].start[j], buffers[i].length[j]);
      }

    free(queue->buffers);
    queue->cnt = 0;
  }
}

int request_mmap_buffers(struct queue* queue,
                         struct v4l2_requestbuffers* reqbuf) {
  const int v4lfd = queue->v4lfd;
  const uint32_t buffer_alloc = reqbuf->count * sizeof(struct mmap_buffers);
  struct mmap_buffers* buffers = (struct mmap_buffers*)malloc(buffer_alloc);
  memset(buffers, 0, buffer_alloc);
  queue->buffers = buffers;
  queue->cnt = reqbuf->count;

  int ret;
  for (uint32_t i = 0; i < reqbuf->count; i++) {
    struct v4l2_buffer buffer;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    memset(&buffer, 0, sizeof(buffer));
    buffer.type = reqbuf->type;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = i;
    buffer.length = queue->num_planes;
    buffer.m.planes = planes;
    ret = do_ioctl(v4lfd, VIDIOC_QUERYBUF, &buffer);
    if (ret != 0) {
      printf("VIDIOC_QUERYBUF failed: %d\n", ret);
      break;
    }

    for (uint32_t j = 0; j < queue->num_planes; j++) {
      buffers[i].length[j] = buffer.m.planes[j].length;
      buffers[i].start[j] =
          mmap(NULL, buffer.m.planes[j].length, PROT_READ | PROT_WRITE,
               MAP_SHARED, v4lfd, buffer.m.planes[j].m.mem_offset);
      if (MAP_FAILED == buffers[i].start[j]) {
        fprintf(stderr,
                "failed to mmap buffer of length(%d) and offset(0x%x)\n",
                buffer.m.planes[j].length, buffer.m.planes[j].m.mem_offset);
      }
    }
  }

  return ret;
}

int queue_CAPTURE_buffer(struct queue* queue, uint32_t index) {
  struct mmap_buffers* buffers = queue->buffers;
  struct v4l2_buffer v4l2_buffer;
  struct v4l2_plane planes[VIDEO_MAX_PLANES];
  memset(&v4l2_buffer, 0, sizeof v4l2_buffer);
  memset(&planes, 0, sizeof planes);

  v4l2_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  v4l2_buffer.memory = V4L2_MEMORY_MMAP;
  v4l2_buffer.index = index;
  v4l2_buffer.m.planes = planes;
  v4l2_buffer.length = queue->num_planes;

  v4l2_buffer.m.planes[0].length = buffers[index].length[0];
  v4l2_buffer.m.planes[0].bytesused = buffers[index].length[0];
  v4l2_buffer.m.planes[0].data_offset = 0;

  int ret = do_ioctl(queue->v4lfd, VIDIOC_QBUF, &v4l2_buffer);
  if (ret != 0) {
    perror("VIDIOC_QBUF failed");
  }

  return ret;
}

// 4.5.2.5. Initialization
int Initialization(struct queue* OUTPUT_queue, struct queue* CAPTURE_queue) {
  int ret = 0;

  // 1. Set the coded format on the CAPTURE queue via VIDIOC_S_FMT().
  if (!ret) {
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));

    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.pixelformat = CAPTURE_queue->fourcc;
    fmt.fmt.pix_mp.width = CAPTURE_queue->raw_width;
    fmt.fmt.pix_mp.height = CAPTURE_queue->raw_height;
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = kInputbufferMaxSize;
    fmt.fmt.pix_mp.num_planes = 1;

    int ret = do_ioctl(CAPTURE_queue->v4lfd, VIDIOC_S_FMT, &fmt);
    if (ret != 0)
      perror("VIDIOC_S_FMT failed");

    CAPTURE_queue->encoded_width = fmt.fmt.pix_mp.width;
    CAPTURE_queue->encoded_height = fmt.fmt.pix_mp.height;
  }

  // 3. Set the raw source format on the OUTPUT queue via VIDIOC_S_FMT().
  if (!ret) {
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));

    fmt.type = OUTPUT_queue->type;
    fmt.fmt.pix_mp.pixelformat = OUTPUT_queue->fourcc;
    fmt.fmt.pix_mp.width = OUTPUT_queue->raw_width;
    fmt.fmt.pix_mp.height = OUTPUT_queue->raw_height;
    fmt.fmt.pix_mp.num_planes = 1;

    int ret = do_ioctl(OUTPUT_queue->v4lfd, VIDIOC_S_FMT, &fmt);
    if (ret != 0)
      perror("VIDIOC_S_FMT failed");

    OUTPUT_queue->encoded_width = fmt.fmt.pix_mp.width;
    OUTPUT_queue->encoded_height = fmt.fmt.pix_mp.height;

    OUTPUT_queue->num_planes = fmt.fmt.pix_mp.num_planes;
    assert(OUTPUT_queue->num_planes <= VIDEO_MAX_PLANES);
    printf("OUTPUT_queue %d planes, (%dx%d)\n", OUTPUT_queue->num_planes,
           OUTPUT_queue->encoded_width, OUTPUT_queue->encoded_height);
    for (int plane = 0; plane < OUTPUT_queue->num_planes; ++plane) {
      OUTPUT_queue->strides[plane] =
          fmt.fmt.pix_mp.plane_fmt[plane].bytesperline;
      printf(" plane %d, stride %d\n", plane, OUTPUT_queue->strides[plane]);
    }
  }

  // 4. Set the raw frame interval on the OUTPUT queue via VIDIOC_S_PARM()
  if (!ret) {
    struct v4l2_streamparm parms;
    memset(&parms, 0, sizeof(parms));
    parms.type = OUTPUT_queue->type;
    // Note that we are provided "frames per second" but V4L2 expects "time per
    // frame"; hence we provide the reciprocal of the framerate here.
    parms.parm.output.timeperframe.numerator = 1;
    parms.parm.output.timeperframe.denominator = OUTPUT_queue->framerate;

    const int temp_ret = do_ioctl(OUTPUT_queue->v4lfd, VIDIOC_S_PARM, &parms);
    if (temp_ret != 0)
      perror("Optional frame rate VIDIOC_S_PARAM failed");
  }

  // 6. Optional. Set the visible resolution for the stream metadata via
  //    VIDIOC_S_SELECTION() on the OUTPUT queue if it is desired to be
  //    different than the full OUTPUT resolution.
  if (!ret) {
    struct v4l2_selection selection_arg;
    memset(&selection_arg, 0, sizeof(selection_arg));
    selection_arg.type = OUTPUT_queue->type;
    selection_arg.target = V4L2_SEL_TGT_CROP;
    selection_arg.r.left = 0;
    selection_arg.r.top = 0;
    selection_arg.r.width = OUTPUT_queue->raw_width;
    selection_arg.r.height = OUTPUT_queue->raw_height;

    const int temp_ret =
        do_ioctl(OUTPUT_queue->v4lfd, VIDIOC_S_SELECTION, &selection_arg);
    if (temp_ret != 0)
      perror("Optional visible rectangle VIDIOC_S_SELECTION failed");

    // TODO(fritz) : check returned values are same as sent values
  }

  // 7. Allocate buffers for both OUTPUT and CAPTURE via VIDIOC_REQBUFS().
  //    This may be performed in any order.
  if (!ret) {
    struct v4l2_requestbuffers reqbuf;
    memset(&reqbuf, 0, sizeof(reqbuf));
    reqbuf.count = kRequestBufferCount;
    reqbuf.type = OUTPUT_queue->type;
    reqbuf.memory = V4L2_MEMORY_MMAP;

    ret = do_ioctl(OUTPUT_queue->v4lfd, VIDIOC_REQBUFS, &reqbuf);
    if (ret != 0)
      perror("VIDIOC_REQBUFS failed");

    printf(
        "%d buffers requested, %d buffers for uncompressed frames returned\n",
        kRequestBufferCount, reqbuf.count);

    ret = request_mmap_buffers(OUTPUT_queue, &reqbuf);
  }

  if (!ret) {
    struct v4l2_requestbuffers reqbuf;
    memset(&reqbuf, 0, sizeof(reqbuf));
    reqbuf.count = kRequestBufferCount;
    reqbuf.type = CAPTURE_queue->type;
    reqbuf.memory = V4L2_MEMORY_MMAP;

    ret = do_ioctl(OUTPUT_queue->v4lfd, VIDIOC_REQBUFS, &reqbuf);
    if (ret != 0)
      perror("VIDIOC_REQBUFS failed");

    printf("%d buffers requested, %d buffers for compressed frames returned\n",
           kRequestBufferCount, reqbuf.count);

    ret = request_mmap_buffers(CAPTURE_queue, &reqbuf);
    for (uint32_t i = 0; i < reqbuf.count; i++) {
      queue_CAPTURE_buffer(CAPTURE_queue, i);
    }
  }

  return ret;
}

void disable_unsupported_controls(int v4lfd,
                                  struct encoder_control* encoder_ctrl) {
  while (encoder_ctrl->id != 0) {
    if (encoder_ctrl->enabled) {
      struct v4l2_query_ext_ctrl query_ext_ctrl;
      memset(&query_ext_ctrl, 0, sizeof(query_ext_ctrl));

      query_ext_ctrl.id = V4L2_CTRL_CLASS_MPEG | encoder_ctrl->id;

      int ret = do_ioctl(v4lfd, VIDIOC_QUERY_EXT_CTRL, &query_ext_ctrl);

      encoder_ctrl->enabled = (ret == 0);

      if (ret != 0) {
        fprintf(stderr, "%s control does not exist on this platform\n",
                encoder_ctrl->name);
      }
    }
    encoder_ctrl++;
  };
}

int enabled_control_count(const struct encoder_control* encoder_ctrl) {
  int cnt = 0;

  for (; encoder_ctrl && encoder_ctrl->id != 0; ++encoder_ctrl)
    cnt += encoder_ctrl->enabled;

  return cnt;
}

void set_control_value(struct encoder_control* encoder_ctrl,
                       uint32_t id,
                       uint32_t value) {
  for (; encoder_ctrl && encoder_ctrl->id != 0; ++encoder_ctrl) {
    if (encoder_ctrl->id == id) {
      encoder_ctrl->value = value;
      encoder_ctrl->enabled = 1;
      break;
    }
  }
}

int get_control_value(const struct encoder_control* encoder_ctrl, uint32_t id) {
  for (; encoder_ctrl && encoder_ctrl->id != 0; ++encoder_ctrl) {
    if (encoder_ctrl->id == id)
      return encoder_ctrl->value;
  };

  return -1;
}

const char* get_control_name(const struct encoder_control* encoder_ctrl,
                             uint32_t id) {
  for (; encoder_ctrl && encoder_ctrl->id != 0; ++encoder_ctrl) {
    if (encoder_ctrl->id == id)
      return encoder_ctrl->name;
  };

  return "unknown";
}

void fill_ext_controls(const struct encoder_control* encoder_ctrl,
                       struct v4l2_ext_control* ext_ctrl) {
  int enabled_i = 0;
  for (; encoder_ctrl && encoder_ctrl->id != 0; ++encoder_ctrl) {
    if (encoder_ctrl->enabled) {
      ext_ctrl[enabled_i].id = encoder_ctrl->id;
      ext_ctrl[enabled_i].value = encoder_ctrl->value;
      enabled_i++;
    }
  }
}

int set_ext_controls(int v4lfd, const struct encoder_control* encoder_ctrl) {
  const int ctrl_cnt = enabled_control_count(encoder_ctrl);

  struct v4l2_ext_control ext_ctrl[ctrl_cnt];
  memset(&ext_ctrl, 0, sizeof(ext_ctrl));

  fill_ext_controls(encoder_ctrl, ext_ctrl);

  struct v4l2_ext_controls ext_ctrls;
  memset(&ext_ctrls, 0, sizeof(ext_ctrls));

  ext_ctrls.ctrl_class = V4L2_CTRL_CLASS_MPEG;
  ext_ctrls.count = ctrl_cnt;
  ext_ctrls.controls = ext_ctrl;

  const int set_ret = do_ioctl(v4lfd, VIDIOC_S_EXT_CTRLS, &ext_ctrls);
  if (set_ret != 0)
    perror("VIDIOC_S_EXT_CTRLS failed");

  for (uint32_t i = 0; i < ctrl_cnt; ++i)
    ext_ctrl[i].value = 0;

  const int get_ret = do_ioctl(v4lfd, VIDIOC_G_EXT_CTRLS, &ext_ctrls);
  if (get_ret != 0)
    perror("VIDIOC_G_EXT_CTRLS failed");

  for (int i = 0; i < ctrl_cnt; ++i) {
    int ctrl_value = get_control_value(encoder_ctrl, ext_ctrl[i].id);
    if (ext_ctrl[i].value != ctrl_value) {
      fprintf(
          stderr,
          "control (%s) used a value of (%d) instead of the requested (%d)\n",
          get_control_name(encoder_ctrl, ext_ctrl[i].id), ext_ctrl[i].value,
          ctrl_value);
    }
  }

  return set_ret | get_ret;
}

int dequeue_buffer(struct queue* queue,
                   uint32_t* index,
                   uint32_t* bytesused,
                   uint32_t* data_offset,
                   uint64_t* timestamp,
                   uint32_t* flags) {
  struct v4l2_buffer v4l2_buffer;
  struct v4l2_plane planes[VIDEO_MAX_PLANES] = {0};
  memset(&v4l2_buffer, 0, sizeof(v4l2_buffer));
  v4l2_buffer.type = queue->type;
  v4l2_buffer.length = queue->num_planes;
  v4l2_buffer.m.planes = planes;
  v4l2_buffer.m.planes[0].bytesused = 0;
  int ret = do_ioctl(queue->v4lfd, VIDIOC_DQBUF, &v4l2_buffer);

  if (ret != 0 && errno != EAGAIN)
    perror("VIDIOC_DQBUF failed");

  *index = v4l2_buffer.index;
  if (bytesused)
    *bytesused = v4l2_buffer.m.planes[0].bytesused;
  if (data_offset)
    *data_offset = v4l2_buffer.m.planes[0].data_offset;
  if (timestamp)
    *timestamp = v4l2_buffer.timestamp.tv_usec;
  if (flags)
    *flags = v4l2_buffer.flags;

  return ret;
}

// 4.5.2.8. Drain
void drain(struct queue* OUTPUT_queue, struct queue* CAPTURE_queue) {
  // 1. Begin the drain sequence by issuing VIDIOC_ENCODER_CMD().
  struct v4l2_encoder_cmd cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.cmd = V4L2_ENC_CMD_STOP;

  // V4L2_ENC_CMD_STOP may not be supported, don't worry about result
  do_ioctl(CAPTURE_queue->v4lfd, VIDIOC_ENCODER_CMD, &cmd);

  // 2. Dequeue buffers
  // The way the encode loop is set up, there shouldn't be any buffers
  // left to dequeue.
  {
    uint32_t index = 0;
    uint32_t bytesused = 0;
    uint32_t flags = 0;

    // check to make sure the queue is empty
    dequeue_buffer(CAPTURE_queue, &index, &bytesused, 0, 0, &flags);

    if (!(flags & V4L2_BUF_FLAG_LAST) && (bytesused != 0))
      fprintf(stderr, "WARNING: CAPTURE queue did not clean up.\n");
  }

  // 3. Reset by issuing VIDIOC_STREAMOFF
  int ret =
      do_ioctl(OUTPUT_queue->v4lfd, VIDIOC_STREAMOFF, &OUTPUT_queue->type);
  if (ret != 0)
    perror("VIDIOC_STREAMOFF failed on OUTPUT");

  ret = do_ioctl(CAPTURE_queue->v4lfd, VIDIOC_STREAMOFF, &CAPTURE_queue->type);
  if (ret != 0)
    perror("VIDIOC_STREAMOFF failed on CAPTURE");
}

int encode(const struct file_buffer* fb,
           const struct file_info* fi,
           char* output_file_name,
           struct queue* OUTPUT_queue,
           struct queue* CAPTURE_queue,
           uint32_t frames_to_encode) {
  if (OUTPUT_queue->num_planes == 0 || OUTPUT_queue->num_planes > 3) {
    fprintf(stderr, " unsupported number of planes: %d\n",
            OUTPUT_queue->num_planes);
    return -1;
  }
  fprintf(stderr, "encoding\n");

  const int use_ivf = CAPTURE_queue->fourcc == v4l2_fourcc('V', 'P', '8', '0');
  strcat(output_file_name, use_ivf ? ".ivf" : ".h264");

  int ret = 0;
  FILE* fp_output = fopen(output_file_name, "wb");
  if (!fp_output) {
    fprintf(stderr, "unable to write to file: %s\n", output_file_name);
    ret = 1;
  }

  // write header
  if (use_ivf) {
    struct ivf_file_header header;
    header.signature = kIVFHeaderSignature;
    header.version = 0;
    header.header_length = sizeof(struct ivf_file_header);
    header.fourcc = CAPTURE_queue->fourcc;
    header.width = CAPTURE_queue->raw_width;
    header.height = CAPTURE_queue->raw_height;
    // hard coded 30fps
    header.denominator = 30;
    header.numerator = 1;
    header.frame_cnt = frames_to_encode;
    header.unused = 0;

    if (fwrite(&header, sizeof(struct ivf_file_header), 1, fp_output) != 1) {
      fprintf(stderr, "unable to write ivf file header\n");
    }
  }

  struct timespec start, stop;
  clock_gettime(CLOCK_REALTIME, &start);
  if (!ret) {
    // prime input by filling up the OUTPUT queue with raw frames
    for (uint32_t i = 0; i < OUTPUT_queue->cnt; ++i) {
      if (submit_raw_frame(fb, fi, OUTPUT_queue, i)) {
        fprintf(stderr, "unable to submit raw frame\n");
        ret = 1;
      }
    }
  }

  if (!ret) {
    ret = do_ioctl(OUTPUT_queue->v4lfd, VIDIOC_STREAMON, &OUTPUT_queue->type);
    if (ret != 0)
      perror("VIDIOC_STREAMON failed on OUTPUT");
  }

  if (!ret) {
    ret = do_ioctl(CAPTURE_queue->v4lfd, VIDIOC_STREAMON, &CAPTURE_queue->type);
    if (ret != 0)
      perror("VIDIOC_STREAMON failed on CAPTURE");
  }

  uint32_t cnt = OUTPUT_queue->cnt;  // We pre-uploaded a few before.
  if (!ret) {
    while (cnt < frames_to_encode) {
      // handle CAPTURE queue first
      {
        uint32_t index = 0;
        uint32_t bytesused = 0;
        uint32_t data_offset = 0;
        uint64_t timestamp = 0;

        // first get the newly encoded frame
        ret = dequeue_buffer(CAPTURE_queue, &index, &bytesused, &data_offset,
                             &timestamp, 0);
        if (ret != 0)
          continue;

        if (use_ivf) {
          struct ivf_frame_header header;
          header.size = bytesused - data_offset;
          header.timestamp = timestamp;

          if (fwrite(&header, sizeof(struct ivf_frame_header), 1, fp_output) !=
              1) {
            fprintf(stderr, "unable to write ivf frame header\n");
          }
        }
        fwrite(CAPTURE_queue->buffers[index].start[0] + data_offset,
               bytesused - data_offset, 1, fp_output);

        // done with the buffer, queue it back up
        queue_CAPTURE_buffer(CAPTURE_queue, index);
      }

      // handle OUTPUT queue second
      {
        uint32_t index = 0;

        ret = dequeue_buffer(OUTPUT_queue, &index, 0, 0, 0, 0);
        if (ret != 0)
          continue;

        if (submit_raw_frame(fb, fi, OUTPUT_queue, index))
          break;
      }
      cnt++;
    }
  }

  drain(OUTPUT_queue, CAPTURE_queue);

  clock_gettime(CLOCK_REALTIME, &stop);
  const double elapsed_ns =
      (stop.tv_sec - start.tv_sec) * 1e9 + (stop.tv_nsec - start.tv_nsec);
  const double fps = cnt * 1e9 / elapsed_ns;
  printf("%d frames encoded in %fns (%ffps)\n", cnt, elapsed_ns, fps);

  if (fp_output) {
    fclose(fp_output);
  }
  return ret;
}

static void print_help(const char* argv0) {
  printf("usage: %s [OPTIONS]\n", argv0);
  printf("  -f, --file         file to encode\n");
  printf("  -i, --file_format  pixel format of the file (yv12, nv12)\n");
  printf(
      "  -o, --output       output file name (will get a .h264 or .ivf suffix "
      "added)\n");
  printf("  -w, --width        width of image\n");
  printf("  -h, --height       height of image\n");
  printf("  -m, --max          max number of frames to decode\n");
  printf("  -r, --rate         frames per second\n");
  printf("  -b, --bitrate      bits per second\n");
  printf("  -g, --gop          gop length\n");
  printf("  -c, --codec        codec\n");
  printf("  -e, --end_usage    rate control mode: VBR (default), CBR\n");
  printf("  -q, --buffer_fmt   OUTPUT queue format\n");
  printf(
      "  -n, --hier_bitrate string of hierarchical layer bitrates, space "
      "or comma separated\n");
}

static const struct option longopts[] = {
    {"file", required_argument, NULL, 'f'},
    {"file_format", required_argument, NULL, 'i'},
    {"output", required_argument, NULL, 'o'},
    {"width", required_argument, NULL, 'w'},
    {"height", required_argument, NULL, 'h'},
    {"max", required_argument, NULL, 'm'},
    {"fps", required_argument, NULL, 'r'},
    {"bitrate", required_argument, NULL, 'b'},
    {"gop", required_argument, NULL, 'g'},
    {"codec", required_argument, NULL, 'c'},
    {"end_usage", required_argument, NULL, 'e'},
    {"buffer_fmt", required_argument, NULL, 'q'},
    {"hier_bitrate", required_argument, NULL, 'n'},
    {0, 0, 0, 0},
};

int main(int argc, char* argv[]) {
  uint32_t file_format = v4l2_fourcc('N', 'V', '1', '2');
  uint32_t OUTPUT_format = v4l2_fourcc('N', 'V', '1', '2');
  uint32_t CAPTURE_format = v4l2_fourcc('H', '2', '6', '4');
  char* file_name = NULL;
  char* output_file_name = NULL;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t frames_to_encode = 0;
  uint32_t framerate = 30;
  int c;

  struct encoder_control common_control_list[] = {
      ENCODER_CTL_DEFAULT(V4L2_CID_MPEG_VIDEO_BITRATE, 1000),
      ENCODER_CTL_DEFAULT(V4L2_CID_MPEG_VIDEO_GOP_SIZE, 20),
      ENCODER_CTL_DEFAULT(V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, 1),
      ENCODER_CTL_DEFAULT(V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
                          V4L2_MPEG_VIDEO_BITRATE_MODE_VBR),
      ENCODER_CTL_DEFAULT(V4L2_CID_MPEG_VIDEO_BITRATE_PEAK, 1500),
      ENCODER_CTL_DEFAULT(0, 0)};

  struct encoder_control h264_control_list[] = {
      ENCODER_CTL_DEFAULT(V4L2_CID_MPEG_VIDEO_H264_PROFILE,
                          V4L2_MPEG_VIDEO_H264_PROFILE_MAIN),
      ENCODER_CTL_DEFAULT(V4L2_CID_MPEG_VIDEO_H264_LEVEL,
                          V4L2_MPEG_VIDEO_H264_LEVEL_4_0),
      ENCODER_CTL_DEFAULT(V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE,
                          V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC),
      ENCODER_CTL_DEFAULT(V4L2_CID_MPEG_VIDEO_HEADER_MODE,
                          V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME),
#if defined(USE_V4LPLUGIN)
      // We need to explicitly request SPSPPS otherwise libv4l doesn't add it.
      ENCODER_CTL_DEFAULT(V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR, 1),
#endif
      ENCODER_CTL(V4L2_CID_MPEG_VIDEO_H264_HIERARCHICAL_CODING),
      ENCODER_CTL(V4L2_CID_MPEG_VIDEO_H264_HIERARCHICAL_CODING_LAYER),
      ENCODER_CTL(V4L2_CID_MPEG_VIDEO_H264_HIER_CODING_L0_BR),
      ENCODER_CTL(V4L2_CID_MPEG_VIDEO_H264_HIER_CODING_L1_BR),
      ENCODER_CTL(V4L2_CID_MPEG_VIDEO_H264_HIER_CODING_L2_BR),
      ENCODER_CTL(V4L2_CID_MPEG_VIDEO_H264_HIER_CODING_L3_BR),
      ENCODER_CTL(V4L2_CID_MPEG_VIDEO_H264_HIER_CODING_L4_BR),
      ENCODER_CTL(V4L2_CID_MPEG_VIDEO_H264_HIER_CODING_L5_BR), ENCODER_CTL(0)};

  while ((c = getopt_long(argc, argv, "f:i:o:w:h:m:r:b:g:c:e:q:n:", longopts,
                          NULL)) != -1) {
    switch (c) {
      case 'f':
        file_name = strdup(optarg);
        break;
      case 'i':
        if (strlen(optarg) == 4) {
          file_format = v4l2_fourcc(toupper(optarg[0]), toupper(optarg[1]),
                                    toupper(optarg[2]), toupper(optarg[3]));
          printf("using (%s) as the file format\n", optarg);
        }
        break;
      case 'o':
        output_file_name = strdup(optarg);
        break;
      case 'w':
        width = atoi(optarg);
        break;
      case 'h':
        height = atoi(optarg);
        break;
      case 'm':
        frames_to_encode = atoi(optarg);
        break;
      case 'r':
        framerate = atoi(optarg);
        break;
      case 'b':
        set_control_value(common_control_list, V4L2_CID_MPEG_VIDEO_BITRATE,
                          atoi(optarg));
        set_control_value(common_control_list, V4L2_CID_MPEG_VIDEO_BITRATE_PEAK,
                          atoi(optarg) * 2);
        break;
      case 'g':
        set_control_value(common_control_list, V4L2_CID_MPEG_VIDEO_GOP_SIZE,
                          atoi(optarg));
        break;
      case 'c':
        if (strlen(optarg) == 4) {
          CAPTURE_format = v4l2_fourcc(toupper(optarg[0]), toupper(optarg[1]),
                                       toupper(optarg[2]), toupper(optarg[3]));
          printf("using (%s) as the codec\n", optarg);
        }
        break;
      case 'e':
        if (strlen(optarg) == 3 && toupper(optarg[0]) == 'C' &&
            toupper(optarg[1]) == 'B' && toupper(optarg[2]) == 'R') {
          set_control_value(common_control_list,
                            V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
                            V4L2_MPEG_VIDEO_BITRATE_MODE_CBR);
        }
        break;
      case 'q':
        if (strlen(optarg) == 4) {
          OUTPUT_format = v4l2_fourcc(toupper(optarg[0]), toupper(optarg[1]),
                                      toupper(optarg[2]), toupper(optarg[3]));
          printf("using (%s) as the OUTPUT_queue buffer format\n", optarg);
        }
        break;
      case 'n': {
        char* pch = strtok(optarg, " ,");
        uint32_t layers = 0;
        while (pch != NULL) {
          const uint32_t layer_bitrate = atoi(pch);

          if (MAX_HIERARCHICAL_LAYERS == layers) {
            fprintf(stderr, "Too many layer bitrates provided. %d supported.\n",
                    MAX_HIERARCHICAL_LAYERS);
            exit(1);
          }

          assert(V4L2_CID_MPEG_VIDEO_H264_HIER_CODING_L1_BR ==
                 (V4L2_CID_MPEG_VIDEO_H264_HIER_CODING_L0_BR + 1));
          assert(V4L2_CID_MPEG_VIDEO_H264_HIER_CODING_L2_BR ==
                 (V4L2_CID_MPEG_VIDEO_H264_HIER_CODING_L0_BR + 2));
          assert(V4L2_CID_MPEG_VIDEO_H264_HIER_CODING_L3_BR ==
                 (V4L2_CID_MPEG_VIDEO_H264_HIER_CODING_L0_BR + 3));
          assert(V4L2_CID_MPEG_VIDEO_H264_HIER_CODING_L4_BR ==
                 (V4L2_CID_MPEG_VIDEO_H264_HIER_CODING_L0_BR + 4));
          assert(V4L2_CID_MPEG_VIDEO_H264_HIER_CODING_L5_BR ==
                 (V4L2_CID_MPEG_VIDEO_H264_HIER_CODING_L0_BR + 5));

          printf("setting layer %d bitrate to %d bps\n", layers, layer_bitrate);
          set_control_value(h264_control_list,
                            V4L2_CID_MPEG_VIDEO_H264_HIER_CODING_L0_BR + layers,
                            layer_bitrate);
          pch = strtok(NULL, " ,");

          ++layers;
        }

        set_control_value(h264_control_list,
                          V4L2_CID_MPEG_VIDEO_H264_HIERARCHICAL_CODING, 1);
        set_control_value(h264_control_list,
                          V4L2_CID_MPEG_VIDEO_H264_HIERARCHICAL_CODING_LAYER,
                          layers);
      } break;
      default:
        break;
    }
  }

  const int bitrate_mode =
      get_control_value(common_control_list, V4L2_CID_MPEG_VIDEO_BITRATE_MODE);
  fprintf(stderr, "encoding %d frames using %s bitrate control\n",
          frames_to_encode,
          (bitrate_mode == V4L2_MPEG_VIDEO_BITRATE_MODE_CBR) ? "CBR" : "VBR");

  int v4lfd = -1;
  const size_t kEncodeDeviceFilesSize =
      sizeof(kEncodeDeviceFiles) / sizeof(char*);
  for (int index = 0; index < kEncodeDeviceFilesSize; ++index) {
    int temp_fd =
        open(kEncodeDeviceFiles[index], O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (temp_fd < 0)
      continue;
#if defined(USE_V4LPLUGIN)
    v4l2_fd_open(temp_fd, V4L2_DISABLE_CONVERSION);
#endif

    if (verify_capabilities(temp_fd, OUTPUT_format, CAPTURE_format) == 0) {
      v4lfd = temp_fd;
      break;
    } else {
      close(temp_fd);
    }
  }

  if (v4lfd < 0) {
    fprintf(stderr,
            "Did not find a device file with the needed capabilities\n");
    exit(EXIT_FAILURE);
  }

  if (!file_name || !output_file_name || width == 0 || height == 0) {
    fprintf(stderr, "Invalid parameters!\n");
    print_help(argv[0]);
    exit(1);
  }

  struct file_info fi = {.format = file_format};
  fi.fp = fopen(file_name, "rb");
  if (!fi.fp) {
    fprintf(stderr, "%s: unable to open file.\n", file_name);
    exit(1);
  }

  // frame calculations assume 4:2:0
  const uint32_t frame_size = (3 * width * height) >> 1;
  if (!frames_to_encode) {
    fseek(fi.fp, 0, SEEK_END);
    uint64_t length = ftell(fi.fp);
    frames_to_encode = length / frame_size;
    fseek(fi.fp, 0, SEEK_SET);
  }

  uint8_t* frame_buffer = malloc(frame_size);
  struct file_buffer fb = {.buffer = frame_buffer, .frame_size = frame_size};

  fprintf(stderr, "encoding %d frames using %s bitrate control\n",
          frames_to_encode,
          (bitrate_mode == V4L2_MPEG_VIDEO_BITRATE_MODE_CBR) ? "CBR" : "VBR");

  struct queue OUTPUT_queue = {.v4lfd = v4lfd,
                               .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                               .fourcc = OUTPUT_format,
                               .raw_width = width,
                               .raw_height = height,
                               .frame_cnt = 0,
                               .num_planes = 1,
                               .framerate = framerate};

  struct queue CAPTURE_queue = {.v4lfd = v4lfd,
                                .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                                .fourcc = CAPTURE_format,
                                .raw_width = width,
                                .raw_height = height,
                                .num_planes = 1,
                                .framerate = framerate};

  int ret = Initialization(&OUTPUT_queue, &CAPTURE_queue);

  if (!ret) {
    // not all configurations are supported on all platforms
    disable_unsupported_controls(v4lfd, common_control_list);
    disable_unsupported_controls(v4lfd, h264_control_list);
    ret = set_ext_controls(v4lfd, common_control_list);

    if (!ret && v4l2_fourcc('H', '2', '6', '4') == CAPTURE_format)
      ret = set_ext_controls(v4lfd, h264_control_list);
  }

  if (!ret) {
    ret = encode(&fb, &fi, output_file_name, &OUTPUT_queue, &CAPTURE_queue,
                 frames_to_encode);
  }

  cleanup_queue(&OUTPUT_queue);
  cleanup_queue(&CAPTURE_queue);
#if defined(USE_V4LPLUGIN)
  v4l2_close(v4lfd);
#endif

  free(fb.buffer);
  close(v4lfd);

  return 0;
}
