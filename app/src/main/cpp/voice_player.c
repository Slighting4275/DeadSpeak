#include "voice_player.h"
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOG_TAG "DeadSpeakVoice"
#define DS_CHUNK_SAMPLES 256

struct DSVoicePlayer {
    AAudioStream *stream;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    _Atomic bool running;
    bool has_work;
    char pending_text[64];
    DSVoiceConfig pending_config;

    /* Geiger counter acoustic feedback */
    _Atomic bool geiger_enabled;
    _Atomic uint32_t geiger_rate_milli; /* clicks/sec * 1000 */
    uint32_t geiger_sample_counter;
    uint32_t geiger_interval_samples;

    /* WAV playback */
    bool wav_playing;
    char wav_path[512];
    int16_t *wav_pcm;
    size_t wav_total_samples;
    size_t wav_current_sample;
    uint32_t wav_sample_rate;
    _Atomic bool stop_requested;
};

static inline float geiger_impulse(uint32_t sample_in_click) {
    if (sample_in_click >= 16) return 0.0f;
    const float decay = expf(-0.35f * (float)sample_in_click);
    const float osc = sinf(2.0f * 3.14159265f * 2800.0f * (float)sample_in_click / (float)DS_VOICE_SAMPLE_RATE);
    return 14000.0f * decay * osc;
}

static void *voice_player_thread(void *arg) {
    DSVoicePlayer *p = (DSVoicePlayer *)arg;
    int16_t *speech_pcm = malloc(DS_VOICE_MAX_SAMPLES * sizeof(int16_t));
    int16_t chunk_pcm[DS_CHUNK_SAMPLES];
    if (!speech_pcm) return NULL;

    AAudioStreamBuilder *builder = NULL;
    if (AAudio_createStreamBuilder(&builder) == AAUDIO_OK && builder) {
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSampleRate(builder, DS_VOICE_SAMPLE_RATE);
        AAudioStreamBuilder_setChannelCount(builder, 1);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        if (AAudioStreamBuilder_openStream(builder, &p->stream) == AAUDIO_OK && p->stream) {
            AAudioStream_requestStart(p->stream);
        }
        AAudioStreamBuilder_delete(builder);
    }

    size_t speech_total = 0, speech_pos = 0;
    uint32_t click_phase = 100;

    while (atomic_load(&p->running)) {
        if (atomic_exchange(&p->stop_requested, false)) {
            speech_total = speech_pos = 0;
            click_phase = 100;
            p->geiger_sample_counter = 0;
        }

        pthread_mutex_lock(&p->mutex);
        if (p->has_work) {
            char text[64] = {0};
            DSVoiceConfig config = p->pending_config;
            strncpy(text, p->pending_text, sizeof(text) - 1);
            p->has_work = false;
            pthread_mutex_unlock(&p->mutex);

            if (text[0] && config.enabled) {
                speech_total = ds_voice_synthesize(text, &config, speech_pcm, DS_VOICE_MAX_SAMPLES);
                speech_pos = 0;
            }
        } else {
            pthread_mutex_unlock(&p->mutex);
        }

        /* Check Geiger rate */
        const bool geiger_on = atomic_load(&p->geiger_enabled);
        const uint32_t rate_milli = atomic_load(&p->geiger_rate_milli);
        uint32_t interval = 0;
        if (geiger_on && rate_milli > 0) {
            const float rate = (float)rate_milli / 1000.0f;
            interval = (uint32_t)((float)DS_VOICE_SAMPLE_RATE / rate);
            if (interval < 32) interval = 32;
        }

        /* Generate chunk */
        bool has_audio = (speech_pos < speech_total) || (p->wav_playing && p->wav_pcm) || (interval > 0);
        if (!has_audio) {
            /* Idle: wait for work with 20ms timeout */
            pthread_mutex_lock(&p->mutex);
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 20000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&p->cond, &p->mutex, &ts);
            pthread_mutex_unlock(&p->mutex);
            continue;
        }

        pthread_mutex_lock(&p->mutex);
        for (size_t i = 0; i < DS_CHUNK_SAMPLES; ++i) {
            float sample = 0.0f;

            /* Speech voice synthesis */
            if (speech_pos < speech_total) {
                sample += (float)speech_pcm[speech_pos++];
            }

            /* WAV file playback */
            if (p->wav_playing && p->wav_pcm) {
                if (p->wav_current_sample < p->wav_total_samples) {
                    sample += (float)p->wav_pcm[p->wav_current_sample++];
                } else {
                    p->wav_playing = false;
                }
            }

            /* Geiger click pulse */
            if (interval > 0) {
                if (++p->geiger_sample_counter >= interval) {
                    p->geiger_sample_counter = 0;
                    click_phase = 0;
                }
                if (click_phase < 16) {
                    sample += geiger_impulse(click_phase++);
                }
            }

            if (sample > 32000.0f) sample = 32000.0f;
            if (sample < -32000.0f) sample = -32000.0f;
            chunk_pcm[i] = (int16_t)sample;
        }
        pthread_mutex_unlock(&p->mutex);

        /* Write chunk to AAudio stream */
        if (!p->stream) {
            AAudioStreamBuilder *rebuilder = NULL;
            if (AAudio_createStreamBuilder(&rebuilder) == AAUDIO_OK && rebuilder) {
                AAudioStreamBuilder_setDirection(rebuilder, AAUDIO_DIRECTION_OUTPUT);
                AAudioStreamBuilder_setSampleRate(rebuilder, DS_VOICE_SAMPLE_RATE);
                AAudioStreamBuilder_setChannelCount(rebuilder, 1);
                AAudioStreamBuilder_setFormat(rebuilder, AAUDIO_FORMAT_PCM_I16);
                AAudioStreamBuilder_setPerformanceMode(rebuilder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
                if (AAudioStreamBuilder_openStream(rebuilder, &p->stream) == AAUDIO_OK && p->stream) {
                    AAudioStream_requestStart(p->stream);
                }
                AAudioStreamBuilder_delete(rebuilder);
            }
        }
        if (p->stream) {
            size_t written = 0;
            while (written < DS_CHUNK_SAMPLES && atomic_load(&p->running)) {
                const aaudio_result_t res = AAudioStream_write(
                    p->stream, chunk_pcm + written, (int32_t)(DS_CHUNK_SAMPLES - written), 20000000LL);
                if (res < 0) {
                    AAudioStream_close(p->stream);
                    p->stream = NULL;
                    break;
                }
                written += (size_t)res;
            }
        }
        if (!p->stream) {
            /* Throttle thread to real-time chunk interval if audio output is unavailable */
            usleep(16000);
        }
    }

    if (p->stream) {
        AAudioStream_requestStop(p->stream);
        AAudioStream_close(p->stream);
        p->stream = NULL;
    }
    if (p->wav_pcm) {
        free(p->wav_pcm);
        p->wav_pcm = NULL;
    }
    free(speech_pcm);
    return NULL;
}

