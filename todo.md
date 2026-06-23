# ТЗ: модуль эмуляции медицинского USB-принтера Sony UP-D898MD / UP-X898MD на ESP32-S3-N16R8

## 1. Цель

Разработать модуль и демо-проект для ESP32-S3-N16R8, который эмулирует USB-принтер Sony UP-D898MD / UP-X898MD, принимает данные печати от Linux-драйвера/Gutenprint, разбирает протокол Sony SPJL-DS / SPDL-DS2 и восстанавливает изображение в PSRAM как 8-bit grayscale bitmap, готовый к дальнейшей печати, сохранению или передаче.

ИИ обязан не фантазировать. Все параметры должны быть основаны только на предоставленных фактах, исходниках Gutenprint, USB-дескрипторах и тестовых дампах. Любые неподтвержденные поля должны быть помечены как `UNKNOWN` с логированием, а не выданы за факт.

---

## 2. Аппаратная платформа

Целевая плата:

- MCU: ESP32-S3
- Flash: 16 MB
- PSRAM: 8 MB
- Модель: ESP32-S3-N16R8
- USB: native USB device
- Framework: ESP-IDF
- USB stack: TinyUSB

Обязательные настройки:

```ini
board_build.flash_size = 16MB
board_upload.flash_size = 16MB
board_build.partitions = default_16MB.csv
board_build.arduino.memory_type = dio_opi
```

Память изображения должна выделяться из PSRAM:

```c
heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
```

---

## 3. Подтвержденные USB-параметры Sony UP-D898MD / UP-X898MD

Реальный принтер определен Linux как:

```text
Bus 005 Device 010: ID 054c:0877 Sony Corp. UP-D898/X898 series
```

USB Device Descriptor:

```text
bcdUSB               2.00
idVendor             0x054c
idProduct            0x0877
iManufacturer        Sony
iProduct             UP-D898MD_X898MD
iSerial              0000000
bNumConfigurations   1
```

USB Interface:

```text
bInterfaceClass      7  Printer
bInterfaceSubClass   1  Printer
bInterfaceProtocol   2  Bidirectional
```

Endpoints:

```text
EP 0x01 OUT  Bulk 512 bytes
EP 0x81 IN   Bulk 512 bytes
```

Модуль должен эмулировать именно эти USB-дескрипторы.

---

## 4. Подтвержденный протокол

Принтер сообщает через IEEE1284 Device ID:

```text
MFG:Sony;
MDL:UP-D898MD_X898MD;
DES:Sony UP-D898MD_X898MD;
CMD:SPJL-DS,SPDL-DS2;
CLS:PRINTER;
```

Следовательно, поддерживаемые протоколы:

```text
SPJL-DS
SPDL-DS2
```

---

## 5. Структура задания печати

Файл задания, полученный из Gutenprint, имеет структуру:

```text
256 bytes: JOBSIZE=PJL-H,<len>,<pagesize>,6,0,0,0
<len> bytes: PJL header

256 bytes: JOBSIZE=PDL,<len>
<len> bytes: PDL block

256 bytes: JOBSIZE=PJL-T,302
302 bytes: PJL trailer
```

PJL header:

```text
ESC %-12345X CR LF
@PJL JOB NAME="Gutenprint" CR LF
@PJL ENTER LANGUAGE=SONY-PDL-DS2 CR LF
```

PJL trailer:

```text
@PJL EOJ CR LF
ESC %-12345X CR LF
```

---

## 6. Подтвержденная структура PDL для UP-D898MD / UP-X898

PDL-блок:

```text
290 bytes: PDL header
width * height bytes: grayscale image payload
7 bytes: PDL footer
```

Все multibyte значения — Big Endian.

Для тестового задания:

```text
JOBSIZE=PDL,1229097
PDL header size = 290
width = 0x0500 = 1280
height = 0x03c0 = 960
image payload = 1280 * 960 = 1,228,800 bytes
PDL footer = 7 bytes

290 + 1,228,800 + 7 = 1,229,097
```

