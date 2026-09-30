#ifndef DEADSPEAK_EXPLORE_H
#define DEADSPEAK_EXPLORE_H

#include <android/native_activity.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DS_EXPLORE_PAGE_SIZE 5

typedef struct {
    int64_t page_id;
    char title[128];
    char region[96];
    char wikidata_id[16];
    double latitude;
    double longitude;
    bool has_coordinates;
} DSExplorePlace;

typedef struct {
    pthread_mutex_t mutex;
    pthread_t thread;
    DSExplorePlace *places;
    size_t count;
    _Atomic bool loading;
    _Atomic bool failed;
    bool thread_created;
    bool requested_once;
    bool force_refresh;
    bool mutex_initialized;
    char cache_path[512];
    ANativeActivity *activity;
} DSExplore;

bool ds_explore_init(DSExplore *explore);
void ds_explore_start(DSExplore *explore, ANativeActivity *activity, const char *cache_path);
void ds_explore_refresh(DSExplore *explore, ANativeActivity *activity, const char *cache_path);
void ds_explore_stop(DSExplore *explore);
void ds_explore_destroy(DSExplore *explore);
size_t ds_explore_page(DSExplore *explore, const char *query, size_t page,
                       DSExplorePlace out[DS_EXPLORE_PAGE_SIZE], size_t *match_count);
bool ds_explore_open_article(ANativeActivity *activity, int64_t page_id);
bool ds_explore_open_wikidata(ANativeActivity *activity, const char *wikidata_id);

#endif
