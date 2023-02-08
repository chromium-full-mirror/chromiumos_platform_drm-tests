/*
 * Copyright 2023 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef __H265_PARTIAL_PARSER_H__
#define __H265_PARTIAL_PARSER_H__

// NAL Unit types are taken from Table 7-1 of HEVC/H265 standard
// http://www.itu.int/rec/T-REC-H.265-201410-I/en
enum H265NaluType {
  kTrailN = 0,
  kTrailR = 1,
  kTsaN = 2,
  kTsaR = 3,
  kStsaN = 4,
  kStsaR = 5,
  kRadlN = 6,
  kRadlR = 7,
  kRaslN = 8,
  kRaslR = 9,
  kRsvVclN10 = 10,
  kRsvVclR11 = 11,
  kRsvVclN12 = 12,
  kRsvVclR13 = 13,
  kRsvVclN14 = 14,
  kRsvVclR15 = 15,
  kBlaWLp = 16,
  kBlaWRadl = 17,
  kBlaNLp = 18,
  kIdrWRadl = 19,
  kIdrNLp = 20,
  kCraNut = 21,
  kRsvIrapVcl22 = 22,
  kRsvIrapVcl23 = 23,
  kRsvVcl24 = 24,
  kRsvVcl25 = 25,
  kRsvVcl26 = 26,
  kRsvVcl27 = 27,
  kRsvVcl28 = 28,
  kRsvVcl29 = 29,
  kRsvVcl30 = 30,
  kRsvVcl31 = 31,
  kVpsNut = 32,
  kSpsNut = 33,
  kPpsNut = 34,
  kAudNut = 35,
  kEosNut = 36,
  kEobNut = 37,
  kFdNut = 38,
  kPrefixSeiNut = 39,
  kSuffixSeiNut = 40,
  kRsvNvcl41 = 41,
  kRsvNvcl42 = 42,
  kRsvNvcl43 = 43,
  kRsvNvcl44 = 44,
  kRsvNvcl45 = 45,
  kRsvNvcl46 = 46,
  kRsvNvcl47 = 47,
  kUnspec48 = 48,
  kUnspec49 = 49,
  kUnspec50 = 50,
  kUnspec51 = 51,
  kUnspec52 = 52,
  kUnspec53 = 53,
  kUnspec54 = 54,
  kUnspec55 = 55,
  kUnspec56 = 56,
  kUnspec57 = 57,
  kUnspec58 = 58,
  kUnspec59 = 59,
  kUnspec60 = 60,
  kUnspec61 = 61,
  kUnspec62 = 62,
  kUnspec63 = 63,
};

bool is_h265_nalu_slice_type(enum H265NaluType type);

typedef struct H265Nalu {
  const uint8_t* data;
  size_t size;

  uint8_t forbidden_bit;
  uint8_t nal_unit_type;
  uint8_t nuh_layer_id;
  uint8_t nuh_temporal_id_plus1;
} H265Nalu;

H265Nalu parse_h265_nalu(uint8_t* curr_pos, uint8_t* end_pos);

bool is_h265_slice_first_in_picture(const H265Nalu *nalu);

#endif // __H265_PARTIAL_PARSER_H__
