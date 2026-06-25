# CLAUDE.md — sony898_emulator

Этот файл для AI-ассистентов. Описывает как правильно интегрировать и использовать компонент `sony898_emulator` в ESP32-S3 проекте.

---

## Что делает компонент

Эмулирует USB-принтер Sony UP-D898MD / UP-X898MD перед хостом (Linux/CUPS/Gutenprint). Принимает задания печати по USB, разбирает протокол SPDL-DS2, складывает grayscale-изображение в PSRAM и отдаёт через callback. Состояние принтера отдаётся хосту через IEEE1284 Device ID.

---

## Файловая структура компонента

```
components/sony898_emulator/
├── Sony898Printer.hpp       ← C++ класс (основной публичный API)
├── Sony898Printer.cpp
├── sony898_emulator.h       ← C API (для C-проектов)
├── sony898_emulator.c
├── config.h                 ← compile-time умолчания (VID/PID/SN/таймеры)
├── sony898_usb.c/h          ← TinyUSB custom class driver (внутренний)
├── sony898_parser.c/h       ← SPDL-DS2 парсер (внутренний)
├── sony898_status.c/h       ← IEEE1284 / port-status (внутренний)
├── sony898_image.c/h        ← PSRAM буфер (внутренний)
├── sony898_sensors.c/h      ← GPIO сенсоры (внутренний)
├── sony898_printer_iface.c/h← интерфейс с модулем печати (внутренний)
└── sony898_counter.c/h      ← NVS счётчик (внутренний)
```

**Правило**: включать только `Sony898Printer.hpp` (C++) или `sony898_emulator.h` (C). Внутренние заголовки не трогать.

---

## Первичный API: C++ класс `Sony898Printer`

### Глобальный синглтон

Определён в `Sony898Printer.cpp`, доступен из любого файла:
```cpp
#include "Sony898Printer.hpp"
// Printer доступен глобально
```

### Инициализация в `app_main`

```cpp
extern "C" void app_main() {
    // 1. NVS обязателен — хранит серийник и счётчик
    nvs_flash_init();  // + обработка ESP_ERR_NVS_NO_FREE_PAGES

    // 2. Поля класса задаются ДО init()
    Printer.serial    = nullptr;          // nullptr → NVS → config.h fallback
    Printer.onJobReady = my_print_cb;     // nullptr → встроенный таймер

    // 3. Необязательные поля идентичности (0/nullptr → config.h)
    Printer.vid          = 0;
    Printer.pid          = 0;
    Printer.manufacturer = nullptr;
    Printer.product      = nullptr;

    // 4. USB события (nullptr → игнорировать)
    Printer.onConnect    = on_usb_connect;
    Printer.onDisconnect = on_usb_disconnect;

    // 5. Инициализировать и запустить
    ESP_ERROR_CHECK(Printer.init());
    Printer.begin();   // запускает USB task + status task
}
```

### Конструктор с серийным номером

```cpp
// Два способа задать SN:
Sony898Printer Printer;               // SN из NVS (серийное производство)
Sony898Printer Printer("A1234567");   // SN зашит в прошивку
```

Глобальный синглтон в `Sony898Printer.cpp` — `Sony898Printer Printer;` (без аргумента). Не переопределять.

---

## Callback печати

```cpp
// Вызывается из status_task — НЕ блокировать, НЕ вызывать notifyDone() здесь
static void my_print_cb(const sony898_print_job_t *job) {
    // job->buf    — PSRAM, read-only, не освобождать
    // job->width  — ширина px
    // job->height — высота px
    // job->size   — width * height байт, grayscale 8-bit
    // job->copies — кол-во копий из PDL-заголовка, >= 1
    xTaskNotifyGive(print_task_handle);  // передать в задачу печати
}

// В задаче печати:
void print_task(void *arg) {
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // ... физическая печать ...
        Printer.notifyDone();    // успех → JOB_DONE
        // или
        Printer.notifyError();   // ошибка → SYSTEM_ERROR
    }
}
```

**Критично**: `notifyDone()` / `notifyError()` вызывать только из задачи печати, не из callback-а.

