#define _POSIX_C_SOURCE 200809L
#include "settings.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

DSSettings ds_settings_defaults(void) {
    return (DSSettings){
        .voice_mode = DS_VOICE_MODE_CLASSIC,
        .voice_pitch_hz = 115.0f,
        .voice_speed = 1.0f,
        .voice_enabled = true,
        .geiger_enabled = true,
        .video_filter = DS_VIDEO_FILTER_RAW,
        .video_aspect = DS_ASPECT_16_9,
        .hud_burn_in = true,
        .sensitivity = DS_SENSITIVITY_HIGH,
        .theme_mode = 0,
        .hud_tint_step = 0,
        .camera_front = false,
        .anomaly_fx = true,
        .zoom = 1.0f,
        .timer_seconds = 0,
        .emf_alerts = true,
        .hpf_enabled = false,
        .mute_on_vox = true,
        .audio_shader = true,
        .hud_layout = DS_HUD_LAYOUT_FULL,
        .audio_visual_mode = DS_AUDIO_VIS_WAVE,
        .tutorial_seen = false,
    };
}

void ds_settings_clamp(DSSettings *s) {
    if (!s) return;
    if ((int)s->voice_mode < DS_VOICE_MODE_CLASSIC || (int)s->voice_mode > DS_VOICE_MODE_SPIRIT_BOX) {
        s->voice_mode = DS_VOICE_MODE_CLASSIC;
    }
    if (!isfinite(s->voice_pitch_hz)) s->voice_pitch_hz = 115.0f;
    else if (s->voice_pitch_hz < 60.0f) s->voice_pitch_hz = 60.0f;
    else if (s->voice_pitch_hz > 300.0f) s->voice_pitch_hz = 300.0f;

    if (!isfinite(s->voice_speed)) s->voice_speed = 1.0f;
    else if (s->voice_speed < 0.5f) s->voice_speed = 0.5f;
    else if (s->voice_speed > 2.5f) s->voice_speed = 2.5f;

    if ((int)s->video_filter < DS_VIDEO_FILTER_NIGHT || (int)s->video_filter > DS_VIDEO_FILTER_RAW) {
        s->video_filter = DS_VIDEO_FILTER_RAW;
    }
    if ((int)s->video_aspect < DS_ASPECT_16_9 || (int)s->video_aspect >= DS_ASPECT_COUNT) {
        s->video_aspect = DS_ASPECT_16_9;
    }
    if ((int)s->sensitivity < DS_SENSITIVITY_HIGH || (int)s->sensitivity > DS_SENSITIVITY_LOW) {
        s->sensitivity = DS_SENSITIVITY_HIGH;
    }
    if (s->theme_mode > 2U) s->theme_mode = 0;
    if (s->hud_tint_step > 6U) s->hud_tint_step = 6U;

    if (!isfinite(s->zoom)) s->zoom = 1.0f;
    else if (s->zoom < 1.0f) s->zoom = 1.0f;
    else if (s->zoom > 3.0f) s->zoom = 3.0f;

    if (s->timer_seconds >= 8U) s->timer_seconds = 10U;
    else if (s->timer_seconds >= 3U) s->timer_seconds = 5U;
    else s->timer_seconds = 0U;

    if ((int)s->hud_layout < DS_HUD_LAYOUT_FULL || (int)s->hud_layout >= DS_HUD_LAYOUT_COUNT) {
        s->hud_layout = DS_HUD_LAYOUT_FULL;
    }
    if ((int)s->audio_visual_mode < DS_AUDIO_VIS_WAVE || (int)s->audio_visual_mode >= DS_AUDIO_VIS_COUNT) {
        s->audio_visual_mode = DS_AUDIO_VIS_WAVE;
    }
    s->audio_shader = (s->audio_visual_mode != DS_AUDIO_VIS_OFF);
}