DSVoicePlayer *ds_voice_player_create(void) {
    DSVoicePlayer *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    pthread_mutex_init(&p->mutex, NULL);
    pthread_cond_init(&p->cond, NULL);
    atomic_store(&p->running, true);
    atomic_store(&p->geiger_enabled, true);
    atomic_store(&p->geiger_rate_milli, 0);
    if (pthread_create(&p->thread, NULL, voice_player_thread, p) != 0) {
        atomic_store(&p->running, false);
        pthread_mutex_destroy(&p->mutex);
        pthread_cond_destroy(&p->cond);
        free(p);
        return NULL;
    }
    return p;
}

void ds_voice_player_destroy(DSVoicePlayer *p) {
    if (!p) return;
    atomic_store(&p->running, false);
    pthread_mutex_lock(&p->mutex);
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->mutex);
    pthread_join(p->thread, NULL);
    pthread_mutex_destroy(&p->mutex);
    pthread_cond_destroy(&p->cond);
    if (p->wav_pcm) free(p->wav_pcm);
    free(p);
}

void ds_voice_player_speak(DSVoicePlayer *p, const char *text, const DSVoiceConfig *config) {
    if (!p || !text || !text[0]) return;
    pthread_mutex_lock(&p->mutex);
    strncpy(p->pending_text, text, sizeof(p->pending_text) - 1);
    p->pending_text[sizeof(p->pending_text) - 1] = '\0';
    p->pending_config = config ? *config : (DSVoiceConfig){.enabled = true, .mode = DS_VOICE_MODE_CLASSIC, .pitch_hz = 115.0f, .speed = 1.0f};
    p->has_work = true;
    pthread_cond_signal(&p->cond);
    pthread_mutex_unlock(&p->mutex);
}

void ds_voice_player_set_geiger_rate(DSVoicePlayer *p, float clicks_per_sec) {
    if (!p) return;
    if (clicks_per_sec < 0.0f) clicks_per_sec = 0.0f;
    if (clicks_per_sec > 100.0f) clicks_per_sec = 100.0f;
    const uint32_t milli = (uint32_t)(clicks_per_sec * 1000.0f);
    atomic_store(&p->geiger_rate_milli, milli);
    if (milli > 0) {
        pthread_mutex_lock(&p->mutex);
        pthread_cond_signal(&p->cond);
        pthread_mutex_unlock(&p->mutex);
    }
}

