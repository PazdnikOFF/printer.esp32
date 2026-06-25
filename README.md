# sony898_emulator

Компонент для ESP32-S3, эмулирует Sony UP-D898MD / UP-X898MD перед USB-хостом. Хост отправляет задание печати, компонент разбирает его, складывает grayscale-изображение в PSRAM и отдаёт вашему модулю печати через C++ API или C API.

---

## Железо

Целевая плата: **ESP32-S3-N16R8**

| Ресурс | Значение |
|---|---|
| Flash | 16 MB |
| PSRAM | 8 MB (OPI) |
| USB | Native USB Device (Full Speed, 12 Mbps) |
| Фреймворк | ESP-IDF + TinyUSB |

Изображение 1280×960 занимает ~1.2 MB в PSRAM. Лимит буфера — 6 MB (`CFG_MAX_IMAGE_BYTES`).

---

## Что видит хост

```
Bus 005 Device 010: ID 054c:0877 Sony Corp. UP-D898/X898 series

bcdUSB               2.00
idVendor             0x054c  Sony Corp.
idProduct            0x0877
iManufacturer        Sony
iProduct             UP-D898MD_X898MD
iSerial              7150628
bInterfaceClass      7  Printer
bInterfaceProtocol   2  Bidirectional
EP 0x01 OUT  Bulk 64 bytes   (FS — реальный принтер HS, 512)
EP 0x81 IN   Bulk 64 bytes
```

Linux, очередь CUPS:
```bash
sudo lpadmin -p sony898 -E \
  -v "usb://Sony/UP-D898MD_X898MD?serial=7150628" \
  -m gutenprint.5.3://sony-upd898md/expert
```

---

## Как это работает изнутри

Хост шлёт задание в формате SPJL-DS / SPDL-DS2 — это PJL-обёртка вокруг бинарного PDL-блока. Компонент делает следующее:

1. Ждёт маркер `@PJL ENTER LANGUAGE=SONY-PDL-DS2\r\n` в потоке Bulk OUT
2. Читает 290-байтный PDL-заголовок, вытаскивает ширину, высоту, кол-во копий
3. Выделяет буфер в PSRAM (`heap_caps_malloc` с `MALLOC_CAP_SPIRAM`)
4. Стримит пиксели (grayscale 8-bit) прямо туда, без промежуточного буфера
5. Читает 7-байтный PDL-футер, проверяет контрольную сумму
6. Переходит в состояние `PRINTING`, вызывает ваш callback

На любой `GET_DEVICE_ID` от хоста отвечает актуальным IEEE1284-строкой с текущими ошибками (`SCMDE`, `SCMCE`, `SCSYE`). Именно так CUPS узнаёт о состоянии принтера.

После сигнала о завершении (`notifyDone`) переходит в `JOB_DONE`, ждёт 2 секунды, затем сбрасывается в `IDLE` и готов к следующему заданию.

---

## Быстрый старт (C++)

### `main.cpp`

```cpp
#include "Sony898Printer.hpp"

extern "C" {
#include "nvs_flash.h"
#include "driver/uart.h"
}

extern "C" void app_main() {
    // NVS обязателен для хранения серийника и счётчика
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Серийный номер этого экземпляра.
    // nullptr — загрузить из NVS. Записать: UART-команда "set_serial A1234567" → reboot.
    // "XXXXXXX" — зашить в прошивку (вариант per-unit build или разработка).
    Printer.serial = nullptr;

    // Подключить модуль печати (опционально)
    Printer.onJobReady   = thermal_printer_on_job_ready;
    Printer.onConnect    = on_usb_connect;
    Printer.onDisconnect = on_usb_disconnect;

    ESP_ERROR_CHECK(Printer.init());
    Printer.begin();
}
```

### Callback печати

```cpp
// Вызывается когда изображение готово в PSRAM — не блокировать
static void thermal_printer_on_job_ready(const sony898_print_job_t *job) {
    // job->buf    — PSRAM, не освобождать
    // job->width  — ширина в пикселях
    // job->height — высота в пикселях
    // job->size   — width * height байт, grayscale 8-bit
    // job->copies — кол-во копий, всегда >= 1
    xTaskNotifyGive(print_task_handle);
}

// В задаче печати, после завершения:
Printer.notifyDone();    // успех → JOB_DONE
// или
Printer.notifyError();   // аппаратная ошибка → SYSTEM_ERROR
```

