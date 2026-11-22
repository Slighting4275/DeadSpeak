#define _POSIX_C_SOURCE 200809L
#include "media_player.h"
#include "preview_frame.h"
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <fcntl.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DS_MP_LOG "DeadSpeakMedia"

struct DSMediaPlayer {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool thread_created;
    _Atomic bool running;

    bool playing;
    bool paused;
    bool stop_requested;
    bool open_requested;
    bool seek_requested;
    uint64_t seek_target_ms;

    char pending_path[512];
    char active_path[512];
    uint64_t position_ms;
    uint64_t duration_ms;
    int rotation;

    uint8_t *frame_rgba;
    uint8_t *scratch_rgba;
    int frame_width;
    int frame_height;
    uint64_t frame_seq;
    uint64_t copied_seq;
};

static int64_t mp_clock_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
}

static AAudioStream *open_mp_audio(int32_t sample_rate, int32_t channels) {
    if (sample_rate <= 0 || channels <= 0 || channels > 2) return NULL;
    AAudioStreamBuilder *builder = NULL;
    AAudioStream *stream = NULL;
    if (AAudio_createStreamBuilder(&builder) == AAUDIO_OK && builder) {
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSampleRate(builder, sample_rate);
        AAudioStreamBuilder_setChannelCount(builder, channels);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        if (AAudioStreamBuilder_openStream(builder, &stream) == AAUDIO_OK && stream) {
            AAudioStream_requestStart(stream);
        } else {
            if (stream) AAudioStream_close(stream);
            stream = NULL;
        }
        AAudioStreamBuilder_delete(builder);
    }
    return stream;
}

static void drain_reader_images(AImageReader *reader) {
    if (!reader) return;
    AImage *img = NULL;
    while (AImageReader_acquireNextImage(reader, &img) == AMEDIA_OK && img) {
        AImage_delete(img);
        img = NULL;
    }
}

static bool convert_reader_image(DSMediaPlayer *mp, AImageReader *reader, bool bt709_default) {
    AImage *img = NULL;
    const media_status_t st = AImageReader_acquireLatestImage(reader, &img);
    if (st != AMEDIA_OK || !img) return false;

    DSPreviewYuv yuv = {0};
    AImageCropRect crop = {0};
    int32_t planes = 0;
    bool ok = AImage_getWidth(img, &yuv.width) == AMEDIA_OK &&
              AImage_getHeight(img, &yuv.height) == AMEDIA_OK &&
              AImage_getNumberOfPlanes(img, &planes) == AMEDIA_OK && planes == 3;
    if (ok && AImage_getCropRect(img, &crop) == AMEDIA_OK &&
        crop.right > crop.left && crop.bottom > crop.top) {
        yuv.left = crop.left; yuv.top = crop.top; yuv.right = crop.right; yuv.bottom = crop.bottom;
    } else {
        yuv.left = 0; yuv.top = 0; yuv.right = yuv.width; yuv.bottom = yuv.height;
    }
    yuv.bt709 = bt709_default || yuv.width >= 1280 || yuv.height >= 720;
    yuv.full_range = false;
    for (int32_t p = 0; ok && p < 3; ++p) {
        uint8_t *data = NULL;
        int len = 0;
        ok = AImage_getPlaneData(img, p, &data, &len) == AMEDIA_OK && len > 0 &&
             AImage_getPlaneRowStride(img, p, &yuv.planes[p].row_stride) == AMEDIA_OK &&
             AImage_getPlanePixelStride(img, p, &yuv.planes[p].pixel_stride) == AMEDIA_OK;
        yuv.planes[p].data = data;
        yuv.planes[p].length = len > 0 ? (size_t)len : 0;
    }
    int out_w = 0, out_h = 0;
    ok = ok && ds_preview_convert(&yuv, mp->scratch_rgba, DS_PREVIEW_MAX_BYTES, &out_w, &out_h);
    AImage_delete(img);
    if (ok && out_w > 0 && out_h > 0) {
        pthread_mutex_lock(&mp->mutex);
        uint8_t *old = mp->frame_rgba;
        mp->frame_rgba = mp->scratch_rgba;
        mp->scratch_rgba = old;
        mp->frame_width = out_w;
        mp->frame_height = out_h;
        ++mp->frame_seq;
        pthread_mutex_unlock(&mp->mutex);
        return true;
    }
    return false;
}

