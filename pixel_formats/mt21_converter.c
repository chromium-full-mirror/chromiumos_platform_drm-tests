/*
 * Copyright 2023 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

// Simple converter from MT21 to NV12.
//
// We convert MT21 to NV12 in two passes. First, we decompress MT21 to MM21.
// Then, we detile MM21 to NV12.
//
// MT21 is divided into 16x32 tiles like MM21. Unlike MM21, however, MT21 is
// further divided into 16x8 blocks, and divided again into 16x4 subblocks.
// Subblocks are generally compressed data of variable length, whereas blocks
// are generally 0 padded to be exactly 128 bytes long. This is to facilitate
// random access.
//
// The core MT21->MM21 decompression loop algorithm is the following:
// 1. Predict the next pixel value based on its previously calculated neighbors.
// 2. Read the compressed "error" value and add it to the prediction.
// 3. Write the real pixel value to the MM21 buffer.
//
// We borrow the LibYUV MM21->NV12 conversion algorithm for the second pass.

#include "pixel_formats/mt21_converter.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BITS_IN_BYTE 8
#define MT21_BLOCK_WIDTH 16
#define MT21_BLOCK_HEIGHT 8
#define MT21_BLOCK_SIZE (MT21_BLOCK_WIDTH * MT21_BLOCK_HEIGHT)
#define MT21_SUBBLOCK_WIDTH 16
#define MT21_SUBBLOCK_HEIGHT 4
#define MT21_SUBBLOCK_SIZE (MT21_SUBBLOCK_WIDTH * MT21_SUBBLOCK_HEIGHT)
#define MT21_SUBBLOCKS_IN_BLOCK 2
#define MT21_TILE_WIDTH 16
#define MT21_TILE_HEIGHT 32
#define MT21_TILE_SIZE (MT21_TILE_WIDTH * MT21_TILE_HEIGHT)
#define MT21_Y_FOOTER_ALIGNMENT 4096
#define MT21_UV_FOOTER_ALIGNMENT 2048

#define ALIGN_UP(x, alignment) (((x) + (alignment)-1) & (~((alignment)-1)))

// MT21 is arranged in 16 byte rows and is read right to left, top to bottom,
// and high bit to low bit.
struct mt21_bitstream {
  size_t bit_idx;
  const uint8_t* buf;
  size_t len;
};

static int read_bit(struct mt21_bitstream* to_read) {
  size_t byte_idx = to_read->bit_idx / BITS_IN_BYTE;
  size_t byte_row, byte_col;
  int ret;

  if (byte_idx >= to_read->len)
    return 0;

  byte_row = byte_idx / MT21_BLOCK_WIDTH;
  // Index goes from right to left, so we need to subtract it from the block
  // width to get the column index from the left.
  byte_col = MT21_BLOCK_WIDTH - 1 - (byte_idx % MT21_BLOCK_WIDTH);
  ret = (to_read->buf[byte_row * MT21_BLOCK_WIDTH + byte_col] >>
         (BITS_IN_BYTE - 1 - (to_read->bit_idx % BITS_IN_BYTE))) &
        0x01;
  to_read->bit_idx++;

  return ret;
}

static uint32_t read_n_bits(struct mt21_bitstream* to_read, int n) {
  uint32_t ret = 0;

  for (int i = 0; i < n; i++) {
    ret = (ret << 1) | read_bit(to_read);
  }

  return ret;
}

// Error values are compressed using a variant of Golomb-Rice coding.
// Golomb-Rice codes have three components:
// 1. A unary component
// 2. A separator 0
// 3. A binary component of length k
// Decoding a Golomb-Rice code is the same as evaluating the expression
// unary_component*(2^k)+binary_component
// Where "k" is a metadata parameter chosen ahead of time.
//
// Golomb-Rice codes are useful for compressing data that tends to skew towards
// "small" values, which makes it good for error encoding. In fact,
// Golomb-Rice is actually equivalent to Huffman coding when the data follows a
// geometric probability distribution.
//
// MT21 error encoding differs from real Golomb-Rice in three regards:
// 1. Errors can be negative. We get around this by just doubling all of our
//    numbers and using the LSB as a sign bit.
// 2. MT21 error encoding is length limited, which means that after we detect a
//    unary component of 8, we bail on the decode and read the error as binary.
//    This puts a cap on our worst case scenario if we need to encode a really
//    big error value and we are afraid the unary component will explode.
// 3. 0s lack an LSB. This is because we use the LSB as a sign bit, and it's
//    unnecessary to have a negative and positive 0.
static int read_error_value(struct mt21_bitstream* to_read, int k) {
  // This is part of the length limiting logic. When we see
  // |escape_sequence_num| number of 1s in a row, we interpret that as an
  // "escape sequence" and switch into binary mode.
  const int escape_sequence_num = 7 + k;
  int num_ones = 0;
  int ret = 0;

  // Read the unary component.
  while (1) {
    const int curr_bit = read_bit(to_read);
    if (curr_bit) {
      num_ones++;

      if (num_ones == escape_sequence_num) {
        break;
      }
    } else {
      break;
    }
  }

  if (num_ones == escape_sequence_num) {
    // Limited length mode. We've hit the escape sequence.
    // I'm not sure why the size of the binary component is smaller for
    // larger "k" values.
    if (k >= 4) {
      ret = read_n_bits(to_read, 7);
    } else {
      ret = read_n_bits(to_read, 8);
    }

    ret += num_ones * (1 << k);
  } else if (num_ones) {
    // Normal error coding.
    ret = (num_ones * (1 << k)) + read_n_bits(to_read, k);
  } else {
    // We need to special case a unary component of 0 because if the error is 0,
    // then we need to stop reading before the sign bit.
    ret = read_n_bits(to_read, k - 1);
    if (ret) {
      ret <<= 1;
      ret += read_bit(to_read);
    }
  }

  // Map the positive-only Golomb-Rice code to a signed number.
  if (ret % 2) {
    return -1 * (ret / 2);
  } else {
    return ret / 2;
  }
}

// Predict the next pixel value based on the neighborhood. MT21 uses the top
// left, top, top right, and right pixel for predictions.
//     ...| A | B | C |...
//     --------------------
//     ...| ? | X | D |...
// In this diagram, we would use A, B, C, and D to predict X.
//
// Note that we process pixels in a "mirrored" raster order, from right to left,
// and top to bottom. This is why we set D to be the right neighbor, not the
// left neighbor.
static uint8_t predict(const uint8_t* subblock,
                       int x,
                       int y,
                       int width,
                       int height) {
  assert(!(x == width - 1 && y == 0));

  if (y == 0) {
    // If we are processing the first row of the subblock, we don't have A, B,
    // or C to reference, so we just predict "D".
    return subblock[x + 1];
  } else if (x == width - 1) {
    // If we are processing the last column of the subblock, we don't have a C
    // or a D, so we just predict "B".
    return subblock[(y - 1) * width + x];
  } else if (x == 0) {
    // If we are processing the first column of the subblock, we don't have an A
    // to use for predictions.
    //
    // This algorithm seems to be similar to the JPEG-LS LOCO-I predictor
    // without the context modeling and flipped to be right-handed instead of
    // left-handed.
    int up_right = subblock[(y - 1) * width + x + 1];
    int up = subblock[(y - 1) * width + x];
    int right = subblock[y * width + x + 1];
    int max_up_right = up > right ? up : right;
    int min_up_right = up < right ? up : right;

    if (up_right <= max_up_right && up_right >= min_up_right) {
      // The basic idea here seems to be to estimate the horizontal gradient at
      // this portion of the image and add that gradient to the right neighbor.
      // This is similar to the JPEG-LS and Paeth prediction algorithms.
      return right + up - up_right;
    } else if (up_right > max_up_right) {
      // In the original JPEG-LS predictor, this should actually return
      // |min_up_right|. The logic there was that this algorithm was detecting
      // either an edge directly above X or an edge directly to the right of X,
      // and it was returning the pixel value on the other side of the edge. But
      // MT21 seems to have the logic backwards, which makes me think there
      // might actually be a mistake in the hardware. This will still yield
      // decent compression performance simply because the spatial correlation
      // of an image means up and right are usually similar values, but we could
      // probably do better.
      return max_up_right;
    } else {
      // This is the other half of the faulty edge detection logic.
      return min_up_right;
    }
  } else {
    // This is what we use to process the "body" of the subblock, where we have
    // A, B, C, and D available to us. The usage of D is where we differ again
    // from the JPEG-LS standard.
    int up_left = subblock[(y - 1) * width + x - 1];
    int up_right = subblock[(y - 1) * width + x + 1];
    int up = subblock[(y - 1) * width + x];
    int right = subblock[y * width + x + 1];
    int max_up_right = up > right ? up : right;
    int min_up_right = up < right ? up : right;

    if (up_right <= max_up_right && up_right >= min_up_right) {
      // This is the horizontal gradient method again.
      return right + up - up_right;
    } else if (up_left <= max_up_right && up_left >= min_up_right) {
      // This appears to be trying to use "A" for a different horizontal
      // gradient estimation based prediction, except we subtract A from B, not
      // B from A, so the sign of the gradient is going to be wrong. This
      // appears to be another mistake in the hardware. Again, not a huge deal,
      // but we could probably be getting better compression ratios.
      return up - up_left + right;
    } else if (up_left >= max_up_right) {
      // I think this is a correct variant of the edge detection approach.
      return max_up_right;
    } else {
      // Other half of the edge detector.
      return min_up_right;
    }
  }
}

// Decompress a subblock.
// Subblocks are structured in 4 parts:
// 1. 3-bits storing k-1
// 2. 8 bits storing the value of the top rightmost pixel (since we can't
//    predict it otherwise).
// 3. Golomb-Rice error codes.
// 4. 0 padding up to the nearest 16 byte boundary. Note that this won't be
//    present if the compressed data happens to already line up to a 16 byte
//    boundary.
static size_t decompress_subblock(uint8_t* dest,
                                  const uint8_t* src,
                                  size_t len,
                                  size_t bit_offset,
                                  int width,
                                  int height) {
  struct mt21_bitstream bitstream = {
      .bit_idx = bit_offset, .buf = src, .len = len};
  int k;

  // Read k value.
  k = read_n_bits(&bitstream, 3) + 1;
  // Read top rightmost pixel.
  dest[width - 1] = read_n_bits(&bitstream, 8);

  if (k == 8) {
    // If k is 8, we go into a special solid color block mode. It makes sense to
    // use k=8 for this purpose since any Golomb-Rice code with a k of 8 would
    // be longer than a byte, and thus defeat the purpose of compression.
    for (int y = 0; y < height; y++) {
      for (int x = 0; x < width; x++) {
        dest[y * width + x] = dest[width - 1];
      }
    }

    return bitstream.bit_idx;
  }

  // We process pixels right to left, top to bottom. This is why we use a
  // right-handed variant of the JPEG-LS prediction algorithm, because we only
  // have the right neighbor available, not the left.
  for (int y = 0; y < height; y++) {
    for (int x = (y == 0 ? width - 2 : width - 1); x >= 0; x--) {
      uint8_t prediction = predict(dest, x, y, width, height);
      int error = read_error_value(&bitstream, k);
      dest[y * width + x] = prediction + error;
    }
  }

  return bitstream.bit_idx;
}

// MT21 contains a footer at the end of each plane. These footers
// are packed with 2-bit values indicating the size, in 16-byte rows, of the
// compressed subblocks. This helps flag passthrough subblocks that couldn't be
// compressed, and it also helps with parallelizing the decompression algorithm
// at the subblock level.
struct block_metadata {
  size_t subblock1_len;
  size_t subblock2_len;
};

static struct block_metadata get_block_metadata(const uint8_t* footer,
                                                size_t block_offset) {
  struct block_metadata ret;

  // Footer metadata is packed in 2-bit pairs from LSB to MSB. This means we can
  // pack 4 subblocks, or 2 blocks into every byte of footer.
  const size_t block_idx = block_offset / MT21_BLOCK_SIZE;
  ret.subblock1_len =
      MT21_BLOCK_WIDTH * (((footer[block_idx / MT21_SUBBLOCKS_IN_BLOCK] >>
                            ((block_idx % MT21_SUBBLOCKS_IN_BLOCK) ? 4 : 0)) &
                           0x3) +
                          1);
  ret.subblock2_len =
      MT21_BLOCK_WIDTH * (((footer[block_idx / MT21_SUBBLOCKS_IN_BLOCK] >>
                            ((block_idx % MT21_SUBBLOCKS_IN_BLOCK) ? 6 : 2)) &
                           0x3) +
                          1);

  return ret;
}

static void decompress_y_block(uint8_t* dest,
                               const uint8_t* src,
                               const uint8_t* footer,
                               size_t block_offset) {
  struct block_metadata metadata = get_block_metadata(footer, block_offset);

  if (metadata.subblock1_len == MT21_SUBBLOCK_SIZE) {
    // If we have a length of 4*16, that signals that the data wasn't able to be
    // compressed and we are looking at a "passthrough" block. Passthrough
    // blocks can be simply copied to the destination with no further
    // decompression.
    memcpy(dest + block_offset, src + block_offset, metadata.subblock1_len);
  } else {
    decompress_subblock(dest + block_offset, src + block_offset,
                        metadata.subblock1_len, 0, MT21_SUBBLOCK_WIDTH,
                        MT21_SUBBLOCK_HEIGHT);
  }

  if (metadata.subblock2_len == MT21_SUBBLOCK_SIZE) {
    memcpy(dest + block_offset + MT21_SUBBLOCK_SIZE,
           src + block_offset + metadata.subblock1_len, metadata.subblock2_len);
  } else {
    decompress_subblock(dest + block_offset + MT21_SUBBLOCK_SIZE,
                        src + block_offset + metadata.subblock1_len,
                        metadata.subblock2_len, 0, MT21_SUBBLOCK_WIDTH,
                        MT21_SUBBLOCK_HEIGHT);
  }
}

// U and V blocks are compressed separately, so we need to re-interleave them to
// produce valid MM21.
static void interleave_uv_block(uint8_t* dest_uv,
                                const uint8_t* src_u,
                                const uint8_t* src_v) {
  for (int i = 0; i < MT21_SUBBLOCK_SIZE / 2; i++) {
    *dest_uv = *src_u;
    dest_uv++;
    src_u++;
    *dest_uv = *src_v;
    dest_uv++;
    src_v++;
  }
}

static void decompress_uv_block(uint8_t* dest,
                                const uint8_t* src,
                                const uint8_t* footer,
                                size_t block_offset) {
  uint8_t scratch_u[MT21_SUBBLOCK_SIZE / 2];
  uint8_t scratch_v[MT21_SUBBLOCK_SIZE / 2];
  struct block_metadata metadata = get_block_metadata(footer, block_offset);

  if (metadata.subblock1_len == MT21_SUBBLOCK_SIZE) {
    memcpy(dest + block_offset, src + block_offset, metadata.subblock1_len);
  } else {
    // U and V blocks are packed back to back with no padding. The V block is
    // compressed first.
    size_t bit_idx = decompress_subblock(
        scratch_v, src + block_offset, metadata.subblock1_len, 0,
        MT21_SUBBLOCK_WIDTH / 2, MT21_SUBBLOCK_HEIGHT);
    decompress_subblock(scratch_u, src + block_offset, metadata.subblock1_len,
                        bit_idx, MT21_SUBBLOCK_WIDTH / 2, MT21_SUBBLOCK_HEIGHT);
    interleave_uv_block(dest + block_offset, scratch_u, scratch_v);
  }

  if (metadata.subblock2_len == MT21_SUBBLOCK_SIZE) {
    memcpy(dest + block_offset + MT21_SUBBLOCK_SIZE,
           src + block_offset + metadata.subblock1_len, metadata.subblock2_len);
  } else {
    size_t bit_idx = decompress_subblock(
        scratch_v, src + block_offset + metadata.subblock1_len,
        metadata.subblock2_len, 0, MT21_SUBBLOCK_WIDTH / 2,
        MT21_SUBBLOCK_HEIGHT);
    decompress_subblock(scratch_u, src + block_offset + metadata.subblock1_len,
                        metadata.subblock2_len, bit_idx,
                        MT21_SUBBLOCK_WIDTH / 2, MT21_SUBBLOCK_HEIGHT);
    interleave_uv_block(dest + block_offset + MT21_SUBBLOCK_SIZE, scratch_u,
                        scratch_v);
  }
}

static size_t compute_footer_offset(size_t plane_size,
                                    size_t buf_len,
                                    size_t alignment) {
  size_t footer_size = plane_size / MT21_BLOCK_SIZE / 2;
  return ((buf_len - footer_size) & (~(alignment - 1)));
}

static void convert_mt21_to_mm21(uint8_t* dest_y,
                                 uint8_t* dest_uv,
                                 const uint8_t* src_y,
                                 const uint8_t* src_uv,
                                 size_t width,
                                 size_t height,
                                 size_t y_buf_len,
                                 size_t uv_buf_len) {
  const size_t aligned_width = ALIGN_UP(width, MT21_TILE_WIDTH);
  const size_t aligned_height = ALIGN_UP(height, MT21_TILE_HEIGHT);
  const size_t y_size = aligned_width * aligned_height;
  const size_t uv_size = y_size / 2;
  const uint8_t* y_footer =
      src_y + compute_footer_offset(y_size, y_buf_len, MT21_Y_FOOTER_ALIGNMENT);
  const uint8_t* uv_footer =
      src_uv +
      compute_footer_offset(uv_size, uv_buf_len, MT21_UV_FOOTER_ALIGNMENT);

  for (size_t i = 0; i < y_size; i += MT21_BLOCK_SIZE) {
    decompress_y_block(dest_y, src_y, y_footer, i);
  }

  for (size_t i = 0; i < uv_size; i += MT21_BLOCK_SIZE) {
    decompress_uv_block(dest_uv, src_uv, uv_footer, i);
  }
}

// Detiles an MM21 row into NV12. See also the implementation in LibYUV.
static void detile_mm21_row(const uint8_t* src,
                            int src_tile_stride,
                            uint8_t* dest,
                            int width) {
  for (int x = 0; x < width - (MT21_TILE_WIDTH - 1); x += MT21_TILE_WIDTH) {
    memcpy(dest, src, MT21_TILE_WIDTH);
    dest += MT21_TILE_WIDTH;
    src += src_tile_stride;
  }

  if (width & (MT21_TILE_WIDTH - 1)) {
    memcpy(dest, src, width & (MT21_TILE_WIDTH - 1));
  }
}

// Convert MM21 to NV12.
static void convert_mm21_to_nv12(uint8_t* dest_y,
                                 uint8_t* dest_uv,
                                 const uint8_t* src_y,
                                 const uint8_t* src_uv,
                                 size_t width,
                                 size_t height) {
  // Convert Y plane.
  for (int y = 0; y < height; y++) {
    detile_mm21_row(src_y, MT21_TILE_SIZE, dest_y, width);
    src_y += MT21_TILE_WIDTH;
    dest_y += width;
    if ((y & (MT21_TILE_HEIGHT - 1)) == MT21_TILE_HEIGHT - 1) {
      src_y += width * MT21_TILE_HEIGHT - MT21_TILE_SIZE;
    }
  }

  for (int y = 0; y < height / 2; y++) {
    detile_mm21_row(src_uv, MT21_TILE_SIZE / 2, dest_uv, width);
    src_uv += MT21_TILE_WIDTH;
    dest_uv += width;
    if ((y & (MT21_TILE_HEIGHT / 2 - 1)) == MT21_TILE_HEIGHT / 2 - 1) {
      src_uv += width * MT21_TILE_HEIGHT / 2 - MT21_TILE_SIZE / 2;
    }
  }
}

void convert_mt21_to_nv12(uint8_t* dest_y,
                          uint8_t* dest_uv,
                          const uint8_t* src_y,
                          const uint8_t* src_uv,
                          size_t width,
                          size_t height,
                          size_t y_buf_len,
                          size_t uv_buf_len) {
  uint8_t* tmp_y;
  uint8_t* tmp_uv;
  const int aligned_width = ALIGN_UP(width, MT21_TILE_WIDTH);
  const int aligned_height = ALIGN_UP(height, MT21_TILE_HEIGHT);

  tmp_y = (uint8_t*)malloc(aligned_width * aligned_height);
  tmp_uv = (uint8_t*)malloc(aligned_width * aligned_height / 2);

  convert_mt21_to_mm21(tmp_y, tmp_uv, src_y, src_uv, aligned_width,
                       aligned_height, y_buf_len, uv_buf_len);
  convert_mm21_to_nv12(dest_y, dest_uv, tmp_y, tmp_uv, aligned_width,
                       aligned_height);

  free(tmp_y);
  free(tmp_uv);
}

// Compute minimum plane sizes when taking into account footers and alignment.
// Used for populating sizeimage fields in V4L2.
size_t compute_mt21_min_y_size(size_t width, size_t height) {
  size_t footer_size = width * height / MT21_BLOCK_SIZE / 2;

  return ((width * height + MT21_Y_FOOTER_ALIGNMENT - 1) &
          (~(MT21_Y_FOOTER_ALIGNMENT - 1))) +
         footer_size;
}

size_t compute_mt21_min_uv_size(size_t width, size_t height) {
  size_t footer_size = width * height / 2 / MT21_BLOCK_SIZE / 2;

  return ((width * height / 2 + MT21_UV_FOOTER_ALIGNMENT - 1) &
          (~(MT21_UV_FOOTER_ALIGNMENT - 1))) +
         footer_size;
}
