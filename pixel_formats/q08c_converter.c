/*
 * Copyright 2024 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "pixel_formats/q08c_converter.h"

#include <stdlib.h>
#include <string.h>

#define BITS_IN_BYTE 8

#define ALIGN_UP(x, alignment) (((x) + (alignment)-1) & (~((alignment)-1)))

#define Q08C_PLANE_ALIGNMENT 4096

// Base structure of Q08C
#define Q08C_BLOCK_HEIGHT 8
#define Q08C_BLOCK_WIDTH 32
#define Q08C_BLOCK_HEADER_SIZE 0x1C
#define Q08C_BLOCK_SIZE (Q08C_BLOCK_HEIGHT * Q08C_BLOCK_WIDTH)

// Compressed blocks are further sub-divided into "quads"
#define Q08C_QUAD_WIDTH (Q08C_BLOCK_WIDTH / 2)
#define Q08C_QUAD_HEIGHT (Q08C_BLOCK_HEIGHT / 2)

// Compressed quads are even further sub-divided into "mini blocks"
#define Q08C_MINI_BLOCK_HEIGHT (Q08C_BLOCK_HEIGHT / 8)
#define Q08C_MINI_BLOCK_WIDTH (Q08C_BLOCK_WIDTH / 8)
#define Q08C_MINI_BLOCK_META_SIZE_BITS 3

// Blocks are grouped into "mega blocks" when serialized in memory.
#define Q08C_MEGA_BLOCK_HEIGHT (Q08C_BLOCK_HEIGHT * 4)
#define Q08C_MEGA_BLOCK_WIDTH (Q08C_BLOCK_WIDTH * 4)
#define Q08C_MEGA_BLOCK_SIZE (Q08C_MEGA_BLOCK_HEIGHT * Q08C_MEGA_BLOCK_WIDTH)
#define Q08C_BLOCKS_IN_MEGA_BLOCK (Q08C_MEGA_BLOCK_SIZE / Q08C_BLOCK_SIZE)

// Metadata for each block is serialized in a grouping called a "jumbo block".
#define Q08C_JUMBO_BLOCK_HEIGHT (Q08C_BLOCK_HEIGHT * 16)
#define Q08C_JUMBO_BLOCK_WIDTH (Q08C_BLOCK_WIDTH * 16)
#define Q08C_JUMBO_ROW_ALIGNMENT 1024

// Uncompressed blocks are swizzled in a fractal Z pattern.
// clang-format off
const uint8_t kYSwizzle[Q08C_BLOCK_HEIGHT][Q08C_BLOCK_WIDTH] = {
  { 0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15, 0x20, 0x21, 0x24, 0x25, 0x30, 0x31, 0x34, 0x35, 0x40, 0x41, 0x44, 0x45, 0x50, 0x51, 0x54, 0x55, 0x60, 0x61, 0x64, 0x65, 0x70, 0x71, 0x74, 0x75,  },
  { 0x02, 0x03, 0x06, 0x07, 0x12, 0x13, 0x16, 0x17, 0x22, 0x23, 0x26, 0x27, 0x32, 0x33, 0x36, 0x37, 0x42, 0x43, 0x46, 0x47, 0x52, 0x53, 0x56, 0x57, 0x62, 0x63, 0x66, 0x67, 0x72, 0x73, 0x76, 0x77,  },
  { 0x08, 0x09, 0x0C, 0x0D, 0x18, 0x19, 0x1C, 0x1D, 0x28, 0x29, 0x2C, 0x2D, 0x38, 0x39, 0x3C, 0x3D, 0x48, 0x49, 0x4C, 0x4D, 0x58, 0x59, 0x5C, 0x5D, 0x68, 0x69, 0x6C, 0x6D, 0x78, 0x79, 0x7C, 0x7D,  },
  { 0x0A, 0x0B, 0x0E, 0x0F, 0x1A, 0x1B, 0x1E, 0x1F, 0x2A, 0x2B, 0x2E, 0x2F, 0x3A, 0x3B, 0x3E, 0x3F, 0x4A, 0x4B, 0x4E, 0x4F, 0x5A, 0x5B, 0x5E, 0x5F, 0x6A, 0x6B, 0x6E, 0x6F, 0x7A, 0x7B, 0x7E, 0x7F,  },
  { 0x80, 0x81, 0x84, 0x85, 0x90, 0x91, 0x94, 0x95, 0xA0, 0xA1, 0xA4, 0xA5, 0xB0, 0xB1, 0xB4, 0xB5, 0xC0, 0xC1, 0xC4, 0xC5, 0xD0, 0xD1, 0xD4, 0xD5, 0xE0, 0xE1, 0xE4, 0xE5, 0xF0, 0xF1, 0xF4, 0xF5,  },
  { 0x82, 0x83, 0x86, 0x87, 0x92, 0x93, 0x96, 0x97, 0xA2, 0xA3, 0xA6, 0xA7, 0xB2, 0xB3, 0xB6, 0xB7, 0xC2, 0xC3, 0xC6, 0xC7, 0xD2, 0xD3, 0xD6, 0xD7, 0xE2, 0xE3, 0xE6, 0xE7, 0xF2, 0xF3, 0xF6, 0xF7,  },
  { 0x88, 0x89, 0x8C, 0x8D, 0x98, 0x99, 0x9C, 0x9D, 0xA8, 0xA9, 0xAC, 0xAD, 0xB8, 0xB9, 0xBC, 0xBD, 0xC8, 0xC9, 0xCC, 0xCD, 0xD8, 0xD9, 0xDC, 0xDD, 0xE8, 0xE9, 0xEC, 0xED, 0xF8, 0xF9, 0xFC, 0xFD,  },
  { 0x8A, 0x8B, 0x8E, 0x8F, 0x9A, 0x9B, 0x9E, 0x9F, 0xAA, 0xAB, 0xAE, 0xAF, 0xBA, 0xBB, 0xBE, 0xBF, 0xCA, 0xCB, 0xCE, 0xCF, 0xDA, 0xDB, 0xDE, 0xDF, 0xEA, 0xEB, 0xEE, 0xEF, 0xFA, 0xFB, 0xFE, 0xFF,  },
};
const uint8_t kUVSwizzle[Q08C_BLOCK_HEIGHT][Q08C_BLOCK_WIDTH] = {
  { 0x00, 0x01, 0x02, 0x03, 0x08, 0x09, 0x0A, 0x0B, 0x20, 0x21, 0x22, 0x23, 0x28, 0x29, 0x2A, 0x2B, 0x40, 0x41, 0x42, 0x43, 0x48, 0x49, 0x4A, 0x4B, 0x60, 0x61, 0x62, 0x63, 0x68, 0x69, 0x6A, 0x6B,  },
  { 0x04, 0x05, 0x06, 0x07, 0x0C, 0x0D, 0x0E, 0x0F, 0x24, 0x25, 0x26, 0x27, 0x2C, 0x2D, 0x2E, 0x2F, 0x44, 0x45, 0x46, 0x47, 0x4C, 0x4D, 0x4E, 0x4F, 0x64, 0x65, 0x66, 0x67, 0x6C, 0x6D, 0x6E, 0x6F,  },
  { 0x10, 0x11, 0x12, 0x13, 0x18, 0x19, 0x1A, 0x1B, 0x30, 0x31, 0x32, 0x33, 0x38, 0x39, 0x3A, 0x3B, 0x50, 0x51, 0x52, 0x53, 0x58, 0x59, 0x5A, 0x5B, 0x70, 0x71, 0x72, 0x73, 0x78, 0x79, 0x7A, 0x7B,  },
  { 0x14, 0x15, 0x16, 0x17, 0x1C, 0x1D, 0x1E, 0x1F, 0x34, 0x35, 0x36, 0x37, 0x3C, 0x3D, 0x3E, 0x3F, 0x54, 0x55, 0x56, 0x57, 0x5C, 0x5D, 0x5E, 0x5F, 0x74, 0x75, 0x76, 0x77, 0x7C, 0x7D, 0x7E, 0x7F,  },
  { 0x80, 0x81, 0x82, 0x83, 0x88, 0x89, 0x8A, 0x8B, 0xA0, 0xA1, 0xA2, 0xA3, 0xA8, 0xA9, 0xAA, 0xAB, 0xC0, 0xC1, 0xC2, 0xC3, 0xC8, 0xC9, 0xCA, 0xCB, 0xE0, 0xE1, 0xE2, 0xE3, 0xE8, 0xE9, 0xEA, 0xEB,  },
  { 0x84, 0x85, 0x86, 0x87, 0x8C, 0x8D, 0x8E, 0x8F, 0xA4, 0xA5, 0xA6, 0xA7, 0xAC, 0xAD, 0xAE, 0xAF, 0xC4, 0xC5, 0xC6, 0xC7, 0xCC, 0xCD, 0xCE, 0xCF, 0xE4, 0xE5, 0xE6, 0xE7, 0xEC, 0xED, 0xEE, 0xEF,  },
  { 0x90, 0x91, 0x92, 0x93, 0x98, 0x99, 0x9A, 0x9B, 0xB0, 0xB1, 0xB2, 0xB3, 0xB8, 0xB9, 0xBA, 0xBB, 0xD0, 0xD1, 0xD2, 0xD3, 0xD8, 0xD9, 0xDA, 0xDB, 0xF0, 0xF1, 0xF2, 0xF3, 0xF8, 0xF9, 0xFA, 0xFB,  },
  { 0x94, 0x95, 0x96, 0x97, 0x9C, 0x9D, 0x9E, 0x9F, 0xB4, 0xB5, 0xB6, 0xB7, 0xBC, 0xBD, 0xBE, 0xBF, 0xD4, 0xD5, 0xD6, 0xD7, 0xDC, 0xDD, 0xDE, 0xDF, 0xF4, 0xF5, 0xF6, 0xF7, 0xFC, 0xFD, 0xFE, 0xFF,  },
};
// clang-format on

// Some utility functions for reading data from compressed blocks.
struct q08c_bitstream {
  size_t bit_idx;
  const uint8_t* buf;
  size_t len;
};

static int read_bit(struct q08c_bitstream* to_read) {
  size_t byte_idx = to_read->bit_idx / BITS_IN_BYTE;

  if (byte_idx >= to_read->len)
    return 0;

  int ret = (to_read->buf[byte_idx] >> (to_read->bit_idx % BITS_IN_BYTE)) & 0x1;
  to_read->bit_idx++;

  return ret;
}

static int read_unsigned(struct q08c_bitstream* to_read, int n) {
  int ret = 0;
  for (int i = 0; i < n; i++) {
    ret |= read_bit(to_read) << i;
  }

  return ret;
}

// Q08C uses an unusual signed ints representation.
static int read_signed(struct q08c_bitstream* to_read, int n) {
  int ret = read_unsigned(to_read, n);
  char sign = ret & 1;

  ret >>= 1;

  if (sign) {
    return -1 * (ret + 1);
  } else {
    return ret;
  }
}

static void unswizzle_block(uint8_t* dest,
                            size_t width,
                            size_t base_x,
                            size_t base_y,
                            const uint8_t* uncompressed_block,
                            char is_chroma) {
  for (size_t y = base_y; y < base_y + Q08C_BLOCK_HEIGHT; y++) {
    for (size_t x = base_x; x < base_x + Q08C_BLOCK_WIDTH; x++) {
      if (is_chroma) {
        dest[y * width + x] =
            uncompressed_block[kUVSwizzle[y - base_y][x - base_x]];
      } else {
        dest[y * width + x] =
            uncompressed_block[kYSwizzle[y - base_y][x - base_x]];
      }
    }
  }
}

static void decompress_block(uint8_t* dest,
                             size_t width,
                             size_t base_x,
                             size_t base_y,
                             const uint8_t* compressed_block,
                             size_t block_size,
                             char is_chroma) {
  struct q08c_bitstream header_reader, body_reader;
  header_reader.bit_idx = 0;
  header_reader.buf = compressed_block;
  header_reader.len = Q08C_BLOCK_HEADER_SIZE;
  body_reader.bit_idx = 0;
  body_reader.buf = compressed_block + Q08C_BLOCK_HEADER_SIZE;
  body_reader.len = block_size - Q08C_BLOCK_HEADER_SIZE;

  // Initialize the top-left corner of each quadrant.
  for (size_t quadrant_y = 0; quadrant_y < Q08C_BLOCK_HEIGHT;
       quadrant_y += Q08C_QUAD_HEIGHT) {
    for (size_t quadrant_x = 0; quadrant_x < Q08C_BLOCK_WIDTH;
         quadrant_x += Q08C_QUAD_WIDTH) {
      dest[(base_y + quadrant_y) * width + base_x + quadrant_x] =
          read_unsigned(&header_reader, BITS_IN_BYTE);
    }
  }

  // Decompress the rest of the block.
  // The core compression algorithm is just a prediction and delta scheme.
  // The lengths of the delta values are encoded in the header. Each mini block
  // has its own length.
  // Deltas are encoded in raster order, but with the twist that the quadrants
  // are interleaved.
  for (size_t y = 0; y < Q08C_QUAD_HEIGHT; y += Q08C_MINI_BLOCK_HEIGHT) {
    for (size_t x = 0; x < Q08C_QUAD_WIDTH; x += Q08C_MINI_BLOCK_WIDTH) {
      for (size_t quadrant_y = 0; quadrant_y < Q08C_BLOCK_HEIGHT;
           quadrant_y += Q08C_QUAD_HEIGHT) {
        for (size_t quadrant_x = 0; quadrant_x < Q08C_BLOCK_WIDTH;
             quadrant_x += Q08C_QUAD_WIDTH) {
          size_t delta_sizes =
              read_unsigned(&header_reader, Q08C_MINI_BLOCK_META_SIZE_BITS);

          // 0x7 is a special escape sequence that indicates the miniblock is
          // encoded directly, with no compression.
          if (delta_sizes == 0x7) {
            for (size_t mini_y = y; mini_y < y + Q08C_MINI_BLOCK_HEIGHT;
                 mini_y++) {
              for (size_t mini_x = x; mini_x < x + Q08C_MINI_BLOCK_WIDTH;
                   mini_x++) {
                dest[(base_y + quadrant_y + mini_y) * width + base_x +
                     quadrant_x + mini_x] =
                    read_unsigned(&body_reader, BITS_IN_BYTE);
              }
            }
          } else {
            for (size_t mini_y = y; mini_y < y + Q08C_MINI_BLOCK_HEIGHT;
                 mini_y++) {
              for (size_t mini_x = x; mini_x < x + Q08C_MINI_BLOCK_WIDTH;
                   mini_x++) {
                // The prediction algorithm is essentially just Paeth.
                // The top row uses the left pixel, the left column uses the top
                // pixel. For all other pixels, the predictor uses the
                // difference between the top and top-left pixel to estimate the
                // gradient of the image at the current position, and adds that
                // gradient to the left pixel.
                uint8_t pred = 0;
                if (mini_y == 0 && mini_x == 0) {
                  pred = dest[(base_y + quadrant_y + mini_y) * width + base_x +
                              quadrant_x + mini_x];
                } else if (mini_y == 0) {
                  pred = dest[(base_y + quadrant_y + mini_y) * width + base_x +
                              quadrant_x + mini_x - 1];
                } else if (mini_x == 0) {
                  pred = dest[(base_y + quadrant_y + mini_y - 1) * width +
                              base_x + quadrant_x + mini_x];
                } else {
                  uint8_t top =
                      dest[(base_y + quadrant_y + mini_y - 1) * width + base_x +
                           quadrant_x + mini_x];
                  uint8_t left = dest[(base_y + quadrant_y + mini_y) * width +
                                      base_x + quadrant_x + mini_x - 1];
                  uint8_t top_left =
                      dest[(base_y + quadrant_y + mini_y - 1) * width + base_x +
                           quadrant_x + mini_x - 1];
                  pred = top - top_left + left;
                }

                int delta = read_signed(&body_reader, delta_sizes);

                dest[(base_y + quadrant_y + mini_y) * width + base_x +
                     quadrant_x + mini_x] = pred + delta;
              }
            }
          }
        }
      }
    }
  }

  // Chroma blocks have their U and V channels de-interleaved into the left and
  // right halves of the block. We need to re-interleave them for NV12.
  if (is_chroma) {
    uint8_t row[Q08C_BLOCK_WIDTH];
    for (size_t y = 0; y < Q08C_BLOCK_HEIGHT; y++) {
      for (size_t x = 0; x < Q08C_BLOCK_WIDTH / 2; x++) {
        row[2 * x] = dest[(base_y + y) * width + base_x + x];
        row[2 * x + 1] =
            dest[(base_y + y) * width + base_x + x + Q08C_BLOCK_WIDTH / 2];
      }
      memcpy(dest + (base_y + y) * width + base_x, row, Q08C_BLOCK_WIDTH);
    }
  }
}

// Megablocks are swizzled in an unusual pattern in memory.
size_t kMegablockXOrder[Q08C_BLOCKS_IN_MEGA_BLOCK] = {
    0, 32, 32, 0, 32, 0, 0, 32, 96, 64, 64, 96, 64, 96, 96, 64};
size_t kMegablockYOrder[Q08C_BLOCKS_IN_MEGA_BLOCK] = {
    0, 8, 24, 16, 16, 24, 8, 0, 24, 16, 0, 8, 8, 0, 16, 24};

static void decode_megablock(uint8_t* dest,
                             int** metadata,
                             const uint8_t* megablock,
                             size_t width,
                             size_t base_x,
                             size_t base_y,
                             char is_chroma) {
  for (int i = 0; i < Q08C_BLOCKS_IN_MEGA_BLOCK; i++) {
    size_t x = base_x + kMegablockXOrder[i];
    size_t y = base_y + kMegablockYOrder[i];
    int meta_val = metadata[y / Q08C_BLOCK_HEIGHT][x / Q08C_BLOCK_WIDTH];
    if (meta_val == -1) {
      unswizzle_block(dest, width, x, y, megablock + i * Q08C_BLOCK_SIZE,
                      is_chroma);
    } else if (meta_val > 0) {
      size_t block_offset = i * Q08C_BLOCK_SIZE;
      // Odd pattern where compressed blocks below a certain size will be offset
      // within the megablock some of the time.
      if ((i % 4 == 1 || i % 4 == 2) && meta_val <= 0x80) {
        block_offset += 0x80;
      }
      decompress_block(dest, width, x, y, megablock + block_offset, meta_val,
                       is_chroma);
    }
  }
}

static size_t parse_metadata_val(int** metadata,
                                 const uint8_t* compressed_plane,
                                 size_t offset,
                                 size_t x,
                                 size_t y) {
  int val = compressed_plane[offset];

  offset += 1;

  // The metadata is usually the length of the block, but there's a special
  // metadata value, 0x1F, that indicates an uncompressed block.
  if (val == 0x1F) {
    val = -1;
  } else if (val == 0x00) {
    val = 0;
  } else {
    // Length of block is given in units of 32 bytes. It's always trailed by a 1
    // and lead by 0001 for some reason.
    val = (((val >> 1) & 0x7) + 1) * 32;
  }

  metadata[y / Q08C_BLOCK_HEIGHT][x / Q08C_BLOCK_WIDTH] = val;

  return offset;
}

static int** parse_metadata(const uint8_t* compressed_plane,
                            size_t* offset,
                            size_t width,
                            size_t height) {
  size_t aligned_width = ALIGN_UP(width, Q08C_JUMBO_BLOCK_WIDTH);
  int** metadata = (int**)malloc(height * sizeof(int*));
  for (size_t y = 0; y < height; y++) {
    metadata[y] = (int*)malloc(aligned_width * sizeof(int));
  }

  for (size_t jumbo_y = 0; jumbo_y < height;
       jumbo_y += Q08C_JUMBO_BLOCK_HEIGHT) {
    for (size_t jumbo_x = 0; jumbo_x < aligned_width;
         jumbo_x += Q08C_JUMBO_BLOCK_WIDTH) {
      // Blocks aren't serialized within jumbo blocks exactly in a raster order,
      // there's actually a further sub division into jumbo block quadrants.
      for (size_t y = jumbo_y; y < jumbo_y + Q08C_JUMBO_BLOCK_HEIGHT / 2;
           y += Q08C_BLOCK_HEIGHT) {
        for (size_t x = jumbo_x; x < jumbo_x + Q08C_JUMBO_BLOCK_WIDTH / 2;
             x += Q08C_BLOCK_WIDTH) {
          *offset =
              parse_metadata_val(metadata, compressed_plane, *offset, x, y);
        }
      }
      for (size_t y = jumbo_y; y < jumbo_y + Q08C_JUMBO_BLOCK_HEIGHT / 2;
           y += Q08C_BLOCK_HEIGHT) {
        for (size_t x = jumbo_x + Q08C_JUMBO_BLOCK_WIDTH / 2;
             x < jumbo_x + Q08C_JUMBO_BLOCK_WIDTH; x += Q08C_BLOCK_WIDTH) {
          *offset =
              parse_metadata_val(metadata, compressed_plane, *offset, x, y);
        }
      }
      for (size_t y = jumbo_y + Q08C_JUMBO_BLOCK_HEIGHT / 2;
           y < jumbo_y + Q08C_JUMBO_BLOCK_HEIGHT; y += Q08C_BLOCK_HEIGHT) {
        for (size_t x = jumbo_x; x < jumbo_x + Q08C_JUMBO_BLOCK_WIDTH / 2;
             x += Q08C_BLOCK_WIDTH) {
          *offset =
              parse_metadata_val(metadata, compressed_plane, *offset, x, y);
        }
      }
      for (size_t y = jumbo_y + Q08C_JUMBO_BLOCK_HEIGHT / 2;
           y < jumbo_y + Q08C_JUMBO_BLOCK_HEIGHT; y += Q08C_BLOCK_HEIGHT) {
        for (size_t x = jumbo_x + Q08C_JUMBO_BLOCK_WIDTH / 2;
             x < jumbo_x + Q08C_JUMBO_BLOCK_WIDTH; x += Q08C_BLOCK_WIDTH) {
          *offset =
              parse_metadata_val(metadata, compressed_plane, *offset, x, y);
        }
      }
    }
    // Every row of "jumbo blocks" is 1024 aligned.
    *offset = ALIGN_UP(*offset, Q08C_JUMBO_ROW_ALIGNMENT);
  }

  // Metadata plane is page aligned.
  *offset = ALIGN_UP(*offset, Q08C_PLANE_ALIGNMENT);

  return metadata;
}

static void cleanup_metadata(int** metadata, size_t height) {
  for (size_t y = 0; y < height; y++) {
    free(metadata[y]);
  }
  free(metadata);
}

static size_t convert_plane(uint8_t* dest,
                            const uint8_t* src,
                            size_t width,
                            size_t height,
                            char is_chroma) {
  size_t offset = 0;
  int** metadata = parse_metadata(src, &offset, width, height);

  for (size_t y = 0; y < height; y += Q08C_MEGA_BLOCK_HEIGHT) {
    for (size_t x = 0; x < width; x += Q08C_MEGA_BLOCK_WIDTH) {
      decode_megablock(dest, metadata, src + offset, width, x, y, is_chroma);
      offset += Q08C_MEGA_BLOCK_SIZE;
    }
  }

  cleanup_metadata(metadata, height);

  return ALIGN_UP(offset, Q08C_PLANE_ALIGNMENT);
}

void convert_q08c_to_nv12(uint8_t* dest_y,
                          uint8_t* dest_uv,
                          const uint8_t* src,
                          size_t width,
                          size_t height) {
  width = ALIGN_UP(width, Q08C_MEGA_BLOCK_WIDTH);
  height = ALIGN_UP(height, Q08C_MEGA_BLOCK_HEIGHT);
  size_t offset = convert_plane(dest_y, src, width, height, 0);
  convert_plane(dest_uv, src + offset, width,
                ALIGN_UP(height / 2, Q08C_MEGA_BLOCK_HEIGHT), 1);
}
