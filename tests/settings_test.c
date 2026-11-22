#define _POSIX_C_SOURCE 200809L
#undef NDEBUG
#include "../app/src/main/cpp/settings.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    /* 1. Defaults & sanitization */
    DSSettings s = ds_settings_defaults();
    assert(s.voice_mode == DS_VOICE_MODE_CLASSIC);
    assert(s.voice_enabled == true);
    assert(fabsf(s.voice_pitch_hz - 115.0f) < 0.01f);
    assert(fabsf(s.voice_speed - 1.0f) < 0.01f);
    assert(s.geiger_enabled == true);
    assert(s.video_filter == DS_VIDEO_FILTER_RAW);
    assert(s.video_aspect == DS_ASPECT_16_9);
    assert(s.hud_burn_in == true);
    assert(s.sensitivity == DS_SENSITIVITY_HIGH);
    assert(s.theme_mode == 0);
    assert(s.hud_tint_step == 0);
    assert(s.camera_front == false);
    assert(s.anomaly_fx == true);
    assert(fabsf(s.zoom - 1.0f) < 0.01f);

    assert(s.emf_alerts == true);
    assert(s.hud_layout == DS_HUD_LAYOUT_FULL);
    assert(s.audio_visual_mode == DS_AUDIO_VIS_WAVE);
    assert(s.tutorial_seen == false);

    /* 2. Clamp invalid / out-of-range fields safely */
    DSSettings dirty = {
        .voice_mode = (DSVoiceMode)99,
        .voice_pitch_hz = 999.0f,
        .voice_speed = NAN,
        .voice_enabled = false,
        .geiger_enabled = false,
        .video_filter = (DSVideoFilter)77,
        .video_aspect = (DSVideoAspect)42,
        .hud_burn_in = false,
        .sensitivity = (DSSensitivity)11,
        .theme_mode = 99,
        .hud_tint_step = 42,
        .camera_front = true,
        .anomaly_fx = false,
        .zoom = 12.0f,
        .hud_layout = (DSHudLayout)99,
        .audio_visual_mode = (DSAudioVisualMode)99,
    };
    ds_settings_clamp(&dirty);
    assert(dirty.voice_mode == DS_VOICE_MODE_CLASSIC);
    assert(dirty.voice_pitch_hz == 300.0f);
    assert(dirty.voice_speed == 1.0f);
    assert(dirty.video_filter == DS_VIDEO_FILTER_RAW);
    assert(dirty.video_aspect == DS_ASPECT_16_9);
    assert(dirty.sensitivity == DS_SENSITIVITY_HIGH);
    assert(dirty.theme_mode == 0);
    assert(dirty.hud_tint_step == 6);
    assert(dirty.camera_front == true);
    assert(dirty.anomaly_fx == false);
    assert(dirty.zoom == 3.0f);
    assert(dirty.hud_layout == DS_HUD_LAYOUT_FULL);
    assert(dirty.audio_visual_mode == DS_AUDIO_VIS_WAVE);

    /* 3. Round-trip atomic save & load, missing-file & null guards */
    assert(!ds_settings_save(NULL, &s));
    assert(!ds_settings_save("", &s));
    assert(!ds_settings_save("/nonexistent", NULL));

    char path[256] = "build/host/settings_test_cfg.tmp";
    unlink(path);

    DSSettings missing = {0};
    assert(!ds_settings_load(path, &missing));
    assert(missing.voice_enabled == true && fabsf(missing.voice_pitch_hz - 115.0f) < 0.01f);

    DSSettings custom = ds_settings_defaults();
    custom.voice_mode = DS_VOICE_MODE_SPIRIT_BOX;
    custom.voice_pitch_hz = 160.0f;
    custom.voice_speed = 1.4f;
    custom.voice_enabled = false;
    custom.geiger_enabled = false;
    custom.video_filter = DS_VIDEO_FILTER_THERMAL;
    custom.video_aspect = DS_ASPECT_4_5;
    custom.hud_burn_in = false;
    custom.sensitivity = DS_SENSITIVITY_LOW;
    custom.theme_mode = 2;
    custom.hud_tint_step = 4;
    custom.camera_front = true;
    custom.anomaly_fx = false;
    custom.zoom = 2.0f;
    custom.timer_seconds = 10;
    custom.emf_alerts = false;
    custom.hpf_enabled = true;
    custom.mute_on_vox = false;
    custom.audio_shader = false;
    custom.hud_layout = DS_HUD_LAYOUT_COMPACT;
    custom.audio_visual_mode = DS_AUDIO_VIS_RIBBON;
    custom.tutorial_seen = true;

    assert(ds_settings_save(path, &custom));

    DSSettings loaded = {0};
    assert(ds_settings_load(path, &loaded));
    assert(loaded.voice_mode == DS_VOICE_MODE_SPIRIT_BOX);
    assert(fabsf(loaded.voice_pitch_hz - 160.0f) < 0.01f);
    assert(fabsf(loaded.voice_speed - 1.4f) < 0.01f);
    assert(loaded.voice_enabled == false);
    assert(loaded.geiger_enabled == false);
    assert(loaded.video_filter == DS_VIDEO_FILTER_THERMAL);
    assert(loaded.video_aspect == DS_ASPECT_4_5);
    assert(loaded.hud_burn_in == false);
    assert(loaded.sensitivity == DS_SENSITIVITY_LOW);
    assert(loaded.theme_mode == 2);
    assert(loaded.hud_tint_step == 4);
    assert(loaded.camera_front == true);
    assert(loaded.anomaly_fx == false);
    assert(fabsf(loaded.zoom - 2.0f) < 0.01f);
    assert(loaded.timer_seconds == 10);
    assert(loaded.emf_alerts == false);
    assert(loaded.hpf_enabled == true);
    assert(loaded.mute_on_vox == false);
    assert(loaded.audio_shader == true);
    assert(loaded.hud_layout == DS_HUD_LAYOUT_COMPACT);
    assert(loaded.audio_visual_mode == DS_AUDIO_VIS_RIBBON);
    assert(loaded.tutorial_seen == true);

    /* Legacy audio_shader=0 without audio_visual_mode key maps to DS_AUDIO_VIS_OFF */
    const char *legacy_path = "build/host/settings_legacy_audio.tmp";
    unlink(legacy_path);
    FILE *lf = fopen(legacy_path, "w");
    assert(lf != NULL);
    fputs("audio_shader=0\n", lf);
    fclose(lf);
    DSSettings legacy = {0};
    assert(ds_settings_load(legacy_path, &legacy));
    assert(legacy.audio_visual_mode == DS_AUDIO_VIS_OFF && !legacy.audio_shader);
    unlink(legacy_path);

    /* 4. Out-of-range uint8 parsing does not wrap modulo 256 */
    const char *overflow_path = "build/host/settings_test_overflow.tmp";
    unlink(overflow_path);
    FILE *of = fopen(overflow_path, "w");
    assert(of != NULL);
    fputs("theme_mode=257\nhud_tint_step=300\n", of);
    fclose(of);
    DSSettings overflow = ds_settings_defaults();
    assert(ds_settings_load(overflow_path, &overflow));
    assert(overflow.theme_mode == 0);
    assert(overflow.hud_tint_step == 6);
    unlink(overflow_path);

    /* 5. Symlink refusal */
    const char *link_path = "build/host/settings_test_symlink.tmp";
    unlink(link_path);
    assert(symlink("settings_test_cfg.tmp", link_path) == 0);
    assert(!ds_settings_save(link_path, &custom));
    assert(!ds_settings_load(link_path, &loaded));

    unlink(link_path);
    unlink(path);
    puts("PASS: persistent settings defaults, clamping, round-trip persistence, and symlink rejection");
    return 0;
}
