#include "voice_synth.h"
#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    float f1, f2, f3;       /* Formant frequencies in Hz */
    float f1_end, f2_end, f3_end; /* Optional diphthong end targets (0 = steady-state) */
    float bw1, bw2, bw3;    /* Bandwidths in Hz */
    float gain1, gain2, gain3;
    float duration_sec;
    bool voiced;
    float noise_mix;        /* 0.0 to 1.0 */
} DSPhoneme;

/* Biquad resonator filter state */
typedef struct {
    float x1, x2;
    float y1, y2;
    float b0, b1, b2;
    float a1, a2;
} DSResonator;

static void resonator_set_coeffs(DSResonator *r, float freq, float bw, float sample_rate) {
    if (freq <= 0.0f || bw <= 0.0f || freq >= sample_rate * 0.48f) {
        r->b0 = r->b1 = r->b2 = r->a1 = r->a2 = 0.0f;
        return;
    }
    const float c = -expf(-2.0f * (float)M_PI * bw / sample_rate);
    const float b = 2.0f * expf(-(float)M_PI * bw / sample_rate) * cosf(2.0f * (float)M_PI * freq / sample_rate);
    const float a = 1.0f - b - c;
    r->b0 = a;
    r->b1 = 0.0f;
    r->b2 = 0.0f;
    r->a1 = -b;
    r->a2 = -c;
}

static void resonator_init(DSResonator *r, float freq, float bw, float sample_rate) {
    r->x1 = r->x2 = r->y1 = r->y2 = 0.0f;
    resonator_set_coeffs(r, freq, bw, sample_rate);
}

static inline float resonator_process(DSResonator *r, float in) {
    const float out = r->b0 * in - r->a1 * r->y1 - r->a2 * r->y2;
    r->y2 = r->y1;
    r->y1 = out;
    return out;
}

static inline float next_noise_rng(uint32_t *rng) {
    *rng = (*rng) * 1664525u + 1013904223u;
    return ((float)(int32_t)(*rng) / 2147483648.0f);
}

static inline float next_unit_random_rng(uint32_t *rng) {
    const float u = next_noise_rng(rng) * 0.5f + 0.5f;
    return u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
}

