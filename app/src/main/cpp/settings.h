#ifndef DEADSPEAK_SETTINGS_H
#define DEADSPEAK_SETTINGS_H

#include "camera_ui.h"
#include "field_baseline.h"
#include "video_capture.h"
#include "voice_synth.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    DSVoiceMode voice_mode;
    float voice_pitch_hz;
    float voice_speed;
    bool voice_enabled;
    bool geiger_enabled;
    DSVideoFilter video_filter;
    DSVideoAspect video_aspect;
    bool hud_burn_in;
    DSSensitivity sensitivity;
    uint8_t theme_mode;
    uint8_t hud_tint_step;
    bool camera_front;
    bool anomaly_fx;
    float zoom;
    uint8_t timer_seconds;
    bool emf_alerts;
    bool hpf_enabled;
    bool mute_on_vox;
    bool audio_shader;
} DSSettings;

/* Returns initialized default settings matching DeadSpeak's baseline behavior. */
DSSettings ds_settings_defaults(void);

/* Clamps and validates all enum/numeric fields in place. */
void ds_settings_clamp(DSSettings *s);

/* Atomically saves settings as key=value text (refusing symlinks). */
bool ds_settings_save(const char *path, const DSSettings *s);

/* Loads settings from path; on missing/invalid/symlink input writes defaults to *out and returns false. */
bool ds_settings_load(const char *path, DSSettings *out);

#endif /* DEADSPEAK_SETTINGS_H */
