#undef NDEBUG
#include "../app/src/main/cpp/explore.c"
#include <assert.h>
#include "../app/src/main/cpp/ui_layout.h"
#include "../app/src/main/cpp/video_orientation.h"

static int attaches, detaches, frames, pops, clears;
static jint env_status = JNI_EDETACHED, attach_status = JNI_OK, frame_status = JNI_OK;
static bool pending;
static JNIEnv test_env;

static jint mock_get_env(JavaVM *vm, void **env, jint version) {
    (void)vm;
    assert(version == JNI_VERSION_1_6);
    if (env_status == JNI_OK) *env = &test_env;
    return env_status;
}
static jint mock_attach(JavaVM *vm, JNIEnv **env, void *args) {
    (void)vm; (void)args;
    ++attaches;
    if (attach_status == JNI_OK) *env = &test_env;
    return attach_status;
}
static jint mock_detach(JavaVM *vm) { (void)vm; ++detaches; return JNI_OK; }
static jint mock_push(JNIEnv *env, jint capacity) {
    assert(env == &test_env && capacity == 32);
    ++frames;
    if (frame_status != JNI_OK) pending = true;
    return frame_status;
}
static jobject mock_pop(JNIEnv *env, jobject result) {
    assert(env == &test_env && result == NULL);
    ++pops;
    return NULL;
}
static jboolean mock_check(JNIEnv *env) { assert(env == &test_env); return pending; }
static void mock_clear(JNIEnv *env) { assert(env == &test_env); pending = false; ++clears; }
static void mock_describe(JNIEnv *env) { assert(env == &test_env); }

static void test_jni_scope(void) {
    const struct JNINativeInterface env_functions = {
        .PushLocalFrame = mock_push, .PopLocalFrame = mock_pop,
        .ExceptionCheck = mock_check, .ExceptionClear = mock_clear, .ExceptionDescribe = mock_describe,
    };
    const struct JNIInvokeInterface vm_functions = {
        .GetEnv = mock_get_env, .AttachCurrentThread = mock_attach, .DetachCurrentThread = mock_detach,
    };
    test_env = &env_functions;
    JavaVM vm = &vm_functions;
    ANativeActivity activity = {.vm = &vm, .clazz = (jobject)(uintptr_t)1};
    DSJniScope scope;
    assert(ds_jni_scope_begin(&scope, &activity));
    assert(scope.env == &test_env && scope.attached);
    assert(ds_jni_scope_end(&scope));
    assert(attaches == 1 && detaches == 1 && frames == 1 && pops == 1);

    env_status = JNI_OK;
    assert(ds_jni_scope_begin(&scope, &activity));
    assert(!scope.attached);
    pending = true;
    assert(!ds_jni_scope_end(&scope));
    assert(attaches == 1 && detaches == 1 && clears == 1 && pops == 2);

    env_status = JNI_EDETACHED;
    frame_status = JNI_ERR;
    assert(!ds_jni_scope_begin(&scope, &activity));
    assert(detaches == 2 && clears == 2 && !pending);
    frame_status = JNI_OK;
    attach_status = JNI_ERR;
    assert(!ds_jni_scope_begin(&scope, &activity));
    assert(detaches == 2);
    assert(!ds_jni_scope_begin(&scope, NULL));
    assert(!ds_explore_open_article(&activity, 0));
    assert(!ds_explore_open_wikidata(&activity, "Q12/evil"));
}

typedef struct { DSExplore *explore; _Atomic bool stop; } Reader;
static void *read_pages(void *arg) {
    Reader *reader = arg;
    do {
        DSExplorePlace places[DS_EXPLORE_PAGE_SIZE];
        size_t count;
        assert(ds_explore_page(reader->explore, "", 0, 5, places, &count) == 2);
        assert(count == 2);
        assert(strcmp(places[0].region, places[1].region) == 0);
    } while (!atomic_load(&reader->stop));
    return NULL;
}

