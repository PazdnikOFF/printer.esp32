#include "sony898_printer_iface.h"
#include "sony898_status.h"
#include "esp_log.h"
#include <stdatomic.h>

static const char *TAG = "sony898_iface";

static sony898_on_job_ready_cb_t _cb        = NULL;
static _Atomic(bool)             _done_flag  = false;
static _Atomic(bool)             _error_flag = false;

void sony898_printer_iface_set_callback(sony898_on_job_ready_cb_t cb) {
    _cb = cb;
    ESP_LOGI(TAG, "print module %s", cb ? "registered" : "unregistered");
}

void sony898_printer_notify_done(void) {
    atomic_store(&_done_flag, true);
}

void sony898_printer_notify_error(void) {
    atomic_store(&_error_flag, true);
}

bool sony898_printer_iface_has_module(void) {
    return _cb != NULL;
}

bool sony898_printer_iface_dispatch(const sony898_print_job_t *job) {
    if (!_cb) return false;
    atomic_store(&_done_flag,  false);
    atomic_store(&_error_flag, false);
    ESP_LOGI(TAG, "dispatching job %"PRIu16"x%"PRIu16" copies=%u",
             job->width, job->height, job->copies);
    _cb(job);
    return true;
}

bool sony898_printer_iface_check_done(void) {
    return atomic_exchange(&_done_flag, false);
}

bool sony898_printer_iface_check_error(void) {
    return atomic_exchange(&_error_flag, false);
}