static DSPhoneme get_phoneme_for_char(char c) {
    c = (char)toupper((unsigned char)c);
    DSPhoneme p = {0};
    p.bw1 = 90.0f;
    p.bw2 = 110.0f;
    p.bw3 = 140.0f;
    p.gain1 = 1.0f;
    p.gain2 = 0.6f;
    p.gain3 = 0.3f;
    p.duration_sec = 0.12f;
    p.voiced = true;
    p.noise_mix = 0.0f;

    switch (c) {
        case 'A': /* "ah" */
            p.f1 = 750.0f; p.f2 = 1200.0f; p.f3 = 2400.0f;
            break;
        case 'E': /* "eh" */
            p.f1 = 530.0f; p.f2 = 1840.0f; p.f3 = 2480.0f;
            break;
        case 'I': /* "ee" */
            p.f1 = 270.0f; p.f2 = 2290.0f; p.f3 = 3010.0f;
            break;
        case 'O': /* "oh" */
            p.f1 = 500.0f; p.f2 = 700.0f;  p.f3 = 2400.0f;
            break;
        case 'U': /* "oo" */
            p.f1 = 300.0f; p.f2 = 870.0f;  p.f3 = 2240.0f;
            break;
        case 'Y':
            p.f1 = 300.0f; p.f2 = 2000.0f; p.f3 = 2800.0f;
            break;
        case 'S':
        case 'C':
            p.f1 = 200.0f; p.f2 = 4000.0f; p.f3 = 6000.0f;
            p.duration_sec = 0.10f;
            p.voiced = false;
            p.noise_mix = 1.0f;
            p.gain1 = 0.1f; p.gain2 = 0.8f; p.gain3 = 1.0f;
            break;
        case 'Z':
            p.f1 = 250.0f; p.f2 = 3500.0f; p.f3 = 5500.0f;
            p.duration_sec = 0.09f;
            p.voiced = true;
            p.noise_mix = 0.7f;
            break;
        case 'T':
            p.f1 = 300.0f; p.f2 = 2500.0f; p.f3 = 4500.0f;
            p.duration_sec = 0.06f;
            p.voiced = false;
            p.noise_mix = 1.0f;
            break;
        case 'K':
        case 'Q':
            p.f1 = 300.0f; p.f2 = 1800.0f; p.f3 = 2700.0f;
            p.duration_sec = 0.07f;
            p.voiced = false;
            p.noise_mix = 0.9f;
            break;
        case 'P':
            p.f1 = 200.0f; p.f2 = 1000.0f; p.f3 = 2200.0f;
            p.duration_sec = 0.06f;
            p.voiced = false;
            p.noise_mix = 0.8f;
            break;
        case 'B':
            p.f1 = 200.0f; p.f2 = 1000.0f; p.f3 = 2200.0f;
            p.duration_sec = 0.07f;
            p.voiced = true;
            p.noise_mix = 0.2f;
            break;
        case 'D':
            p.f1 = 300.0f; p.f2 = 2000.0f; p.f3 = 3000.0f;
            p.duration_sec = 0.07f;
            p.voiced = true;
            p.noise_mix = 0.3f;
            break;
        case 'G':
            p.f1 = 300.0f; p.f2 = 1800.0f; p.f3 = 2600.0f;
            p.duration_sec = 0.07f;
            p.voiced = true;
            p.noise_mix = 0.2f;
            break;
        case 'M':
            p.f1 = 250.0f; p.f2 = 1000.0f; p.f3 = 2200.0f;
            p.duration_sec = 0.10f;
            p.voiced = true;
            p.gain1 = 1.2f; p.gain2 = 0.2f; p.gain3 = 0.1f;
            break;
        case 'N':
            p.f1 = 250.0f; p.f2 = 1700.0f; p.f3 = 2600.0f;
            p.duration_sec = 0.10f;
            p.voiced = true;
            p.gain1 = 1.1f; p.gain2 = 0.3f; p.gain3 = 0.1f;
            break;
        case 'L':
            p.f1 = 380.0f; p.f2 = 1200.0f; p.f3 = 2700.0f;
            p.duration_sec = 0.09f;
            p.voiced = true;
            break;
        case 'R':
            p.f1 = 420.0f; p.f2 = 1300.0f; p.f3 = 1600.0f;
            p.duration_sec = 0.10f;
            p.voiced = true;
            break;
        case 'F':
            p.f1 = 250.0f; p.f2 = 1500.0f; p.f3 = 3500.0f;
            p.duration_sec = 0.09f;
            p.voiced = false;
            p.noise_mix = 1.0f;
            break;
        case 'V':
            p.f1 = 250.0f; p.f2 = 1500.0f; p.f3 = 3500.0f;
            p.duration_sec = 0.09f;
            p.voiced = true;
            p.noise_mix = 0.5f;
            break;
        case 'W':
            p.f1 = 300.0f; p.f2 = 700.0f;  p.f3 = 2200.0f;
            p.duration_sec = 0.08f;
            p.voiced = true;
            break;
        case 'J':
            p.f1 = 280.0f; p.f2 = 2500.0f; p.f3 = 4200.0f;
            p.duration_sec = 0.09f;
            p.voiced = true;
            p.noise_mix = 0.65f;
            break;
        case 'X':
            p.f1 = 240.0f; p.f2 = 3200.0f; p.f3 = 5200.0f;
            p.duration_sec = 0.10f;
            p.voiced = false;
            p.noise_mix = 1.0f;
            p.gain1 = 0.2f; p.gain2 = 0.8f; p.gain3 = 0.9f;
            break;
        case 'H':
            p.f1 = 400.0f; p.f2 = 1500.0f; p.f3 = 2500.0f;
            p.duration_sec = 0.08f;
            p.voiced = false;
            p.noise_mix = 1.0f;
            break;
        case ' ':
        default:
            p.f1 = 0.0f; p.f2 = 0.0f; p.f3 = 0.0f;
            p.duration_sec = 0.06f;
            p.voiced = false;
            break;
    }
    return p;
}