Подтвержденные offset’ы относительно начала PDL header:

```text
0x24: width  BE16
0x26: height BE16
0x122: image payload start
```

То есть:

```c
width  = be16(pdl_header + 0x24);
height = be16(pdl_header + 0x26);
image  = pdl_header + 0x122;
```

---

## 7. Обязательная логика парсера

Модуль должен реализовать потоковый парсер USB Bulk OUT.

Состояния парсера:

```text
WAIT_JOBSIZE_PJL_H
READ_PJL_HEADER
WAIT_JOBSIZE_PDL
READ_PDL_HEADER
READ_IMAGE_PAYLOAD
READ_PDL_FOOTER
WAIT_JOBSIZE_PJL_T
READ_PJL_TRAILER
JOB_COMPLETE
ERROR
```

Парсер обязан:

1. Читать данные кусками из USB OUT endpoint 0x01.
2. Накапливать 256-байтные `JOBSIZE` заголовки.
3. Проверять, что блок начинается с `JOBSIZE=`.
4. Разбирать тип блока:
   - `PJL-H`
   - `PDL`
   - `PJL-T`
5. Проверять длину блока.
6. Для `PDL`:
   - читать первые 290 байт как PDL header;
   - извлекать width/height;
   - проверять, что `pdl_len == 290 + width * height + 7`;
   - выделять буфер изображения в PSRAM;
   - сохранять туда `width * height` байт;
   - читать 7 байт footer.
7. По завершении задания выставлять флаг `image_ready`.

---

## 8. Ограничения памяти

Для 1280×960:

```text
image size = 1,228,800 bytes
```

Это должно помещаться в PSRAM.

Максимальный допустимый размер изображения на первом этапе:

```text
MAX_IMAGE_BYTES = 6 * 1024 * 1024
```

Если `width * height > MAX_IMAGE_BYTES`, задание должно быть отклонено с ошибкой.

Нельзя выделять крупные буферы во внутренней RAM.

---

## 9. Статусный ответ принтера

Linux/Gutenprint перед отправкой задания запрашивает статус. Если принтер сообщает ошибку, backend не отправляет PDL.

Подтвержденный код ошибки:

```text
SCMDE=0000
SCMCE=01
SCSYE=00
```

Gutenprint расшифровывает:

```text
MC=01 = Cover open
```

Эмулятор обязан отвечать статусом без ошибок:

```text
SCMDE=0000
SCMCE=00
SCSYE=00
SCPRS=0000
```

Эмулятор должен вернуть IEEE1284 Device ID с полями:

```text
MFG:Sony;
MDL:UP-D898MD_X898MD;
DES:Sony UP-D898MD_X898MD;
CMD:SPJL-DS,SPDL-DS2;
CLS:PRINTER;
SCDIV:0100;
SCSYV:01010000;
SCSYS:0000001000010000000000;
SCMDS:00000500000100000000;
SCSYE:00;
SCMDE:0000;
SCMCE:00;
SCSYI:100005001000050000000000014500;
SCSVI:000204000204;
SCMDI:200406;
SCSNO:0000000---------;
SCJBS:0000;
SCCAI:00000000000000;
SCGSI:01;
SCQTI:0001;
SPUQI:0000;
```

Важно: поля, смысл которых не подтвержден, не документировать как расшифрованные. Использовать их как совместимый статусный шаблон.

---

## 10. API модуля

Создать библиотеку:

```text
components/sony898_emulator/
```

Файлы:

```text
sony898_usb.h
sony898_usb.c
sony898_parser.h
sony898_parser.c
sony898_status.h
sony898_status.c
sony898_image.h
sony898_image.c
```

Минимальный API:

```c
esp_err_t sony898_usb_init(void);

void sony898_parser_reset(void);

esp_err_t sony898_parser_feed(const uint8_t *data, size_t len);

bool sony898_image_ready(void);

const uint8_t *sony898_get_image_buffer(void);

size_t sony898_get_image_size(void);

uint16_t sony898_get_width(void);

uint16_t sony898_get_height(void);

void sony898_release_image(void);
```