---

## Сенсоры физического принтера

```cpp
// Вызывать из GPIO ISR или задачи мониторинга сенсоров
Printer.setPaper(gpio_get_level(PIN_PAPER));    // false → SONY898_STATE_NO_PAPER
Printer.setCover(gpio_get_level(PIN_COVER));    // false → SONY898_STATE_COVER_OPEN
Printer.setRibbon(gpio_get_level(PIN_RIBBON));  // false → SONY898_STATE_NO_RIBBON
Printer.setMediaMatch(media_ok);                // false → SONY898_STATE_MEDIA_MISMATCH
Printer.setSystemError(hw_fault);              // true  → SONY898_STATE_SYSTEM_ERROR
```

Приоритет при нескольких ошибках одновременно:
`system_error` > `cover_open` > `no_ribbon` > `no_paper` > `media_mismatch`

Состояние обновляется немедленно и видно хосту на следующем `GET_DEVICE_ID`.

---

## Серийный номер

Три уровня приоритета (высший → низший):
1. Поле `Printer.serial` (или аргумент конструктора) — runtime override
2. NVS namespace `"sony898"`, key `"serial"` — записывается командой `set_serial`
3. `CFG_USB_SERIAL` в `config.h` — compile-time fallback, только при пустом NVS

**Записать через UART** (производственный стенд):
```
set_serial A1234567
→ OK: saved — reboot to apply: A1234567
```
После reboot применяется из NVS.

**Программно** (записать в NVS, применяется после reboot):
```cpp
Printer.setSerial("A1234567");
```

**Формат**: до 16 символов. В IEEE1284 Device ID поле SCSNO: `"A1234567--------"` (дополняется `-` до 16).

---

## Состояния принтера

```cpp
typedef enum {
    SONY898_STATE_IDLE,
    SONY898_STATE_RECEIVING_JOB,
    SONY898_STATE_PRINTING,
    SONY898_STATE_JOB_DONE,
    SONY898_STATE_COVER_OPEN,
    SONY898_STATE_NO_PAPER,
    SONY898_STATE_NO_RIBBON,
    SONY898_STATE_NO_MEDIA,
    SONY898_STATE_MEDIA_MISMATCH,
    SONY898_STATE_SYSTEM_ERROR,
} sony898_state_t;
```

Состояние определяет что хост видит в ответах `GET_DEVICE_ID` и `GET_PORT_STATUS`. Сенсорные методы (`set*`) переключают состояние автоматически по приоритету.

Жизненный цикл задания:
```
IDLE → (bulk OUT от хоста) → RECEIVING_JOB → (image готово) →
PRINTING → (notifyDone) → JOB_DONE → (2 сек) → IDLE
```

---

## Опросные методы

```cpp
// USB
bool Printer.usbIsConnected();   // VBUS + enumeration
bool Printer.usbIsConfigured();  // SET_CONFIGURATION получен
bool Printer.usbIsReady();       // connected + configured + IDLE/JOB_DONE + parser idle

// Состояние
sony898_state_t Printer.getState();
const char     *Printer.getStateName();   // "idle", "printing", ...

// Изображение
bool           Printer.imageReady();
uint16_t       Printer.getWidth();
uint16_t       Printer.getHeight();
size_t         Printer.getImageSize();
const uint8_t *Printer.getImageBuffer();  // PSRAM, не освобождать
uint8_t        Printer.getCopies();
void           Printer.clearJob();        // освободить PSRAM, сбросить парсер

// Статистика
uint32_t Printer.getPrintCount();         // из NVS, не сбрасывается

// Идентичность (актуальны после init())
uint16_t    Printer.getVid();
uint16_t    Printer.getPid();
const char *Printer.getManufacturer();
const char *Printer.getProduct();
const char *Printer.getSerial();
const char *Printer.getProtocol();    // "SPJL-DS,SPDL-DS2"
const char *Printer.getUsbClass();    // "7/1/2 Printer Bidirectional"
const char *Printer.getDeviceId();    // полный IEEE1284 Device ID
```

---

