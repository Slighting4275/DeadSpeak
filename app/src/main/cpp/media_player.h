#ifndef DEADSPEAK_MEDIA_PLAYER_H
#define DEADSPEAK_MEDIA_PLAYER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct DSMediaPlayer DSMediaPlayer;

/* Pure helpers (host-testable) */
static inline uint64_t ds_media_clamp_seek_ms(uint64_t target_ms, uint64_t duration_ms) {
    return (duration_ms > 0 && target_ms > duration_ms) ? duration_ms : target_ms;
}

static inline float ds_media_progress(uint64_t pos_ms, uint64_t duration_ms) {
    if (duration_ms == 0) return 0.0f;
    if (pos_ms >= duration_ms) return 1.0f;
    return (float)pos_ms / (float)duration_ms;
}

DSMediaPlayer *ds_media_player_create(void);
void ds_media_player_destroy(DSMediaPlayer *mp);

/* Start non-blocking MP4 video+audio playback from local file path. */
bool ds_media_player_open(DSMediaPlayer *mp, const char *mp4_path);
void ds_media_player_stop(DSMediaPlayer *mp);
void ds_media_player_seek_ms(DSMediaPlayer *mp, uint64_t position_ms);
void ds_media_player_set_paused(DSMediaPlayer *mp, bool paused);
bool ds_media_player_is_paused(const DSMediaPlayer *mp);

bool ds_media_player_is_playing(const DSMediaPlayer *mp, char *out_name, size_t max_name, float *out_progress);
uint64_t ds_media_player_get_position_ms(const DSMediaPlayer *mp);
uint64_t ds_media_player_get_duration_ms(const DSMediaPlayer *mp);
int ds_media_player_rotation(const DSMediaPlayer *mp);

/* Copies newly decoded video frame into RGBA buffer (<=640x480*4). */
bool ds_media_player_copy_frame(DSMediaPlayer *mp, uint8_t *rgba, size_t capacity,
                                int *out_width, int *out_height);

#endif /* DEADSPEAK_MEDIA_PLAYER_H */
