# Сводка по основному I2C RAM-буферу STM32 <-> ESP

Дата: 2026-03-26

## 1. Назначение документа

Этот документ — практическая сводка по тому, как в проекте `sip_periph` используется основной буфер обмена по I2C между:

- `ESP32` — master
- `STM32F411` — slave

Основной интерес:

- общий RAM-буфер `I2C1 slave`
- публикация событий STM32 -> ESP (`TIME`, `UID PN532`, `PIN`, `Wiegand`, `HMI PIN`)
- записи ESP -> STM32 (`auth result`, `HMI message`, `TIME set`, `service`, `config`)
- компактная матрица `reg/type/len/layout/producer/consumer`

---

## 2. Источники истины

### Нормативные документы

- `docs/i2c_global_contract.md` — основной транспортный контракт I2C
- `docs/i2c_uid532_contract.md` — сценарий `UID_532`
- `docs/i2c_slave_outbox_flow.md` — логика slave-publish / outbox
- `docs/i2c_time_reliability_report_2026-03-24.md` — практические выводы по `TIME`

### Кодовые источники истины

- `Core/Inc/main.h` — адреса RAM-окон, типы пакетов, фиксированные длины
- `Core/Inc/app_i2c_slave.h` — публичный API I2C slave FSM
- `Core/Src/app_i2c_slave.c` — реальная FSM, outbox, strict RX, diag export
- `Core/Src/service_time_sync.c` — источник `TIME`
- `Core/Src/service_tca6408.c` — trigger публикации `TIME` от DS3231 1Hz / TCA6408A
- `Core/Src/service_pn532_task.c` — источник `UID PN532`
- `Core/Src/service_matrix_kbd.c` — источник `PACKET_PIN`
- `Core/Src/wiegand.c` — источник `PACKET_WIEGAND`
- `Core/Src/hmi.c` — источник `PACKET_PIN_HMI`
- `Core/Src/main.c` — инициализация I2C, queues, tasks, комментарий по trigger для `TIME`

---

## 3. Что такое основной I2C буфер

Основной буфер — это массив:

- `Core/Src/app_i2c_slave.c`
- `static uint8_t s_ram[256];`

Он используется как общее RAM-регистровое пространство `I2C slave`, которое ESP читает и пишет через `I2C1`.

### Базовые параметры

- slave address: `0x11` (7-bit)
- в `HAL` это видно как `OwnAddress1 = 34`, то есть `0x22` в 8-bit форме
- скорость шины: `400 kHz`
- STM32 — всегда `slave`
- ESP — всегда `master`

Подтверждение:

- `docs/i2c_global_contract.md`
- `Core/Src/main.c` -> `MX_I2C1_Init()`

---

## 4. Базовая форма транзакций

### 4.1 Master write

ESP всегда пишет в форме:

`[reg, len, payload...]`

Пример:

- `[0x70, 0x05, result, b3, b2, b1, b0]`
- `[0x88, 0x07, time_bcd7...]`
- `[0xE0, 0x10, cfg[16]]`

### 4.2 Master read

ESP читает EEPROM-style:

1. отправляет header `[reg, len]`
2. делает `repeated-start`
3. читает ровно `len` байт

То есть `len` всегда задаёт master, а не STM32.

---

## 5. Общая карта RAM-окон

По `Core/Inc/main.h` и `docs/i2c_global_contract.md`:

