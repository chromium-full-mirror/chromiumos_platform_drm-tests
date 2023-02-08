/*
 * Copyright 2023 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "bitstreams/bitstream_helper_ivf.h"

#include <assert.h>
#include <linux/videodev2.h>
#include <string.h>

#include "bitstreams/bitstream_helper.h"
#include "v4l2_macros.h"

static const uint32_t kIVFHeaderSignature = v4l2_fourcc('D', 'K', 'I', 'F');

int i = 0;

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

struct ivf_file_header file_header;

bool init_bitstream_ivf(void) {
  file_header = *((struct ivf_file_header*)file_buf);

  if (file_header.signature != kIVFHeaderSignature) {
    LOG_ERROR("Incorrect header signature: 0x%x != 0x%x", file_header.signature,
              kIVFHeaderSignature);
    return false;
  }

  LOG_INFO("Initializing IVF bitstream");
  LOG_INFO("IVF FourCC %c%c%c%c", file_header.fourcc & 0xFF,
           file_header.fourcc << 8 & 0xFF, file_header.fourcc << 16 & 0xFF,
           file_header.fourcc << 24 & 0xFF);
  LOG_INFO("Initial image dimensions: Width: %d, Height: %d", file_header.width,
           file_header.height);

  curr_pos += sizeof(struct ivf_file_header);
  return true;
}

uint32_t get_fourcc_ivf(void) {
  return file_header.fourcc;
}

bool is_end_of_stream_ivf(void) {
  return curr_pos >= filesize;
}

size_t fill_compressed_buffer_ivf(uint8_t* dest, size_t max_len) {
  assert(dest);
  if (sizeof(struct ivf_frame_header) > filesize - curr_pos) {
    curr_pos = filesize;
    return 0;
  }

  struct ivf_frame_header frame_header =
      *(struct ivf_frame_header*)(file_buf + curr_pos);
  uint32_t num_bytes_filled = frame_header.size;
  assert(num_bytes_filled < max_len);
  curr_pos += sizeof(struct ivf_frame_header);
  if (num_bytes_filled + curr_pos > filesize)
    num_bytes_filled = filesize - curr_pos;
  memcpy(dest, file_buf + curr_pos, num_bytes_filled);
  curr_pos += num_bytes_filled;
  return num_bytes_filled;
}
