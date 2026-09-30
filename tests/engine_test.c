#undef NDEBUG
#include "../app/src/main/cpp/engine.h"
#include "../app/src/main/cpp/field_baseline.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    DSFieldBaseline baseline = {0};
    assert(!baseline.ready && !ds_baseline_word_trigger(&baseline, 90, 100));
    ds_baseline_sample(&baseline, NAN);
    ds_baseline_sample(&baseline, -1);
    assert(baseline.count == 0);
    for (unsigned i = 0; i < 19; ++i) ds_baseline_sample(&baseline, 60);
    assert(!baseline.ready);
    ds_baseline_sample(&baseline, 60);
    assert(baseline.ready && baseline.field == 60);
    assert(ds_engine_signal(60, baseline.field) == 0);
    assert(fabsf(ds_engine_signal(70, baseline.field) - 0.5f) < 0.0001f);
    assert(ds_engine_signal(200, baseline.field) == 1);
    assert(ds_engine_signal(NAN, baseline.field) == 0);
    assert(!ds_baseline_word_trigger(&baseline, 62, INT64_C(1000000000)));
    assert(ds_baseline_word_trigger(&baseline, 64, INT64_C(1000000000)));
    assert(!ds_baseline_word_trigger(&baseline, 66, INT64_C(2000000000)));
    assert(!ds_baseline_word_trigger(&baseline, 66, INT64_C(500000000)));
    assert(ds_baseline_word_trigger(&baseline, 64, INT64_C(6000000000)));
    ds_baseline_reset(&baseline);
    assert(!baseline.ready && baseline.count == 0 && !baseline.last_word_ns);

    int16_t samples[4800];
    float bins[24];
    memset(samples, 0, sizeof(samples));
    ds_engine_spectrum_bins(samples, 4800, 48000, bins, 24);
    for (unsigned i = 0; i < 24; ++i) assert(bins[i] == 0);
    const unsigned expected = 12;
    const double frequency = 125 * exp(expected / 23.0 * log(64.0));
    for (unsigned i = 0; i < 4800; ++i)
        samples[i] = (int16_t)(16000 * sin(6.283185307179586 * frequency * i / 48000));
    ds_engine_spectrum_bins(samples, 4800, 48000, bins, 24);
    unsigned strongest = 0;
    for (unsigned i = 0; i < 24; ++i) {
        assert(isfinite(bins[i]) && bins[i] >= 0 && bins[i] <= 1);
        if (bins[i] > bins[strongest]) strongest = i;
    }
    assert(strongest == expected && bins[expected] > 0.2f);
    ds_engine_spectrum_bins(samples, 4800, 1000, bins, 24);
    for (unsigned i = 8; i < 24; ++i) assert(bins[i] == 0);
    const uint8_t dictionary[] = {'D','S','W','B', 1,0,0,0, 1,0,0,0, 4,0, 'T','E','S','T'};
    char word[32];
    assert(ds_engine_next_word(NAN, 1, dictionary, sizeof(dictionary), word, sizeof(word)) == 0 && !word[0]);
    assert(ds_engine_next_word(INFINITY, 1, dictionary, sizeof(dictionary), word, sizeof(word)) == 0);
    assert(ds_engine_next_word(60, 1, dictionary, sizeof(dictionary), word, sizeof(word)) == 4 && !strcmp(word, "TEST"));
    puts("PASS: measured calibration, deviation-only word triggers, debounce, real spectrum bins and invalid-field guards");
    return 0;
}
