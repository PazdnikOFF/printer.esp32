# sony898_emulator

Компонент для ESP32-S3, эмулирует Sony UP-D898MD / UP-X898MD перед USB-хостом. Хост отправляет задание печати, а компонент разбирает его, складывает grayscale-изображение в PSRAM и отдаёт вашему модулю печати через C API.

---

## Железо

Целевая плата: **ESP32-S3-N16R8**

| Ресурс | Значение |
|---|---|
| Flash | 16 MB |
| PSRAM | 8 MB (OPI) |
| USB | Native USB Device (Full Speed, 12 Mbps) |
| Фреймворк | ESP-IDF + TinyUSB |

Изображение 1280×960 занимает ~1.2 MB в PSRAM. Лимит буфера — 6 MB (настраивается в `config.h`).

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

Linux настраивает очередь CUPS так:
```bash
sudo lpadmin -p sony898 -E \
  -v "usb://Sony/UP-D898MD_X898MD?serial=7150628" \
  -m gutenprint.5.3://sony-upd898md/expert
```

---

## Как это работает изнутри

Хост шлёт задание в формате SPJL-DS / SPDL-DS2 — это PJL-обёртка вокруг бинарного PDL-блока. Компонент делает следующее:

1. Ждёт маркер `@PJL ENTER LANGUAGE=SONY-PDL-DS2\r\n` в потоке Bulk OUT
2. Читает 290-байтный PDL-заголовок, вытаскивает из него `width`, `height`, кол-во копий
3. Выделяет буфер в PSRAM (`heap_caps_malloc` с `MALLOC_CAP_SPIRAM`)
4. Стримит пиксели (grayscale 8-bit) прямо туда, без промежуточного буфера
5. Читает 7-байтный PDL-футер, считает контрольную сумму
6. Переходит в состояние `PRINTING` и вызывает ваш callback

На любой `GET_DEVICE_ID` от хоста отвечает живым IEEE1284-строкой, в которой зашиты текущие ошибки (`SCMDE`, `SCMCE`, `SCSYE`). Именно так CUPS узнаёт о проблемах — через этот же запрос, без отдельного канала.

После сигнала о завершении печати (`sony898_printer_notify_done`) переходит в `JOB_DONE`, ждёт 2 секунды чтобы хост успел прочитать статус, потом сбрасывается в `IDLE` и готов к следующему заданию.

---

## Быстрый старт

### Инициализация (в `app_main`)

```c
#include "sony898_usb.h"
#include "sony898_image.h"
#include "sony898_parser.h"
#include "sony898_status.h"
#include "sony898_sensors.h"
#include "sony898_printer_iface.h"
#include "sony898_counter.h"

// NVS нужен для счётчика печатей
esp_err_t ret = nvs_flash_init();
if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
}

sony898_image_init();
sony898_parser_init();
sony898_status_init();
sony898_sensor_init();
sony898_counter_init();

// Регистрируем модуль печати
sony898_printer_iface_set_callback(my_printer_on_job_ready);

// Поднимаем USB
ESP_ERROR_CHECK(sony898_usb_init());
sony898_usb_start_task();
```

### Сторона модуля печати

```c
// Это вызовется когда изображение готово в PSRAM
static void my_printer_on_job_ready(const sony898_print_job_t *job) {
    // job->buf    — указатель на PSRAM (не освобождать!)
    // job->width  — ширина в пикселях
    // job->height — высота в пикселях
    // job->size   — width * height байт, grayscale 8-bit
    // job->copies — кол-во копий из PDL-заголовка, всегда >= 1

    // Запускаем печать асинхронно и выходим быстро
    xTaskNotifyGive(print_task_handle);
}

// В задаче печати, после завершения:
sony898_printer_notify_done();   // всё ок, переходим в JOB_DONE
// или
sony898_printer_notify_error();  // механическая ошибка, переходим в SYSTEM_ERROR
```

### Сенсоры физического принтера

Вызывайте из GPIO-обработчиков или задачи мониторинга сенсоров:

```c
// true = всё хорошо, false = проблема
sony898_sensor_set_paper(gpio_get_level(PIN_PAPER_SENSOR));   // бумага
sony898_sensor_set_cover(gpio_get_level(PIN_COVER_SENSOR));   // крышка закрыта
sony898_sensor_set_ribbon(gpio_get_level(PIN_RIBBON_SENSOR)); // лента установлена
sony898_sensor_set_media_match(media_ok);   // тип носителя совпадает
sony898_sensor_set_system_error(hw_fault);  // перегрев, ошибка механики
```

Состояние применяется немедленно. Приоритет при нескольких ошибках одновременно:
`system_error` > `cover_open` > `no_ribbon` > `no_paper` > `media_mismatch`

Когда все сенсоры в норме — принтер возвращается в `IDLE` автоматически.

---

## API Reference

### `sony898_usb.h` — USB

```c
// Инициализировать TinyUSB как Sony UP-D898MD. Вызвать один раз при старте.
esp_err_t sony898_usb_init(void);

// Запустить USB-задачу. Вызвать после usb_init().
void sony898_usb_start_task(void);

// true — хост видит устройство (VBUS + энумерация прошла)
bool sony898_usb_is_connected(void);

// true — хост отправил SET_CONFIGURATION, Bulk OUT готов принимать данные
bool sony898_usb_is_configured(void);

// true — connected AND configured AND статус ready AND парсер свободен
bool sony898_usb_is_ready_for_print(void);
```

---

### `sony898_image.h` — буфер изображения

```c
// true — изображение принято и лежит в PSRAM готовое к печати
bool sony898_image_ready(void);

// Указатель на буфер в PSRAM. NULL если изображение не готово.
// Не освобождать — буфер принадлежит модулю.
const uint8_t *sony898_get_image_buffer(void);

// Размер в байтах (= width * height)
size_t sony898_get_image_size(void);

uint16_t sony898_get_width(void);
uint16_t sony898_get_height(void);

// Освободить буфер PSRAM. Вызывается автоматически при сбросе парсера.
void sony898_release_image(void);
```

---

### `sony898_parser.h` — парсер потока

```c
// Кол-во копий из PDL-заголовка. Всегда >= 1. Доступно после image_ready.
uint8_t sony898_parser_get_copies(void);

// true — парсер в начальном состоянии, готов принять новое задание
bool sony898_parser_can_accept_job(void);

// Текущее состояние парсера (для отладки)
parser_state_t sony898_parser_get_state(void);

// Полный сброс: освобождает буфер PSRAM, возвращает в IDLE
void sony898_parser_reset(void);

// Сброс только состояния парсера, изображение в PSRAM остаётся
void sony898_parser_prepare_for_next_job(void);
```

Состояния парсера:

| Состояние | Что происходит |
|---|---|
| `PARSER_SCAN_PJL_HEADER` | Ждёт маркер `@PJL ENTER LANGUAGE=...` |
| `PARSER_READ_PDL_HEADER` | Накапливает 290-байтный PDL-заголовок |
| `PARSER_READ_IMAGE_PAYLOAD` | Стримит пиксели в PSRAM |
| `PARSER_READ_PDL_FOOTER` | Читает 7-байтный футер |
| `PARSER_JOB_COMPLETE` | Изображение готово, ждёт следующего задания |
| `PARSER_ERROR` | Ошибка протокола |

---

### `sony898_status.h` — состояние принтера для хоста

```c
// Получить / установить текущее состояние
sony898_state_t sony898_status_get_state(void);
void            sony898_status_set_state(sony898_state_t state);

// Человекочитаемое имя состояния (для логов)
const char *sony898_status_state_name(sony898_state_t state);

// IEEE1284 Device ID — именно это хост читает через GET_DEVICE_ID
// Содержит динамические поля: SCMDE, SCMCE, SCSYE, SCJBS
const char *sony898_status_get_ieee1284_id(void);

// 1-байтный статус для GET_PORT_STATUS
uint8_t sony898_status_get_port_status(void);

// true — принтер в IDLE или JOB_DONE, может принять задание
bool sony898_status_is_ready(void);
```