static DSPhoneme get_next_phoneme(const char *text, size_t text_len, size_t *index) {
    const size_t i = *index;
    const char c0 = (char)toupper((unsigned char)text[i]);
    const char c1 = (i + 1 < text_len) ? (char)toupper((unsigned char)text[i + 1]) : '\0';
    DSPhoneme p = {0};
    p.bw1 = 90.0f; p.bw2 = 110.0f; p.bw3 = 140.0f;
    p.gain1 = 1.0f; p.gain2 = 0.6f; p.gain3 = 0.3f;
    p.duration_sec = 0.12f; p.voiced = true;

    if (c0 == 'S' && c1 == 'H') {
        p.f1 = 300.0f; p.f2 = 2600.0f; p.f3 = 4800.0f;
        p.bw2 = 220.0f; p.bw3 = 280.0f;
        p.duration_sec = 0.11f; p.voiced = false; p.noise_mix = 1.0f;
        p.gain1 = 0.15f; p.gain2 = 0.90f; p.gain3 = 0.90f;
        *index += 2;
        return p;
    }
    if (c0 == 'C' && c1 == 'H') {
        p.f1 = 300.0f; p.f2 = 2800.0f; p.f3 = 5000.0f;
        p.bw2 = 220.0f; p.bw3 = 280.0f;
        p.duration_sec = 0.09f; p.voiced = false; p.noise_mix = 1.0f;
        p.gain1 = 0.15f; p.gain2 = 0.85f; p.gain3 = 0.95f;
        *index += 2;
        return p;
    }
    if (c0 == 'T' && c1 == 'H') {
        p.f1 = 320.0f; p.f2 = 1800.0f; p.f3 = 4200.0f;
        p.bw2 = 200.0f; p.bw3 = 260.0f;
        p.duration_sec = 0.08f; p.voiced = false; p.noise_mix = 0.90f;
        p.gain1 = 0.20f; p.gain2 = 0.60f; p.gain3 = 0.80f;
        *index += 2;
        return p;
    }
    if (c0 == 'P' && c1 == 'H') {
        *index += 2;
        return get_phoneme_for_char('F');
    }
    if (c0 == 'W' && c1 == 'H') {
        p.f1 = 320.0f; p.f2 = 750.0f; p.f3 = 2300.0f;
        p.duration_sec = 0.09f; p.noise_mix = 0.30f;
        *index += 2;
        return p;
    }
    if (c0 == 'N' && c1 == 'G') {
        p.f1 = 250.0f; p.f2 = 1900.0f; p.f3 = 2700.0f;
        p.duration_sec = 0.10f;
        p.gain1 = 1.10f; p.gain2 = 0.35f; p.gain3 = 0.12f;
        *index += 2;
        return p;
    }
    if (c0 == 'Q' && c1 == 'U') {
        p.f1 = 310.0f; p.f2 = 1100.0f; p.f3 = 2350.0f;
        p.f1_end = 300.0f; p.f2_end = 700.0f; p.f3_end = 2200.0f;
        p.duration_sec = 0.10f; p.noise_mix = 0.35f;
        *index += 2;
        return p;
    }
    if (c0 == 'O' && c1 == 'O') {
        p.f1 = 300.0f; p.f2 = 870.0f; p.f3 = 2240.0f;
        p.duration_sec = 0.13f;
        *index += 2;
        return p;
    }
    if ((c0 == 'E' && c1 == 'E') || (c0 == 'E' && c1 == 'A')) {
        p.f1 = 270.0f; p.f2 = 2290.0f; p.f3 = 3010.0f;
        p.duration_sec = 0.13f;
        *index += 2;
        return p;
    }
    if (c0 == 'A' && c1 == 'I') {
        p.f1 = 480.0f; p.f2 = 1950.0f; p.f3 = 2550.0f;
        p.f1_end = 320.0f; p.f2_end = 2250.0f; p.f3_end = 2900.0f;
        p.duration_sec = 0.14f;
        *index += 2;
        return p;
    }
    if (c0 == 'O' && c1 == 'A') {
        p.f1 = 500.0f; p.f2 = 750.0f; p.f3 = 2400.0f;
        p.f1_end = 330.0f; p.f2_end = 820.0f; p.f3_end = 2280.0f;
        p.duration_sec = 0.14f;
        *index += 2;
        return p;
    }
    if (c0 == 'O' && (c1 == 'U' || c1 == 'W')) {
        p.f1 = 720.0f; p.f2 = 1200.0f; p.f3 = 2400.0f;
        p.f1_end = 340.0f; p.f2_end = 850.0f; p.f3_end = 2250.0f;
        p.duration_sec = 0.14f;
        *index += 2;
        return p;
    }
    if (c0 == 'A' && (c1 == 'U' || c1 == 'W')) {
        p.f1 = 580.0f; p.f2 = 850.0f; p.f3 = 2400.0f;
        p.duration_sec = 0.13f;
        *index += 2;
        return p;
    }
    if ((c0 == 'E' || c0 == 'I' || c0 == 'U') && c1 == 'R') {
        p.f1 = 480.0f; p.f2 = 1350.0f; p.f3 = 1690.0f;
        p.duration_sec = 0.12f;
        *index += 2;
        return p;
    }

    *index += 1;
    return get_phoneme_for_char(c0);
}

