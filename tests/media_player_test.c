#undef NDEBUG
#include "../app/src/main/cpp/media_player.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void) {
    /* 1. Pure seek/progress helpers */
    assert(ds_media_clamp_seek_ms(500, 1000) == 500);
    assert(ds_media_clamp_seek_ms(1500, 1000) == 1000);
    assert(ds_media_clamp_seek_ms(300, 0) == 300);
    assert(ds_media_progress(0, 0) == 0.0f);
    assert(fabsf(ds_media_progress(250, 1000) - 0.25f) < 0.001f);
    assert(ds_media_progress(2000, 1000) == 1.0f);

    /* 2. Null & invalid open guards */
    assert(!ds_media_player_open(NULL, "/data/local/tmp/nope.mp4"));
    DSMediaPlayer *mp = ds_media_player_create();
    assert(mp != NULL);
    assert(!ds_media_player_open(mp, NULL));
    assert(!ds_media_player_open(mp, ""));
    assert(!ds_media_player_open(mp, "/data/local/tmp/does_not_exist_12345.mp4"));
    assert(!ds_media_player_is_playing(mp, NULL, 0, NULL));
    assert(ds_media_player_get_position_ms(mp) == 0);
    assert(ds_media_player_get_duration_ms(mp) == 0);
    assert(ds_media_player_rotation(mp) == 0);

    /* 3. Pause, seek, copy_frame, and teardown guards */
    ds_media_player_set_paused(mp, true);
    assert(ds_media_player_is_paused(mp));
    ds_media_player_set_paused(mp, false);
    assert(!ds_media_player_is_paused(mp));
    ds_media_player_seek_ms(mp, 500);

    uint8_t dummy[64];
    int w = 0, h = 0;
    assert(!ds_media_player_copy_frame(mp, dummy, sizeof(dummy), &w, &h));

    ds_media_player_stop(mp);
    ds_media_player_destroy(mp);
    puts("PASS: media player clamping, progress, lifecycle, pause/seek guards, and clean teardown");
    return 0;
}