Состояния принтера и что видит хост:

| Состояние | SCMDE | SCMCE | Что это значит |
|---|---|---|---|
| `IDLE` | 0000 | 00 | Готов к работе |
| `RECEIVING_JOB` | 0000 | 00 | Принимает данные |
| `PRINTING` | 0000 | 00 | Печатает |
| `JOB_DONE` | 0000 | 00 | Печать завершена, ещё не idle |
| `COVER_OPEN` | 0800 | 01 | Крышка открыта |
| `NO_PAPER` | 0A00 | 00 | Нет бумаги |
| `NO_RIBBON` | 0002 | 00 | Нет ленты |
| `NO_MEDIA` | 0300 | 00 | Нет носителя |
| `MEDIA_MISMATCH` | 2000 | 00 | Тип носителя не совпадает |
| `SYSTEM_ERROR` | 0001 | 00 | Системная ошибка |

---

### `sony898_sensors.h` — сенсоры физического принтера

```c
// Инициализация. Вызвать один раз. По умолчанию все сенсоры в норме.
void sony898_sensor_init(void);

// true = бумага есть  /  false = нет бумаги → NO_PAPER
void sony898_sensor_set_paper(bool present);

// true = крышка закрыта  /  false = крышка открыта → COVER_OPEN
void sony898_sensor_set_cover(bool closed);

// true = лента установлена  /  false = нет ленты → NO_RIBBON
void sony898_sensor_set_ribbon(bool present);

// true = тип носителя совпадает  /  false → MEDIA_MISMATCH
void sony898_sensor_set_media_match(bool matches);

// true = аппаратная ошибка (перегрев, механика) → SYSTEM_ERROR
void sony898_sensor_set_system_error(bool error);

// Сбросить все ошибки и вернуться в IDLE
void sony898_sensor_clear_all(void);

// true — хотя бы один сенсор в состоянии ошибки
bool sony898_sensor_has_error(void);
```

Все функции потокобезопасны, можно вызывать из GPIO ISR или любой задачи.

---

### `sony898_printer_iface.h` — интерфейс с модулем печати

```c
// Структура задания — передаётся в callback при готовности изображения
typedef struct {
    const uint8_t *buf;     // указатель на PSRAM, не освобождать
    uint16_t       width;
    uint16_t       height;
    size_t         size;    // width * height байт, grayscale 8-bit
    uint8_t        copies;  // из PDL-заголовка, всегда >= 1
} sony898_print_job_t;

// Тип callback-а
typedef void (*sony898_on_job_ready_cb_t)(const sony898_print_job_t *job);

// Зарегистрировать callback модуля печати. Вызвать один раз при старте.
// Если не вызвать — работает встроенный таймер (высота/133 сек).
void sony898_printer_iface_set_callback(sony898_on_job_ready_cb_t cb);

// Сообщить об успешном завершении печати → переход в JOB_DONE
// Вызывать из задачи модуля печати, не из callback-а
void sony898_printer_notify_done(void);

// Сообщить об аппаратной ошибке → переход в SYSTEM_ERROR
void sony898_printer_notify_error(void);

// true — callback зарегистрирован
bool sony898_printer_iface_has_module(void);
```

---

### `sony898_counter.h` — счётчик печатей

```c
// Инициализация, читает значение из NVS. Вызвать после nvs_flash_init().
esp_err_t sony898_counter_init(void);

// Текущее суммарное количество успешно напечатанных заданий.
// Хранится в NVS, не сбрасывается при перезагрузке.
uint32_t sony898_counter_get(void);

// Увеличить на 1 и записать в NVS. Вызывается автоматически при JOB_DONE.
esp_err_t sony898_counter_increment(void);
```

---

## Схема взаимодействия модулей