#define COMB1_LEN 520
#define COMB2_LEN 690
#define COMB3_LEN 840
#define AP_LEN    180

void ds_voice_synth_reverb(int16_t *pcm, size_t num_samples, float wet_mix) {
    if (!pcm || num_samples == 0 || wet_mix <= 0.0f) return;
    if (wet_mix > 1.0f) wet_mix = 1.0f;

    float comb1_buf[COMB1_LEN];
    float comb2_buf[COMB2_LEN];
    float comb3_buf[COMB3_LEN];
    float ap_buf[AP_LEN];

    memset(comb1_buf, 0, sizeof(comb1_buf));
    memset(comb2_buf, 0, sizeof(comb2_buf));
    memset(comb3_buf, 0, sizeof(comb3_buf));
    memset(ap_buf, 0, sizeof(ap_buf));

    size_t c1_idx = 0, c2_idx = 0, c3_idx = 0, ap_idx = 0;
    const float dry_mix = 1.0f - wet_mix * 0.4f;

    for (size_t i = 0; i < num_samples; ++i) {
        const float in = (float)pcm[i];

        /* Comb filters in parallel */
        const float c1_out = comb1_buf[c1_idx];
        comb1_buf[c1_idx] = in + c1_out * 0.72f;
        c1_idx = (c1_idx + 1) % COMB1_LEN;

        const float c2_out = comb2_buf[c2_idx];
        comb2_buf[c2_idx] = in + c2_out * 0.68f;
        c2_idx = (c2_idx + 1) % COMB2_LEN;

        const float c3_out = comb3_buf[c3_idx];
        comb3_buf[c3_idx] = in + c3_out * 0.64f;
        c3_idx = (c3_idx + 1) % COMB3_LEN;

        const float comb_sum = (c1_out + c2_out + c3_out) * 0.33f;

        /* All-pass diffuser in series */
        const float ap_in = comb_sum;
        const float ap_delayed = ap_buf[ap_idx];
        const float ap_out = -0.5f * ap_in + ap_delayed;
        ap_buf[ap_idx] = ap_in + 0.5f * ap_delayed;
        ap_idx = (ap_idx + 1) % AP_LEN;

        float out = in * dry_mix + ap_out * wet_mix;
        if (out > 32000.0f) out = 32000.0f;
        if (out < -32000.0f) out = -32000.0f;
        pcm[i] = (int16_t)out;
    }
}

static _Atomic uint32_t ds_dynamic_seq = 1u;

