/*
 * Copyright 2022 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "logging.h"

// Constants are taken from
// https://chromium-review.googlesource.com/c/chromiumos/third_party/kernel/+/323975

// Control register for EMI bus monitor.
#define EMI_BMEN 0x400
// Counter for number of accesses. Unit is 8 bytes.
#define EMI_WACT 0x420

// The memory address of the EMI registers is the same for everything after the
// MT8183.
// TODO(greenjustin): Add support for Elm and Hana.
#define EMI_ADDR_MT8183 0x10219000

// Flag masks for the control register.
// BUS_MONITOR_ENABLE controls whether or not the bus monitor is enabled.
// Notable, it also clears the EMI_WACT register when it's set to false, or at
// least it's supposed to. BUS_MONITOR_PAUSE simple pauses the count without
// clearing it. BUS_MONITOR_PAUSE is sometimes in an unexpected state, so it's
// always best to clear it when setting or clearing BUS_MONITOR_ENABLE.
#define BUS_MONITOR_ENABLE 1
#define BUS_MONITOR_PAUSE 2

#define EMI_REG_LEN 4096
#define EMI_ACCESS_UNIT_SIZE 8

#define MICROSECONDS_IN_MILLISECOND 1000

static const char* compatible_devices[4] = {
    "mediatek,mt8183", "mediatek,mt8186", "mediatek,mt8192", "mediatek,mt8195"};

int mem_fd = -1;
volatile void* emi_registers = NULL;

// This variable is never actually read from, it's just used to guarantee the
// compiler won't optimize out the read32 we use to flush writes.
volatile uint32_t discard = 0;

int64_t elapsed_time_us(struct timespec start, struct timespec end) {
  return ((int64_t)end.tv_sec - (int64_t)start.tv_sec) * 1000000 +
         ((int64_t)end.tv_nsec - (int64_t)start.tv_nsec) / 1000;
}

bool check_compatibility() {
  char compatible_string[512];
  int compatible_string_len = -1;
  int i = 0;
  int device_tree_fd = open("/proc/device-tree/compatible", O_RDONLY);

  if (device_tree_fd < 0)
    return false;

  compatible_string_len =
      read(device_tree_fd, compatible_string, sizeof(compatible_string));
  if (compatible_string_len < 0)
    return false;

  while (i < compatible_string_len) {
    for (int j = 0;
         j < sizeof(compatible_devices) / sizeof(compatible_devices[0]); j++) {
      if (!strncmp(compatible_string + i, compatible_devices[j],
                   strlen(compatible_devices[j])))
        return true;
    }
    i += strlen(compatible_string + i) + 1;
  }

  return false;
}

void init() {
  if (!check_compatibility()) {
    printf("Error! Incompatible device!\n");
    printf(
        "This program currently only supports MT8183, MT8186, MT8192, and "
        "MT8195.\n");
    exit(EXIT_FAILURE);
  }

  mem_fd = open("/dev/mem", O_RDWR);
  if (mem_fd < 0)
    LOG_FATAL("Error opening /dev/mem! %s\n", strerror(errno));

  emi_registers = mmap(NULL, EMI_REG_LEN, PROT_READ | PROT_WRITE, MAP_SHARED,
                       mem_fd, EMI_ADDR_MT8183);

  if (!emi_registers)
    LOG_FATAL("Error mapping /dev/mem! %s\n", strerror(errno));
}

void cleanup() {
  munmap((void*)emi_registers, EMI_REG_LEN);
  close(mem_fd);
}

uint32_t read32(uint32_t offset) {
  return *(uint32_t*)(emi_registers + offset);
}

void write32(uint32_t offset, uint32_t val) {
  *(uint32_t*)(emi_registers + offset) = val;

#pragma clang optimize off
  // Writes don't immediately flush with /dev/mem, so we have to read back to
  // force a flush.
  discard = read32(offset);
#pragma clang optimize on
}

void pause_bus_monitor() {
  uint32_t val = read32(EMI_BMEN);
  val = val | BUS_MONITOR_PAUSE;
  write32(EMI_BMEN, val);
}

uint32_t get_word_counter() {
  return read32(EMI_WACT);
}

void disable_bus_monitor() {
  uint32_t val = read32(EMI_BMEN);
  write32(EMI_BMEN, val & ~(BUS_MONITOR_PAUSE | BUS_MONITOR_ENABLE));
}

void enable_bus_monitor() {
  uint32_t val = read32(EMI_BMEN);
  write32(EMI_BMEN, (val & (~BUS_MONITOR_PAUSE)) | BUS_MONITOR_ENABLE);
}

void start_bus_monitor() {
  disable_bus_monitor();

  uint32_t val = get_word_counter();

  // Disabling the bandwidth monitor is supposed to clear the counters, but for
  // some reason this is sticky even with the fencing instructions. The android
  // driver gets around this by just enabling and disabling the counters up to
  // 100 times and checking the values, although anecdotally it looks like this
  // number should be closer to 1000.
  int retry_count = 1000;
  while (val && retry_count--) {
    enable_bus_monitor();
    disable_bus_monitor();
    val = get_word_counter();
  }

  if (!retry_count)
    LOG_FATAL("Error! Could not reset bus monitor!\n");

  enable_bus_monitor();
}

void print_help() {
  printf("dram_tool\n");
  printf("A simple program used for querying current DRAM bandwidth usage.\n");
  printf("dram_tool prints out the current DRAM bandwidth usage in bytes \n");
  printf("per second.\n");
  printf("Usage: dram_tool [-l measure_time_in_milliseconds]\n");
  printf("-l: Run measurement for the given number of milliseconds.\n");
  printf("    Default is 1000ms.\n");
}

int main(int argc, char** argv) {
  int measure_time_ms = 1000;

  int c;
  while ((c = getopt(argc, argv, "hl:")) != -1) {
    switch (c) {
      case 'l':
        measure_time_ms = atoi(optarg);
        assert(measure_time_ms > 0);
        break;
      case 'h':
        print_help();
        exit(0);
      default:
        LOG_ERROR("Error! Unrecognized option %c.\n", c);
        print_help();
        exit(EXIT_FAILURE);
    }
  }

  init();

  // Sample the bandwidth counters at 1KHz. They're only 32 bit, so they
  // overflow pretty easily, which is why we sample so fast.
  double avg_bandwidth_usage = 0.0;
  struct timespec start;
  struct timespec end;
  for (int i = 0; i < measure_time_ms; i++) {
    start_bus_monitor();
    clock_gettime(CLOCK_MONOTONIC, &start);
    usleep(MICROSECONDS_IN_MILLISECOND);
    pause_bus_monitor();
    clock_gettime(CLOCK_MONOTONIC, &end);
    uint32_t word_count = get_word_counter();
    avg_bandwidth_usage += (double)word_count * EMI_ACCESS_UNIT_SIZE *
                           1000000.0 / ((double)elapsed_time_us(start, end));
  }
  avg_bandwidth_usage = avg_bandwidth_usage / ((double)measure_time_ms);

  printf("%f B/s\n", avg_bandwidth_usage);

  cleanup();

  return 0;
}
