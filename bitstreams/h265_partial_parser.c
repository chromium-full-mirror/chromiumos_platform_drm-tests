/*
 * Copyright 2023 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include "bitstreams/bitfield_stream.h"
#include "bitstreams/h265_partial_parser.h"
#include "bitstreams/nalu_parser.h"

#include <assert.h>
#include <string.h>

#include "bitstream_helper.h"
#include "v4l2_macros.h"

bool is_h265_nalu_slice_type(enum H265NaluType type) {
  switch (type) {
    case kTrailN:
    case kTrailR:
    case kTsaN:
    case kTsaR:
    case kStsaN:
    case kStsaR:
    case kRadlN:
    case kRadlR:
    case kRaslN:
    case kRaslR:
    case kBlaWLp:
    case kBlaWRadl:
    case kBlaNLp:
    case kIdrWRadl:
    case kIdrNLp:
    case kCraNut:
      return true;
    default:
      return false;
  }
}

bool is_h265_slice_first_in_picture(const H265Nalu *nalu) {
  bool first_slice_segment_in_pic_flag = false;
  BitfieldStream bfs = bitfield_stream_init(nalu->data + NALU_SIGNATURE_SIZE,
                                            nalu->data + nalu->size);
  bitfield_stream_skip_bits(&bfs, 16);
  bitfield_stream_read_bits(&bfs, 1, &first_slice_segment_in_pic_flag);
  return first_slice_segment_in_pic_flag;
}

H265Nalu parse_h265_nalu(uint8_t* curr_pos, uint8_t* end_pos) {
  const Nalu base_nalu = tokenize_nalu(curr_pos, end_pos);

  H265Nalu nalu = {.data=base_nalu.data,
                   .size=base_nalu.size};

  BitfieldStream bfs = bitfield_stream_init(nalu.data + NALU_SIGNATURE_SIZE,
                                            nalu.data + nalu.size);

  bitfield_stream_read_bits(&bfs, 1, &nalu.forbidden_bit);
  bitfield_stream_read_bits(&bfs, 6, &nalu.nal_unit_type);
  bitfield_stream_read_bits(&bfs, 6, &nalu.nuh_layer_id);
  bitfield_stream_read_bits(&bfs, 3, &nalu.nuh_temporal_id_plus1);

  return nalu;
}
