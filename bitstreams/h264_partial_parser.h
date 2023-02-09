/*
 * Copyright 2023 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef __H264_PARTIAL_PARSER_H__
#define __H264_PARTIAL_PARSER_H__

enum H264NaluType {
  kNonIDRSlice = 1,
  kIDRSlice = 5,
  kSPS = 7,
  kPPS = 8,
  kAUD = 9,
  /* These are NALU types not supported by the partial parser:
  kUnspecified = 0,
  kSliceDataA = 2,
  kSliceDataB = 3,
  kSliceDataC = 4,
  kSEIMessage = 6,
  kEOSeq = 10,
  kEOStream = 11,
  kFiller = 12,
  kSPSExt = 13,
  kReserved14 = 14,
  kReserved15 = 15,
  kReserved16 = 16,
  kReserved17 = 17,
  kReserved18 = 18,
  kCodedSliceAux = 19,
  kCodedSliceExtension = 20,
  */
};

enum H264ProfileIDC {
  kProfileIDCBaseline = 66,
  kProfileIDCConstrainedBaseline = kProfileIDCBaseline,
  kProfileIDCMain = 77,
  kProfileIDCHigh = 100,
  // All below profiles are not supported by Chrome and will be rejected by the
  // test binary.
  kProfileIDScalableBaseline = 83,
  kProfileIDScalableHigh = 86,
  kProfileIDHigh10 = 110,
  kProfileIDSMultiviewHigh = 118,
  kProfileIDHigh422 = 122,
  kProfileIDStereoHigh = 128,
  kProfileIDHigh444Predictive = 244,
  kProfileIDHigh444Intra = 44,
};

#define H264_NUM_SCALING_LISTS 6
#define H264_SCALING_LIST_4X4_LENGTH 16
#define H264_SCALING_LIST_8X8_LENGTH 64

typedef struct H264SPS {
  int profile_idc;
  bool constraint_set0_flag;
  bool constraint_set1_flag;
  bool constraint_set2_flag;
  bool constraint_set3_flag;
  bool constraint_set4_flag;
  bool constraint_set5_flag;
  int level_idc;
  int seq_parameter_set_id;

  int chroma_format_idc;
  bool separate_colour_plane_flag;
  int bit_depth_luma_minus8;
  int bit_depth_chroma_minus8;
  bool qpprime_y_zero_transform_bypass_flag;

  bool seq_scaling_matrix_present_flag;
  int scaling_list4x4[H264_NUM_SCALING_LISTS][H264_SCALING_LIST_4X4_LENGTH];
  int scaling_list8x8[H264_NUM_SCALING_LISTS][H264_SCALING_LIST_8X8_LENGTH];

  int log2_max_frame_num_minus4;
  int pic_order_cnt_type;
  int log2_max_pic_order_cnt_lsb_minus4;
  bool delta_pic_order_always_zero_flag;
  int offset_for_non_ref_pic;
  int offset_for_top_to_bottom_field;
  int num_ref_frames_in_pic_order_cnt_cycle;
  int expected_delta_per_pic_order_cnt_cycle;
  int offset_for_ref_frame[255];
  int max_num_ref_frames;
  bool gaps_in_frame_num_value_allowed_flag;
  int pic_width_in_mbs_minus1;
  int pic_height_in_map_units_minus1;
  bool frame_mbs_only_flag;
} H264SPS;

typedef struct H264PPS {
  int pic_parameter_set_id;
  int seq_parameter_set_id;
  bool entropy_coding_mode_flag;
  bool bottom_field_pic_order_in_frame_present_flag;
} H264PPS;

typedef struct H264SliceHeader {
  bool idr_pic_flag;
  int nal_ref_idc;
  int first_mb_in_slice;
  int slice_type;
  int pic_parameter_set_id;
  int colour_plane_id;
  int frame_num;
  bool field_pic_flag;
  bool bottom_field_flag;
  int idr_pic_id;
  int pic_order_cnt_lsb;
  int delta_pic_order_cnt_bottom;
  int delta_pic_order_cnt0;
  int delta_pic_order_cnt1;
} H264SliceHeader;

typedef struct H264Nalu {
  const uint8_t* data;
  size_t size;

  uint8_t forbidden_bit;
  int nal_ref_idc;
  int nal_unit_type;
} H264Nalu;

extern H264SPS curr_sps;
extern H264PPS curr_pps;
extern H264SliceHeader curr_slice_header;

H264Nalu parse_h264_nalu_header(uint8_t* curr_pos, uint8_t* end_pos);
H264Nalu parse_h264_nalu(uint8_t* curr_pos, uint8_t* end_pos);

#endif
