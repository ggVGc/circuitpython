// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft
//
// SPDX-License-Identifier: MIT

#pragma once

#include "extmod/vfs_fat.h"
#include "py/obj.h"

#include "shared-module/audiocore/__init__.h"

typedef struct Buffer {
  uint8_t *data;
  uint32_t length;
} Buffer;

typedef struct FileReader {
  pyb_file_obj_t *handle;
  uint32_t length;     // In bytes
  uint16_t data_start; // Where the data values start
  uint32_t bytes_remaining;

} FileReader;

typedef struct {
  mp_obj_base_t base;
  struct FileReader file;
  struct Buffer buffer1;
  struct Buffer buffer2;
  uint16_t buffer_index;

  uint32_t sample_rate;
  float stretch_factor;

} audioio_monowavefile_obj_t;

// These are not available from Python because it may be called in an interrupt.
void audioio_monowavefile_reset_buffer(audioio_monowavefile_obj_t *self,
                                       bool single_channel_output,
                                       uint8_t channel);
audioio_get_buffer_result_t
audioio_monowavefile_get_buffer(audioio_monowavefile_obj_t *self,
                                bool single_channel_output, uint8_t channel,
                                uint8_t **buffer,
                                uint32_t *buffer_length); // length in bytes
void audioio_monowavefile_get_buffer_structure(audioio_monowavefile_obj_t *self,
                                               bool single_channel_output,
                                               bool *single_buffer,
                                               bool *samples_signed,
                                               uint32_t *max_buffer_length,
                                               uint8_t *spacing);
