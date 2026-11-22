#undef NDEBUG
#include "../app/src/main/cpp/voice_player.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void write_test_wav(const char *path, uint32_t sample_rate, uint32_t num_samples) {
    FILE *f = fopen(path, "wb");
    assert(f);
    const uint32_t data_bytes = num_samples * sizeof(int16_t);
    const uint32_t riff_size = data_bytes + 36U;
    const uint16_t format = 1, channels = 1, bits = 16, block_align = 2;
    const uint32_t byte_rate = sample_rate * 2U, fmt_size = 16;
    size_t w = 0;
    w += fwrite("RIFF", 1, 4, f);
    w += fwrite(&riff_size, 4, 1, f) * 4;
    w += fwrite("WAVE", 1, 4, f);
    w += fwrite("fmt ", 1, 4, f);
    w += fwrite(&fmt_size, 4, 1, f) * 4;
    w += fwrite(&format, 2, 1, f) * 2;
    w += fwrite(&channels, 2, 1, f) * 2;
    w += fwrite(&sample_rate, 4, 1, f) * 4;
    w += fwrite(&byte_rate, 4, 1, f) * 4;
    w += fwrite(&block_align, 2, 1, f) * 2;
    w += fwrite(&bits, 2, 1, f) * 2;
    w += fwrite("data", 1, 4, f);
    w += fwrite(&data_bytes, 4, 1, f) * 4;
    assert(w == 44);
    for (uint32_t i = 0; i < num_samples; ++i) {
        int16_t sample = (int16_t)(8000.0 * sin(6.2831853 * 440.0 * (double)i / (double)sample_rate));
        size_t sw = fwrite(&sample, sizeof(int16_t), 1, f);
        assert(sw == 1);
    }
    int rc = fclose(f);
    assert(rc == 0);
}

int main(void) {
    DSVoicePlayer *p = ds_voice_player_create();
    assert(p != NULL);

    /* 1. Geiger settings */
    assert(ds_voice_player_get_geiger_enabled(p));
    ds_voice_player_set_geiger_enabled(p, false);
    assert(!ds_voice_player_get_geiger_enabled(p));
    ds_voice_player_set_geiger_enabled(p, true);
    assert(ds_voice_player_get_geiger_enabled(p));
    ds_voice_player_set_geiger_rate(p, 50.0f);
    ds_voice_player_set_geiger_rate(p, -10.0f);
    ds_voice_player_set_geiger_rate(p, 200.0f);

    /* 2. Voice synthesis */
    DSVoiceConfig config = {
        .mode = DS_VOICE_MODE_CLASSIC,
        .pitch_hz = 120.0f,
        .speed = 1.0f,
        .enabled = true
    };
    ds_voice_player_speak(p, "HELLO", &config);
    assert(ds_voice_player_is_speaking(p));
    assert(!ds_voice_player_is_speaking(NULL));
    nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 50000000L}, NULL); /* 50ms */
    ds_voice_player_stop(p);
    assert(!ds_voice_player_is_speaking(p));

    /* 3. WAV playback */
    const char *wav_path_48k = "/data/local/tmp/test_48k.wav";
    write_test_wav(wav_path_48k, 48000, 4800); /* 100ms at 48kHz */
    assert(ds_voice_player_play_wav(p, wav_path_48k));

    char name[128] = {0};
    float progress = 0.0f;
    assert(ds_voice_player_is_playing_wav(p, name, sizeof(name), &progress));
    assert(strstr(name, "test_48k.wav") != NULL);
    assert(progress >= 0.0f && progress <= 1.0f);
    assert(ds_voice_player_get_wav_duration_ms(p) >= 90); /* ~100ms */
    ds_voice_player_seek_wav_ms(p, 50);
    assert(ds_voice_player_get_wav_position_ms(p) >= 40);

    ds_voice_player_set_wav_speed(p, 0.5f);
    assert(fabsf(ds_voice_player_get_wav_speed(p) - 0.5f) < 0.01f);
    ds_voice_player_set_wav_speed(p, 5.0f);
    assert(fabsf(ds_voice_player_get_wav_speed(p) - 2.0f) < 0.01f);
    ds_voice_player_set_wav_filter(p, true);
    assert(ds_voice_player_get_wav_filter(p));
    ds_voice_player_set_wav_filter(p, false);
    assert(!ds_voice_player_get_wav_filter(p));
    ds_voice_player_set_wav_loop(p, 20, 80);
    ds_voice_player_play_cue(p, DS_CUE_START);

    ds_voice_player_stop_wav(p);
    assert(!ds_voice_player_is_playing_wav(p, NULL, 0, NULL));
    unlink(wav_path_48k);

    /* 4. Invalid WAV guards */
    assert(!ds_voice_player_play_wav(p, "/data/local/tmp/nonexistent.wav"));
    assert(!ds_voice_player_play_wav(p, ""));
    assert(!ds_voice_player_play_wav(NULL, wav_path_48k));

    /* 5. Clean teardown */
    ds_voice_player_destroy(p);
    puts("PASS: voice player stream lifecycle, geiger rate, speech trigger, WAV playback downsampling and immediate stop");
    return 0;
}