| Reg | Symbol | Назначение |
|---|---|---|
| `0x00` | `I2C_PACKET_TYPE_ADDR` | тип pending slave packet |
| `0x01` | `I2C_REG_532_ADDR` | окно `UID PN532` |
| `0x10` | `I2C_REG_MATRIX_PIN_ADDR` | окно `matrix PIN` |
| `0x20` | `I2C_REG_WIEGAND_ADDR` | окно `Wiegand` |
| `0x30` | `I2C_REG_COUNTER_ADDR` | service / counter write window |
| `0x40` | `I2C_REG_HMI_PIN_ADDR` | окно `HMI PIN` |
| `0x50` | `I2C_REG_HMI_MSG_ADDR` | сообщение для HMI от ESP |
| `0x70` | `I2C_REG_HMI_ACT_ADDR` | auth result / action result от ESP |
| `0x80` | `I2C_REG_HW_TIME_ADDR` | окно `TIME` STM32 -> ESP |
| `0x88` | `I2C_REG_HW_TIME_SET_ADDR` | запись времени ESP -> STM32 |
| `0xE0` | `I2C_REG_CFG_ADDR` | runtime config block `E0..EF` |
| `0xF0` | `I2C_REG_STM32_ERROR_ADDR` | diagnostics / error export |

---

## 6. Как работает slave-publish через outbox

### 6.1 Общая идея

Когда STM32 хочет сообщить событие ESP, он не пишет напрямую в шину, а публикует пакет в `s_ram` через outbox-механику.

Ключевая функция:

- `app_i2c_slave_publish()` в `Core/Src/app_i2c_slave.c`

Она делает следующее:

1. определяет целевой `reg` по `type`
2. определяет контрактную длину окна
3. пишет `type` в `s_ram[0x00]`
4. копирует payload в `s_ram[target_reg]`
5. ставит флаги outbox (`s_outbox_busy`, `s_outbox_reg`, `s_outbox_len`)
6. тянет линию `PIN_EVENT_TO_ESP` в active low

### 6.2 Что делает ESP

После IRQ ESP должен:

1. прочитать `0x00` с `len=1`
2. получить `type`
3. по `type` выбрать нужный `reg`
4. прочитать payload-окно с фиксированной длиной

### 6.3 Когда пакет считается доставленным

Это важная особенность реализации.

Пакет считается подтверждённым только после чтения **реального payload окна**.

То есть:

- чтение `0x00` только говорит, **какой пакет ждёт**
- чтение `0x00` не снимает outbox
- outbox снимается только после успешного чтения payload-регистра

Это поведение реализовано в:

- `app_i2c_slave_tx_complete()`
- `outbox_complete_ack()`

Подробное описание:

- `docs/i2c_slave_outbox_flow.md`

---

## 7. Compact matrix: reg/type/len/layout/producer/consumer

Ниже — компактная рабочая матрица по основным окнам.

### 7.1 Slave-published packets (STM32 -> ESP)

| Reg | Type | Len | Layout | Producer | Consumer |
|---:|---|---:|---|---|---|
| `0x00` | `PACKET_*` | `1` | `byte0 = packet type` | `app_i2c_slave_publish()` | ESP poll/read stage 1 |
| `0x01` | `PACKET_UID_532` | `15` | `byte0=uid_len`, `byte1..14=uid/data` | `service_pn532_task.c` -> queue -> `app_i2c_slave_publish()` | ESP auth/input pipeline |
| `0x10` | `PACKET_PIN` | `13` | ASCII/byte PIN buffer from matrix keypad, padded to contract window | `service_matrix_kbd.c` -> queue -> `app_i2c_slave_publish()` | ESP input/auth pipeline |
| `0x20` | `PACKET_WIEGAND` | `15` | reader payload from `get_wiegand_data()`, padded to window | `wiegand.c` -> queue -> `app_i2c_slave_publish()` | ESP input/auth pipeline |
| `0x40` | `PACKET_PIN_HMI` | `15` | `rtype`, `bitlength`, `rdata[]` snapshot from HMI PIN | `hmi.c` -> queue -> `app_i2c_slave_publish()` | ESP input/auth pipeline |
| `0x80` | `PACKET_TIME` | `8` | `byte0..6 = BCD time`, `byte7` reserved/zero-filled by window padding | `service_time_sync.c` -> queue -> `app_i2c_slave_publish()` | ESP time sync / telemetry |
| `0xF0` | `PACKET_ERROR`* | `16` | diag counters and line states | `app_i2c_slave_sync_diag_to_ram()` | ESP diagnostics |

