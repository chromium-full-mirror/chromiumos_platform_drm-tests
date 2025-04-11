# Copyright 2012 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Pull in chromium os defaults
OUT ?= $(PWD)/build-opt-local

include common.mk

# TODO: Re-enable these warnings.
CFLAGS += -Wno-implicit-fallthrough -Wno-unreachable-code

PC_DEPS = libdrm egl gbm
PC_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PC_DEPS))
PC_LIBS := $(shell $(PKG_CONFIG) --libs $(PC_DEPS))

DRM_LIBS = -lGLESv2
CFLAGS += $(PC_CFLAGS) -DEGL_EGLEXT_PROTOTYPES -DGL_GLEXT_PROTOTYPES
CFLAGS += -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE
CFLAGS += -I$(SRC)
LDLIBS += $(PC_LIBS)

all: \
	CC_BINARY(atomictest) \
	CC_BINARY(mtk_dram_tool) \
	CC_BINARY(drm_cursor_test) \
	CC_BINARY(gamma_test) \
	CC_BINARY(gbmtest) \
	CC_BINARY(linear_bo_test) \
	CC_BINARY(mali_stats) \
	CC_BINARY(mapped_access_perf_test) \
	CC_BINARY(mapped_texture_test) \
	CC_BINARY(mmap_test) \
	CC_BINARY(null_platform_test) \
	CC_BINARY(plane_test) \
	CC_BINARY(stripe) \
	CC_BINARY(swrast_test) \
	CC_BINARY(synctest) \
	CC_BINARY(udmabuf_create_test) \
	CC_BINARY(dmabuf_test) \
	CC_BINARY(v4l2_stateful_decoder) \
	CC_BINARY(v4l2_stateful_encoder) \
	CC_BINARY(yuv_to_rgb_test) \

ifeq ($(USE_VULKAN),1)
all: CC_BINARY(vk_glow)
endif

CC_BINARY(drm_cursor_test): drm_cursor_test.o CC_STATIC_LIBRARY(libbsdrm.pic.a)

CC_BINARY(mtk_dram_tool): mtk_dram_tool.o

CC_BINARY(null_platform_test): null_platform_test.o CC_STATIC_LIBRARY(libbsdrm.pic.a)
CC_BINARY(null_platform_test): LDLIBS += $(DRM_LIBS)

CC_BINARY(gbmtest): gbmtest.o CC_STATIC_LIBRARY(libbsdrm.pic.a)

CC_BINARY(dmabuf_test): dmabuf_test.o CC_STATIC_LIBRARY(libbsdrm.pic.a)
CC_BINARY(mmap_test): mmap_test.o CC_STATIC_LIBRARY(libbsdrm.pic.a)

CC_BINARY(linear_bo_test): linear_bo_test.o CC_STATIC_LIBRARY(libbsdrm.pic.a)
CC_BINARY(linear_bo_test): LDLIBS += -lGLESv2

CC_BINARY(swrast_test): swrast_test.o CC_STATIC_LIBRARY(libbsdrm.pic.a)
CC_BINARY(swrast_test): LDLIBS += -lGLESv2

CC_BINARY(atomictest): atomictest.o CC_STATIC_LIBRARY(libbsdrm.pic.a)
CC_BINARY(atomictest): CFLAGS += -DUSE_ATOMIC_API
CC_BINARY(atomictest): LDLIBS += $(DRM_LIBS)
CC_BINARY(atomictest): LDLIBS +=  -lsync -lpthread

CC_BINARY(gamma_test): gamma_test.o CC_STATIC_LIBRARY(libbsdrm.pic.a)
CC_BINARY(gamma_test): LDLIBS += -lm $(DRM_LIBS)

CC_BINARY(plane_test): plane_test.o CC_STATIC_LIBRARY(libbsdrm.pic.a)
CC_BINARY(plane_test): LDLIBS += -lm $(DRM_LIBS)

CC_BINARY(mapped_texture_test): mapped_texture_test.o CC_STATIC_LIBRARY(libbsdrm.pic.a)
CC_BINARY(mapped_texture_test): LDLIBS += -lGLESv2

CC_BINARY(synctest): synctest.o
CC_BINARY(synctest): LDLIBS += -lsync

CC_BINARY(udmabuf_create_test): udmabuf_create_test.o \
	CC_STATIC_LIBRARY(libbsdrm.pic.a)
CC_BINARY(udmabuf_create_test): LDLIBS += -lGLESv2 -lm

ifeq ($(USE_VULKAN),1)
CC_BINARY(vk_glow): vk_glow.o CC_STATIC_LIBRARY(libbsdrm.pic.a)
CC_BINARY(vk_glow): LDLIBS += -lm -lvulkan $(DRM_LIBS)
endif

v4l2_stateful_decoder.o: $(SRC)/v4l2_stateful_decoder.c
v4l2_stateful_decoder.o: $(SRC)/bitstreams/bitstream_helper.h
v4l2_stateful_decoder.o: $(SRC)/v4l2_macros.h $(SRC)/logging.h
CC_BINARY(v4l2_stateful_decoder): v4l2_stateful_decoder.o
CC_BINARY(v4l2_stateful_decoder): CC_STATIC_LIBRARY(libbitstreams.pic.a)
CC_BINARY(v4l2_stateful_decoder): CC_STATIC_LIBRARY(libbsdrm.pic.a)
CC_BINARY(v4l2_stateful_decoder): $(SRC)/pixel_formats/mt21_converter.h
CC_BINARY(v4l2_stateful_decoder): CC_STATIC_LIBRARY(libmt21.pic.a)
CC_BINARY(v4l2_stateful_decoder): $(SRC)/pixel_formats/q08c_converter.h
CC_BINARY(v4l2_stateful_decoder): CC_STATIC_LIBRARY(libq08c.pic.a)
CC_BINARY(v4l2_stateful_decoder): LDLIBS += $(DRM_LIBS)
CC_BINARY(v4l2_stateful_decoder): LDLIBS += -lssl -lcrypto

CC_BINARY(mali_stats): mali_stats.o
CC_BINARY(mali_stats): $(SRC)/logging.h $(SRC)/mali/mali_ioctl.h
CC_BINARY(mali_stats): CC_STATIC_LIBRARY(libmali.pic.a)

CC_BINARY(v4l2_stateful_encoder): v4l2_stateful_encoder.o
ifeq ($(USE_V4LPLUGIN),1)
CC_BINARY(v4l2_stateful_encoder): LDLIBS += -lv4l2
endif

CC_BINARY(mapped_access_perf_test): mapped_access_perf_test.o \
	CC_STATIC_LIBRARY(libbsdrm.pic.a)

CC_BINARY(yuv_to_rgb_test): yuv_to_rgb_test.o CC_STATIC_LIBRARY(libbsdrm.pic.a)
CC_BINARY(yuv_to_rgb_test): LDLIBS += -lGLESv2