## CMakeLists.txt проекта

В `src/CMakeLists.txt` (или `main/CMakeLists.txt` для IDF-проекта):
```cmake
idf_component_register(
    SRCS "main.cpp"
    PRIV_REQUIRES
        sony898_emulator
        driver
        nvs_flash
        log
)
```

Если появляется ошибка **"Couldn't find the main target"** — проверить что glob не ограничен `*.c` при использовании `.cpp` файлов.

---

## Задачи FreeRTOS внутри компонента

| Задача | Приоритет | Стек | Описание |
|---|---|---|---|
| `usb` | `configMAX_PRIORITIES - 1` | 4096 | TinyUSB `tud_task()` |
| `sony898_st` | 4 | 3072 | Диспетчер заданий, мониторинг USB |

Запускаются внутри `Printer.begin()`. Не запускать вручную.

---

## Поведение USB disconnect на ESP32-S3

На ESP32-S3 физическое отключение кабеля вызывает `tud_suspend_cb`, а НЕ `tud_umount_cb`. Это обрабатывается внутри компонента. `tud_umount_cb` вызывается при `libusb_reset_device()` (программный reset от Gutenprint). Оба случая корректно обрабатываются: сбрасывают парсер и вызывают `onDisconnect`.

---

## config.h — что можно менять

```c
#define CFG_USB_VID          0x054Cu        // USB Vendor ID (fallback)
#define CFG_USB_PID          0x0877u        // USB Product ID (fallback)
#define CFG_USB_MANUFACTURER "Sony"         // iManufacturer (fallback)
#define CFG_USB_PRODUCT      "UP-D898MD_X898MD" // iProduct (fallback)
#define CFG_USB_SERIAL       "0000000"      // iSerial (fallback, нормально задаётся NVS)

#define CFG_MAX_IMAGE_BYTES  (6u * 1024u * 1024u)  // лимит PSRAM под изображение
#define CFG_PRINT_SPEED_LPS  133u           // строк/сек для встроенного таймера
#define CFG_JOB_DONE_IDLE_DELAY_MS  2000u  // задержка JOB_DONE → IDLE
```

Все идентификаторы (VID/PID/Manufacturer/Product/Serial) переопределяются в рантайме через поля класса — `config.h` служит только compile-time fallback.

---

## Частые ошибки

| Ошибка | Причина | Решение |
|---|---|---|
| Linker: undefined reference к `sony898_sensors` и др. | Стale CMake cache | `rm -rf .pio/build/<env>` |
| `Couldn't find the main target` | `CMakeLists.txt` ищет `*.c`, файл `.cpp` | Указать файл явно: `SRCS "main.cpp"` |
| SN не применяется после `set_serial` | Нет reboot | После `set_serial` сделать reboot |
| `onJobReady` не вызывается | Не задан до `init()` | Установить поле до `Printer.init()` |
| `notifyDone()` вызван из callback | Дедлок в state machine | Вызывать только из отдельной задачи |
| LSP ошибки: `'esp_err.h' file not found` | clang не видит IDF headers | Ложные срабатывания, компилятор ESP-IDF ошибок не даёт |

---

## C API (альтернатива для C-проектов)

```c
#include "sony898_emulator.h"

// Минимальная инициализация
sony898_config_t cfg = {0};
cfg.serial       = NULL;          // NULL → NVS → config.h
cfg.on_job_ready = my_callback;
ESP_ERROR_CHECK(sony898_emulator_init(&cfg));
sony898_emulator_start();

// Опрос
sony898_state_t s = sony898_emulator_get_state();
bool ready        = sony898_emulator_usb_is_ready();
uint32_t cnt      = sony898_emulator_get_print_count();

// Сенсоры
sony898_emulator_set_paper(true);
sony898_emulator_set_cover(false);   // → COVER_OPEN

// Завершение печати
sony898_emulator_notify_done();
```

`sony898_config_t` поля: `vid`, `pid`, `manufacturer`, `product`, `serial`, `on_job_ready`, `on_connect`, `on_disconnect`. Все опциональны — 0/NULL дают compile-time умолчания.
