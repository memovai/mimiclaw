#ifndef MP3_DECODER_H
#define MP3_DECODER_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t mp3_decoder_init(void);

esp_err_t mp3_decode(const uint8_t *mp3_data, size_t mp3_len,
                     int16_t **pcm_out, size_t *pcm_len, int *sample_rate);

#endif
