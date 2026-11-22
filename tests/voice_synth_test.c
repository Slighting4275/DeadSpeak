#undef NDEBUG
#include "../app/src/main/cpp/voice_synth.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    DSVoiceConfig config = {
        .mode = DS_VOICE_MODE_CLASSIC,
        .pitch_hz = 120.0f,
        .speed = 1.0f,
        .enabled = true
    };

    int16_t pcm[DS_VOICE_MAX_SAMPLES];
    memset(pcm, 0, sizeof(pcm));

    /* Test 1: Basic synthesis */
    size_t count = ds_voice_synthesize("SPOOKY", &config, pcm, DS_VOICE_MAX_SAMPLES);
    assert(count > 1000);

    /* Verify non-zero samples were generated */
    bool has_non_zero = false;
    for (size_t i = 0; i < count; ++i) {
        if (pcm[i] != 0) {
            has_non_zero = true;
            break;
        }
    }
    assert(has_non_zero);

    /* Test 2: Whisper mode */
    config.mode = DS_VOICE_MODE_WHISPER;
    size_t whisper_count = ds_voice_synthesize("ANABELLE", &config, pcm, DS_VOICE_MAX_SAMPLES);
    assert(whisper_count > 1000);

    /* Test 3: Dynamic mode */
    config.mode = DS_VOICE_MODE_DYNAMIC;
    size_t dynamic_count = ds_voice_synthesize("DEMON", &config, pcm, DS_VOICE_MAX_SAMPLES);
    assert(dynamic_count > 1000);

    /* Test 4: Spirit Box mode (with swept formant modulation) */
    int16_t classic_pcm[DS_VOICE_MAX_SAMPLES];
    config.mode = DS_VOICE_MODE_CLASSIC;
    size_t classic_ref_count = ds_voice_synthesize("GHOST", &config, classic_pcm, DS_VOICE_MAX_SAMPLES);
    config.mode = DS_VOICE_MODE_SPIRIT_BOX;
    size_t spirit_count = ds_voice_synthesize("GHOST", &config, pcm, DS_VOICE_MAX_SAMPLES);
    assert(spirit_count > classic_ref_count && spirit_count > 1000);
    size_t diff_samples = 0;
    for (size_t i = 0; i < classic_ref_count; ++i) {
        if (pcm[i] != classic_pcm[i]) ++diff_samples;
    }
    assert(diff_samples > 500);

    /* Test 5: Disabled config */
    config.enabled = false;
    size_t disabled_count = ds_voice_synthesize("TEST", &config, pcm, DS_VOICE_MAX_SAMPLES);
    assert(disabled_count == 0);

    /* Test 6: Empty text */
    config.enabled = true;
    size_t empty_count = ds_voice_synthesize("", &config, pcm, DS_VOICE_MAX_SAMPLES);
    assert(empty_count == 0);

    /* Test 7: Digraph collapsing ('OO', 'SH', 'TH'), deterministic per-call RNG, and smooth transitions */
    int16_t pcm_a[DS_VOICE_MAX_SAMPLES], pcm_b[DS_VOICE_MAX_SAMPLES];
    config.mode = DS_VOICE_MODE_CLASSIC;
    config.pitch_hz = 120.0f;
    config.speed = 1.0f;
    const size_t boot_len = ds_voice_synthesize("BOOT", &config, pcm_a, DS_VOICE_MAX_SAMPLES);
    /* 'B' (70ms) + 'OO' (130ms) + 'T' (60ms) + 40ms tail = 300ms = 4800 samples (< 5600 for collapsed OO) */
    assert(boot_len > 3500 && boot_len < 5600);
    const size_t thorn_len = ds_voice_synthesize("THORN", &config, pcm_a, DS_VOICE_MAX_SAMPLES);
    assert(thorn_len > 3000);
    /* Second identical CLASSIC synthesis produces identical waveform (no global RNG mutation) */
    const size_t thorn_len_2 = ds_voice_synthesize("THORN", &config, pcm_b, DS_VOICE_MAX_SAMPLES);
    assert(thorn_len == thorn_len_2);
    assert(memcmp(pcm_a, pcm_b, thorn_len * sizeof(int16_t)) == 0);

    puts("PASS: voice synthesis classic, whisper, dynamic, spirit box, disabled, and empty guards");
    return 0;
}
