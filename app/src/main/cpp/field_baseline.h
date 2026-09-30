#ifndef DEADSPEAK_FIELD_BASELINE_H
#define DEADSPEAK_FIELD_BASELINE_H
#include <stdbool.h>
#include <stdint.h>
#include <math.h>

#define DS_BASELINE_SAMPLES 20U
typedef struct {
    double sum;
    unsigned count;
    float field;
    bool ready;
    int64_t last_word_ns;
} DSFieldBaseline;

static inline void ds_baseline_reset(DSFieldBaseline *baseline) {
    *baseline = (DSFieldBaseline){0};
}
static inline void ds_baseline_sample(DSFieldBaseline *baseline, float field) {
    if (baseline->ready || !isfinite(field) || field < 0 || field > 1000000) return;
    baseline->sum += field;
    if (++baseline->count == DS_BASELINE_SAMPLES) {
        baseline->field = (float)(baseline->sum / DS_BASELINE_SAMPLES);
        baseline->ready = true;
    }
}
static inline bool ds_baseline_word_trigger(DSFieldBaseline *baseline, float field, int64_t timestamp_ns) {
    if (!baseline->ready || !isfinite(field) || timestamp_ns <= 0 ||
        fabsf(field - baseline->field) < 3.0f ||
        (baseline->last_word_ns && (timestamp_ns <= baseline->last_word_ns ||
         timestamp_ns - baseline->last_word_ns < INT64_C(5000000000)))) return false;
    baseline->last_word_ns = timestamp_ns;
    return true;
}
#endif