`*` В mapping `PACKET_ERROR` присутствует, но в текущем коде диагностическое окно `0xF0..0xFF` выглядит как постоянно доступный export RAM, а не как явно найденный отдельный producer через `myQueueToMasterHandle`.

### 7.2 Master writes (ESP -> STM32)

| Reg | Type | Len | Layout | Producer | Consumer |
|---:|---|---:|---|---|---|
| `0x30` | n/a | `2` | service bytes, обычно `[0x00, 0x03]` | ESP service cycle | `runtime_config_apply_from_ram()` |
| `0x50` | n/a | profile-based | `byte0=msg_len`, `byte1=ttl`, `byte2=lock`, `byte3...=msg` | ESP HMI/message logic | `process_host_message_from_isr()` -> `myQueueHmiMsgHandle` |
| `0x70` | n/a | `5` | `[result, reg1_b3, reg1_b2, reg1_b1, reg1_b0]` | ESP auth result flow | `process_auth_result_write()` |
| `0x88` | n/a | `7` | BCD RTC packet | ESP time sync flow | `service_time_sync_from_master()` |
| `0xE0` | n/a | `16` | config block `E0..EF` | ESP runtime config | `runtime_config_apply_from_ram()` |

---

## 8. Подробно по `TIME`

## 8.1 Откуда появляется `TIME`

Producer:

- `Core/Src/service_time_sync.c`
- `service_time_sync_on_tick(uint8_t *ram)`

Что делает функция:

1. читает 7 байт времени из DS3231 в `s_hw_time`
2. копирует эти 7 байт в `ram[0x60..0x66]`
3. отправляет в `myQueueToMasterHandle` пакет:
   - `type = PACKET_TIME`
   - `len = 7`
   - `payload = s_hw_time`

### Важное уточнение по trigger

В `Core/Src/main.c` у `cb_OneSec()` есть комментарий:

- periodic 1-second callback сам по себе сейчас не генерирует `TIME`
- `TIME packets are generated by TCA6408a interrupt from DS3231 1Hz output`

Это подтверждается и кодом в `Core/Src/service_tca6408.c`, где по соответствующему событию вызывается:

- `service_time_sync_on_tick(iram)`

То есть фактический business-trigger для публикации `TIME` — не таймер `cb_OneSec()`, а внешний interrupt path через `TCA6408A`.

## 8.2 Как `TIME` попадает в I2C RAM

Далее `StartTaskRxTxI2c1()` забирает пакет из очереди и вызывает `app_i2c_slave_publish()`.

На publish-стадии:

- `type` пишется в `s_ram[0x00]`
- payload копируется в `s_ram[0x80..]`
- outbox ждёт, пока ESP сначала прочитает `0x00`, а затем `0x80`

## 8.3 Почему `TIME` читается длиной 8, хотя producer даёт 7

Это особенность контракта:

- producer создаёт пакет с `len = 7`
- но `packet_len_for_type(PACKET_TIME, ...)` возвращает контрактное окно `8`

Значит на wire-level master читает:

- `reg = 0x80`
- `len = 8`

Практически это означает:

- `byte0..6` — реальные BCD данные времени
- `byte7` — дополнительный байт окна, обычно нулевой после `memset`

## 8.4 Связанные внутренние RAM-области

Помимо окна `0x80`, у `TIME` есть ещё внутренние служебные зоны:

- `0x60..0x66` — локальный snapshot времени
- `0x88..0x8E` — входящий packet `TIME set` от ESP
- `0x6F = 1` — флаг после применения `service_time_sync_from_master()`

---

## 9. Подробно по `UID PN532`

Producer:

- `Core/Src/service_pn532_task.c`

При чтении данных PN532 в очередь уходит пакет:

- `type = PACKET_UID_532`
- `payload = &s_slaveTxData[13]`
- `len = 8`

На publish-стадии он попадает в контрактное окно:

- `reg = 0x01`
- `len = 15`

С точки зрения ESP важен именно контракт окна `0x01/15`.

