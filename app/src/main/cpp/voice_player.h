#ifndef DEADSPEAK_VOICE_PLAYER_H
#define DEADSPEAK_VOICE_PLAYER_H

#include "voice_synth.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DSVoicePlayer DSVoicePlayer;

DSVoicePlayer *ds_voice_player_create(void);
void ds_voice_player_destroy(DSVoicePlayer *p);

/* Queues a text string to be synthesized and spoken. Non-blocking. */
void ds_voice_player_speak(DSVoicePlayer *p, const char *text, const DSVoiceConfig *config);
bool ds_voice_player_is_speaking(const DSVoicePlayer *p);

/* Set acoustic Geiger EMF tick frequency (0.0 to 100.0 clicks/sec). */
void ds_voice_player_set_geiger_rate(DSVoicePlayer *p, float clicks_per_sec);
void ds_voice_player_set_geiger_enabled(DSVoicePlayer *p, bool enabled);
bool ds_voice_player_get_geiger_enabled(const DSVoicePlayer *p);

/* Play an EVP WAV file from disk. Non-blocking. */
bool ds_voice_player_play_wav(DSVoicePlayer *p, const char *wav_path);
void ds_voice_player_stop_wav(DSVoicePlayer *p);
bool ds_voice_player_is_playing_wav(const DSVoicePlayer *p, char *out_name, size_t max_name, float *out_progress);
uint64_t ds_voice_player_get_wav_position_ms(const DSVoicePlayer *p);
uint64_t ds_voice_player_get_wav_duration_ms(const DSVoicePlayer *p);
void ds_voice_player_seek_wav_ms(DSVoicePlayer *p, uint64_t position_ms);

/* Stop any currently playing audio immediately. */
void ds_voice_player_stop(DSVoicePlayer *p);

#ifdef __cplusplus
}
#endif

#endif /* DEADSPEAK_VOICE_PLAYER_H */