void ds_voice_player_set_geiger_enabled(DSVoicePlayer *p, bool enabled) {
    if (p) atomic_store(&p->geiger_enabled, enabled);
}

bool ds_voice_player_get_geiger_enabled(const DSVoicePlayer *p) {
    return p ? atomic_load(&p->geiger_enabled) : false;
}

bool ds_voice_player_play_wav(DSVoicePlayer *p, const char *wav_path) {
    if (!p || !wav_path || !wav_path[0]) return false;
    FILE *f = fopen(wav_path, "rb");
    if (!f) return false;

    uint8_t header[44];
    if (fread(header, 1, 44, f) != 44 || memcmp(header, "RIFF", 4) != 0 ||
        memcmp(header + 8, "WAVE", 4) != 0 || memcmp(header + 12, "fmt ", 4) != 0 ||
        memcmp(header + 36, "data", 4) != 0) {
        fclose(f);
        return false;
    }

    uint32_t sample_rate = 0, data_bytes = 0;
    memcpy(&sample_rate, header + 24, sizeof(sample_rate));
    memcpy(&data_bytes, header + 40, sizeof(data_bytes));
    if (sample_rate == 0 || data_bytes == 0 || data_bytes > 30 * 1024 * 1024) {
        fclose(f);
        return false;
    }

    const size_t num_samples = data_bytes / sizeof(int16_t);
    int16_t *buf = malloc(num_samples * sizeof(int16_t));
    if (!buf) {
        fclose(f);
        return false;
    }

    if (fread(buf, sizeof(int16_t), num_samples, f) != num_samples) {
        free(buf);
        fclose(f);
        return false;
    }
    fclose(f);

    /* If original WAV is 48kHz and playback is 16kHz, downsample 3:1 */
    int16_t *final_buf = buf;
    size_t final_samples = num_samples;
    if (sample_rate == 48000) {
        const size_t downsampled = num_samples / 3;
        int16_t *resampled = malloc(downsampled * sizeof(int16_t));
        if (resampled) {
            for (size_t i = 0; i < downsampled; ++i) {
                resampled[i] = buf[i * 3];
            }
            free(buf);
            final_buf = resampled;
            final_samples = downsampled;
        }
    }

    pthread_mutex_lock(&p->mutex);
    if (p->wav_pcm) free(p->wav_pcm);
    p->wav_pcm = final_buf;
    p->wav_total_samples = final_samples;
    p->wav_current_sample = 0;
    p->wav_sample_rate = sample_rate;
    p->wav_playing = true;
    strncpy(p->wav_path, wav_path, sizeof(p->wav_path) - 1);
    p->wav_path[sizeof(p->wav_path) - 1] = '\0';
    pthread_cond_signal(&p->cond);
    pthread_mutex_unlock(&p->mutex);

    return true;
}

void ds_voice_player_stop_wav(DSVoicePlayer *p) {
    if (!p) return;
    pthread_mutex_lock(&p->mutex);
    p->wav_playing = false;
    p->wav_current_sample = 0;
    pthread_mutex_unlock(&p->mutex);
}

bool ds_voice_player_is_playing_wav(const DSVoicePlayer *p, char *out_name, size_t max_name, float *out_progress) {
    if (!p) return false;
    DSVoicePlayer *mutable_p = (DSVoicePlayer *)p;
    pthread_mutex_lock(&mutable_p->mutex);
    const bool playing = mutable_p->wav_playing;
    if (out_name && max_name > 0) {
        const char *slash = strrchr(mutable_p->wav_path, '/');
        const char *name = slash ? slash + 1 : mutable_p->wav_path;
        strncpy(out_name, name, max_name - 1);
        out_name[max_name - 1] = '\0';
    }
    if (out_progress) {
        *out_progress = (mutable_p->wav_total_samples > 0)
                        ? ((float)mutable_p->wav_current_sample / (float)mutable_p->wav_total_samples)
                        : 0.0f;
    }
    pthread_mutex_unlock(&mutable_p->mutex);
    return playing;
}

void ds_voice_player_stop(DSVoicePlayer *p) {
    if (!p) return;
    atomic_store(&p->stop_requested, true);
    atomic_store(&p->geiger_rate_milli, 0);
    pthread_mutex_lock(&p->mutex);
    p->has_work = false;
    p->pending_text[0] = '\0';
    p->wav_playing = false;
    p->wav_current_sample = 0;
    pthread_cond_signal(&p->cond);
    pthread_mutex_unlock(&p->mutex);
}