static bool send_decoder_eos(AMediaCodec *dec) {
    if (!dec) return true;
    const ssize_t idx = AMediaCodec_dequeueInputBuffer(dec, 2000);
    if (idx < 0) return false;
    AMediaCodec_queueInputBuffer(dec, (size_t)idx, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    return true;
}

static void play_file_session(DSMediaPlayer *mp, const char *path) {
    AMediaExtractor *ex = NULL;
    int v_track = -1, a_track = -1;
    int32_t vw = 0, vh = 0, rot = 0, sr = 48000, ch = 1;
    int64_t max_dur_us = 0;
    AMediaCodec *vdec = NULL, *adec = NULL;
    AImageReader *vreader = NULL;
    AAudioStream *astream = NULL;

    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st = {0};
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size <= 0) {
        if (fd >= 0) close(fd);
        __android_log_print(ANDROID_LOG_ERROR, DS_MP_LOG, "cannot open MP4: %s", path);
        goto session_cleanup;
    }

    ex = AMediaExtractor_new();
    if (!ex || AMediaExtractor_setDataSourceFd(ex, fd, 0, st.st_size) != AMEDIA_OK) {
        close(fd);
        __android_log_print(ANDROID_LOG_ERROR, DS_MP_LOG, "AMediaExtractor setDataSourceFd failed");
        goto session_cleanup;
    }
    close(fd);

    const size_t ntracks = AMediaExtractor_getTrackCount(ex);
    for (size_t i = 0; i < ntracks; ++i) {
        AMediaFormat *fmt = AMediaExtractor_getTrackFormat(ex, i);
        if (!fmt) continue;
        const char *mime = NULL;
        int64_t dur_us = 0;
        if (AMediaFormat_getString(fmt, AMEDIAFORMAT_KEY_MIME, &mime) && mime) {
            if (AMediaFormat_getInt64(fmt, AMEDIAFORMAT_KEY_DURATION, &dur_us) && dur_us > max_dur_us) {
                max_dur_us = dur_us;
            }
            if (strncmp(mime, "video/", 6) == 0 && v_track < 0) {
                if (AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, &vw) &&
                    AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, &vh) && vw > 0 && vh > 0) {
                    (void)AMediaFormat_getInt32(fmt, "rotation-degrees", &rot);
                    AMediaCodec *cand_v = AMediaCodec_createDecoderByType(mime);
                    AImageReader *cand_r = NULL;
                    ANativeWindow *cand_w = NULL;
                    if (cand_v &&
                        AImageReader_new(vw, vh, AIMAGE_FORMAT_YUV_420_888, 3, &cand_r) == AMEDIA_OK && cand_r &&
                        AImageReader_getWindow(cand_r, &cand_w) == AMEDIA_OK && cand_w &&
                        AMediaCodec_configure(cand_v, fmt, cand_w, NULL, 0) == AMEDIA_OK &&
                        AMediaCodec_start(cand_v) == AMEDIA_OK) {
                        vdec = cand_v;
                        vreader = cand_r;
                        v_track = (int)i;
                        AMediaExtractor_selectTrack(ex, i);
                    } else {
                        if (cand_v) AMediaCodec_delete(cand_v);
                        if (cand_r) AImageReader_delete(cand_r);
                    }
                }
            } else if (strncmp(mime, "audio/", 6) == 0 && a_track < 0) {
                (void)AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_SAMPLE_RATE, &sr);
                (void)AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &ch);
                AMediaCodec *cand_a = AMediaCodec_createDecoderByType(mime);
                if (cand_a &&
                    AMediaCodec_configure(cand_a, fmt, NULL, NULL, 0) == AMEDIA_OK &&
                    AMediaCodec_start(cand_a) == AMEDIA_OK) {
                    adec = cand_a;
                    a_track = (int)i;
                    AMediaExtractor_selectTrack(ex, i);
                    astream = open_mp_audio(sr, ch);
                } else {
                    if (cand_a) AMediaCodec_delete(cand_a);
                }
            }
        }
        AMediaFormat_delete(fmt);
    }

    if (!vdec && !adec) goto session_cleanup;

    pthread_mutex_lock(&mp->mutex);
    snprintf(mp->active_path, sizeof(mp->active_path), "%s", path);
    mp->duration_ms = max_dur_us > 0 ? (uint64_t)(max_dur_us / 1000LL) : 0;
    mp->position_ms = 0;
    mp->rotation = (rot == 90 || rot == 180 || rot == 270) ? rot : 0;
    mp->playing = true;
    mp->paused = false;
    mp->seek_requested = false;
    pthread_mutex_unlock(&mp->mutex);

    bool in_eos = false;
    bool v_eos_sent = (vdec == NULL), a_eos_sent = (adec == NULL);
    bool v_eos = (vdec == NULL), a_eos = (adec == NULL);
    bool seek_preview_pending = false;
    int64_t anchor_wall_ns = mp_clock_ns();
    int64_t anchor_media_us = 0;
    bool anchor_set = false;
    const bool bt709 = (vw >= 1280 || vh >= 720);

    while (atomic_load(&mp->running)) {
        pthread_mutex_lock(&mp->mutex);
        const bool want_stop = mp->stop_requested || mp->open_requested;
        const bool paused = mp->paused;
        const bool want_seek = mp->seek_requested;
        const uint64_t seek_ms = ds_media_clamp_seek_ms(mp->seek_target_ms, mp->duration_ms);
        mp->seek_requested = false;
        pthread_mutex_unlock(&mp->mutex);

        if (want_stop) break;

        if (want_seek) {
            const int64_t seek_us = (int64_t)seek_ms * 1000LL;
            AMediaExtractor_seekTo(ex, seek_us, AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC);
            if (vdec) AMediaCodec_flush(vdec);
            if (adec) AMediaCodec_flush(adec);
            drain_reader_images(vreader);
            in_eos = false;
            v_eos_sent = (vdec == NULL);
            a_eos_sent = (adec == NULL);
            v_eos = (vdec == NULL);
            a_eos = (adec == NULL);
            seek_preview_pending = (vdec != NULL);
            anchor_media_us = seek_us;
            anchor_wall_ns = mp_clock_ns();
            anchor_set = true;
            pthread_mutex_lock(&mp->mutex);
            mp->position_ms = seek_ms;
            pthread_mutex_unlock(&mp->mutex);
        }

        if (paused && !seek_preview_pending) {
            anchor_set = false;
            nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 15000000L}, NULL);
            continue;
        }

        if (v_eos && a_eos) break;

        /* 1. Feed extractor samples or signal EOS reliably until accepted */
        if (!in_eos) {
            const int tidx = AMediaExtractor_getSampleTrackIndex(ex);
            AMediaCodec *target = (tidx == v_track) ? vdec : (tidx == a_track ? adec : NULL);
            if (tidx < 0) {
                in_eos = true;
            } else if (target) {
                const ssize_t in_idx = AMediaCodec_dequeueInputBuffer(target, 2000);
                if (in_idx >= 0) {
                    size_t cap = 0;
                    uint8_t *buf = AMediaCodec_getInputBuffer(target, (size_t)in_idx, &cap);
                    const ssize_t sz = buf ? AMediaExtractor_readSampleData(ex, buf, cap) : -1;
                    const int64_t pts = AMediaExtractor_getSampleTime(ex);
                    if (sz <= 0) {
                        AMediaCodec_queueInputBuffer(target, (size_t)in_idx, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                        if (target == vdec) v_eos_sent = true;
                        if (target == adec) a_eos_sent = true;
                        in_eos = true;
                    } else {
                        AMediaCodec_queueInputBuffer(target, (size_t)in_idx, 0, (size_t)sz, (uint64_t)(pts > 0 ? pts : 0), 0);
                        AMediaExtractor_advance(ex);
                    }
                }
            } else {
                AMediaExtractor_advance(ex);
            }
        }
        if (in_eos) {
            if (!v_eos_sent) v_eos_sent = send_decoder_eos(vdec);
            if (!a_eos_sent) a_eos_sent = send_decoder_eos(adec);
        }

        /* 2. Drain audio decoder -> AAudio */
        if (adec && !a_eos) {
            AMediaCodecBufferInfo ainfo = {0};
            const ssize_t ao = AMediaCodec_dequeueOutputBuffer(adec, &ainfo, 1000);
            if (ao == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                AMediaFormat *ofmt = AMediaCodec_getOutputFormat(adec);
                if (ofmt) {
                    int32_t osr = sr, och = ch;
                    (void)AMediaFormat_getInt32(ofmt, AMEDIAFORMAT_KEY_SAMPLE_RATE, &osr);
                    (void)AMediaFormat_getInt32(ofmt, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &och);
                    if (osr != sr || och != ch || !astream) {
                        if (astream) AAudioStream_close(astream);
                        sr = osr; ch = och;
                        astream = open_mp_audio(sr, ch);
                    }
                    AMediaFormat_delete(ofmt);
                }
            } else if (ao >= 0) {
                if ((ainfo.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0U) a_eos = true;
                if (!paused && ainfo.size > 0 && astream) {
                    size_t acap = 0;
                    uint8_t *abuf = AMediaCodec_getOutputBuffer(adec, (size_t)ao, &acap);
                    if (abuf && ainfo.offset >= 0 && (size_t)(ainfo.offset + ainfo.size) <= acap) {
                        const size_t frame_bytes = sizeof(int16_t) * (size_t)(ch > 0 ? ch : 1);
                        const int32_t frames = frame_bytes > 0 ? (int32_t)((size_t)ainfo.size / frame_bytes) : 0;
                        if (frames > 0) {
                            (void)AAudioStream_write(astream, abuf + ainfo.offset, frames, 20000000LL);
                        }
                    }
                }
                AMediaCodec_releaseOutputBuffer(adec, (size_t)ao, false);
            }
        }

        /* 3. Drain video decoder -> AImageReader -> RGBA frame */
        if (vdec && !v_eos) {
            AMediaCodecBufferInfo vinfo = {0};
            const ssize_t vo = AMediaCodec_dequeueOutputBuffer(vdec, &vinfo, 1000);
            if (vo >= 0) {
                if ((vinfo.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0U) {
                    v_eos = true;
                    seek_preview_pending = false;
                }
                if (vinfo.size > 0) {
                    const int64_t pts_us = vinfo.presentationTimeUs >= 0 ? vinfo.presentationTimeUs : 0;
                    if (!paused && !seek_preview_pending) {
                        const int64_t now_ns = mp_clock_ns();
                        if (!anchor_set) {
                            anchor_media_us = pts_us;
                            anchor_wall_ns = now_ns;
                            anchor_set = true;
                        }
                        const int64_t wait_ns = anchor_wall_ns + (pts_us - anchor_media_us) * 1000LL - now_ns;
                        if (wait_ns > INT64_C(2000000) && wait_ns < INT64_C(250000000)) {
                            nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = (long)wait_ns}, NULL);
                        }
                    }
                    AMediaCodec_releaseOutputBuffer(vdec, (size_t)vo, true);
                    if (convert_reader_image(mp, vreader, bt709)) {
                        seek_preview_pending = false;
                    }
                    pthread_mutex_lock(&mp->mutex);
                    mp->position_ms = (uint64_t)(pts_us / 1000LL);
                    pthread_mutex_unlock(&mp->mutex);
                } else {
                    AMediaCodec_releaseOutputBuffer(vdec, (size_t)vo, false);
                }
            }
        }
    }

