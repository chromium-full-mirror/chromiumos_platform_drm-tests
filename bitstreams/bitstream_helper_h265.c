/*
 * Copyright 2023 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <assert.h>
#include <linux/videodev2.h>
#include <string.h>

#include "bitstreams/bitstream_helper.h"
#include "bitstreams/bitstream_helper_h265.h"
#include "bitstreams/h265_partial_parser.h"
#include "bitstreams/nalu_parser.h"
#include "v4l2_macros.h"

static const uint32_t kH265Fourcc = v4l2_fourcc('H', 'E', 'V', 'C');

bool init_bitstream_h265(void) {
  // Sometimes H.265 NALU streams are padded with a leading zero, sometimes
  // they are not.
  assert(filesize > 5);
  uint8_t* first_nalu = find_next_nalu(file_buf, file_buf + 3);
  if (NULL == first_nalu)
    return false;

  curr_pos = first_nalu - file_buf;

  // Parse the first NALU to determine the type
  const H265Nalu nalu = parse_h265_nalu(file_buf + curr_pos,
                                        file_buf + filesize);

  if (nalu.forbidden_bit != 0)
    return false;

  // TODO: Make a better "is H.265" test. All tast test vectors start with one
  // of the following NALU types: VPS_NUT, AUD_NUT.
  if (nalu.nal_unit_type != kVpsNut && nalu.nal_unit_type != kAudNut)
    return false;

  return true;
}

uint32_t get_fourcc_h265(void) {
  return kH265Fourcc;
}

bool is_end_of_stream_h265(void) {
  return curr_pos >= filesize;
}

// V4L2 drivers expect all NALUs associated with a given frame to appear in the
// same buffer. This function helps group NALUs together into a frame by parsing
// them individually and then checking for new pictures.
size_t fill_compressed_buffer_h265(uint8_t* dest, size_t max_len) {
  assert(dest);

  size_t num_bytes_filled = 0;
  bool found_first_slice = false;

  while (curr_pos + num_bytes_filled < filesize) {
    // Parse the next NALU
    const H265Nalu nalu = parse_h265_nalu(
        file_buf + curr_pos + num_bytes_filled, file_buf + filesize);

    assert(nalu.forbidden_bit == 0);

    // Frame boundaries can only be detected with slice headers.
    if (is_h265_nalu_slice_type(nalu.nal_unit_type)) {
      if (found_first_slice && is_h265_slice_first_in_picture(&nalu))
        break;

      found_first_slice = true;
    }

    num_bytes_filled += nalu.size;
  }

  // Fill the buffer with all data we've parsed
  assert(num_bytes_filled < max_len);
  if (num_bytes_filled) {
    memcpy(dest, file_buf + curr_pos, num_bytes_filled);
  }
  curr_pos += num_bytes_filled;
  return num_bytes_filled;
}