### Сенсоры физического принтера

```cpp
// Из GPIO-обработчиков или задачи мониторинга:
Printer.setPaper(gpio_get_level(PIN_PAPER));    // false → NO_PAPER
Printer.setCover(gpio_get_level(PIN_COVER));    // false → COVER_OPEN
Printer.setRibbon(gpio_get_level(PIN_RIBBON));  // false → NO_RIBBON
Printer.setMediaMatch(media_ok);                // false → MEDIA_MISMATCH
Printer.setSystemError(hw_fault);               // true  → SYSTEM_ERROR
```

---

## C++ класс `Sony898Printer`

### Конструктор

```cpp
explicit Sony898Printer(const char *sn = nullptr);
```

Позволяет привязать серийный номер при создании экземпляра:

```cpp
Sony898Printer Printer;               // SN загружается из NVS
Sony898Printer Printer("A1234567");   // SN зашит в прошивку
```

Глобальный синглтон `Printer` определён в `Sony898Printer.cpp` и доступен в любом файле проекта.

### Поля конфигурации (задать до `init()`)

```cpp
// Идентичность устройства — всё опционально.
// 0 / nullptr → умолчание из config.h (CFG_USB_*)
uint16_t    vid          = 0;        // USB Vendor  ID
uint16_t    pid          = 0;        // USB Product ID
const char *manufacturer = nullptr;  // iManufacturer
const char *product      = nullptr;  // iProduct
const char *serial       = nullptr;  // iSerial + IEEE1284 SCSNO

// Callback-и
sony898_on_job_ready_cb_t onJobReady   = nullptr;  // nullptr = встроенный таймер
sony898_usb_event_cb_t    onConnect    = nullptr;
sony898_usb_event_cb_t    onDisconnect = nullptr;
```

OEM-пример:
```cpp
Printer.vid          = 0xABCDu;
Printer.pid          = 0x1234u;
Printer.manufacturer = "ACME Corp";
Printer.product      = "FastPrint 9000";
Printer.serial       = nullptr;          // из NVS
Printer.init();
```

### Методы

#### Жизненный цикл

```cpp
esp_err_t init();   // инициализировать все подмодули, вызвать после nvs_flash_init()
void      begin();  // запустить USB-задачу и задачу состояния
```

#### Идентичность устройства

```cpp
uint16_t    getVid()          const;   // активный VID
uint16_t    getPid()          const;   // активный PID
const char *getManufacturer() const;   // активная строка iManufacturer
const char *getProduct()      const;   // активная строка iProduct
const char *getSerial()       const;   // активный серийный номер
const char *getProtocol()     const;   // "SPJL-DS,SPDL-DS2"
const char *getUsbClass()     const;   // "7/1/2 Printer Bidirectional"
const char *getDeviceId()     const;   // полный IEEE1284 Device ID

// Записать серийный номер в NVS. Применится после перезагрузки.
// Заводской workflow: flash → setSerial("A1234567") → reboot
esp_err_t setSerial(const char *s);
```

#### USB-состояние

```cpp
bool usbIsConnected()  const;   // хост видит устройство
bool usbIsConfigured() const;   // SET_CONFIGURATION получен
bool usbIsReady()      const;   // connected + configured + ready + parser idle
```

#### Состояние принтера

```cpp
sony898_state_t getState()       const;
void            setState(sony898_state_t state);
const char     *getStateName()   const;    // "idle", "printing", "cover_open", ...
uint8_t         getPortStatus()  const;    // GET_PORT_STATUS byte
bool            isPrinterReady() const;    // принтер может принять задание
```

#### Изображение / задание

```cpp
bool           imageReady()     const;
uint16_t       getWidth()       const;
uint16_t       getHeight()      const;
size_t         getImageSize()   const;
const uint8_t *getImageBuffer() const;   // PSRAM, не освобождать
uint8_t        getCopies()      const;
void           clearJob();               // освободить буфер, сбросить парсер
```

#### Сенсоры

```cpp
void setPaper(bool present);       // false → NO_PAPER
void setCover(bool closed);        // false → COVER_OPEN
void setRibbon(bool present);      // false → NO_RIBBON
void setMediaMatch(bool matches);  // false → MEDIA_MISMATCH
void setSystemError(bool error);   // true  → SYSTEM_ERROR
void clearSensors();               // все ошибки → обратно в IDLE
bool hasSensorError() const;
```