session_cleanup:
    if (astream) { AAudioStream_requestStop(astream); AAudioStream_close(astream); }
    if (vdec) { AMediaCodec_stop(vdec); AMediaCodec_delete(vdec); }
    if (adec) { AMediaCodec_stop(adec); AMediaCodec_delete(adec); }
    if (vreader) AImageReader_delete(vreader);
    if (ex) AMediaExtractor_delete(ex);

    pthread_mutex_lock(&mp->mutex);
    if (!mp->open_requested) {
        mp->playing = false;
        mp->paused = false;
        mp->stop_requested = false;
    }
    pthread_mutex_unlock(&mp->mutex);
}

static void *media_player_thread(void *arg) {
    DSMediaPlayer *mp = (DSMediaPlayer *)arg;
    while (atomic_load(&mp->running)) {
        char path[512] = {0};
        pthread_mutex_lock(&mp->mutex);
        while (atomic_load(&mp->running) && !mp->open_requested) {
            pthread_cond_wait(&mp->cond, &mp->mutex);
        }
        if (!atomic_load(&mp->running)) {
            pthread_mutex_unlock(&mp->mutex);
            break;
        }
        snprintf(path, sizeof(path), "%s", mp->pending_path);
        mp->open_requested = false;
        mp->stop_requested = false;
        pthread_mutex_unlock(&mp->mutex);

        if (path[0]) play_file_session(mp, path);
    }
    return NULL;
}