Дополнительный обязательный API для быстрой проверки USB-подключения:

```c
bool sony898_usb_is_connected(void);
```

Назначение:

- вернуть `true`, если ESP32-S3 USB Device подключен к USB host и прошел basic bus attach/enumeration;
- вернуть `false`, если USB-кабель не подключен, host не виден или устройство не находится в состоянии подключения.

Дополнительный API для проверки, что host уже сконфигурировал устройство:

```c
bool sony898_usb_is_configured(void);
```

Назначение:

- вернуть `true`, если USB host выдал `SET_CONFIGURATION` и интерфейс принтера готов принимать Bulk OUT;
- вернуть `false`, если устройство физически подключено, но еще не сконфигурировано.

Дополнительный API для быстрой проверки готовности эмулятора к печати:

```c
bool sony898_usb_is_ready_for_print(void);
```

Логика:

```c
return sony898_usb_is_connected() &&
       sony898_usb_is_configured() &&
       sony898_status_is_ready() &&
       sony898_parser_can_accept_job();
```

Дополнительный API статуса:

```c
bool sony898_status_is_ready(void);

void sony898_status_set_ready(void);

void sony898_status_set_cover_open(bool enabled);

void sony898_status_set_busy(bool enabled);

const char *sony898_status_get_ieee1284_id(void);
```

Требование: `sony898_usb_is_connected()` должна быть быстрой, неблокирующей и безопасной для вызова из основного цикла.

---

## 11. Демо-проект

Демо должно:

1. Поднять USB Device как Sony UP-D898MD_X898MD.
2. Принимать печать из Linux через Gutenprint.
3. Разбирать SPJL/SPDL.
4. Сохранять изображение в PSRAM.
5. Печатать в UART лог:
   - USB connected/disconnected;
   - USB configured/not configured;
   - ready_for_print;
   - полученный `JOBSIZE`;
   - PDL длину;
   - width;
   - height;
   - image size;
   - checksum изображения;
   - статус `image_ready`.
6. По команде через UART отдавать изображение на ПК в формате PGM.

Команды UART:

```text
status
dump_pgm
clear
info
usb
```

Команда `usb` должна выводить:

```text
connected=<0|1>
configured=<0|1>
ready_for_print=<0|1>
image_ready=<0|1>
```

Формат PGM:

```text
P5
<width> <height>
255
<raw grayscale bytes>
```

---

## 12. Linux-команды для тестирования

Создание тестового изображения:

```bash
convert -size 1280x960 xc:white \
  -gravity center -pointsize 80 -fill black \
  -annotate 0 "SONY TEST" sony_test.png
```

Создание очереди CUPS:

```bash
sudo lpadmin \
  -p sony898 \
  -E \
  -v "usb://Sony/UP-D898MD_X898MD?serial=0000000" \
  -m gutenprint.5.3://sony-upd898md/expert
```

Печать:

```bash
lp -d sony898 sony_test.png
```

Проверка:

```bash
lpstat -t
```

---

## 13. Самопроверка ИИ

Перед выдачей результата ИИ обязан выполнить самопроверку.

### 13.1 Проверка USB-дескрипторов

Сравнить с эталоном:

```text
VID 054c
PID 0877
Class 7
Subclass 1
Protocol 2
EP OUT 0x01 Bulk 512
EP IN  0x81 Bulk 512
Manufacturer Sony
Product UP-D898MD_X898MD
Serial 0000000
```

### 13.2 Проверка USB API

Unit-test или runtime-test должен проверить:

```text
sony898_usb_is_connected() возвращает false до подключения host
sony898_usb_is_connected() возвращает true после USB attach/enumeration
sony898_usb_is_configured() возвращает true после SET_CONFIGURATION
sony898_usb_is_ready_for_print() возвращает true только при ready status и рабочем parser state
```

