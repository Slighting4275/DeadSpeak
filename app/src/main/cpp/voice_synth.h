#ifndef DEADSPEAK_VOICE_SYNTH_H
#define DEADSPEAK_VOICE_SYNTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DS_VOICE_MODE_CLASSIC = 0,    /* Eerie resonant formant voice */
    DS_VOICE_MODE_WHISPER = 1,    /* Breathy spectral whisper */
    DS_VOICE_MODE_DYNAMIC = 2,    /* Dynamic EMF-anomaly modulated reactive pitch and rate */
    DS_VOICE_MODE_SPIRIT_BOX = 3  /* Resonant formant sweeps with radio static */
} DSVoiceMode;

typedef struct {
    DSVoiceMode mode;
    float pitch_hz;  /* Default ~110.0 Hz */
    float speed;     /* Default 1.0 */
    bool enabled;    /* Audio speech enabled */
} DSVoiceConfig;

#define DS_VOICE_SAMPLE_RATE 16000
#define DS_VOICE_MAX_SAMPLES ((size_t)DS_VOICE_SAMPLE_RATE * 5UL) /* Up to 5 seconds */

/* Synthesize a text word into 16-bit mono 16 kHz PCM audio buffer.
 * Returns the number of samples generated (0 if failed or disabled). */
size_t ds_voice_synthesize(const char *text, const DSVoiceConfig *config,
                           int16_t *pcm_out, size_t max_samples);

/* Apply atmospheric Schroeder comb-filter reverb and flutter echo to PCM buffer in-place. */
void ds_voice_synth_reverb(int16_t *pcm, size_t num_samples, float wet_mix);

#ifdef __cplusplus
}
#endif

#endif /* DEADSPEAK_VOICE_SYNTH_H */