size_t ds_voice_synthesize(const char *text, const DSVoiceConfig *config,
                           int16_t *pcm_out, size_t max_samples) {
    if (!text || !text[0] || !pcm_out || max_samples < 256) return 0;
    if (config && !config->enabled) return 0;

    const DSVoiceMode mode = config ? config->mode : DS_VOICE_MODE_CLASSIC;
    uint32_t rng = 0x811c9dc5u ^ ((uint32_t)mode * 0x9e3779b9u);
    for (const char *tp = text; *tp; ++tp) {
        rng = (rng ^ (uint8_t)toupper((unsigned char)*tp)) * 0x01000193u;
    }
    if (mode == DS_VOICE_MODE_DYNAMIC) {
        rng ^= atomic_fetch_add(&ds_dynamic_seq, 1u) * 2654435761u;
    }
    if (rng == 0u) rng = 0x12345678u;

    float base_pitch = (config && config->pitch_hz >= 60.0f && config->pitch_hz <= 300.0f)
                             ? config->pitch_hz : 115.0f;
    float speed_mult = (config && config->speed >= 0.5f && config->speed <= 2.5f)
                             ? config->speed : 1.0f;

    /* Dynamic Voice Mode: per-word stochastic perturbation of playback speed [0.80, 1.10] and pitch [0.50, 1.30] */
    if (mode == DS_VOICE_MODE_DYNAMIC) {
        const float rnd_speed = next_unit_random_rng(&rng) * 0.30f + 0.80f;
        const float rnd_pitch = next_unit_random_rng(&rng) * 0.80f + 0.50f;
        speed_mult *= rnd_speed;
        base_pitch *= rnd_pitch;
    }

    size_t out_index = 0;
    float phase = 0.0f;
    const float sample_rate = (float)DS_VOICE_SAMPLE_RATE;

    /* If Spirit Box mode: prepend a brief static radio burst */
    if (mode == DS_VOICE_MODE_SPIRIT_BOX) {
        const size_t burst_samples = (size_t)(sample_rate * 0.05f);
        for (size_t i = 0; i < burst_samples && out_index < max_samples; ++i) {
            const float env = 1.0f - (float)i / (float)burst_samples;
            const float val = next_noise_rng(&rng) * env * 4000.0f;
            pcm_out[out_index++] = (int16_t)val;
        }
    }

    DSResonator r1 = {0}, r2 = {0}, r3 = {0};
    bool have_prev = false;
    float prev_f1 = 500.0f, prev_f2 = 1500.0f, prev_f3 = 2500.0f;

    const size_t text_len = strlen(text);
    size_t i = 0;
    while (i < text_len) {
        const char c = text[i];
        if (!isalnum((unsigned char)c) && c != ' ') {
            ++i;
            continue;
        }

        DSPhoneme phoneme = get_next_phoneme(text, text_len, &i);
        if (phoneme.f1 <= 0.0f && phoneme.noise_mix <= 0.0f) {
            /* Pause / space */
            const size_t pause_samples = (size_t)(phoneme.duration_sec / speed_mult * sample_rate);
            for (size_t s = 0; s < pause_samples && out_index < max_samples; ++s) {
                pcm_out[out_index++] = 0;
            }
            have_prev = false;
            continue;
        }

        /* Adjust phoneme parameters for Whisper / Spirit Box / Dynamic */
        if (mode == DS_VOICE_MODE_WHISPER) {
            phoneme.voiced = false;
            phoneme.noise_mix = 1.0f;
            phoneme.gain1 *= 0.5f;
            phoneme.gain2 *= 1.2f;
            phoneme.gain3 *= 1.4f;
        } else if (mode == DS_VOICE_MODE_DYNAMIC) {
            phoneme.gain2 *= 1.15f;
            phoneme.gain3 *= 1.25f;
            phoneme.noise_mix = fmaxf(phoneme.noise_mix, 0.08f);
        }

        const float end_f1 = (phoneme.f1_end > 0.0f) ? phoneme.f1_end : phoneme.f1;
        const float end_f2 = (phoneme.f2_end > 0.0f) ? phoneme.f2_end : phoneme.f2;
        const float end_f3 = (phoneme.f3_end > 0.0f) ? phoneme.f3_end : phoneme.f3;

        if (!have_prev) {
            resonator_init(&r1, phoneme.f1, phoneme.bw1, sample_rate);
            resonator_init(&r2, phoneme.f2, phoneme.bw2, sample_rate);
            resonator_init(&r3, phoneme.f3, phoneme.bw3, sample_rate);
            prev_f1 = phoneme.f1;
            prev_f2 = phoneme.f2;
            prev_f3 = phoneme.f3;
            have_prev = true;
        }

        const size_t num_samples = (size_t)(phoneme.duration_sec / speed_mult * sample_rate);
        if (num_samples == 0) continue;

        for (size_t s = 0; s < num_samples && out_index < max_samples; ++s) {
            const float progress = (float)s / (float)num_samples;

            /* Smooth coarticulation + diphthong interpolation + optional Spirit Box sweep every 8 samples */
            if ((s & 7U) == 0U) {
                const float coart = (progress < 0.25f) ? (progress / 0.25f) : 1.0f;
                const float glide = (phoneme.f1_end > 0.0f) ? progress : 0.0f;
                const float tgt_f1 = phoneme.f1 + (end_f1 - phoneme.f1) * glide;
                const float tgt_f2 = phoneme.f2 + (end_f2 - phoneme.f2) * glide;
                const float tgt_f3 = phoneme.f3 + (end_f3 - phoneme.f3) * glide;
                float cur_f1 = prev_f1 + (tgt_f1 - prev_f1) * coart;
                float cur_f2 = prev_f2 + (tgt_f2 - prev_f2) * coart;
                float cur_f3 = prev_f3 + (tgt_f3 - prev_f3) * coart;
                float cur_bw1 = phoneme.bw1, cur_bw2 = phoneme.bw2, cur_bw3 = phoneme.bw3;

                if (mode == DS_VOICE_MODE_SPIRIT_BOX) {
                    const float sweep_phase = 2.0f * (float)M_PI * (3.5f * (float)out_index / sample_rate + progress * 0.75f);
                    const float sweep1 = sinf(sweep_phase);
                    const float sweep2 = cosf(sweep_phase * 1.3f);
                    cur_f1 *= (1.0f + 0.18f * sweep1);
                    cur_f2 *= (1.0f + 0.25f * sweep2);
                    cur_f3 *= (1.0f + 0.15f * sweep1);
                    cur_bw1 *= 1.15f;
                    cur_bw2 *= 1.15f;
                }
                resonator_set_coeffs(&r1, cur_f1, cur_bw1, sample_rate);
                resonator_set_coeffs(&r2, cur_f2, cur_bw2, sample_rate);
                resonator_set_coeffs(&r3, cur_f3, cur_bw3, sample_rate);
            }

            /* Smooth Attack-Decay envelope */
            float env = 1.0f;
            if (progress < 0.15f) env = progress / 0.15f;
            else if (progress > 0.85f) env = (1.0f - progress) / 0.15f;

            /* Dynamic microtonal vibrato */
            float eff_pitch = base_pitch;
            if (mode == DS_VOICE_MODE_DYNAMIC) {
                const float vibrato = sinf(2.0f * (float)M_PI * 5.2f * (float)s / sample_rate);
                eff_pitch = base_pitch * (1.0f + 0.035f * vibrato);
            }
            const float pitch_step = 2.0f * (float)M_PI * eff_pitch / sample_rate;

            /* Excitation signal */
            float excitation = 0.0f;
            if (phoneme.voiced) {
                /* Band-limited glottal pulse approximation */
                phase += pitch_step;
                if (phase >= 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
                const float pulse = (phase < 0.4f * (float)M_PI) ? sinf(phase * 2.5f) : 0.0f;
                excitation += pulse;
            }

            if (phoneme.noise_mix > 0.0f) {
                excitation += next_noise_rng(&rng) * phoneme.noise_mix;
            }

            /* Resonators */
            const float out1 = resonator_process(&r1, excitation) * phoneme.gain1;
            const float out2 = resonator_process(&r2, excitation) * phoneme.gain2;
            const float out3 = resonator_process(&r3, excitation) * phoneme.gain3;
            float total = (out1 + out2 + out3) * env;

            /* Spirit box modulation */
            if (mode == DS_VOICE_MODE_SPIRIT_BOX) {
                total += next_noise_rng(&rng) * 0.08f * env;
            }

            /* Master scaling and soft clipping */
            total *= 12000.0f;
            if (total > 32000.0f) total = 32000.0f;
            if (total < -32000.0f) total = -32000.0f;

            pcm_out[out_index++] = (int16_t)total;
        }

        prev_f1 = end_f1;
        prev_f2 = end_f2;
        prev_f3 = end_f3;
    }

    /* Trailing decay */
    const size_t decay_samples = (size_t)(sample_rate * 0.04f);
    for (size_t s = 0; s < decay_samples && out_index < max_samples; ++s) {
        pcm_out[out_index++] = 0;
    }

    /* Atmospheric spatial reverberation */
    const float wet = (mode == DS_VOICE_MODE_SPIRIT_BOX) ? 0.48f :
                      (mode == DS_VOICE_MODE_WHISPER) ? 0.35f :
                      (mode == DS_VOICE_MODE_DYNAMIC) ? 0.32f : 0.22f;
    ds_voice_synth_reverb(pcm_out, out_index, wet);

    return out_index;
}
