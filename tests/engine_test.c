#undef NDEBUG
#include "../app/src/main/cpp/engine.h"
#include "../app/src/main/cpp/field_baseline.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    DSFieldBaseline baseline = {0};
    assert(!baseline.ready && !ds_baseline_word_trigger_3d(&baseline, 90, 0, 0, false, 100));
    ds_baseline_sample_3d(&baseline, NAN, 0, 0, false, 0);
    assert(baseline.count == 0);
    for (unsigned i = 0; i < 19; ++i) ds_baseline_sample_3d(&baseline, 0, 0, 60, false, 0);
    assert(!baseline.ready);
    ds_baseline_sample_3d(&baseline, 0, 0, 60, false, 0);
    assert(baseline.ready && fabsf(baseline.field - 60.0f) < 0.001f);
    assert(baseline.armed);

    /* Signal evaluation */
    assert(ds_engine_signal(60, baseline.field) == 0);
    assert(fabsf(ds_engine_signal(70, baseline.field) - 0.5f) < 0.0001f);
    assert(ds_engine_signal(200, baseline.field) == 1);
    assert(ds_engine_signal(NAN, baseline.field) == 0);

    /* 1. Calm ambient: minor drift (delta 2 uT < threshold 8 uT) produces SILENCE */
    assert(!ds_baseline_word_trigger_3d(&baseline, 0, 0, 62, false, INT64_C(1000000000)));

    /* 2. Phone in motion: suppression locks out word triggering and clears accumulation */
    assert(!ds_baseline_word_trigger_3d(&baseline, 0, 0, 75, false, INT64_C(1050000000))); /* 1 spike sample */
    assert(baseline.spike_samples == 1);
    assert(!ds_baseline_word_trigger_3d(&baseline, 0, 0, 85, true, INT64_C(1100000000))); /* Motion clears accumulation */
    assert(baseline.spike_samples == 0 && !baseline.accumulating);

    /* 3. Event-driven anomaly spike: sustained for 2 samples above threshold, fires at peak */
    assert(!ds_baseline_word_trigger_3d(&baseline, 0, 0, 75, false, INT64_C(1200000000)));
    assert(ds_baseline_word_trigger_3d(&baseline, 0, 0, 75, false, INT64_C(1300000000)));
    assert(baseline.last_word_ns == INT64_C(1300000000));
    assert(!baseline.armed); /* Disarmed after firing */
    assert(fabsf(baseline.triggered_z - 75.0f) < 0.001f);

    /* 4. Sustained interference: static magnetic field does NOT repeat every 5 seconds */
    assert(!ds_baseline_word_trigger_3d(&baseline, 0, 0, 75, false, INT64_C(1400000000)));
    assert(!ds_baseline_word_trigger_3d(&baseline, 0, 0, 75, false, INT64_C(6300000000))); /* 5s later: still silent */
    assert(!ds_baseline_word_trigger_3d(&baseline, 0, 0, 75, false, INT64_C(15000000000))); /* 13s later: still silent */

    /* 5. Field returns to calm baseline, re-arms after cooldown */
    ds_baseline_sample_3d(&baseline, 0, 0, 60, false, INT64_C(18000000000));
    assert(baseline.armed);

    /* 6. Sensitivity levels */
    baseline.sensitivity = DS_SENSITIVITY_HIGH;
    assert(ds_baseline_threshold(baseline.sensitivity) == 4.0f);
    baseline.sensitivity = DS_SENSITIVITY_LOW;
    assert(ds_baseline_threshold(baseline.sensitivity) == 15.0f);
    baseline.sensitivity = DS_SENSITIVITY_MEDIUM;
    assert(ds_baseline_threshold(baseline.sensitivity) == 8.0f);

    ds_baseline_reset(&baseline);
    assert(!baseline.ready && baseline.count == 0 && !baseline.last_word_ns);

    /* Spectrum analysis tests */
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

    /* 7. Deterministic 3D vector word selection */
    const uint8_t dictionary[] = {
        'D','S','W','B', 1,0,0,0, 2,0,0,0,
        5,0, 'A','L','P','H','A',
        4,0, 'B','E','T','A'
    };
    char word1[32], word2[32], word3[32];
    assert(ds_engine_vector_word(NAN, 0, 0, 0, 0, 0, dictionary, sizeof(dictionary), word1, sizeof(word1)) == 0);
    assert(ds_engine_vector_word(INFINITY, 0, 0, 0, 0, 0, dictionary, sizeof(dictionary), word1, sizeof(word1)) == 0);
    assert(ds_engine_vector_word(60, 0, 0, 60, 0, 0, dictionary, sizeof(dictionary), word1, sizeof(word1)) == 0); /* 0 delta */

    /* Identical vector perturbations yield the identical word (deterministic repeatability) */
    size_t len1 = ds_engine_vector_word(15.2f, -8.3f, 4.1f, 0, 0, 0, dictionary, sizeof(dictionary), word1, sizeof(word1));
    size_t len2 = ds_engine_vector_word(15.2f, -8.3f, 4.1f, 0, 0, 0, dictionary, sizeof(dictionary), word2, sizeof(word2));
    assert(len1 > 0 && len1 == len2);
    assert(strcmp(word1, word2) == 0);

    /* Boundary vectors: extreme elevation poles and negative coordinates safely handled */
    assert(ds_engine_vector_word(0, 0, 100, 0, 0, 0, dictionary, sizeof(dictionary), word1, sizeof(word1)) > 0);
    assert(ds_engine_vector_word(0, 0, -100, 0, 0, 0, dictionary, sizeof(dictionary), word2, sizeof(word2)) > 0);

    /* Legacy wrapper test */
    assert(ds_engine_next_word(60, 999, dictionary, sizeof(dictionary), word3, sizeof(word3)) > 0);

    /* 8. Cooldown timing & sub-bass Goertzel energy via Zig engine */
    assert(ds_baseline_cooldown_ns(DS_SENSITIVITY_HIGH) == INT64_C(1500000000));
    assert(ds_baseline_cooldown_ns(DS_SENSITIVITY_MEDIUM) == INT64_C(2500000000));
    assert(ds_baseline_cooldown_ns(DS_SENSITIVITY_LOW) == INT64_C(4000000000));

    memset(samples, 0, sizeof(samples));
    assert(ds_engine_sub_bass_energy(samples, 4800, 48000) == 0.0f);
    for (unsigned i = 0; i < 4800; ++i)
        samples[i] = (int16_t)(16000 * sin(6.283185307179586 * 160.0 * i / 48000.0));
    const float sub_energy = ds_engine_sub_bass_energy(samples, 4800, 48000);
    assert(isfinite(sub_energy) && sub_energy > 20.0f);
    for (unsigned i = 0; i < 4800; ++i)
        samples[i] = (int16_t)(16000 * sin(6.283185307179586 * 1000.0 * i / 48000.0));
    const float high_energy = ds_engine_sub_bass_energy(samples, 4800, 48000);
    assert(isfinite(high_energy) && high_energy < sub_energy * 0.25f);

    /* 9. Hann-windowed dB spectrum & speech-band energy ratio */
    float hann_bins[24];
    memset(samples, 0, sizeof(samples));
    ds_engine_spectrum_hann_db(samples, 1024, 48000, hann_bins, 24);
    for (unsigned i = 0; i < 24; ++i) assert(hann_bins[i] == 0.0f);
    assert(ds_engine_speech_ratio(samples, 1024, 48000) == 0.0f);
    assert(ds_engine_speech_ratio(NULL, 1024, 48000) == 0.0f);

    for (unsigned i = 0; i < 1024; ++i)
        samples[i] = (int16_t)(16000 * sin(6.283185307179586 * 913.5 * i / 48000.0));
    ds_engine_spectrum_hann_db(samples, 1024, 48000, hann_bins, 24);
    assert(hann_bins[11] > 0.6f && hann_bins[0] < 0.05f && hann_bins[23] < 0.05f);

    for (unsigned i = 0; i < 1024; ++i)
        samples[i] = (int16_t)(16000 * sin(6.283185307179586 * 1000.0 * i / 48000.0));
    /* Hann-windowed 1024-sample 1000 Hz tone has negligible sub-bass sidelobe leakage and high speech ratio */
    assert(ds_engine_sub_bass_energy(samples, 1024, 48000) < 10.0f);
    assert(ds_engine_speech_ratio(samples, 1024, 48000) > 2.0f);

    for (unsigned i = 0; i < 1024; ++i)
        samples[i] = (int16_t)(16000 * sin(6.283185307179586 * 160.0 * i / 48000.0));
    assert(ds_engine_speech_ratio(samples, 1024, 48000) < 0.5f);

    puts("PASS: measured calibration, deviation-only word triggers, debounce, real spectrum bins and invalid-field guards");
    return 0;
}
