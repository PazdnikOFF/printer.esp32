/*
 * Sony UP-D898MD/UP-X898MD USB Printer emulator — TinyUSB custom class driver.
 *
 * USB descriptors confirmed from ТЗ §3:
 *   VID 0x054C  PID 0x0877
 *   Class 7 / SubClass 1 / Protocol 2 (Bidirectional Printer)
 *   EP 0x01 OUT Bulk  EP 0x81 IN Bulk
 *
 * NOTE: ESP32-S3 is Full Speed (12 Mbps).  Bulk endpoint max-packet-size
 * is 64 bytes (FS limit), NOT 512 as on the real HS printer.  The Linux
 * driver negotiates this from the descriptor — protocol behaviour is unchanged.
 */

#include "sony898_usb.h"
#include "sony898_parser.h"
#include "sony898_status.h"
#include "sony898_image.h"
#include "config.h"
#include "esp_log.h"
#include "esp_check.h"
#include "tinyusb.h"
#include "tusb.h"
#include "device/usbd_pvt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdatomic.h>

static const char *TAG = "sony898_usb";

/* ── USB identifiers ─────────────────────────────────────────────────────── */
#define PRINTER_EP_OUT    0x01u
#define PRINTER_EP_IN     0x81u
#define PRINTER_EP_SIZE   64u      /* Full Speed bulk max packet size */

/* ── USB Printer Class requests ───────────────────────────────────────────── */
#define PRINTER_REQ_GET_DEVICE_ID   0x00u
#define PRINTER_REQ_GET_PORT_STATUS 0x01u
#define PRINTER_REQ_SOFT_RESET      0x02u

/* ── DMA buffers — must be in DRAM, 4-byte aligned ───────────────────────── */
static uint8_t _out_buf[512]    __attribute__((aligned(4)));
static uint8_t _in_buf[512]     __attribute__((aligned(4)));
static uint8_t _dev_id_buf[512] __attribute__((aligned(4)));  /* rebuilt per GET_DEVICE_ID */

/* ── Connection state tracked via TinyUSB callbacks ──────────────────────── */
static volatile atomic_bool _connected  = false;
static volatile atomic_bool _configured = false;

/* ── Connection event callbacks ───────────────────────────────────────────── */
static sony898_usb_event_cb_t _connect_cb    = NULL;
static sony898_usb_event_cb_t _disconnect_cb = NULL;

void sony898_usb_set_connect_cb(sony898_usb_event_cb_t cb)    { _connect_cb    = cb; }
void sony898_usb_set_disconnect_cb(sony898_usb_event_cb_t cb) { _disconnect_cb = cb; }

/* ── Custom printer class driver state ───────────────────────────────────── */
static struct {
    uint8_t itf_num;
    uint8_t ep_out;
    uint8_t ep_in;
    bool    open;
    bool    in_busy;   /* true while IN transfer is in flight */
} _prt;

/* ── Device descriptor (ТЗ §3) ───────────────────────────────────────────── */
static tusb_desc_device_t const _desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = CFG_USB_VID,
    .idProduct          = CFG_USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1,
};

/* ── Configuration + interface + endpoint descriptors ────────────────────── */
#define DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + 9u + 7u + 7u)

static uint8_t const _desc_config[] = {
    /* Configuration */
    TUD_CONFIG_DESCRIPTOR(1, /*num_itf=*/1, /*string_idx=*/0,
                          DESC_TOTAL_LEN, /*attribute=*/0x00, /*power_ma=*/100),

    /* Interface: Printer, Bidirectional */
    9, TUSB_DESC_INTERFACE, /*itf=*/0, /*alt=*/0, /*ep_cnt=*/2,
       TUSB_CLASS_PRINTER, /*subclass=*/0x01, /*protocol=*/0x02, /*iface_str=*/0,

    /* EP 0x01 OUT Bulk 64 */
    7, TUSB_DESC_ENDPOINT, PRINTER_EP_OUT, TUSB_XFER_BULK,
       U16_TO_U8S_LE(PRINTER_EP_SIZE), 0,

    /* EP 0x81 IN Bulk 64 */
    7, TUSB_DESC_ENDPOINT, PRINTER_EP_IN,  TUSB_XFER_BULK,
       U16_TO_U8S_LE(PRINTER_EP_SIZE), 0,
};

/* ── String descriptors ───────────────────────────────────────────────────── */
static char _serial_str[32] = CFG_USB_SERIAL;  /* filled at init from config */

static char const *_desc_strings[] = {
    (const char[]) { 0x09, 0x04 },   /* 0: LANGID = English (0x0409) */
    CFG_USB_MANUFACTURER,             /* 1: Manufacturer */
    CFG_USB_PRODUCT,                  /* 2: Product */
    _serial_str,                      /* 3: Serial — runtime per-unit value */
};

/*
 * tud_descriptor_device_cb / tud_descriptor_configuration_cb /
 * tud_descriptor_string_cb are NOT defined here.
 * esp_tinyusb (descriptors_control.c) provides them and reads the
 * descriptors from the tinyusb_config_t passed to tinyusb_driver_install().
 */