static void test_refresh(const char *directory) {
    DSExplore live, fresh, disk;
    assert(ds_explore_init(&live) && ds_explore_init(&fresh) && ds_explore_init(&disk));
    assert(add_place(&live, 10, "OLD", "Old name", "Q10", true, 1.0, 2.0));
    assert(add_place(&live, 20, "OLD", "Removed page", "", false, 0.0, 0.0));
    snprintf(live.cache_path, sizeof(live.cache_path), "%s/cache.tsv", directory);
    save_cache(&live);
    assert(add_place(&fresh, 10, "NEW", "Updated name", "Q99", true, 3.0, 4.0));
    assert(add_place(&fresh, 30, "NEW", "New page", "", false, 0.0, 0.0));

    Reader reader = {.explore = &live};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, read_pages, &reader) == 0);
    publish_places(&live, &fresh);
    /* Readers see one complete generation, even during rapid publication. */
    for (unsigned i = 0; i < 100; ++i) publish_places(&live, &fresh);
    atomic_store(&reader.stop, true);
    assert(pthread_join(thread, NULL) == 0);

    DSExplorePlace places[DS_EXPLORE_PAGE_SIZE];
    size_t count;
    assert(ds_explore_page(&live, "", 0, 5, places, &count) == 2 && count == 2);
    assert(places[0].page_id == 10 && strcmp(places[0].title, "UPDATED NAME") == 0);
    assert(strcmp(places[0].wikidata_id, "Q99") == 0 && places[0].latitude == 3.0);
    assert(places[1].page_id == 30);
    assert(ds_explore_page(&live, "removed", 0, 5, places, &count) == 0 && count == 0);
    assert(ds_explore_page(&live, "updated", 0, 5, places, &count) == 1 && count == 1);
    assert(ds_explore_page(&live, "", 0, 1, places, &count) == 1 && places[0].page_id == 10 && count == 2);
    assert(ds_explore_page(&live, "", 1, 1, places, &count) == 1 && places[0].page_id == 30);
    assert(ds_explore_page(&live, "", SIZE_MAX, 5, places, &count) == 0 && count == 2);
    assert(ds_explore_page(&live, "", 0, 0, places, &count) == 0 && count == 0);
    assert(ds_explore_page(&live, "", 0, 6, places, &count) == 0 && count == 0);

    save_cache(&live);
    snprintf(disk.cache_path, sizeof(disk.cache_path), "%s", live.cache_path);
    time_t modified = 0;
    assert(load_cache(&disk, &modified) == 2 && modified > 0);
    assert(ds_explore_page(&disk, "updated", 0, 5, places, &count) == 1);
    assert(strcmp(places[0].wikidata_id, "Q99") == 0 && places[0].longitude == 4.0);
    assert(unlink(live.cache_path) == 0);
    ds_explore_destroy(&fresh);
    ds_explore_destroy(&disk);
    ds_explore_destroy(&live);
}

static void test_responsive_pagination(void) {
    DSExplore index;
    assert(ds_explore_init(&index));
    for (int64_t id = 1; id <= 13; ++id)
        assert(add_place(&index, id, "TEST", "Place", "", false, 0, 0));
    for (size_t page_size = 1; page_size <= DS_EXPLORE_PAGE_SIZE; ++page_size) {
        int64_t expected = 1;
        for (size_t page = 0; page <= 13 / page_size; ++page) {
            DSExplorePlace places[DS_EXPLORE_PAGE_SIZE];
            size_t matches;
            const size_t count = ds_explore_page(&index, "", page, page_size, places, &matches);
            assert(matches == 13 && count <= page_size);
            for (size_t i = 0; i < count; ++i) assert(places[i].page_id == expected++);
        }
        assert(expected == 14);
    }
    ds_explore_destroy(&index);
}

static void test_layout(void) {
    for (int width = 240; width <= 1400; width += 40) {
        for (int height = 240; height <= 1400; height += 40) {
            const DSLayout ui = ds_layout((float)width, (float)height);
            assert(ui.body.h > 0 && ui.body.y + ui.body.h <= ui.nav_top);
            assert(ui.field.w > 0 && ui.field.h >= 250);
            assert(ui.capture.x >= ui.body.x && ui.capture.x + ui.capture.w <= ui.body.x + ui.body.w + 0.01f);
            assert(ui.spectrum.y + ui.spectrum.h <= ui.body.y + ui.scan_height + 0.01f);
            assert(ui.explore_page_size >= 1 && ui.explore_page_size <= DS_EXPLORE_PAGE_SIZE);
            assert(ui.explore_rows.y + ui.explore_rows.h <= ui.explore_footer_y - 16.0f + 0.01f);
            assert(!ds_rect_contains(ui.capture, ui.field.x + 1, ui.field.y + 1));
            assert(ds_rect_contains(ui.capture, ui.capture.x + 1, ui.capture.y + 1));
            assert(!ds_rect_contains(ui.capture, ui.capture.x + ui.capture.w, ui.capture.y + 1));
            const float scroll = ds_scroll_limit(ui.scan_height, ui.body);
            assert(scroll >= 0 && ui.body.y + ui.scan_height - scroll <= ui.body.y + ui.body.h + 0.01f);
        }
    }
    const DSLayout portrait = ds_layout(448, 997), landscape = ds_layout(997, 448);
    assert(!portrait.wide && landscape.wide && landscape.compact);
    assert(portrait.explore_page_size == 5 && landscape.explore_page_size < 5);
    assert(portrait.field.y + portrait.field.h < portrait.capture.y);
    assert(landscape.field.x + landscape.field.w < landscape.capture.x);
}

static void test_video_orientation(void) {
    for (int sensor = 0; sensor < 360; sensor += 90) {
        for (int display = 0; display < 4; ++display) {
            const int hint = ds_video_orientation_hint(sensor, display);
            assert(hint >= 0 && hint < 360 && hint % 90 == 0);
            assert((hint + display * 90) % 360 == sensor);
        }
    }
    assert(ds_video_orientation_hint(90, 0) == 90);
    assert(ds_video_orientation_hint(90, 1) == 0);
    assert(ds_video_orientation_hint(90, 3) == 180);
    assert(ds_video_orientation_hint(45, 0) == -1);
    assert(ds_video_orientation_hint(90, 4) == -1);
}

int main(void) {
    char directory[] = "/data/local/tmp/deadspeak-regression-XXXXXX";
    assert(mkdtemp(directory));
    test_jni_scope();
    test_refresh(directory);
    test_responsive_pagination();
    test_layout();
    test_video_orientation();
    assert(rmdir(directory) == 0);
    puts("PASS: JNI ownership, refresh replacement, concurrent readers, cache, responsive layout and pagination");
    return 0;
}