По `docs/i2c_uid532_contract.md` ожидание такое:

- `byte0 = uid_len`
- `byte1..14 = uid/data`

То есть локальная внутренняя длина producer может быть меньше, но в `s_ram` окно всё равно готовится как фиксированное 15-байтное.

---

## 10. Подробно по `PACKET_PIN`, `PACKET_WIEGAND`, `PACKET_PIN_HMI`

## 10.1 `PACKET_PIN` (`0x10`)

Источник:

- `Core/Src/service_matrix_kbd.c`

Когда пользователь завершает ввод на matrix keypad (`#`):

- собирается буфер `s_keyb.buf`
- в очередь кладётся:
  - `type = PACKET_PIN`
  - `payload = s_keyb.buf`
  - `len = s_keyb.offset`

В I2C RAM публикуется контрактное окно `13` байт.

## 10.2 `PACKET_WIEGAND` (`0x20`)

Источник:

- `Core/Src/wiegand.c`

В очередь кладётся:

- `type = PACKET_WIEGAND`
- `payload = get_wiegand_data()`
- `len = inputdata.bitlength + 2`

В I2C RAM это публикуется как окно `15` байт.

## 10.3 `PACKET_PIN_HMI` (`0x40`)

Источник:

- `Core/Src/hmi.c`

После подтверждения PIN на HMI в очередь кладётся:

- `type = PACKET_PIN_HMI`
- `payload = &pin_snapshot`
- `len = saved_len + 2`

Фактический layout строится из `READER_t`:

- `rtype`
- `bitlength`
- `rdata[]`

В I2C RAM окно публикуется как `15` байт.

---

## 11. Подробно по записям ESP -> STM32

## 11.1 `0x70` — auth result

Нормативная форма:

- `[0x70, 0x05, result, reg1_b3, reg1_b2, reg1_b1, reg1_b0]`

Проверка длины:

- `strict_rx_len_for_register()` требует ровно `5`

После полного приёма вызывается deferred processing:

- `process_auth_result_write()`

Поведение:

- `0x00` -> fail -> buzzer
- `0x01` -> success -> relay pulse
- остальные значения принимаются без критичных side effects

## 11.2 `0x50` — HMI message

ESP пишет payload в фиксированной header-форме `[reg, len, payload...]`.

В текущей STM32-логике ожидается layout внутри RAM:

- `ram[0x50]` = `msg_len`
- `ram[0x51]` = `msg_ttl`
- `ram[0x52]` = `hmi_lock`
- `ram[0x53...]` = `msg_buf`

Обработка:

- `process_host_message_from_isr()`
- дальше сообщение уходит в `myQueueHmiMsgHandle`

## 11.3 `0x88` — установка времени

ESP пишет:

- `[0x88, 0x07, bcd7...]`

После полного приёма в task context выполняется:

- `service_time_sync_from_master()`
- затем `service_time_sync_datetimepack()`

STM32 валидирует пакет и при необходимости обновляет DS3231.

## 11.4 `0x30` — service/counter

ESP пишет:

- `[0x30, 0x02, 0x00, 0x03]`

Это воспринимается как service-cycle write и приводит к:

- `runtime_config_apply_from_ram(s_ram)`

## 11.5 `0xE0..0xEF` — config block

ESP пишет 16 байт конфигурации:

- `[0xE0, 0x10, cfg[16]]`

После полного приёма вызывается:

- `runtime_config_apply_from_ram(s_ram)`

---

## 12. Диагностическое окно `0xF0..0xFF`

`app_i2c_slave_sync_diag_to_ram()` регулярно синхронизирует это окно в guard task.

Layout:

| Offset | Значение |
|---:|---|
| `0` | `progress_timeout_count` |
| `1` | `stuck_scl_count` |
| `2` | `stuck_sda_count` |
| `3` | `abort_count` |
| `4` | `relisten_count` |
| `5` | `hard_recover_count` |
| `6` | `malformed_count` |
| `7` | `recover_fail_count` |
| `8..9` | `last_recovery_ms` (LE) |
| `10..11` | `max_recovery_ms` (LE) |
| `12` | `HAL_I2C state` |
| `13` | physical `SCL` level |
| `14` | physical `SDA` level |
| `15` | reserved |

