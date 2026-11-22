#undef NDEBUG
#include "../app/src/main/cpp/sensor_state.h"
#include <assert.h>
#include <float.h>
#include <stdio.h>

static void test_setup_and_access(void) {
    DSSensorReading reading = {0};
    const int64_t start = INT64_C(1000000000);
    assert(!reading.valid && !ds_sensor_live(&reading, start));
    ds_sensor_configure(&reading, false, true, 0, 0, start);
    assert(reading.status == DS_SENSOR_ABSENT && !reading.enabled);
    ds_sensor_configure(&reading, true, false, 0, 0, start);
    assert(reading.status == DS_SENSOR_BLOCKED && !reading.enabled);
    ds_sensor_configure(&reading, true, true, -1, 0, start);
    assert(reading.status == DS_SENSOR_BLOCKED && reading.error == -1 && !reading.enabled);
    ds_sensor_configure(&reading, true, true, 0, -22, start);
    assert(reading.status == DS_SENSOR_BLOCKED && reading.error == -22 && !reading.enabled);
    assert(ds_sensor_accept(&reading, start + 1, start + 2, 3, 4, 0, 3) == DS_SAMPLE_REJECTED);
    ds_sensor_configure(&reading, true, true, 0, 0, start);
    assert(reading.status == DS_SENSOR_WAITING && !reading.valid && reading.count == 0);
    assert(ds_sensor_accept(&reading, start + 1, start + 2, 3, 4, 0, 3) == DS_SAMPLE_FIRST);
    assert(reading.magnitude == 5 && reading.xyz[0] == 3 && reading.xyz[1] == 4 && reading.count == 1);
    assert(ds_sensor_live(&reading, start + 2));
    assert(ds_sensor_accept(&reading, start + 2, start + 3, 0, 0, 60, 3) == DS_SAMPLE_NEXT);
    assert(reading.magnitude == 60 && reading.count == 2);
    reading.enabled = false;
    ds_sensor_clear(&reading, DS_SENSOR_BLOCKED);
    assert(!ds_sensor_live(&reading, start + 4) && !reading.valid && reading.magnitude == 0);
    assert(ds_sensor_accept(&reading, start + 3, start + 4, 0, 0, 61, 3) == DS_SAMPLE_REJECTED);
    assert(reading.count == 2);
    ds_sensor_configure(&reading, true, true, 0, 0, start + 100);
    assert(ds_sensor_accept(&reading, start + 4, start + 101, 1, 1, 1, 3) == DS_SAMPLE_REJECTED);
    assert(ds_sensor_accept(&reading, start + 101, start + 102, 0, 0, 62, 3) == DS_SAMPLE_FIRST);
    assert(reading.magnitude == 62 && reading.count == 3);
}
static void test_invalid_and_freshness(void) {
    DSSensorReading reading = {0};
    const int64_t start = INT64_C(10000000000);
    ds_sensor_configure(&reading, true, true, 0, 0, start);
    const int64_t bad_times[] = {0, -1, start - 1, start + 10};
    for (size_t i = 0; i < sizeof(bad_times) / sizeof(bad_times[0]); ++i) {
        assert(ds_sensor_accept(&reading, bad_times[i], start + 1, 0, 0, 60, 3) == DS_SAMPLE_REJECTED);
        assert(!reading.valid && !reading.count);
    }
    assert(ds_sensor_accept(&reading, start + 1, start + 2, NAN, 0, 1, 3) == DS_SAMPLE_REJECTED);
    assert(ds_sensor_accept(&reading, start + 1, start + 2, 1, INFINITY, 1, 3) == DS_SAMPLE_REJECTED);
    assert(ds_sensor_accept(&reading, start + 1, start + 2, FLT_MAX, FLT_MAX, FLT_MAX, 3) == DS_SAMPLE_REJECTED);
    assert(ds_sensor_accept(&reading, start + 1, start + 2, 1, 0, 1, -1) == DS_SAMPLE_REJECTED);
    assert(ds_sensor_accept(&reading, start + 1, start + 2, 0, 0, 60, 3) == DS_SAMPLE_FIRST);
    assert(ds_sensor_accept(&reading, start + 1, start + 2, 0, 0, 61, 3) == DS_SAMPLE_REJECTED);
    assert(reading.count == 1 && !reading.valid);
    assert(ds_sensor_accept(&reading, start + 2, start + 3, 0, 0, 62, 0) == DS_SAMPLE_FIRST);
    assert(reading.accuracy == 0 && reading.count == 2);
    assert(ds_sensor_live(&reading, start + 2 + DS_SENSOR_FRESH_NS));
    ds_sensor_expire(&reading, start + 3 + DS_SENSOR_FRESH_NS);
    assert(reading.status == DS_SENSOR_STALE && !reading.valid && reading.magnitude == 0);
    assert(ds_sensor_accept(&reading, start + 3, start + 4 + DS_SENSOR_FRESH_NS, 0, 0, 63, 3) == DS_SAMPLE_REJECTED);
    const int64_t now = start + 5 + DS_SENSOR_FRESH_NS;
    assert(ds_sensor_accept(&reading, now, now, 0, 0, 64, 3) == DS_SAMPLE_FIRST);
    assert(reading.count == 3 && reading.magnitude == 64);
}
static void test_three_real_vectors(void) {
    DSSensorReading readings[3] = {0};
    const float xyz[3][3] = {{10,20,50},{0,0,9.81f},{0.1f,-0.2f,0.3f}};
    for (unsigned i = 0; i < 3; ++i) {
        ds_sensor_configure(&readings[i], true, true, 0, 0, 1);
        assert(ds_sensor_accept(&readings[i], 2, 3, xyz[i][0], xyz[i][1], xyz[i][2], 3) == DS_SAMPLE_FIRST);
        for (unsigned axis = 0; axis < 3; ++axis) assert(readings[i].xyz[axis] == xyz[i][axis]);
        assert(readings[i].timestamp_ns == 2 && readings[i].count == 1);
    }
    ds_sensor_configure(&readings[0], true, false, 0, 0, 4);
    assert(!ds_sensor_live(&readings[0], 4));
    assert(ds_sensor_live(&readings[1], 4) && ds_sensor_live(&readings[2], 4));
}
static void test_proximity_telemetry(void) {
    DSProximityState prox;
    ds_proximity_init(&prox);
    assert(!prox.near_active && prox.break_count == 0U && ds_proximity_surge(&prox, 100ULL) == 0.0f);
    assert(!ds_proximity_update(&prox, NAN, 100ULL));
    assert(!ds_proximity_update(&prox, -1.0f, 100ULL));
    assert(!ds_proximity_update(&prox, 5.0f, 100ULL));
    assert(prox.baseline_ready && prox.baseline_cm == 5.0f && !prox.near_active);
    assert(ds_proximity_update(&prox, 0.0f, 200ULL));
    assert(prox.near_active && prox.break_count == 1U && ds_proximity_surge(&prox, 250ULL) == 1.0f);
    assert(fabsf(ds_proximity_delta(&prox) - (-5.0f)) < 0.01f);
    assert(!ds_proximity_update(&prox, 1.0f, 300ULL));
    assert(prox.near_active && prox.break_count == 1U);
    assert(!ds_proximity_update(&prox, 5.0f, 500ULL));
    assert(!prox.near_active && prox.break_count == 1U);
    assert(ds_proximity_surge(&prox, 1750ULL) > 0.4f && ds_proximity_surge(&prox, 1750ULL) < 0.6f);
    assert(ds_proximity_surge(&prox, 3100ULL) == 0.0f);
    assert(ds_proximity_update(&prox, 2.5f, 3200ULL));
    assert(prox.near_active && prox.break_count == 2U);
}
int main(void) {
    test_setup_and_access(); test_invalid_and_freshness(); test_three_real_vectors(); test_proximity_telemetry();
    puts("PASS: sensor access failures, no seeded readings, first real sample, finite/monotonic/fresh samples, pause/revoke/re-enable and three independent vectors");
    return 0;
}