bool ds_settings_save(const char *path, const DSSettings *s) {
    if (!path || !path[0] || !s || strlen(path) >= 500U) return false;
    struct stat st = {0};
    if (lstat(path, &st) == 0 && S_ISLNK(st.st_mode)) return false;

    DSSettings clean = *s;
    ds_settings_clamp(&clean);

    char tmp_path[544];
    const int tlen = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld", path, (long)getpid());
    if (tlen < 0 || (size_t)tlen >= sizeof(tmp_path)) return false;
    unlink(tmp_path);

    const int fd = open(tmp_path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(tmp_path);
        return false;
    }

    const int wrote = fprintf(f,
        "version=1\n"
        "voice_mode=%d\n"
        "voice_pitch_hz=%.2f\n"
        "voice_speed=%.2f\n"
        "voice_enabled=%d\n"
        "geiger_enabled=%d\n"
        "video_filter=%d\n"
        "video_aspect=%d\n"
        "hud_burn_in=%d\n"
        "sensitivity=%d\n"
        "theme_mode=%u\n"
        "hud_tint_step=%u\n"
        "camera_front=%d\n"
        "anomaly_fx=%d\n"
        "zoom=%.2f\n"
        "timer_seconds=%u\n"
        "emf_alerts=%d\n"
        "hpf_enabled=%d\n"
        "mute_on_vox=%d\n"
        "audio_shader=%d\n"
        "hud_layout=%d\n"
        "audio_visual_mode=%d\n"
        "tutorial_seen=%d\n",
        (int)clean.voice_mode, clean.voice_pitch_hz, clean.voice_speed,
        clean.voice_enabled ? 1 : 0, clean.geiger_enabled ? 1 : 0,
        (int)clean.video_filter, (int)clean.video_aspect,
        clean.hud_burn_in ? 1 : 0, (int)clean.sensitivity,
        (unsigned)clean.theme_mode, (unsigned)clean.hud_tint_step,
        clean.camera_front ? 1 : 0, clean.anomaly_fx ? 1 : 0, clean.zoom,
        (unsigned)clean.timer_seconds, clean.emf_alerts ? 1 : 0,
        clean.hpf_enabled ? 1 : 0, clean.mute_on_vox ? 1 : 0,
        clean.audio_shader ? 1 : 0, (int)clean.hud_layout,
        (int)clean.audio_visual_mode, clean.tutorial_seen ? 1 : 0);

    const bool ok = wrote > 0 && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0 || !ok) {
        unlink(tmp_path);
        return false;
    }
    if (lstat(path, &st) == 0 && S_ISLNK(st.st_mode)) {
        unlink(tmp_path);
        return false;
    }
    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return false;
    }
    return true;
}

bool ds_settings_load(const char *path, DSSettings *out) {
    if (!out) return false;
    *out = ds_settings_defaults();
    if (!path || !path[0]) return false;

    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat st = {0};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return false;
    }
    FILE *f = fdopen(fd, "r");
    if (!f) {
        close(fd);
        return false;
    }

    char line[128];
    bool saw_vis_mode = false;
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *val = eq + 1;
        if (strcmp(line, "voice_mode") == 0) out->voice_mode = (DSVoiceMode)strtol(val, NULL, 10);
        else if (strcmp(line, "voice_pitch_hz") == 0) out->voice_pitch_hz = strtof(val, NULL);
        else if (strcmp(line, "voice_speed") == 0) out->voice_speed = strtof(val, NULL);
        else if (strcmp(line, "voice_enabled") == 0) out->voice_enabled = strtol(val, NULL, 10) != 0;
        else if (strcmp(line, "geiger_enabled") == 0) out->geiger_enabled = strtol(val, NULL, 10) != 0;
        else if (strcmp(line, "video_filter") == 0) out->video_filter = (DSVideoFilter)strtol(val, NULL, 10);
        else if (strcmp(line, "video_aspect") == 0) out->video_aspect = (DSVideoAspect)strtol(val, NULL, 10);
        else if (strcmp(line, "hud_burn_in") == 0) out->hud_burn_in = strtol(val, NULL, 10) != 0;
        else if (strcmp(line, "sensitivity") == 0) out->sensitivity = (DSSensitivity)strtol(val, NULL, 10);
        else if (strcmp(line, "theme_mode") == 0) {
            const unsigned long v = strtoul(val, NULL, 10);
            out->theme_mode = (uint8_t)(v > 255UL ? 255UL : v);
        } else if (strcmp(line, "hud_tint_step") == 0) {
            const unsigned long v = strtoul(val, NULL, 10);
            out->hud_tint_step = (uint8_t)(v > 255UL ? 255UL : v);
        }
        else if (strcmp(line, "camera_front") == 0) out->camera_front = strtol(val, NULL, 10) != 0;
        else if (strcmp(line, "anomaly_fx") == 0) out->anomaly_fx = strtol(val, NULL, 10) != 0;
        else if (strcmp(line, "zoom") == 0) out->zoom = strtof(val, NULL);
        else if (strcmp(line, "timer_seconds") == 0) {
            const unsigned long v = strtoul(val, NULL, 10);
            out->timer_seconds = (uint8_t)(v > 255UL ? 255UL : v);
        } else if (strcmp(line, "emf_alerts") == 0) out->emf_alerts = strtol(val, NULL, 10) != 0;
        else if (strcmp(line, "hpf_enabled") == 0) out->hpf_enabled = strtol(val, NULL, 10) != 0;
        else if (strcmp(line, "mute_on_vox") == 0) out->mute_on_vox = strtol(val, NULL, 10) != 0;
        else if (strcmp(line, "audio_shader") == 0) {
            out->audio_shader = strtol(val, NULL, 10) != 0;
            if (!saw_vis_mode) {
                out->audio_visual_mode = out->audio_shader ? DS_AUDIO_VIS_WAVE : DS_AUDIO_VIS_OFF;
            }
        }
        else if (strcmp(line, "hud_layout") == 0) out->hud_layout = (DSHudLayout)strtol(val, NULL, 10);
        else if (strcmp(line, "audio_visual_mode") == 0) {
            saw_vis_mode = true;
            out->audio_visual_mode = (DSAudioVisualMode)strtol(val, NULL, 10);
        } else if (strcmp(line, "tutorial_seen") == 0) out->tutorial_seen = strtol(val, NULL, 10) != 0;
    }
    fclose(f);
    ds_settings_clamp(out);
    return true;
}
