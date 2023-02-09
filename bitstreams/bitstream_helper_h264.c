/*
 * Copyright 2023 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "bitstream_helper_h264.h"

#include <assert.h>
#include <linux/videodev2.h>
#include <string.h>

#include "bitstreams/bitstream_helper.h"
#include "bitstreams/h264_partial_parser.h"
#include "bitstreams/nalu_parser.h"
#include "v4l2_macros.h"

struct h264_private {
  H264SliceHeader prev_slice_header;
  size_t prev_slice_size;
  bool found_first_slice;
};

static const uint32_t kH264Fourcc = v4l2_fourcc('H', '2', '6', '4');
static const int kMaxNALUSearch = 32;

bool init_bitstream_h264(struct bitstream* bts) {
  // Sometimes H.264 NALU streams are padded with a leading byte, sometimes
  // they are not.
  uint8_t* first_nalu = find_next_nalu(bts->file_buf, bts->file_buf + 3);
  if (NULL == first_nalu)
    return false;

  bts->curr_pos = first_nalu;
  uint8_t* search_pos = first_nalu;

  // The best indicator that this is a h.264 bitstream is a SPS nalu.
  // Searching the entire bitstream is not necessary.  If none of the first
  // |kMaxNALUSearch| nalus are not a SPS, give up.
  int cnt;
  for (cnt = 0; cnt < kMaxNALUSearch; ++cnt) {
    // Parse the next NALU
    const H264Nalu nalu = parse_h264_nalu_header(search_pos, bts->eof);

    // There is the possibility that a bitstream may not be valid and the
    // parser accidentally thinks the nalu is valid. In that case the
    // forbidden bit should be set.
    if (nalu.forbidden_bit != 0)
      return false;

    search_pos += nalu.size;
    if (search_pos >= bts->eof)
      return false;

    if (nalu.nal_unit_type == kSPS)
      break;
  }

  if (cnt == kMaxNALUSearch)
    return false;

  bts->fourcc = kH264Fourcc;

  bts->private = malloc(sizeof(struct h264_private));
  struct h264_private* h264_private = (struct h264_private*)(bts->private);
  h264_private->prev_slice_size = 0;
  h264_private->found_first_slice = false;

  return true;
}

// ITU-T H.264 7.4.1.2.4 implementation. Assumes non-interlaced.
static bool is_new_frame(H264SPS* sps,
                         H264PPS* pps,
                         H264SliceHeader* prev_slice_header,
                         H264SliceHeader* curr_slice_header) {
  if (curr_slice_header->frame_num != prev_slice_header->frame_num ||
      curr_slice_header->pic_parameter_set_id != pps->pic_parameter_set_id ||
      curr_slice_header->nal_ref_idc != prev_slice_header->nal_ref_idc ||
      curr_slice_header->idr_pic_flag != prev_slice_header->idr_pic_flag ||
      (curr_slice_header->idr_pic_flag &&
       (curr_slice_header->idr_pic_id != prev_slice_header->idr_pic_id ||
        curr_slice_header->first_mb_in_slice == 0))) {
    return true;
  }

  if (sps->pic_order_cnt_type == 0) {
    if (curr_slice_header->pic_order_cnt_lsb !=
            prev_slice_header->pic_order_cnt_lsb ||
        curr_slice_header->delta_pic_order_cnt_bottom !=
            prev_slice_header->delta_pic_order_cnt_bottom) {
      return true;
    }
  } else if (sps->pic_order_cnt_type == 1) {
    if (curr_slice_header->delta_pic_order_cnt0 !=
            prev_slice_header->delta_pic_order_cnt0 ||
        curr_slice_header->delta_pic_order_cnt1 !=
            prev_slice_header->delta_pic_order_cnt1) {
      return true;
    }
  }

  return false;
}

// V4L2 drivers expect all NALUs associated with a given frame to appear in the
// same buffer. This function helps group NALUs together into a frame by parsing
// them individually and then checking the conditions enumerated in ITU-T H.264
// 7.4.1.2.4 for a frame boundary. See is_new_frame() above for more information
// about frame boundary detection.
size_t fill_compressed_buffer_h264(uint8_t* dest,
                                  size_t max_len,
                                  struct bitstream* bts) {
  struct h264_private* h264_private = (struct h264_private*)(bts->private);

  assert(dest);
  if (is_end_of_stream(bts))
    return 0;

  size_t num_bytes_filled = 0;

  // We never know that we've hit a frame boundary until after we've already
  // parsed the first slice header in the next frame. This code handles the
  // spillover logic.
  if (h264_private->found_first_slice)
    num_bytes_filled += h264_private->prev_slice_size;

  while (true) {
    // This handles reading bytes at the end of the file so we don't
    // accidentally read memory past our file map.
    if (bts->curr_pos + num_bytes_filled >= bts->eof) {
      num_bytes_filled = bts->eof - bts->curr_pos;
      assert(num_bytes_filled < max_len);
      memcpy(dest, bts->curr_pos, num_bytes_filled);
      bts->curr_pos += num_bytes_filled;
      return num_bytes_filled;
    }

    // Parse the next NALU
    const H264Nalu nalu = parse_h264_nalu(
        bts->curr_pos + num_bytes_filled, bts->eof);

    assert(nalu.forbidden_bit == 0);

    // Frame boundaries can only be detected with slice headers.
    if (nalu.nal_unit_type == kNonIDRSlice || nalu.nal_unit_type == kIDRSlice) {
      const bool new_frame =
          h264_private->found_first_slice &&
          is_new_frame(&curr_sps, &curr_pps, &h264_private->prev_slice_header,
                       &curr_slice_header);

      h264_private->prev_slice_size = nalu.size;
      h264_private->prev_slice_header = curr_slice_header;
      h264_private->found_first_slice = true;

      if (new_frame) {
        // If we've found a frame boundary, fill the V4L2 buffer.
        assert(num_bytes_filled < max_len);
        memcpy(dest, bts->curr_pos, num_bytes_filled);
        bts->curr_pos += num_bytes_filled;
        return num_bytes_filled;
      }
    }

    num_bytes_filled += nalu.size;
  }
}