На практике это основной канал наблюдения за состоянием I2C slave.

---

## 13. Как реально устроена FSM при чтении и записи

## 13.1 Header phases

FSM в `app_i2c_slave.c` жёстко разделяет приём на этапы:

1. принять `reg`
2. принять `len`
3. затем либо:
   - принять `payload` при master-write
   - либо передать `payload` при master-read

Ключевые поля FSM:

- `offset`
- `rx_count`
- `tx_count`
- `first`
- `second`
- `final`

## 13.2 Side effects только после полного write frame

Это важно для контракта:

- ни `0x70`, ни `0x88`, ни `0x50`, ни `0xE0` не должны обрабатываться до полного получения payload

Это соблюдается через:

- `app_i2c_slave_rx_complete()`
- `process_register_write_from_isr()`
- deferred flags в `process_deferred_actions()`

## 13.3 Read validation

При чтении STM32 проверяет:

- что `reg` известен
- что `len > 0`
- что чтение не выходит за пределы `s_ram`

---

## 14. Практическая summary для ESP

Если смотреть на контракт глазами ESP, то логика такая:

### При получении IRQ от STM32

1. прочитать `0x00`, `len=1`
2. получить `packet type`
3. по типу выбрать payload окно
4. прочитать payload фиксированной длины

### Основные slave-publish окна

- `PACKET_UID_532` -> читать `0x01`, `len=15`
- `PACKET_PIN` -> читать `0x10`, `len=13`
- `PACKET_WIEGAND` -> читать `0x20`, `len=15`
- `PACKET_PIN_HMI` -> читать `0x40`, `len=15`
- `PACKET_TIME` -> читать `0x80`, `len=8`
- diagnostics -> читать `0xF0`, `len=16`

### Основные master-write окна

- auth result -> писать `0x70`, `len=5`
- HMI message -> писать `0x50`, `len=profile-based`, но всегда с header `[reg,len]`
- TIME set -> писать `0x88`, `len=7`
- service -> писать `0x30`, `len=2`
- config -> писать `0xE0`, `len=16`

---

## 15. Нюансы и caveats

1. `type` в `0x00` не заменяет `reg`.
   - `type` только говорит, какой пакет ждёт.
   - реальное чтение всё равно делается через конкретное `reg` и `len`.

2. Для `TIME` есть расхождение между producer length и wire window length.
   - producer даёт `7`
   - контракт окна — `8`
   - это нормально для текущей реализации

3. `0xF0..0xFF` — это надёжное окно диагностики.
   - оно особенно полезно при анализе stuck bus / recovery

4. `PACKET_ERROR` объявлен в mapping, но в текущем просмотренном коде не найден как отдельный явный queue-based producer.
   - practically: использовать `0xF0..0xFF` как diag read window

---

## 16. Короткий итог

В проекте используется единый `s_ram[256]` как основное I2C register-space STM32 slave.

Ключевая модель:

- `0x00` — это индекс pending packet type
- реальные payload-данные лежат в отдельных fixed windows (`0x01`, `0x10`, `0x20`, `0x40`, `0x80`, `0xF0`)
- STM32 публикует события через outbox + IRQ pin
- ESP всегда делает двухшаговое чтение: сначала `type`, потом payload
- записи ESP также идут в этот же RAM по строго определённым окнам (`0x70`, `0x50`, `0x88`, `0x30`, `0xE0`)

Для практической интеграции ESP главным рабочим reference можно считать:

- `docs/i2c_global_contract.md`
- `docs/i2c_uid532_contract.md`
- `docs/i2c_slave_outbox_flow.md`
- `Core/Inc/main.h`
- `Core/Src/app_i2c_slave.c`
- `Core/Src/service_tca6408.c`