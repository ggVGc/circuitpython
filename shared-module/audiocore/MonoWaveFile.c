// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft for Adafruit
// Industries
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/audiocore/MonoWaveFile.h"

#include <stdint.h>
#include <string.h>

#include "py/mperrno.h"
#include "py/runtime.h"

#include "shared-module/audiocore/MonoWaveFile.h"

// Simplified version of WaveFile.
// - Only supports 8bit mono files
// - Static buffer size
// - Suports adjustable playback speed, up to double speed

#define BITS_PER_SAMPLE 8
#define MAX_BUFFER_LENGTH 256

#define LOG(...) mp_printf(&mp_plat_print, __VA_ARGS__);

struct wave_format_chunk {
  uint16_t audio_format;
  uint16_t num_channels;
  uint32_t sample_rate;
  uint32_t byte_rate;
  uint16_t block_align;
  uint16_t bits_per_sample;
  uint16_t extra_params; // Assumed to be zero below.
};

void common_hal_audioio_monowavefile_set_speed(audioio_monowavefile_obj_t *self,
                                               float speed) {
  if (speed > 2) {
    speed = 2;
  }
  if (speed < 0) {
    speed = 0.1;
  }

  LOG("MonoWaveFile: Setting speed: %f\n", (double)speed);
  self->stretch_factor = 1 / speed;
}

void common_hal_audioio_monowavefile_construct(audioio_monowavefile_obj_t *self,
                                               pyb_file_obj_t *file,
                                               uint8_t *buffer,
                                               size_t _buffer_size) {
  self->stretch_factor = 1.;
  // Load the wave
  self->file.handle = file;
  uint8_t chunk_header[16];
  FIL *fp = &self->file.handle->fp;
  f_rewind(fp);
  UINT bytes_read;
  if (f_read(fp, chunk_header, 16, &bytes_read) != FR_OK) {
    mp_raise_OSError(MP_EIO);
  }
  if (bytes_read != 16 || memcmp(chunk_header, "RIFF", 4) != 0 ||
      memcmp(chunk_header + 8, "WAVEfmt ", 8) != 0) {
    mp_arg_error_invalid(MP_QSTR_file);
  }
  uint32_t format_size;
  if (f_read(fp, &format_size, 4, &bytes_read) != FR_OK) {
    mp_raise_OSError(MP_EIO);
  }
  if (bytes_read != 4 || format_size > sizeof(struct wave_format_chunk)) {
    mp_raise_ValueError(MP_ERROR_TEXT("Invalid format chunk size"));
  }
  struct wave_format_chunk format;
  if (f_read(fp, &format, format_size, &bytes_read) != FR_OK) {
    mp_raise_OSError(MP_EIO);
  }
  if (bytes_read != format_size) {
  }

  if (format.audio_format != 1 || format.num_channels > 1 ||
      format.bits_per_sample != 8 ||
      (format_size == 18 && format.extra_params != 0)) {
    mp_raise_ValueError(MP_ERROR_TEXT("Unsupported format"));
  }
  // Get the sample_rate
  self->sample_rate = format.sample_rate;

  // TODO(tannewt): Skip any extra chunks that occur before the data section.

  uint8_t data_tag[4];
  if (f_read(fp, &data_tag, 4, &bytes_read) != FR_OK) {
    mp_raise_OSError(MP_EIO);
  }
  if (bytes_read != 4 || memcmp((uint8_t *)data_tag, "data", 4) != 0) {
    mp_raise_ValueError(MP_ERROR_TEXT("Data chunk must follow fmt chunk"));
  }

  uint32_t data_length;
  if (f_read(fp, &data_length, 4, &bytes_read) != FR_OK) {
    mp_raise_OSError(MP_EIO);
  }
  if (bytes_read != 4) {
    mp_arg_error_invalid(MP_QSTR_file);
  }
  self->file.length = data_length;
  self->file.data_start = fp->fptr;

  self->buffer1.data = m_malloc(MAX_BUFFER_LENGTH);
  self->buffer1.length = MAX_BUFFER_LENGTH;
  if (self->buffer1.data == NULL) {
    common_hal_audioio_monowavefile_deinit(self);
    m_malloc_fail(MAX_BUFFER_LENGTH);
  }

  self->buffer2.data = m_malloc(MAX_BUFFER_LENGTH);
  self->buffer2.length = MAX_BUFFER_LENGTH;
  if (self->buffer2.data == NULL) {
    common_hal_audioio_monowavefile_deinit(self);
    m_malloc_fail(MAX_BUFFER_LENGTH);
  }
}

