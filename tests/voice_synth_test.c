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

    /* Test 3: Spirit Box mode */
    config.mode = DS_VOICE_MODE_SPIRIT_BOX;
    size_t spirit_count = ds_voice_synthesize("GHOST", &config, pcm, DS_VOICE_MAX_SAMPLES);
    assert(spirit_count > 1000);

    /* Test 4: Disabled config */
    config.enabled = false;
    size_t disabled_count = ds_voice_synthesize("TEST", &config, pcm, DS_VOICE_MAX_SAMPLES);
    assert(disabled_count == 0);

    /* Test 5: Empty text */
    config.enabled = true;
    size_t empty_count = ds_voice_synthesize("", &config, pcm, DS_VOICE_MAX_SAMPLES);
    assert(empty_count == 0);

    puts("PASS: voice synthesis classic, whisper, spirit box, disabled, and empty guards");
    return 0;
}
