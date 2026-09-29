#ifndef DEADSPEAK_ENGINE_H
#define DEADSPEAK_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

float ds_engine_smooth(float previous, float current, float alpha);
float ds_engine_signal(float magnetic_field);
void ds_engine_spectrum(const int16_t *samples, size_t count, uint32_t sample_rate, float bands[3]);
size_t ds_engine_next_word(float magnetic_field, uint32_t sample, const uint8_t *dictionary,
                           size_t dictionary_size, char *out, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
