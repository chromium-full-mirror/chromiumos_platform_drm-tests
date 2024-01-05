# Copyright 2023 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

include common.mk

CFLAGS += -std=gnu99 -I$(SRC)

CC_STATIC_LIBRARY(libmt21.pic.a): \
 pixel_formats/mt21_converter.o

CC_STATIC_LIBRARY(libq08c.pic.a): \
 pixel_formats/q08c_converter.o