DSMediaPlayer *ds_media_player_create(void) {
    DSMediaPlayer *mp = calloc(1, sizeof(*mp));
    if (!mp) return NULL;
    mp->frame_rgba = malloc(DS_PREVIEW_MAX_BYTES);
    mp->scratch_rgba = malloc(DS_PREVIEW_MAX_BYTES);
    if (!mp->frame_rgba || !mp->scratch_rgba) {
        free(mp->frame_rgba); free(mp->scratch_rgba); free(mp);
        return NULL;
    }
    pthread_mutex_init(&mp->mutex, NULL);
    pthread_cond_init(&mp->cond, NULL);
    atomic_store(&mp->running, true);
    if (pthread_create(&mp->thread, NULL, media_player_thread, mp) != 0) {
        atomic_store(&mp->running, false);
        pthread_cond_destroy(&mp->cond);
        pthread_mutex_destroy(&mp->mutex);
        free(mp->frame_rgba); free(mp->scratch_rgba); free(mp);
        return NULL;
    }
    mp->thread_created = true;
    return mp;
}

void ds_media_player_destroy(DSMediaPlayer *mp) {
    if (!mp) return;
    atomic_store(&mp->running, false);
    pthread_mutex_lock(&mp->mutex);
    mp->stop_requested = true;
    pthread_cond_broadcast(&mp->cond);
    pthread_mutex_unlock(&mp->mutex);
    if (mp->thread_created) pthread_join(mp->thread, NULL);
    pthread_cond_destroy(&mp->cond);
    pthread_mutex_destroy(&mp->mutex);
    free(mp->frame_rgba);
    free(mp->scratch_rgba);
    free(mp);
}

