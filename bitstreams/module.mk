# Copyright 2023 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

include common.mk

CFLAGS += -std=gnu99 -I$(SRC)

CC_STATIC_LIBRARY(libbitstreams.pic.a): \
  bitstreams/bitfield_stream.o \
  bitstreams/bitstream_helper.o \
  bitstreams/bitstream_helper_ivf.o \
  bitstreams/bitstream_helper_h264.o \
  bitstreams/bitstream_helper_h265.o \
  bitstreams/h264_partial_parser.o \
  bitstreams/h265_partial_parser.o \
  bitstreams/nalu_parser.o \
