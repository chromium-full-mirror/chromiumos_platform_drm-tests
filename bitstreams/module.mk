# Copyright 2022 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

include common.mk

CFLAGS += -std=gnu99 -I$(SRC)

CC_STATIC_LIBRARY(libbitstreams.pic.a): \
  bitstreams/bitstream_helper.o \
  bitstreams/bitstream_helper_ivf.o
