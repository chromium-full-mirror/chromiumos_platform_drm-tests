/*
 * Copyright 2023 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "bitfield_stream.h"
#include <assert.h>

#define BITS_PER_BYTE 8
#define MS_BIT_SET_MASK 0b10000000

BitfieldStream bitfield_stream_init(const uint8_t* new_bitstream,
                                    const uint8_t* new_end_pos) {
  BitfieldStream stream = {.bitstream = new_bitstream,
                           .bit_idx = 0,
                           .byte_idx = 0,
                           .bitstream_mask = MS_BIT_SET_MASK,
                           .bitstream_end_pos = new_end_pos};
  return stream;
}

void bitfield_stream_read_bits(BitfieldStream* bfs,
                               int num_bits,
                               void* dest_) {
  uint8_t* dest = (uint8_t*)dest_;

  assert(dest);
  assert(bfs);
  assert(bfs->bitstream);
  assert(bfs->bit_idx < BITS_PER_BYTE);

  int written_bits = 0;
  uint8_t buf = 0;
  while (written_bits < num_bits) {
    assert(bfs->bitstream + bfs->byte_idx < bfs->bitstream_end_pos);

    buf = (buf << 1) | (!!(bfs->bitstream[bfs->byte_idx] &
                           bfs->bitstream_mask));
    bfs->bitstream_mask >>= 1;
    written_bits++;

    if (!(written_bits % BITS_PER_BYTE)) {
      *dest = buf;
      dest++;
      buf = 0;
    }

    bfs->bit_idx++;
    bfs->byte_idx += (bfs->bit_idx == BITS_PER_BYTE);
    bfs->bit_idx = bfs->bit_idx % BITS_PER_BYTE;
    if (!bfs->bit_idx)
      bfs->bitstream_mask = MS_BIT_SET_MASK;
  }

  if (written_bits % BITS_PER_BYTE)
    *dest = buf;
}

void bitfield_stream_skip_bits(BitfieldStream* bfs, size_t bits_to_skip) {
  assert(bfs->bitstream);
  assert(bfs->bit_idx < BITS_PER_BYTE);
  bfs->byte_idx += (bits_to_skip + bfs->bit_idx) / BITS_PER_BYTE;
  bfs->bit_idx = (bits_to_skip + bfs->bit_idx) % BITS_PER_BYTE;
  bfs->bitstream_mask = MS_BIT_SET_MASK >> bfs->bit_idx;
}
