/*
 * Copyright 2023 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include "bitstreams/h264_partial_parser.h"
#include "bitstreams/bitfield_stream.h"
#include "bitstreams/nalu_parser.h"

#include <assert.h>
#include <string.h>

#include "bitstream_helper.h"
#include "v4l2_macros.h"

H264SPS curr_sps;
H264PPS curr_pps;
H264SliceHeader curr_slice_header;

// Outlined in section 9.1 of the ITU-T H.264.
static void read_exp_golomb_unsigned_int(BitfieldStream *bfs, int* ret) {
  int leading_zero_bits = -1;
  uint8_t bit = 0;
  while (!bit) {
    leading_zero_bits++;
    bitfield_stream_read_bits(bfs, 1, &bit);
  }

  assert(leading_zero_bits < 32);

  *ret = (1 << leading_zero_bits) - 1;
  uint32_t less_significant_bits = 0;
  bitfield_stream_read_bits(bfs, leading_zero_bits, &less_significant_bits);
  *ret += less_significant_bits;

  assert(leading_zero_bits < 31 || !less_significant_bits);
}

// 9.1.1
static void read_exp_golomb_signed_int(BitfieldStream *bfs, int* ret) {
  read_exp_golomb_unsigned_int(bfs, ret);

  if (*ret & 1)
    *ret = (*ret >> 1) + 1;
  else
    *ret = -1 * (*ret >> 1);
}

static void parse_scaling_list(BitfieldStream* bfs, int size,
                               int* scaling_list, bool* use_default) {
  // 7.3.2.1.1.1
  int last_scale = 8;
  int next_scale = 8;
  int delta_scale;

  *use_default = false;

  for (int j = 0; j < size; j++) {
    if (next_scale) {
      read_exp_golomb_signed_int(bfs, &delta_scale);
      assert(-128 < delta_scale);
      assert(delta_scale < 127);
      next_scale = (last_scale + delta_scale + 256) & 0xff;

      if (j == 0 && next_scale == 0) {
        *use_default = true;
        return;
      }
    }

    scaling_list[j] = (next_scale == 0) ? last_scale : next_scale;
    last_scale = scaling_list[j];
  }
}

static void parse_scaling_list_sps(BitfieldStream* bfs, H264SPS* sps) {
  // See 7.4.2.1.1.
  bool seq_scaling_list_present_flag;
  bool use_default;

  // Parse |scaling_list4x4|.
  for (int i = 0; i < H264_NUM_SCALING_LISTS; i++) {
    bitfield_stream_read_bits(bfs, 1, &seq_scaling_list_present_flag);

    if (seq_scaling_list_present_flag) {
      parse_scaling_list(bfs,
          sizeof(int) * H264_NUM_SCALING_LISTS * H264_SCALING_LIST_4X4_LENGTH,
          sps->scaling_list4x4[i], &use_default);
    }
  }

  // Parse |scaling_list8x8|.
  for (int i = 0;
       i < ((sps->chroma_format_idc != 3) ? 2 : H264_NUM_SCALING_LISTS); ++i) {
    bitfield_stream_read_bits(bfs, 1, &seq_scaling_list_present_flag);

    if (seq_scaling_list_present_flag) {
      parse_scaling_list(bfs,
          sizeof(int) * H264_NUM_SCALING_LISTS * H264_SCALING_LIST_8X8_LENGTH,
          sps->scaling_list8x8[i], &use_default);
    }
  }
}

// ITU-T H.264 section 7.3.2.1.1 Sequence parameter set data. Note that
// this is not a full implementation, just enough for us to detect frame
// boundaries.
static void parse_sps(BitfieldStream* bfs, H264Nalu* nalu) {
  H264SPS* sps = &curr_sps;
  memset(sps, 0, sizeof(H264SPS));

  bitfield_stream_read_bits(bfs, 8, &sps->profile_idc);
  bitfield_stream_read_bits(bfs, 1, &sps->constraint_set0_flag);
  bitfield_stream_read_bits(bfs, 1, &sps->constraint_set1_flag);
  bitfield_stream_read_bits(bfs, 1, &sps->constraint_set2_flag);
  bitfield_stream_read_bits(bfs, 1, &sps->constraint_set3_flag);
  bitfield_stream_read_bits(bfs, 1, &sps->constraint_set4_flag);
  bitfield_stream_read_bits(bfs, 1, &sps->constraint_set5_flag);
  bitfield_stream_skip_bits(bfs, 2);
  bitfield_stream_read_bits(bfs, 8, &sps->level_idc);

  read_exp_golomb_unsigned_int(bfs, &sps->seq_parameter_set_id);

  assert(!(sps->profile_idc == kProfileIDHigh422 ||
           sps->profile_idc == kProfileIDHigh444Predictive ||
           sps->profile_idc == kProfileIDHigh444Intra ||
           sps->profile_idc == kProfileIDScalableBaseline ||
           sps->profile_idc == kProfileIDScalableHigh ||
           sps->profile_idc == kProfileIDSMultiviewHigh ||
           sps->profile_idc == kProfileIDStereoHigh));
  if (sps->profile_idc == kProfileIDCHigh ||
      sps->profile_idc == kProfileIDHigh10) {
    read_exp_golomb_unsigned_int(bfs, &sps->chroma_format_idc);
    assert(sps->chroma_format_idc < 4);

    if (sps->chroma_format_idc == 3)
      bitfield_stream_read_bits(bfs, 1, &sps->separate_colour_plane_flag);

    read_exp_golomb_unsigned_int(bfs, &sps->bit_depth_luma_minus8);
    assert(sps->bit_depth_luma_minus8 < 7);

    read_exp_golomb_unsigned_int(bfs, &sps->bit_depth_chroma_minus8);
    assert(sps->bit_depth_chroma_minus8 < 7);

    bitfield_stream_read_bits(bfs, 1,
                              &sps->qpprime_y_zero_transform_bypass_flag);
    bitfield_stream_read_bits(bfs, 1, &sps->seq_scaling_matrix_present_flag);

    if (sps->seq_scaling_matrix_present_flag)
      parse_scaling_list_sps(bfs, sps);
  } else {
    sps->chroma_format_idc = 1;
  }

  read_exp_golomb_unsigned_int(bfs, &sps->log2_max_frame_num_minus4);
  assert(sps->log2_max_frame_num_minus4 < 13);

  read_exp_golomb_unsigned_int(bfs, &sps->pic_order_cnt_type);
  assert(sps->pic_order_cnt_type < 3);

  if (sps->pic_order_cnt_type == 0) {
    read_exp_golomb_unsigned_int(bfs, &sps->log2_max_pic_order_cnt_lsb_minus4);
    assert(sps->log2_max_pic_order_cnt_lsb_minus4 < 13);
    sps->expected_delta_per_pic_order_cnt_cycle = 0;
  } else if (sps->pic_order_cnt_type == 1) {
    bitfield_stream_read_bits(bfs, 1, &sps->delta_pic_order_always_zero_flag);
    read_exp_golomb_signed_int(bfs, &sps->offset_for_non_ref_pic);
    read_exp_golomb_signed_int(bfs, &sps->offset_for_top_to_bottom_field);
    read_exp_golomb_unsigned_int(bfs,
                                 &sps->num_ref_frames_in_pic_order_cnt_cycle);
    assert(sps->num_ref_frames_in_pic_order_cnt_cycle < 255);

    int offset_acc = 0;
    for (int i = 0; i < sps->num_ref_frames_in_pic_order_cnt_cycle; ++i) {
      read_exp_golomb_signed_int(bfs, &sps->offset_for_ref_frame[i]);
      offset_acc += sps->offset_for_ref_frame[i];
    }
    sps->expected_delta_per_pic_order_cnt_cycle = offset_acc;
  }

  read_exp_golomb_unsigned_int(bfs, &sps->max_num_ref_frames);
  bitfield_stream_read_bits(bfs, 1, &sps->gaps_in_frame_num_value_allowed_flag);

  read_exp_golomb_unsigned_int(bfs, &sps->pic_width_in_mbs_minus1);
  read_exp_golomb_unsigned_int(bfs, &sps->pic_height_in_map_units_minus1);

  bitfield_stream_read_bits(bfs, 1, &sps->frame_mbs_only_flag);
}

// ITU-T H.264 section 7.3.2.2 Picture parameter set RBSP syntax. Note that
// this is not a full implementation, just enough for frame boundaries.
static void parse_pps(BitfieldStream* bfs, H264Nalu* nalu) {
  H264PPS* pps = &curr_pps;
  memset(pps, 0, sizeof(H264PPS));

  read_exp_golomb_unsigned_int(bfs, &pps->pic_parameter_set_id);
  read_exp_golomb_unsigned_int(bfs, &pps->seq_parameter_set_id);

  bitfield_stream_read_bits(bfs, 1, &pps->entropy_coding_mode_flag);
  bitfield_stream_read_bits(bfs, 1,
                            &pps->bottom_field_pic_order_in_frame_present_flag);
}

// ITU-T H.264 section 7.3.3 Slice header syntax. Note that this is not a full
// implementation, just enough for frame boundaries.
static void parse_slice_header(BitfieldStream* bfs, H264Nalu* nalu) {
  H264SliceHeader* slice_header = &curr_slice_header;
  memset(slice_header, 0, sizeof(H264SliceHeader));

  slice_header->idr_pic_flag = (nalu->nal_unit_type == kIDRSlice);
  slice_header->nal_ref_idc = nalu->nal_ref_idc;

  read_exp_golomb_unsigned_int(bfs, &slice_header->first_mb_in_slice);
  read_exp_golomb_unsigned_int(bfs, &slice_header->slice_type);
  assert(slice_header->slice_type < 10);

  read_exp_golomb_unsigned_int(bfs, &slice_header->pic_parameter_set_id);

  assert(!curr_sps.separate_colour_plane_flag);

  bitfield_stream_read_bits(bfs, curr_sps.log2_max_frame_num_minus4 + 4,
                            &slice_header->frame_num);

  if (!curr_sps.frame_mbs_only_flag) {
    bitfield_stream_read_bits(bfs, 1, &slice_header->field_pic_flag);
    assert(!slice_header->field_pic_flag);
  }

  if (slice_header->idr_pic_flag)
    read_exp_golomb_unsigned_int(bfs, &slice_header->idr_pic_id);

  if (curr_sps.pic_order_cnt_type == 0) {
    bitfield_stream_read_bits(bfs,
                              curr_sps.log2_max_pic_order_cnt_lsb_minus4 + 4,
                              &slice_header->pic_order_cnt_lsb);
    if (curr_pps.bottom_field_pic_order_in_frame_present_flag &&
        !slice_header->field_pic_flag) {
      read_exp_golomb_signed_int(bfs,
                                 &slice_header->delta_pic_order_cnt_bottom);
    }
  } else if (curr_sps.pic_order_cnt_type == 1 &&
             !curr_sps.delta_pic_order_always_zero_flag) {
    read_exp_golomb_signed_int(bfs, &slice_header->delta_pic_order_cnt0);
    if (curr_pps.bottom_field_pic_order_in_frame_present_flag &&
        !slice_header->field_pic_flag) {
      read_exp_golomb_signed_int(bfs, &slice_header->delta_pic_order_cnt1);
    }
  }
}


H264Nalu parse_h264_nalu_header(uint8_t* curr_pos, uint8_t* end_pos) {
  const Nalu base_nalu = tokenize_nalu(curr_pos, end_pos);

  H264Nalu nalu = {.data=base_nalu.data,
                   .size=base_nalu.size};

  BitfieldStream bfs = bitfield_stream_init(nalu.data + NALU_SIGNATURE_SIZE,
                                            nalu.data + nalu.size);

  bitfield_stream_read_bits(&bfs, 1, &nalu.forbidden_bit);
  bitfield_stream_read_bits(&bfs, 2, &nalu.nal_ref_idc);
  bitfield_stream_read_bits(&bfs, 5, &nalu.nal_unit_type);

  return nalu;
}

H264Nalu parse_h264_nalu(uint8_t* curr_pos, uint8_t* end_pos) {
  H264Nalu nalu = parse_h264_nalu_header(curr_pos, end_pos);

  // Skip the bits that were already parsed as part of the header parsing
  BitfieldStream bfs = bitfield_stream_init(nalu.data + NALU_SIGNATURE_SIZE,
                                            nalu.data + nalu.size);
  bitfield_stream_skip_bits(&bfs, 8);

  if (nalu.forbidden_bit != 0)
    return nalu;

  switch (nalu.nal_unit_type) {
    case kSPS:
      parse_sps(&bfs, &nalu);
      break;
    case kPPS:
      parse_pps(&bfs, &nalu);
      break;
    case kNonIDRSlice:
    case kIDRSlice:
      parse_slice_header(&bfs, &nalu);
      break;
    default:
      break;
  }

  return nalu;
}
