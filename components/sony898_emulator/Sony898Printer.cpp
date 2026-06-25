#include "Sony898Printer.hpp"

/* All method bodies delegate to the C emulator API.
 * The singleton holds no state — everything lives in the C submodules. */

Sony898Printer Printer;

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

esp_err_t Sony898Printer::init() {
    sony898_config_t cfg = {};
    cfg.vid           = vid;
    cfg.pid           = pid;
    cfg.manufacturer  = manufacturer;
    cfg.product       = product;
    cfg.serial        = serial;
    cfg.on_job_ready  = onJobReady;
    cfg.on_connect    = onConnect;
    cfg.on_disconnect = onDisconnect;
    return sony898_emulator_init(&cfg);
}

void Sony898Printer::begin() {
    sony898_emulator_start();
}

/* ── Identity ────────────────────────────────────────────────────────────── */

uint16_t    Sony898Printer::getVid()          const { return sony898_emulator_get_vid(); }
uint16_t    Sony898Printer::getPid()          const { return sony898_emulator_get_pid(); }
const char *Sony898Printer::getManufacturer() const { return sony898_emulator_get_manufacturer(); }
const char *Sony898Printer::getProduct()      const { return sony898_emulator_get_product(); }
const char *Sony898Printer::getProtocol()     const { return sony898_emulator_get_protocol(); }
const char *Sony898Printer::getUsbClass()     const { return sony898_emulator_get_usb_class(); }
const char *Sony898Printer::getSerial()       const { return sony898_emulator_get_serial(); }
esp_err_t   Sony898Printer::setSerial(const char *s) { return sony898_emulator_set_serial(s); }
const char *Sony898Printer::getDeviceId()     const { return sony898_emulator_get_ieee1284_id(); }

/* ── USB state ───────────────────────────────────────────────────────────── */

bool Sony898Printer::usbIsConnected()  const { return sony898_emulator_usb_is_connected(); }
bool Sony898Printer::usbIsConfigured() const { return sony898_emulator_usb_is_configured(); }
bool Sony898Printer::usbIsReady()      const { return sony898_emulator_usb_is_ready(); }

/* ── Printer state ───────────────────────────────────────────────────────── */

sony898_state_t Sony898Printer::getState()       const { return sony898_emulator_get_state(); }
void            Sony898Printer::setState(sony898_state_t s) { sony898_emulator_set_state(s); }
const char     *Sony898Printer::getStateName()   const { return sony898_emulator_get_state_name(); }
uint8_t         Sony898Printer::getPortStatus()  const { return sony898_emulator_get_port_status(); }
bool            Sony898Printer::isPrinterReady() const { return sony898_emulator_is_printer_ready(); }

/* ── Image / job ─────────────────────────────────────────────────────────── */

bool           Sony898Printer::imageReady()    const { return sony898_emulator_image_ready(); }
uint16_t       Sony898Printer::getWidth()      const { return sony898_emulator_get_width(); }
uint16_t       Sony898Printer::getHeight()     const { return sony898_emulator_get_height(); }
size_t         Sony898Printer::getImageSize()  const { return sony898_emulator_get_image_size(); }
const uint8_t *Sony898Printer::getImageBuffer() const { return sony898_emulator_get_image_buffer(); }
uint8_t        Sony898Printer::getCopies()     const { return sony898_emulator_get_copies(); }
void           Sony898Printer::clearJob()            { sony898_emulator_clear_job(); }

/* ── Sensor inputs ───────────────────────────────────────────────────────── */

void Sony898Printer::setPaper(bool v)       { sony898_emulator_set_paper(v); }
void Sony898Printer::setCover(bool v)       { sony898_emulator_set_cover(v); }
void Sony898Printer::setRibbon(bool v)      { sony898_emulator_set_ribbon(v); }
void Sony898Printer::setMediaMatch(bool v)  { sony898_emulator_set_media_match(v); }
void Sony898Printer::setSystemError(bool v) { sony898_emulator_set_system_error(v); }
void Sony898Printer::clearSensors()         { sony898_emulator_clear_sensors(); }
bool Sony898Printer::hasSensorError() const { return sony898_emulator_has_sensor_error(); }

/* ── Print completion ────────────────────────────────────────────────────── */

void Sony898Printer::notifyDone()          { sony898_emulator_notify_done(); }
void Sony898Printer::notifyError()         { sony898_emulator_notify_error(); }
bool Sony898Printer::hasPrintModule() const { return sony898_emulator_has_print_module(); }

/* ── Statistics ──────────────────────────────────────────────────────────── */

uint32_t Sony898Printer::getPrintCount() const { return sony898_emulator_get_print_count(); }