void common_hal_audioio_monowavefile_deinit(audioio_monowavefile_obj_t *self) {
  self->buffer1.data = NULL;
  self->buffer2.data = NULL;
}

bool common_hal_audioio_monowavefile_deinited(
    audioio_monowavefile_obj_t *self) {
  return self->buffer1.data == NULL;
}

uint32_t common_hal_audioio_monowavefile_get_sample_rate(
    audioio_monowavefile_obj_t *self) {
  return self->sample_rate;
}

void common_hal_audioio_monowavefile_set_sample_rate(
    audioio_monowavefile_obj_t *self, uint32_t sample_rate) {
  self->sample_rate = sample_rate;
}

uint8_t common_hal_audioio_monowavefile_get_bits_per_sample(
    audioio_monowavefile_obj_t *self) {
  return BITS_PER_SAMPLE;
}

uint8_t common_hal_audioio_monowavefile_get_channel_count(
    audioio_monowavefile_obj_t *self) {
  return 1;
}

void audioio_monowavefile_reset_buffer(audioio_monowavefile_obj_t *self,
                                       bool _single_channel_output,
                                       uint8_t _channel) {
  // We don't reset the buffer index in case we're looping and we have an odd
  // number of buffer loads
  self->file.bytes_remaining = self->file.length;
  f_lseek(&self->file.handle->fp, self->file.data_start);
}

static uint32_t add_padding(uint8_t *buffer, UINT length_read) {
  const uint32_t pad_count = length_read % sizeof(uint32_t);
  for (uint32_t i = 0; i < pad_count; i++) {
    ((uint8_t *)(buffer))[length_read / sizeof(uint8_t) - i - 1] = 0x80;
  }

  return pad_count;
}

static Buffer *get_indexed_buffer(audioio_monowavefile_obj_t *self,
                                  const uint8_t index) {
  if (index % 2 == 1) {
    return &self->buffer2;
  } else {
    return &self->buffer1;
  }
}

audioio_get_buffer_result_t audioio_monowavefile_get_buffer(
    audioio_monowavefile_obj_t *self, const bool _single_channel_output,
    uint8_t _channel, uint8_t **buffer, uint32_t *buffer_length) {

  if (self->file.bytes_remaining == 0) {
    *buffer = NULL;
    *buffer_length = 0;
    return GET_BUFFER_DONE;
  }

  struct Buffer *target_buffer = get_indexed_buffer(self, self->buffer_index);

  const uint32_t bytes_to_read =
      (MAX_BUFFER_LENGTH > self->file.bytes_remaining)
          ? self->file.bytes_remaining
          : (self->stretch_factor * MAX_BUFFER_LENGTH);

  LOG("bytes_to_read: %u\n", bytes_to_read);

  UINT read_count;
  uint8_t bytes[MAX_BUFFER_LENGTH * 2];
  const bool read_result =
      f_read(&self->file.handle->fp, bytes, bytes_to_read, &read_count);
  if (read_result != FR_OK || read_count != bytes_to_read) {
    return GET_BUFFER_ERROR;
  }

  self->file.bytes_remaining -= read_count;
  LOG("bytes remaining: %u\n", self->file.bytes_remaining);

  uint32_t target_index = 0;
  for (; target_index < MAX_BUFFER_LENGTH; ++target_index) {
    const uint32_t source_index = target_index * self->stretch_factor;
    if (source_index >= read_count) {
      break;
    }

    target_buffer->data[target_index] = bytes[source_index];
  }

  target_buffer->length = target_index;

  // Pad the last buffer to word align it.
  const bool is_last_buffer = self->file.bytes_remaining == 0 &&
                              target_buffer->length % sizeof(uint32_t) != 0;
  if (is_last_buffer) {
    LOG("Padding last buffer\n");
    target_buffer->length +=
        add_padding(target_buffer->data, target_buffer->length);
  }

  struct Buffer *out_buffer = get_indexed_buffer(self, self->buffer_index + 1);

  *buffer = out_buffer->data;
  *buffer_length = out_buffer->length;
  self->buffer_index += 1;

  LOG("Test print!\n");

  return self->file.bytes_remaining == 0 ? GET_BUFFER_DONE
                                         : GET_BUFFER_MORE_DATA;
}

void audioio_monowavefile_get_buffer_structure(audioio_monowavefile_obj_t *self,
                                               bool _single_channel_output,
                                               bool *single_buffer,
                                               bool *samples_signed,
                                               uint32_t *max_buffer_length,
                                               uint8_t *spacing) {
  *single_buffer = false;
  // In WAV files, 8-bit samples are always unsigned, and larger samples are
  // always signed.
  *samples_signed = false;
  *max_buffer_length = 512;
  *spacing = 1;
}
