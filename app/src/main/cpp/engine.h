#ifndef DEADSPEAK_ENGINE_H
#define DEADSPEAK_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

float ds_engine_smooth(float previous, float current, float alpha);
float ds_engine_signal(float magnetic_field, float measured_baseline);
void ds_engine_spectrum(const int16_t *samples, size_t count, uint32_t sample_rate, float bands[3]);
void ds_engine_spectrum_bins(const int16_t *samples, size_t count, uint32_t sample_rate, float *bins, size_t bin_count);
float ds_engine_sub_bass_energy(const int16_t *samples, size_t count, uint32_t sample_rate);
size_t ds_engine_vector_word(float bx, float by, float bz,
                             float base_x, float base_y, float base_z,
                             const uint8_t *dictionary, size_t dictionary_size,
                             char *out, size_t capacity);
size_t ds_engine_next_word(float magnetic_field, uint32_t sample, const uint8_t *dictionary,
                           size_t dictionary_size, char *out, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
