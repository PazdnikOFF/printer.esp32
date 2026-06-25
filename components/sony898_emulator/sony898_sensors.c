#include "sony898_sensors.h"
#include "sony898_status.h"
#include "esp_log.h"
#include <stdatomic.h>

static const char *TAG = "sony898_sensors";

static struct {
    _Atomic(bool) paper_present;
    _Atomic(bool) cover_closed;
    _Atomic(bool) ribbon_present;
    _Atomic(bool) media_match;
    _Atomic(bool) system_error;
} _s;

void sony898_sensor_init(void) {
    atomic_store(&_s.paper_present,  true);
    atomic_store(&_s.cover_closed,   true);
    atomic_store(&_s.ribbon_present, true);
    atomic_store(&_s.media_match,    true);
    atomic_store(&_s.system_error,   false);
}

/* Compute and apply the highest-priority error state, or clear to IDLE. */
static void _apply(void) {
    if (atomic_load(&_s.system_error)) {
        sony898_status_set_state(SONY898_STATE_SYSTEM_ERROR);
        return;
    }
    if (!atomic_load(&_s.cover_closed)) {
        sony898_status_set_state(SONY898_STATE_COVER_OPEN);
        return;
    }
    if (!atomic_load(&_s.ribbon_present)) {
        sony898_status_set_state(SONY898_STATE_NO_RIBBON);
        return;
    }
    if (!atomic_load(&_s.paper_present)) {
        sony898_status_set_state(SONY898_STATE_NO_PAPER);
        return;
    }
    if (!atomic_load(&_s.media_match)) {
        sony898_status_set_state(SONY898_STATE_MEDIA_MISMATCH);
        return;
    }

    /* All sensors OK — only return to IDLE if we were in an error state.
     * Do not override RECEIVING_JOB / PRINTING / JOB_DONE. */
    sony898_state_t cur = sony898_status_get_state();
    switch (cur) {
    case SONY898_STATE_COVER_OPEN:
    case SONY898_STATE_NO_PAPER:
    case SONY898_STATE_NO_RIBBON:
    case SONY898_STATE_NO_MEDIA:
    case SONY898_STATE_MEDIA_MISMATCH:
    case SONY898_STATE_SYSTEM_ERROR:
    case SONY898_STATE_UNKNOWN_ERROR:
        sony898_status_set_state(SONY898_STATE_IDLE);
        break;
    default:
        break;
    }
}

void sony898_sensor_set_paper(bool present) {
    ESP_LOGI(TAG, "paper %s", present ? "present" : "empty");
    atomic_store(&_s.paper_present, present);
    _apply();
}

void sony898_sensor_set_cover(bool closed) {
    ESP_LOGI(TAG, "cover %s", closed ? "closed" : "open");
    atomic_store(&_s.cover_closed, closed);
    _apply();
}

void sony898_sensor_set_ribbon(bool present) {
    ESP_LOGI(TAG, "ribbon %s", present ? "present" : "absent");
    atomic_store(&_s.ribbon_present, present);
    _apply();
}

void sony898_sensor_set_media_match(bool matches) {
    ESP_LOGI(TAG, "media_match=%d", (int)matches);
    atomic_store(&_s.media_match, matches);
    _apply();
}

void sony898_sensor_set_system_error(bool error) {
    ESP_LOGI(TAG, "system_error=%d", (int)error);
    atomic_store(&_s.system_error, error);
    _apply();
}

void sony898_sensor_clear_all(void) {
    ESP_LOGI(TAG, "sensors cleared");
    atomic_store(&_s.system_error,   false);
    atomic_store(&_s.cover_closed,   true);
    atomic_store(&_s.ribbon_present, true);
    atomic_store(&_s.paper_present,  true);
    atomic_store(&_s.media_match,    true);
    _apply();
}

bool sony898_sensor_has_error(void) {
    return atomic_load(&_s.system_error)
        || !atomic_load(&_s.cover_closed)
        || !atomic_load(&_s.ribbon_present)
        || !atomic_load(&_s.paper_present)
        || !atomic_load(&_s.media_match);
}
