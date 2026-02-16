#ifndef MP3_ENCODER_H
#define MP3_ENCODER_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t mp3_encoder_init(void);

esp_err_t mp3_encode_pcm(const int16_t *pcm_data, size_t pcm_len, 
                         uint32_t sample_rate, uint8_t **mp3_out, size_t *mp3_len);

#endif