Приоритет при нескольких ошибках: `system_error` > `cover_open` > `no_ribbon` > `no_paper` > `media_mismatch`

#### Завершение печати

```cpp
void notifyDone();                 // успех → JOB_DONE
void notifyError();                // ошибка → SYSTEM_ERROR
bool hasPrintModule() const;       // true = callback зарегистрирован
```

#### Статистика

```cpp
uint32_t getPrintCount() const;   // всего напечатано (NVS, не сбрасывается)
```

---

## C API (`sony898_emulator.h`)

Для проектов на чистом C — один include вместо семи:

```c
#include "sony898_emulator.h"

sony898_config_t cfg = {0};
cfg.serial       = "A1234567";   // или NULL → NVS → config.h
cfg.on_job_ready = my_callback;
cfg.on_connect   = on_connect;

ESP_ERROR_CHECK(sony898_emulator_init(&cfg));
sony898_emulator_start();
```

Поля `sony898_config_t`:

```c
typedef struct {
    uint16_t    vid;          // 0 → CFG_USB_VID
    uint16_t    pid;          // 0 → CFG_USB_PID
    const char *manufacturer; // NULL → CFG_USB_MANUFACTURER
    const char *product;      // NULL → CFG_USB_PRODUCT
    const char *serial;       // NULL → NVS → CFG_USB_SERIAL
    sony898_on_job_ready_cb_t on_job_ready;
    sony898_usb_event_cb_t    on_connect;
    sony898_usb_event_cb_t    on_disconnect;
} sony898_config_t;
```

Все `sony898_emulator_*` функции зеркально повторяют методы C++ класса.

---

## Состояния принтера

| Состояние | SCMDE | SCMCE | Что это |
|---|---|---|---|
| `IDLE` | 0000 | 00 | Готов к работе |
| `RECEIVING_JOB` | 0000 | 00 | Принимает данные |
| `PRINTING` | 0000 | 00 | Печатает |
| `JOB_DONE` | 0000 | 00 | Завершено, ещё не idle |
| `COVER_OPEN` | 0800 | 01 | Крышка открыта |
| `NO_PAPER` | 0A00 | 00 | Нет бумаги |
| `NO_RIBBON` | 0002 | 00 | Нет ленты |
| `NO_MEDIA` | 0300 | 00 | Нет носителя |
| `MEDIA_MISMATCH` | 2000 | 00 | Тип носителя не совпадает |
| `SYSTEM_ERROR` | 0001 | 00 | Системная ошибка |

---

## Серийный номер — производственный workflow

`CFG_USB_SERIAL` в `config.h` — это только fallback для первого старта или после erase. Нормальный путь:

```
1. Прошить firmware (одинаковый для всех единиц)
2. UART: set_serial A1234567
3. Перезагрузка
4. UART: info  →  убедиться что Serial = A1234567
```

Серийный номер хранится в NVS (`namespace: sony898`, key: `serial`) и переживает перепрошивку. Из него же формируется поле `SCSNO` в IEEE1284 Device ID (16 символов, дополняется `-` справа).

---

## Сервисные UART-команды

Скорость: 115200 8N1. Ведущие пробелы в командах игнорируются.

| Команда | Описание |
|---|---|
| `status` | Полный статус: состояние, USB, изображение, сенсоры, счётчик |
| `device_id` | IEEE1284 Device ID (то что видит хост через GET_DEVICE_ID) |
| `usb` | Флаги connected / configured / ready |
| `serial` | Активный серийный номер текущей сессии |
| `counter` | Суммарное количество успешных печатей |
| `info` | VID, PID, Manufacturer, Product, Serial, Class, Protocol |
| `dump_pgm` | Бинарный вывод изображения в формате PGM через UART |
| `clear` | Освободить буфер PSRAM, сбросить парсер |
| `set_serial <SN>` | Записать SN в NVS. Применяется после перезагрузки. |
| `set <state>` | Форсировать состояние (для тестирования ошибок) |

Допустимые значения `set`: `idle`, `receiving_job`, `printing`, `job_done`, `cover_open`, `no_paper`, `no_ribbon`, `no_media`, `media_mismatch`, `error`

---

## Архитектура

