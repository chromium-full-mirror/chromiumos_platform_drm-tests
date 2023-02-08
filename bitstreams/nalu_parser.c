/*
 * Copyright 2023 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include "bitstreams/nalu_parser.h"

#include <assert.h>

uint8_t* find_next_nalu(uint8_t* start_pos, uint8_t* end_pos) {
  uint8_t* next_nalu_pos = start_pos;
  // Initializes "previous byte" buffers b0, b1, b2 so they won't match the
  // NALU start code sequence
  uint8_t b0 = 0xFF; // b0 holds *(next_nalu_pos-2)
  uint8_t b1 = 0xFF; // b1 holds *(next_nalu_pos-1)
  uint8_t b2 = 0xFF; // b2 holds *(next_nalu_pos)
  while (next_nalu_pos < end_pos) {
    b0 = b1;
    b1 = b2;
    b2 = *(++next_nalu_pos);
    // Checks to see if a nalu started an next_nalu_pos - 2
    if (b0 == 0x00 && b1 == 0x00 && b2 == 0x01)
      return next_nalu_pos - 2;
  }
  return NULL;
}

Nalu tokenize_nalu(uint8_t* curr_pos, uint8_t* end_pos) {
  assert(end_pos > curr_pos + NALU_SIGNATURE_SIZE);
  assert(*curr_pos == 0x00);
  assert(*(curr_pos + 1) == 0x00);
  assert(*(curr_pos + 2) == 0x01);
  uint8_t* next_nalu = find_next_nalu(curr_pos + NALU_SIGNATURE_SIZE, end_pos);
  Nalu nalu= {.data=curr_pos,
              .size=(next_nalu ? next_nalu : end_pos) - curr_pos};
  return nalu;
}
