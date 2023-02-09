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

bool init_bitstream_ivf(struct bitstream* bts) {
  file_header = *((struct ivf_file_header*)(bts->file_buf));

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

  bts->curr_pos = bts->file_buf + sizeof(struct ivf_file_header);

  bts->private = NULL;
  bts->fourcc = file_header.fourcc;

  return true;
}

size_t fill_compressed_buffer_ivf(uint8_t* dest,
                                 size_t max_len,
                                 struct bitstream* bts) {
  assert(dest);
  if (bts->curr_pos + sizeof(struct ivf_frame_header) > bts->eof) {
    bts->curr_pos = bts->eof;
    return 0;
  }

  struct ivf_frame_header frame_header =
      *(struct ivf_frame_header*)(bts->curr_pos);
  uint32_t num_bytes_filled = frame_header.size;
  assert(num_bytes_filled < max_len);
  bts->curr_pos += sizeof(struct ivf_frame_header);
  if (bts->curr_pos + num_bytes_filled > bts->eof)
    num_bytes_filled = bts->eof - bts->curr_pos;
  memcpy(dest, bts->curr_pos, num_bytes_filled);
  bts->curr_pos += num_bytes_filled;
  return num_bytes_filled;
}