bool ds_media_player_open(DSMediaPlayer *mp, const char *mp4_path) {
    if (!mp || !mp4_path || !mp4_path[0] || strlen(mp4_path) >= sizeof(mp->pending_path)) return false;
    struct stat st = {0};
    if (stat(mp4_path, &st) != 0 || st.st_size <= 0) return false;
    pthread_mutex_lock(&mp->mutex);
    snprintf(mp->pending_path, sizeof(mp->pending_path), "%s", mp4_path);
    snprintf(mp->active_path, sizeof(mp->active_path), "%s", mp4_path);
    mp->open_requested = true;
    mp->stop_requested = false;
    mp->playing = true;
    mp->frame_width = mp->frame_height = 0;
    pthread_cond_broadcast(&mp->cond);
    pthread_mutex_unlock(&mp->mutex);
    return true;
}

void ds_media_player_stop(DSMediaPlayer *mp) {
    if (!mp) return;
    pthread_mutex_lock(&mp->mutex);
    mp->stop_requested = true;
    mp->open_requested = false;
    mp->playing = false;
    mp->paused = false;
    pthread_cond_broadcast(&mp->cond);
    pthread_mutex_unlock(&mp->mutex);
}

void ds_media_player_seek_ms(DSMediaPlayer *mp, uint64_t position_ms) {
    if (!mp) return;
    pthread_mutex_lock(&mp->mutex);
    mp->seek_target_ms = position_ms;
    mp->seek_requested = true;
    pthread_cond_broadcast(&mp->cond);
    pthread_mutex_unlock(&mp->mutex);
}

