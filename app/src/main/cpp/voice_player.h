#ifndef DEADSPEAK_VOICE_PLAYER_H
#define DEADSPEAK_VOICE_PLAYER_H

#include "voice_synth.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DSVoicePlayer DSVoicePlayer;

DSVoicePlayer *ds_voice_player_create(void);
void ds_voice_player_destroy(DSVoicePlayer *player);

/* Queues a text string to be synthesized and spoken. Non-blocking. */
void ds_voice_player_speak(DSVoicePlayer *player, const char *text, const DSVoiceConfig *config);

/* Set acoustic Geiger EMF tick frequency (0.0 to 100.0 clicks/sec). */
void ds_voice_player_set_geiger_rate(DSVoicePlayer *player, float clicks_per_sec);
void ds_voice_player_set_geiger_enabled(DSVoicePlayer *player, bool enabled);
bool ds_voice_player_get_geiger_enabled(const DSVoicePlayer *player);

/* Play an EVP WAV file from disk. Non-blocking. */
bool ds_voice_player_play_wav(DSVoicePlayer *player, const char *wav_path);
void ds_voice_player_stop_wav(DSVoicePlayer *player);
bool ds_voice_player_is_playing_wav(const DSVoicePlayer *player, char *out_name, size_t max_name, float *out_progress);

/* Stop any currently playing audio immediately. */
void ds_voice_player_stop(DSVoicePlayer *player);

#ifdef __cplusplus
}
#endif

#endif /* DEADSPEAK_VOICE_PLAYER_H */
