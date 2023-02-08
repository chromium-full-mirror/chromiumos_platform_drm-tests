/*
 * Copyright 2023 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef __BITFIELD_STREAM_H__
#define __BITFIELD_STREAM_H__

#include <stdlib.h>
#include <stdint.h>

typedef struct BitfieldStream {
  const uint8_t* bitstream;
  uint8_t bit_idx;
  size_t byte_idx;
  uint8_t bitstream_mask;
  const uint8_t* bitstream_end_pos;
} BitfieldStream;

BitfieldStream bitfield_stream_init(const uint8_t* new_bitstream,
                                    const uint8_t* new_end_pos);

void bitfield_stream_read_bits(BitfieldStream* bfs,
                               int num_bits,
                               void* dest_);

void bitfield_stream_skip_bits(BitfieldStream* bfs, size_t bits_to_skip);

#endif