Запрещено реализовывать `sony898_usb_is_connected()` как заглушку, всегда возвращающую `true`.

### 13.3 Проверка парсера на файле `sony898_spdl.bin`

ИИ должен написать unit-test, который читает файл `sony898_spdl.bin` и проверяет:

```text
найден JOBSIZE=PJL-H
найден @PJL ENTER LANGUAGE=SONY-PDL-DS2
найден JOBSIZE=PDL,1229097
PDL header size = 290
width = 1280
height = 960
image payload = 1228800 bytes
footer = 7 bytes
найден JOBSIZE=PJL-T,302
найден @PJL EOJ
```

Проверка длины:

```text
1229097 == 290 + 1280 * 960 + 7
```

### 13.4 Проверка PSRAM

Проверить:

```c
heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
```

До и после выделения буфера.

Если буфер выделен не из PSRAM — ошибка.

### 13.5 Проверка восстановления изображения

Unit-test должен сохранить payload как:

```text
out.pgm
```

И убедиться, что:

```text
file out.pgm
```

показывает PGM image, 1280×960, grayscale.

### 13.6 Проверка отказоустойчивости

Проверить поврежденные входные данные:

- неправильный `JOBSIZE`;
- отсутствует `PDL`;
- неверная длина PDL;
- width/height = 0;
- width*height больше лимита;
- обрыв потока посередине изображения;
- повторная печать второго задания после первого;
- USB disconnected во время приема изображения;
- USB connected, но not configured.

---

## 14. Запреты

ИИ запрещено:

1. Придумывать неподтвержденные значения протокола.
2. Называть неизвестные поля header’а расшифрованными.
3. Игнорировать ошибки длины.
4. Хранить весь print job во внутренней RAM.
5. Использовать blocking loop без watchdog/yield.
6. Считать, что всегда будет только 1280×960.
7. Убирать статусный ответ — без него Gutenprint не начнет печатать.
8. Подменять задачу приемом PNG/JPEG. Модуль должен принимать именно SPJL/SPDL поток от USB-принтера.
9. Делать `sony898_usb_is_connected()` фиктивной функцией без связи с реальным состоянием USB.

---

## 15. Критерии готовности

Работа считается выполненной, если:

1. ESP32-S3 определяется в Linux как Sony UP-D898MD_X898MD.
2. `lsusb -v` показывает правильный Printer Class 7/1/2.
3. CUPS/Gutenprint создает очередь для Sony UP-D898MD.
4. `lp -d sony898 sony_test.png` отправляет задание без ошибки.
5. ESP32 принимает полный PDL-блок.
6. Из PDL извлекается изображение 1280×960 grayscale.
7. Изображение сохраняется в PSRAM.
8. Через UART можно получить PGM-файл.
9. Unit-test на `sony898_spdl.bin` проходит.
10. Лог содержит все этапы разбора задания.
11. Все UNKNOWN-поля протокола явно помечены как UNKNOWN.
12. Нет неподтвержденных утверждений в документации и комментариях.
13. `sony898_usb_is_connected()` корректно показывает наличие USB host.
14. `sony898_usb_is_configured()` корректно показывает состояние USB configuration.
15. `sony898_usb_is_ready_for_print()` корректно показывает готовность принять задание.

---

## 16. Исходные подтвержденные материалы

Использовать как базу:

```text
USB VID/PID: 054c:0877
Product: UP-D898MD_X898MD
USB Class: Printer 7/1/2
Endpoints: 0x01 OUT Bulk, 0x81 IN Bulk
Gutenprint driver: sony-upd898md
Source files:
  src/main/print-dyesub.c
  src/cups/backend_sonyupdneo.c
Protocol:
  SPJL-DS
  SPDL-DS2
PDL header:
  290 bytes
Image:
  8-bit grayscale, width * height bytes
Test sample:
  width 1280
  height 960
  PDL length 1229097
```