/* ── Connection/configuration state callbacks ────────────────────────────── */

void tud_mount_cb(void) {
    atomic_store(&_connected,  true);
    atomic_store(&_configured, true);
    ESP_LOGI(TAG, "USB connected and configured");
    if (_connect_cb) _connect_cb();
}

void tud_umount_cb(void) {
    /*
     * Called on libusb_reset_device() (bus reset from Gutenprint driver).
     * For physical cable unplug the suspend callback fires instead.
     * Use atomic_exchange to avoid double-firing if suspend already ran.
     */
    bool was_connected = atomic_exchange(&_connected, false);
    atomic_store(&_configured, false);

    if (sony898_parser_get_state() == PARSER_JOB_COMPLETE) {
        sony898_parser_prepare_for_next_job();
    } else {
        sony898_parser_reset();
    }

    if (was_connected) {
        ESP_LOGW(TAG, "USB unmounted (bus reset) — parser reset for next job");
        if (_disconnect_cb) _disconnect_cb();
    } else {
        ESP_LOGD(TAG, "USB unmounted (already disconnected)");
    }
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    /*
     * Physical cable unplug arrives here as a suspend event on ESP32-S3 —
     * tud_umount_cb is NOT called in that case.  Update state here so that
     * polling in status_log_task detects the disconnect within one cycle.
     */
    bool was_connected = atomic_exchange(&_connected, false);
    atomic_store(&_configured, false);
    if (was_connected) {
        if (sony898_parser_get_state() == PARSER_JOB_COMPLETE) {
            sony898_parser_prepare_for_next_job();
        } else {
            sony898_parser_reset();
        }
        ESP_LOGW(TAG, "USB suspended / cable unplugged — parser reset");
        if (_disconnect_cb) _disconnect_cb();
    } else {
        ESP_LOGD(TAG, "USB suspended (already disconnected)");
    }
}

void tud_resume_cb(void) {
    ESP_LOGD(TAG, "USB resumed");
}

/* ── Bulk IN: send status response ───────────────────────────────────────── */

static void send_bulk_status(void) {
    if (!_prt.open || _prt.in_busy) return;
    size_t len = sony898_status_get_bulk_len();
    if (len > sizeof(_in_buf)) len = sizeof(_in_buf);
    memcpy(_in_buf, sony898_status_get_bulk_status(), len);
    _prt.in_busy = true;
    if (!usbd_edpt_xfer(TUD_OPT_RHPORT, _prt.ep_in, _in_buf, (uint16_t)len)) {
        ESP_LOGE(TAG, "bulk IN queue failed (ep=0x%02x len=%u)", _prt.ep_in, (unsigned)len);
        _prt.in_busy = false;
    } else {
        ESP_LOGD(TAG, "bulk IN queued %u bytes", (unsigned)len);
    }
}

/* ── Custom Printer class driver ─────────────────────────────────────────── */

static void printer_init(void) {
    memset(&_prt, 0, sizeof(_prt));
}

static void printer_reset(uint8_t rhport) {
    (void)rhport;
    memset(&_prt, 0, sizeof(_prt));
}

static uint16_t printer_open(uint8_t rhport,
                              tusb_desc_interface_t const *desc_intf,
                              uint16_t max_len) {
    TU_VERIFY(TUSB_CLASS_PRINTER    == desc_intf->bInterfaceClass   &&
              0x01u                 == desc_intf->bInterfaceSubClass &&
              0x02u                 == desc_intf->bInterfaceProtocol, 0);

    uint16_t drv_len = (uint16_t)(sizeof(tusb_desc_interface_t) +
                       desc_intf->bNumEndpoints * sizeof(tusb_desc_endpoint_t));
    TU_VERIFY(max_len >= drv_len, 0);

    _prt.itf_num = desc_intf->bInterfaceNumber;

    uint8_t const *p = tu_desc_next(desc_intf);
    for (uint8_t i = 0; i < desc_intf->bNumEndpoints; i++) {
        tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const *)p;
        TU_ASSERT(TUSB_DESC_ENDPOINT == ep->bDescriptorType, 0);
        TU_ASSERT(usbd_edpt_open(rhport, ep), 0);
        if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_OUT) {
            _prt.ep_out = ep->bEndpointAddress;
        } else {
            _prt.ep_in = ep->bEndpointAddress;
        }
        p = tu_desc_next(p);
    }

    _prt.open    = true;
    _prt.in_busy = false;

    /* arm the OUT endpoint */
    usbd_edpt_xfer(rhport, _prt.ep_out, _out_buf, sizeof(_out_buf));

    /* proactively queue a status response on IN */
    send_bulk_status();

    ESP_LOGI(TAG, "printer interface open: ep_out=0x%02x ep_in=0x%02x",
             _prt.ep_out, _prt.ep_in);
    return drv_len;
}

