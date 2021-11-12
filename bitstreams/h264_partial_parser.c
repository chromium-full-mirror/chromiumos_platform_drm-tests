/*
 * Copyright 2022 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include "bitstreams/h264_partial_parser.h"

#include <assert.h>
#include <string.h>

#include "bitstream_helper.h"
#include "v4l2_macros.h"

#define BITS_PER_BYTE 8
#define MS_BIT_SET_MASK 0b10000000

// TODO: This assumes little-endianness. Consider replacing with something more
// portable.
static const uint32_t kNaluSignature = 0x00010000;
static const uint32_t kNaluSignatureMask = 0x00FFFFFF;
static const uint32_t kNaluSignatureSize = 3;

H264SPS curr_sps;
H264PPS curr_pps;
H264SliceHeader curr_slice_header;

const uint8_t* bitstream = NULL;
uint8_t bit_idx = 0;
size_t byte_idx = 0;
uint8_t bitstream_mask = MS_BIT_SET_MASK;
const uint8_t* bitstream_end_pos = NULL;

void init_bitfield_stream(const uint8_t* new_bitstream,
                          const uint8_t* new_end_pos) {
  bitstream = new_bitstream;
  bit_idx = 0;
  byte_idx = 0;
  bitstream_mask = MS_BIT_SET_MASK;
  bitstream_end_pos = new_end_pos;
}

void read_bits(int num_bits, void* dest_) {
  uint8_t* dest = (uint8_t*)dest_;

  assert(dest);
  assert(bitstream);
  assert(bit_idx < BITS_PER_BYTE);

  int written_bits = 0;
  uint8_t buf = 0;
  while (written_bits < num_bits) {
    assert(bitstream + byte_idx < bitstream_end_pos);

    buf = (buf << 1) | (!!(bitstream[byte_idx] & bitstream_mask));
    bitstream_mask >>= 1;
    written_bits++;

    if (!(written_bits % BITS_PER_BYTE)) {
      *dest = buf;
      dest++;
      buf = 0;
    }

    bit_idx++;
    byte_idx += (bit_idx == BITS_PER_BYTE);
    bit_idx = bit_idx % BITS_PER_BYTE;
    if (!bit_idx)
      bitstream_mask = MS_BIT_SET_MASK;
  }

  if (written_bits % BITS_PER_BYTE)
    *dest = buf;
}

// Outlined in section 9.1 of the ITU-T H.264.
void read_exp_golomb_unsigned_int(int* ret) {
  int leading_zero_bits = -1;
  uint8_t bit = 0;
  while (!bit) {
    leading_zero_bits++;
    read_bits(1, &bit);
  }

  assert(leading_zero_bits < 32);

  *ret = (1 << leading_zero_bits) - 1;
  uint32_t less_significant_bits = 0;
  read_bits(leading_zero_bits, &less_significant_bits);
  *ret += less_significant_bits;

  assert(leading_zero_bits < 31 || !less_significant_bits);
}

// 9.1.1
void read_exp_golomb_signed_int(int* ret) {
  read_exp_golomb_unsigned_int(ret);

  if (*ret & 1)
    *ret = (*ret >> 1) + 1;
  else
    *ret = -1 * (*ret >> 1);
}

uint8_t* find_next_nalu(uint8_t* start_pos, uint8_t* end_pos) {
  uint8_t* next_nalu_pos = start_pos;
  while (next_nalu_pos < end_pos &&
         (*(uint32_t*)next_nalu_pos & kNaluSignatureMask) != kNaluSignature) {
    next_nalu_pos++;
  }
  return next_nalu_pos;
}

void tokenize_nalu(uint8_t* curr_pos, uint8_t* end_pos, H264Nalu* nalu) {
  nalu->data = curr_pos;
  nalu->size = (size_t)(find_next_nalu(curr_pos + kNaluSignatureSize, end_pos) -
                        curr_pos);
}

void parse_scaling_list(int size, int* scaling_list, bool* use_default) {
  // 7.3.2.1.1.1
  int last_scale = 8;
  int next_scale = 8;
  int delta_scale;

  *use_default = false;

  for (int j = 0; j < size; j++) {
    if (next_scale) {
      read_exp_golomb_signed_int(&delta_scale);
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

void parse_scaling_list_sps(H264SPS* sps) {
  // See 7.4.2.1.1.
  bool seq_scaling_list_present_flag;
  bool use_default;

  // Parse |scaling_list4x4|.
  for (int i = 0; i < H264_NUM_SCALING_LISTS; i++) {
    read_bits(1, &seq_scaling_list_present_flag);

    if (seq_scaling_list_present_flag) {
      parse_scaling_list(
          sizeof(int) * H264_NUM_SCALING_LISTS * H264_SCALING_LIST_4X4_LENGTH,
          sps->scaling_list4x4[i], &use_default);
    }
  }

  // Parse |scaling_list8x8|.
  for (int i = 0;
       i < ((sps->chroma_format_idc != 3) ? 2 : H264_NUM_SCALING_LISTS); ++i) {
    read_bits(1, &seq_scaling_list_present_flag);

    if (seq_scaling_list_present_flag) {
      parse_scaling_list(
          sizeof(int) * H264_NUM_SCALING_LISTS * H264_SCALING_LIST_8X8_LENGTH,
          sps->scaling_list8x8[i], &use_default);
    }
  }
}

// ITU-T H.264 section 7.3.2.1.1 Sequence parameter set data. Note that
// this is not a full implementation, just enough for us to detect frame
// boundaries.
void parse_sps(H264Nalu* nalu) {
  H264SPS* sps = &curr_sps;
  memset(sps, 0, sizeof(H264SPS));

  read_bits(8, &sps->profile_idc);
  read_bits(1, &sps->constraint_set0_flag);
  read_bits(1, &sps->constraint_set1_flag);
  read_bits(1, &sps->constraint_set2_flag);
  read_bits(1, &sps->constraint_set3_flag);
  read_bits(1, &sps->constraint_set4_flag);
  read_bits(1, &sps->constraint_set5_flag);
  int discard;
  read_bits(2, &discard);
  read_bits(8, &sps->level_idc);

  read_exp_golomb_unsigned_int(&sps->seq_parameter_set_id);

  assert(!(sps->profile_idc == kProfileIDHigh422 ||
           sps->profile_idc == kProfileIDHigh444Predictive ||
           sps->profile_idc == kProfileIDHigh444Intra ||
           sps->profile_idc == kProfileIDScalableBaseline ||
           sps->profile_idc == kProfileIDScalableHigh ||
           sps->profile_idc == kProfileIDSMultiviewHigh ||
           sps->profile_idc == kProfileIDStereoHigh));
  if (sps->profile_idc == kProfileIDCHigh ||
      sps->profile_idc == kProfileIDHigh10) {
    read_exp_golomb_unsigned_int(&sps->chroma_format_idc);
    assert(sps->chroma_format_idc < 4);

    if (sps->chroma_format_idc == 3)
      read_bits(1, &sps->separate_colour_plane_flag);

    read_exp_golomb_unsigned_int(&sps->bit_depth_luma_minus8);
    assert(sps->bit_depth_luma_minus8 < 7);

    read_exp_golomb_unsigned_int(&sps->bit_depth_chroma_minus8);
    assert(sps->bit_depth_chroma_minus8 < 7);

    read_bits(1, &sps->qpprime_y_zero_transform_bypass_flag);
    read_bits(1, &sps->seq_scaling_matrix_present_flag);

    if (sps->seq_scaling_matrix_present_flag)
      parse_scaling_list_sps(sps);
  } else {
    sps->chroma_format_idc = 1;
  }

  read_exp_golomb_unsigned_int(&sps->log2_max_frame_num_minus4);
  assert(sps->log2_max_frame_num_minus4 < 13);

  read_exp_golomb_unsigned_int(&sps->pic_order_cnt_type);
  assert(sps->pic_order_cnt_type < 3);

  if (sps->pic_order_cnt_type == 0) {
    read_exp_golomb_unsigned_int(&sps->log2_max_pic_order_cnt_lsb_minus4);
    assert(sps->log2_max_pic_order_cnt_lsb_minus4 < 13);
    sps->expected_delta_per_pic_order_cnt_cycle = 0;
  } else if (sps->pic_order_cnt_type == 1) {
    read_bits(1, &sps->delta_pic_order_always_zero_flag);
    read_exp_golomb_signed_int(&sps->offset_for_non_ref_pic);
    read_exp_golomb_signed_int(&sps->offset_for_top_to_bottom_field);
    read_exp_golomb_unsigned_int(&sps->num_ref_frames_in_pic_order_cnt_cycle);
    assert(sps->num_ref_frames_in_pic_order_cnt_cycle < 255);

    int offset_acc = 0;
    for (int i = 0; i < sps->num_ref_frames_in_pic_order_cnt_cycle; ++i) {
      read_exp_golomb_signed_int(&sps->offset_for_ref_frame[i]);
      offset_acc += sps->offset_for_ref_frame[i];
    }
    sps->expected_delta_per_pic_order_cnt_cycle = offset_acc;
  }

  read_exp_golomb_unsigned_int(&sps->max_num_ref_frames);
  read_bits(1, &sps->gaps_in_frame_num_value_allowed_flag);

  read_exp_golomb_unsigned_int(&sps->pic_width_in_mbs_minus1);
  read_exp_golomb_unsigned_int(&sps->pic_height_in_map_units_minus1);

  read_bits(1, &sps->frame_mbs_only_flag);
}

// ITU-T H.264 section 7.3.2.2 Picture parameter set RBSP syntax. Note that
// this is not a full implementation, just enough for frame boundaries.
void parse_pps(H264Nalu* nalu) {
  H264PPS* pps = &curr_pps;
  memset(pps, 0, sizeof(H264PPS));

  read_exp_golomb_unsigned_int(&pps->pic_parameter_set_id);
  read_exp_golomb_unsigned_int(&pps->seq_parameter_set_id);

  read_bits(1, &pps->entropy_coding_mode_flag);
  read_bits(1, &pps->bottom_field_pic_order_in_frame_present_flag);
}

// ITU-T H.264 section 7.3.3 Slice header syntax. Note that this is not a full
// implementation, just enough for frame boundaries.
void parse_slice_header(H264Nalu* nalu) {
  H264SliceHeader* slice_header = &curr_slice_header;
  memset(slice_header, 0, sizeof(H264SliceHeader));

  slice_header->idr_pic_flag = (nalu->nal_unit_type == kIDRSlice);
  slice_header->nal_ref_idc = nalu->nal_ref_idc;

  read_exp_golomb_unsigned_int(&slice_header->first_mb_in_slice);
  read_exp_golomb_unsigned_int(&slice_header->slice_type);
  assert(slice_header->slice_type < 10);

  read_exp_golomb_unsigned_int(&slice_header->pic_parameter_set_id);

  assert(!curr_sps.separate_colour_plane_flag);

  read_bits(curr_sps.log2_max_frame_num_minus4 + 4, &slice_header->frame_num);

  if (!curr_sps.frame_mbs_only_flag) {
    read_bits(1, &slice_header->field_pic_flag);
    assert(!slice_header->field_pic_flag);
  }

  if (slice_header->idr_pic_flag)
    read_exp_golomb_unsigned_int(&slice_header->idr_pic_id);

  if (curr_sps.pic_order_cnt_type == 0) {
    read_bits(curr_sps.log2_max_pic_order_cnt_lsb_minus4 + 4,
              &slice_header->pic_order_cnt_lsb);
    if (curr_pps.bottom_field_pic_order_in_frame_present_flag &&
        !slice_header->field_pic_flag) {
      read_exp_golomb_signed_int(&slice_header->delta_pic_order_cnt_bottom);
    }
  } else if (curr_sps.pic_order_cnt_type == 1 &&
             !curr_sps.delta_pic_order_always_zero_flag) {
    read_exp_golomb_signed_int(&slice_header->delta_pic_order_cnt0);
    if (curr_pps.bottom_field_pic_order_in_frame_present_flag &&
        !slice_header->field_pic_flag) {
      read_exp_golomb_signed_int(&slice_header->delta_pic_order_cnt1);
    }
  }
}

H264Nalu parse_h264_nalu(uint8_t* curr_pos, uint8_t* end_pos) {
  H264Nalu nalu;
  memset(&nalu, 0, sizeof(H264Nalu));

  tokenize_nalu(curr_pos, end_pos, &nalu);

  init_bitfield_stream(nalu.data + kNaluSignatureSize, nalu.data + nalu.size);

  int data;
  read_bits(1, &data);
  read_bits(2, &nalu.nal_ref_idc);
  read_bits(5, &nalu.nal_unit_type);

  switch (nalu.nal_unit_type) {
    case kSPS:
      parse_sps(&nalu);
      break;
    case kPPS:
      parse_pps(&nalu);
      break;
    case kNonIDRSlice:
    case kIDRSlice:
      parse_slice_header(&nalu);
      break;
    default:
      break;
  }

  return nalu;
}
