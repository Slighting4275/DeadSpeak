#define _POSIX_C_SOURCE 200809L
#undef NDEBUG
#include "../app/src/main/cpp/evp_ops.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    /* 1. Cue synthesis */
    int16_t cue_buf[16000];
    assert(ds_synthesize_cue(DS_CUE_NONE, 16000, cue_buf, 16000) == 0);
    assert(ds_synthesize_cue(DS_CUE_START, 0, cue_buf, 16000) == 0);
    const size_t n_start = ds_synthesize_cue(DS_CUE_START, 16000, cue_buf, 16000);
    assert(n_start > 500 && n_start <= 16000);
    int peak = 0;
    for (size_t i = 0; i < n_start; ++i) {
        if (abs(cue_buf[i]) > peak) peak = abs(cue_buf[i]);
    }
    assert(peak > 1000 && peak <= 32767);
    assert(ds_synthesize_cue(DS_CUE_STOP, 16000, cue_buf, 16000) > 500);
    assert(ds_synthesize_cue(DS_CUE_EMF_ALERT, 16000, cue_buf, 16000) > 500);
    assert(ds_synthesize_cue(DS_CUE_TICK, 16000, cue_buf, 16000) > 100);

    /* 2. EVP 80Hz high-pass + 60Hz notch filter attenuates 50/60Hz hum while passing 1kHz tone */
    DSEvpFilterState flt = {0};
    ds_evp_filter_init(&flt, 16000);
    assert(flt.initialized);
    int16_t hum[3200];
    for (size_t i = 0; i < 3200; ++i) {
        hum[i] = (int16_t)(12000.0 * sin(2.0 * 3.141592653589793 * 60.0 * (double)i / 16000.0));
    }
    ds_evp_filter_process(&flt, hum, 3200);
    double rms_hum = 0.0;
    for (size_t i = 1600; i < 3200; ++i) {
        rms_hum += (double)hum[i] * (double)hum[i];
    }
    rms_hum = sqrt(rms_hum / 1600.0);
    assert(rms_hum < 2500.0); /* strongly attenuated 60 Hz hum */

    ds_evp_filter_init(&flt, 16000);
    int16_t voice[3200];
    for (size_t i = 0; i < 3200; ++i) {
        voice[i] = (int16_t)(12000.0 * sin(2.0 * 3.141592653589793 * 900.0 * (double)i / 16000.0));
    }
    ds_evp_filter_process(&flt, voice, 3200);
    double rms_voice = 0.0;
    for (size_t i = 1600; i < 3200; ++i) {
        rms_voice += (double)voice[i] * (double)voice[i];
    }
    rms_voice = sqrt(rms_voice / 1600.0);
    assert(rms_voice > 6000.0); /* passes speech frequencies */

    /* 3. Speed & A-B loop clamping */
    assert(fabsf(ds_clamp_playback_speed(NAN) - 1.0f) < 0.01f);
    assert(fabsf(ds_clamp_playback_speed(0.1f) - 0.5f) < 0.01f);
    assert(fabsf(ds_clamp_playback_speed(9.0f) - 2.0f) < 0.01f);
    uint64_t ls = 5000, le = 3000;
    ds_clamp_loop_range(4000, &ls, &le);
    assert(ls == 0 && le == 0);
    ls = 1000; le = 9000;
    ds_clamp_loop_range(4000, &ls, &le);
    assert(ls == 1000 && le == 4000);

    /* 4. Filter + BMP round-trip snapshot & symlink guard */
    uint8_t src[4 * 3 * 4];
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 4; ++x) {
            uint8_t *p = src + ((size_t)y * 4U + (size_t)x) * 4U;
            p[0] = (uint8_t)(40 + x * 40);
            p[1] = (uint8_t)(30 + y * 60);
            p[2] = 120;
            p[3] = 255;
        }
    }
    const char *bmp_path = "build/host/evp_ops_snap_test.bmp";
    unlink(bmp_path);
    assert(ds_write_filtered_bmp(bmp_path, src, 4, 3, 90, false, 1.0f, DS_VIDEO_FILTER_RAW));
    uint8_t loaded[16 * 16 * 4];
    int lw = 0, lh = 0;
    assert(ds_load_bmp_rgba(bmp_path, loaded, sizeof(loaded), &lw, &lh));
    assert(lw == 3 && lh == 4); /* rotated 90 degrees swaps width/height */

    /* Filtered NIGHT snapshot turns green channel dominant */
    unlink(bmp_path);
    assert(ds_write_filtered_bmp(bmp_path, src, 4, 3, 0, true, 1.5f, DS_VIDEO_FILTER_NIGHT));
    assert(ds_load_bmp_rgba(bmp_path, loaded, sizeof(loaded), &lw, &lh));
    assert(lw == 4 && lh == 3);
    assert(loaded[1] >= loaded[0] && loaded[1] >= loaded[2]);

    /* Symlink refusal */
    const char *sym_path = "build/host/evp_ops_sym.bmp";
    unlink(sym_path);
    assert(symlink("evp_ops_snap_test.bmp", sym_path) == 0);
    assert(!ds_write_filtered_bmp(sym_path, src, 4, 3, 0, false, 1.0f, DS_VIDEO_FILTER_RAW));
    assert(!ds_load_bmp_rgba(sym_path, loaded, sizeof(loaded), &lw, &lh));
    unlink(sym_path);
    unlink(bmp_path);

    /* 5. Companion tag append */
    const char *tags_path = "build/host/evp_ops_tags_test.txt";
    unlink(tags_path);
    assert(ds_append_tag_entry(tags_path, 1250ULL, 3U));
    assert(ds_append_tag_entry(tags_path, 4500ULL, 3U));
    FILE *tf = fopen(tags_path, "r");
    assert(tf != NULL);
    char line[64] = {0};
    assert(fgets(line, sizeof(line), tf) != NULL);
    assert(strcmp(line, "1250,3\n") == 0);
    assert(fgets(line, sizeof(line), tf) != NULL);
    assert(strcmp(line, "4500,3\n") == 0);
    fclose(tf);

    /* 6. Timeline tag parsing, waveform resampling & scrub math */
    DSTimelineMarker markers[8];
    const size_t n_markers = ds_parse_tags_file(tags_path, markers, 8);
    assert(n_markers == 2);
    assert(markers[0].timestamp_ms == 1250ULL && markers[0].tag_id == 3U);
    assert(markers[1].timestamp_ms == 4500ULL && markers[1].tag_id == 3U);
    assert(ds_find_nearest_marker(markers, n_markers, 1300ULL, 250ULL) == 0);
    assert(ds_find_nearest_marker(markers, n_markers, 2500ULL, 250ULL) == -1);
    assert(ds_scrub_fraction_to_ms(-0.5f, 10000ULL) == 0ULL);
    assert(ds_scrub_fraction_to_ms(0.5f, 10000ULL) == 5000ULL);
    assert(ds_scrub_fraction_to_ms(1.5f, 10000ULL) == 10000ULL);

    const char *wf_path = "build/host/evp_ops_wf_test.txt";
    unlink(wf_path);
    FILE *wff = fopen(wf_path, "w");
    assert(wff != NULL);
    fputs("0,0.10\n250,0.90\n500,0.20\n750,0.60\n", wff);
    fclose(wff);
    float wf_bars[8] = {0};
    uint64_t wf_dur = 0;
    const size_t wf_n = ds_resample_waveform_file(wf_path, wf_bars, 8, &wf_dur);
    assert(wf_n == 4 && wf_dur == 750ULL);
    assert(fabsf(wf_bars[1] - 0.90f) < 0.01f);
    unlink(wf_path);

    /* 7. Multi-band EQ smoother & peak-hold tracker */
    DSEqMeterState eq = {0};
    const float hot[8] = {0.8f, 0.6f, 0.4f, 0.2f, 0.9f, 0.5f, 0.3f, 0.1f};
    ds_eq_meter_update(&eq, hot, 33.0f);
    assert(eq.levels[0] > 0.3f && eq.peaks[0] >= eq.levels[0]);
    const float peak0 = eq.peaks[0];
    const float quiet[8] = {0};
    ds_eq_meter_update(&eq, quiet, 100.0f);
    assert(eq.levels[0] < hot[0] && fabsf(eq.peaks[0] - peak0) < 0.001f);
    ds_eq_meter_update(&eq, quiet, 600.0f);
    assert(eq.peaks[0] < peak0);

    /* 8. Snapshot pan/zoom UV + brightness/contrast/filter edit & atomic export */
    DSSnapEditParams ed = ds_snap_edit_defaults();
    assert(ed.zoom == 1.0f && ed.pan_x == 0.0f && ed.brightness == 0.0f && ed.contrast == 1.0f);
    ed.zoom = 2.0f; ed.pan_x = 9.0f; ed.brightness = 2.0f; ed.contrast = 9.0f;
    ds_snap_edit_clamp(&ed);
    assert(ed.zoom == 2.0f && ed.pan_x <= 0.251f && ed.brightness == 0.5f && ed.contrast == 2.0f);
    float suv[12];
    assert(ds_snap_compute_uv(&ed, suv));
    for (size_t i = 0; i < 12; ++i) assert(isfinite(suv[i]) && suv[i] >= -1e-4f && suv[i] <= 1.0001f);

    uint8_t base_rgba[2 * 2 * 4] = {
        100,100,100,255, 140,140,140,255,
        80, 80, 80, 255, 180,180,180,255,
    };
    const char *src_bmp = "build/host/evp_ops_edit_src.bmp";
    const char *dst_bmp = "build/host/evp_ops_edit_dst.bmp";
    unlink(src_bmp); unlink(dst_bmp);
    assert(ds_write_filtered_bmp(src_bmp, base_rgba, 2, 2, 0, false, 1.0f, DS_VIDEO_FILTER_RAW));
    ed = ds_snap_edit_defaults();
    ed.brightness = 0.2f;
    ed.contrast = 1.5f;
    ed.filter = DS_VIDEO_FILTER_NIGHT;
    assert(ds_edit_and_save_bmp(src_bmp, dst_bmp, &ed));
    int ew = 0, eh = 0;
    uint8_t edited_px[4 * 4];
    assert(ds_load_bmp_rgba(dst_bmp, edited_px, sizeof(edited_px), &ew, &eh));
    assert(ew == 2 && eh == 2 && edited_px[1] >= edited_px[0] && edited_px[1] >= edited_px[2]);
    unlink(src_bmp); unlink(dst_bmp);
    unlink(tags_path);

    puts("PASS: evp_ops cue synth, EVP highpass/notch, playback loop/speed, BMP snapshot, and tag append");
    return 0;
}