```
                     ESP32-S3-N16R8
┌──────────────────────────────────────────────────────┐
│                                                      │
│  [Linux / CUPS / Gutenprint]                         │
│          │  USB Printer Class 7/1/2                  │
│          │  GET_DEVICE_ID  GET_PORT_STATUS           │
│          │  Bulk OUT: SPJL-DS / SPDL-DS2             │
│          ▼                                           │
│  ┌──────────────────────────────────┐               │
│  │       sony898_emulator           │               │
│  │                                  │               │
│  │  USB Driver  →  Parser           │               │
│  │                    │             │               │
│  │               PSRAM image        │               │
│  │                    │             │               │
│  │              on_job_ready ───────┼──► [модуль    │
│  │                                  │     печати]   │
│  │  ◄── sony898_sensor_set_*() ─────┼──── GPIO      │
│  │  ◄── sony898_printer_notify_*() ─┼──── печать    │
│  │                                  │     готова    │
│  │  Статус → GET_DEVICE_ID response │               │
│  └──────────────────────────────────┘               │
│                                                      │
│  app_main — здесь всё инициализируется и связывается │
└──────────────────────────────────────────────────────┘
```

---

## Настройка (`config.h`)

```c
// USB-идентификаторы
#define CFG_USB_VID          0x054Cu   // Sony
#define CFG_USB_PID          0x0877u   // UP-D898/X898
#define CFG_USB_MANUFACTURER "Sony"
#define CFG_USB_PRODUCT      "UP-D898MD_X898MD"
#define CFG_USB_SERIAL       "7150628"

// Максимальный размер изображения в PSRAM
#define CFG_MAX_IMAGE_BYTES  (6u * 1024u * 1024u)

// Скорость печати для таймера (если модуль печати не зарегистрирован)
// UP-D898MD ≈ 133 строки/сек → 960 строк ≈ 7.2 сек
#define CFG_PRINT_SPEED_LPS  133u

// Сколько держать статус JOB_DONE перед переходом в IDLE
#define CFG_JOB_DONE_IDLE_DELAY_MS  2000u
```

---

## Сервисные UART-команды

Только для разработки и отладки. Скорость: 115200 8N1.

| Команда | Описание |
|---|---|
| `status` | Состояние принтера, парсера, изображения, счётчик |
| `device_id` | IEEE1284 Device ID (то, что видит хост) |
| `usb` | Состояние USB-подключения |
| `counter` | Количество успешных печатей |
| `dump_pgm` | Бинарный вывод изображения в формате PGM через UART |
| `clear` | Сброс парсера и освобождение буфера PSRAM |
| `info` | USB-идентификаторы |
| `set <state>` | Форсировать состояние принтера (для тестирования ошибок) |

Допустимые состояния для `set`: `idle`, `receiving_job`, `printing`, `job_done`, `cover_open`, `no_paper`, `no_ribbon`, `no_media`, `media_mismatch`, `error`.

Для захвата изображения через UART есть скрипт `tools/dump_pgm.py`:
```bash
python3 tools/dump_pgm.py /dev/cu.usbserial-XXXX /tmp/captured.pgm
```

---

## Сборка и прошивка

```bash
pio run -e sony898-esp32s3 --target upload
pio device monitor -b 115200
```

Тест парсера на хосте (нужен файл захвата USB-потока `sony898_spdl.bin`):
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

- **Full Speed USB** — ESP32-S3 работает на 12 Mbps, реальный принтер на 480 Mbps. Передача 1.2 MB занимает ~1.8 сек вместо ~0.1 сек. Для Gutenprint это не проблема.
- **Один буфер** — одновременно хранится только одно изображение. Следующее задание принимается только после освобождения буфера.
- **Только grayscale** — протокол SPDL-DS2 для UP-D898MD передаёт 8-bit grayscale. Цветная печать (другие модели серии) не поддерживается.
- **Bulk IN не читается хостом** — Gutenprint получает статус исключительно через `GET_DEVICE_ID`. Bulk IN endpoint присутствует в дескрипторе для совместимости.
