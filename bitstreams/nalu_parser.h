/*
 * Copyright 2023 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <stdlib.h>
#include <stdint.h>

#ifndef __NALU_PARSER_H__
#define __NALU_PARSER_H__

#define NALU_SIGNATURE_SIZE 3

typedef struct Nalu {
  const uint8_t* data;
  size_t size;
} Nalu;

// Searches from start_pos to end_pos for the byte series: 0x00 0x00 0x01
// Returns the position of the first 0x00 if it is found, otherwise, returns
// NULL.
uint8_t* find_next_nalu(uint8_t* start_pos, uint8_t* end_pos);

// start_pos should start at an NALU. Determines the NALU size by searching for
// the next NALU.
Nalu tokenize_nalu(uint8_t* start_pos, uint8_t* end_pos);

#endif