```
                     ESP32-S3-N16R8
┌──────────────────────────────────────────────────────────┐
│                                                          │
│  [Linux / CUPS / Gutenprint]                             │
│          │  USB Printer Class 7/1/2                      │
│          │  GET_DEVICE_ID  GET_PORT_STATUS               │
│          │  Bulk OUT: SPJL-DS / SPDL-DS2                 │
│          ▼                                               │
│  ┌────────────────────────────────────────────────────┐  │
│  │              Sony898Printer (C++ класс)            │  │
│  │                        │                           │  │
│  │              sony898_emulator (C API)              │  │
│  │                                                    │  │
│  │   sony898_usb  →  sony898_parser  →  sony898_image │  │
│  │        │               │                  │        │  │
│  │   TinyUSB          PDL-парсер         PSRAM буфер  │  │
│  │                        │                  │        │  │
│  │              on_job_ready ────────────────┘        │  │
│  │                    │                               │  │
│  │                    └──────────────────► [модуль    │  │
│  │                                          печати]   │  │
│  │                                                    │  │
│  │  ◄── Printer.set*() ─────────────────────── GPIO   │  │
│  │  ◄── Printer.notify*() ──────────────── печать ОК  │  │
│  │                                                    │  │
│  │  sony898_status → GET_DEVICE_ID response           │  │
│  │  sony898_counter → NVS счётчик                     │  │
│  └────────────────────────────────────────────────────┘  │
│                                                          │
│  main.cpp — инициализация, UART-сервис                   │
└──────────────────────────────────────────────────────────┘
```

---

## Настройка (`config.h`)

Значения по умолчанию — все переопределяемы через поля класса или `sony898_config_t` в рантайме:

```c
// USB-идентификаторы (fallback, если поля класса не заданы)
#define CFG_USB_VID          0x054Cu
#define CFG_USB_PID          0x0877u
#define CFG_USB_MANUFACTURER "Sony"
#define CFG_USB_PRODUCT      "UP-D898MD_X898MD"
#define CFG_USB_SERIAL       "0000000"   // только fallback, нормально задаётся через NVS

// Максимальный размер изображения в PSRAM
#define CFG_MAX_IMAGE_BYTES  (6u * 1024u * 1024u)

// Скорость печати для встроенного таймера (когда модуль печати не зарегистрирован)
// UP-D898MD ≈ 133 строки/сек → 960 строк ≈ 7.2 сек
#define CFG_PRINT_SPEED_LPS  133u

// Задержка перед переходом из JOB_DONE в IDLE
#define CFG_JOB_DONE_IDLE_DELAY_MS  2000u
```

> `CFG_USB_SERIAL` задаёт только fallback при первом старте. После записи через `set_serial` или `Printer.setSerial()` значение в NVS имеет приоритет. Поле SCSNO в IEEE1284 Device ID формируется из активного серийного номера автоматически.

---

## Сборка и прошивка

```bash
pio run -e sony898-esp32s3 --target upload
pio device monitor -b 115200
```

При добавлении новых `.c`/`.cpp` файлов в компонент — сбросить CMake-кэш:
```bash
rm -rf .pio/build/sony898-esp32s3
```

Тест парсера на хосте (нужен захват USB-потока `sony898_spdl.bin`):
```bash
gcc -std=c11 -Wall -I test_host -I components/sony898_emulator \
    components/sony898_emulator/sony898_image.c \
    components/sony898_emulator/sony898_status.c \
    components/sony898_emulator/sony898_parser.c \
    test_host/test_parser.c -o test_host/test_parser

./test_host/test_parser sony898_spdl.bin
```

---

## Известные ограничения

- **Full Speed USB** — ESP32-S3 работает на 12 Mbps, реальный принтер на 480 Mbps (High Speed). High Speed требует внешнего ULPI PHY-чипа (USB3300 и аналоги) и редизайна платы. Для Gutenprint текущая скорость не критична.
- **Один буфер** — одновременно хранится только одно изображение. Следующее задание принимается после `clearJob()` или автоматически при переходе в IDLE.
- **Только grayscale** — протокол SPDL-DS2 для UP-D898MD передаёт 8-bit grayscale. Цветная печать (другие модели серии) не поддерживается.
- **Bulk IN не читается хостом** — Gutenprint получает статус через `GET_DEVICE_ID`. Bulk IN endpoint присутствует в дескрипторе для совместимости со спецификацией.