void ds_media_player_set_paused(DSMediaPlayer *mp, bool paused) {
    if (!mp) return;
    pthread_mutex_lock(&mp->mutex);
    mp->paused = paused;
    pthread_cond_broadcast(&mp->cond);
    pthread_mutex_unlock(&mp->mutex);
}

bool ds_media_player_is_paused(const DSMediaPlayer *mp) {
    if (!mp) return false;
    DSMediaPlayer *m = (DSMediaPlayer *)mp;
    pthread_mutex_lock(&m->mutex);
    const bool p = m->paused;
    pthread_mutex_unlock(&m->mutex);
    return p;
}

bool ds_media_player_is_playing(const DSMediaPlayer *mp, char *out_name, size_t max_name, float *out_progress) {
    if (!mp) return false;
    DSMediaPlayer *m = (DSMediaPlayer *)mp;
    pthread_mutex_lock(&m->mutex);
    const bool playing = m->playing;
    if (out_name && max_name > 0) {
        const char *slash = strrchr(m->active_path, '/');
        const char *name = slash ? slash + 1 : m->active_path;
        snprintf(out_name, max_name, "%s", name);
    }
    if (out_progress) *out_progress = ds_media_progress(m->position_ms, m->duration_ms);
    pthread_mutex_unlock(&m->mutex);
    return playing;
}

uint64_t ds_media_player_get_position_ms(const DSMediaPlayer *mp) {
    if (!mp) return 0;
    DSMediaPlayer *m = (DSMediaPlayer *)mp;
    pthread_mutex_lock(&m->mutex);
    const uint64_t v = m->position_ms;
    pthread_mutex_unlock(&m->mutex);
    return v;
}

uint64_t ds_media_player_get_duration_ms(const DSMediaPlayer *mp) {
    if (!mp) return 0;
    DSMediaPlayer *m = (DSMediaPlayer *)mp;
    pthread_mutex_lock(&m->mutex);
    const uint64_t v = m->duration_ms;
    pthread_mutex_unlock(&m->mutex);
    return v;
}

int ds_media_player_rotation(const DSMediaPlayer *mp) {
    if (!mp) return 0;
    DSMediaPlayer *m = (DSMediaPlayer *)mp;
    pthread_mutex_lock(&m->mutex);
    const int r = m->rotation;
    pthread_mutex_unlock(&m->mutex);
    return r;
}

bool ds_media_player_copy_frame(DSMediaPlayer *mp, uint8_t *rgba, size_t capacity,
                                int *out_width, int *out_height) {
    if (!mp || !rgba || !out_width || !out_height) return false;
    if (pthread_mutex_trylock(&mp->mutex) != 0) return false;
    bool ok = mp->frame_width > 0 && mp->frame_height > 0 && mp->frame_seq != mp->copied_seq;
    if (ok) {
        const size_t bytes = (size_t)mp->frame_width * (size_t)mp->frame_height * 4U;
        ok = bytes <= capacity;
        if (ok) {
            memcpy(rgba, mp->frame_rgba, bytes);
            *out_width = mp->frame_width;
            *out_height = mp->frame_height;
            mp->copied_seq = mp->frame_seq;
        }
    }
    pthread_mutex_unlock(&mp->mutex);
    return ok;
}