static bool printer_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                     tusb_control_request_t const *req) {
    if (stage != CONTROL_STAGE_SETUP) return true;

    if (req->bmRequestType_bit.type      != TUSB_REQ_TYPE_CLASS   ||
        req->bmRequestType_bit.recipient != TUSB_REQ_RCPT_INTERFACE ||
        req->wIndex != _prt.itf_num) {
        return false;
    }

    switch (req->bRequest) {

    case PRINTER_REQ_GET_DEVICE_ID: {
        size_t str_len = sony898_status_get_ieee1284_len();
        if (str_len > sizeof(_dev_id_buf) - 2) str_len = sizeof(_dev_id_buf) - 2;
        uint16_t total = (uint16_t)(str_len + 2);
        _dev_id_buf[0] = (uint8_t)(total >> 8);
        _dev_id_buf[1] = (uint8_t)(total & 0xFF);
        memcpy(_dev_id_buf + 2, sony898_status_get_ieee1284_id(), str_len);
        uint16_t resp_len = tu_min16(req->wLength, total);
        ESP_LOGD(TAG, "GET_DEVICE_ID → %u bytes", resp_len);
        return tud_control_xfer(rhport, req, _dev_id_buf, resp_len);
    }

    case PRINTER_REQ_GET_PORT_STATUS: {
        static uint8_t port_status;
        port_status = sony898_status_get_port_status();
        ESP_LOGD(TAG, "GET_PORT_STATUS → 0x%02X (%s)", port_status,
                 (port_status & 0x08) ? "ready" : "fault");
        return tud_control_xfer(rhport, req, &port_status, 1);
    }

    case PRINTER_REQ_SOFT_RESET:
        ESP_LOGI(TAG, "SOFT_RESET");
        sony898_parser_reset();
        return tud_control_status(rhport, req);

    default:
        return false;
    }
}

static bool printer_xfer_cb(uint8_t rhport, uint8_t ep_addr,
                             xfer_result_t result, uint32_t xferred_bytes) {
    if (ep_addr == _prt.ep_out) {
        if (result == XFER_RESULT_SUCCESS && xferred_bytes > 0) {
            /*
             * All bulk OUT data is print job content.
             * Gutenprint's gutenprint53+usb backend never sends status queries
             * via bulk OUT — status is always read via GET_DEVICE_ID (control).
             */
            sony898_parser_feed(_out_buf, xferred_bytes);
        }
        /* re-arm OUT endpoint */
        usbd_edpt_xfer(rhport, _prt.ep_out, _out_buf, sizeof(_out_buf));

    } else if (ep_addr == _prt.ep_in) {
        _prt.in_busy = false;
        ESP_LOGD(TAG, "bulk IN %"PRIu32" bytes sent", xferred_bytes);
        send_bulk_status();
    }

    return true;
}

static usbd_class_driver_t const _printer_driver = {
#if CFG_TUSB_DEBUG >= CFG_TUD_LOG_LEVEL
    .name            = "PRINTER",
#endif
    .init            = printer_init,
    .reset           = printer_reset,
    .open            = printer_open,
    .control_xfer_cb = printer_control_xfer_cb,
    .xfer_cb         = printer_xfer_cb,
    .sof             = NULL,
};

/* Override TinyUSB weak symbol to register our custom class driver. */
usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &_printer_driver;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

esp_err_t sony898_usb_init(const char *serial) {
    if (serial && serial[0]) {
        strncpy(_serial_str, serial, sizeof(_serial_str) - 1);
        _serial_str[sizeof(_serial_str) - 1] = '\0';
    }

    tinyusb_config_t cfg = {
        .device_descriptor        = &_desc_device,
        .string_descriptor        = _desc_strings,
        .string_descriptor_count  = TU_ARRAY_SIZE(_desc_strings),
        .external_phy             = false,
        .configuration_descriptor = _desc_config,
    };

    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&cfg),
                        TAG, "tinyusb_driver_install failed");

    ESP_LOGI(TAG, "USB printer: VID=%04X PID=%04X  serial=%s",
             CFG_USB_VID, CFG_USB_PID, _serial_str);
    ESP_LOGI(TAG, "  %s / %s  Class 7/1/2  EP OUT 0x%02X  EP IN 0x%02X",
             CFG_USB_MANUFACTURER, CFG_USB_PRODUCT,
             PRINTER_EP_OUT, PRINTER_EP_IN);
    return ESP_OK;
}

bool sony898_usb_is_connected(void) {
    return atomic_load(&_connected);
}

bool sony898_usb_is_configured(void) {
    return atomic_load(&_configured);
}

bool sony898_usb_is_ready_for_print(void) {
    return sony898_usb_is_connected()  &&
           sony898_usb_is_configured() &&
           sony898_status_is_ready()   &&
           sony898_parser_can_accept_job();
}

static void _usb_device_task(void *arg) {
    (void)arg;
    while (1) {
        tud_task();
        taskYIELD();
    }
}

void sony898_usb_start_task(void) {
    xTaskCreate(_usb_device_task, "usb", 4096, NULL,
                configMAX_PRIORITIES - 1, NULL);
}
