# STM32 I2C Bus Checkup (ESP32 <-> STM32)

Документ для разработчика STM32 по текущему контракту шины I2C в проекте `sip`.

## 1. Базовые параметры шины

- Master: ESP32
- Slave: STM32
- 7-bit адрес STM32: `0x11`
- Линии: `SDA=GPIO14`, `SCL=GPIO13`
- Скорость: `400 kHz`
- IRQ от STM32 в ESP32: `EVENT_INT_PIN=34`, фронт `negedge`
- Таймауты master (ESP32):
  - mutex: 1000 ms
  - transmit: 1000 ms
  - receive: 200 ms

## 2. Wire-формат транзакций (важно)

ESP32 использует единый протокол заголовка:

- `READ`: master отправляет 2 байта `[reg_addr, data_len]`, затем читает `data_len` байт
- `WRITE`: master отправляет `[reg_addr, data_len, payload...]`

STM32 должен корректно поддерживать оба сценария на slave-стороне.

## 3. Карта регистров

- `0x00` - `I2C_PACKET_TYPE_ADDR`
- `0x01` - `I2C_REG_532_ADDR`
- `0x10` - `I2C_REG_MATRIX_PIN_ADDR`
- `0x20` - `I2C_REG_WIEGAND_ADDR`
- `0x30` - `I2C_REG_COUNTER_ADDR`
- `0x40` - `I2C_REG_HMI_PIN_ADDR`
- `0x50` - `I2C_REG_HMI_MSG_ADDR`
- `0x70` - `I2C_REG_HMI_ACT_ADDR`
- `0x80` - `I2C_REG_HW_TIME_ADDR`
- `0xE0` - `I2C_REG_CFG_ADDR`
- `0xF0` - `I2C_REG_STM32_ERROR_ADDR`

## 4. Типы пакетов (`I2C_PACKET_TYPE_ADDR`)

- `0` - `PACKET_NULL`
- `1` - `PACKET_UID_532`
- `2` - `PACKET_PIN`
- `3` - `PACKET_WIEGAND`
- `4` - `PACKET_HMI` (в ESP32 пока не обрабатывается)
- `5` - `PACKET_TIME`
- `6` - `PACKET_PIN_HMI`
- `7` - `PACKET_ACK` (в ESP32 пока не обрабатывается)
- `8` - `PACKET_NACK` (в ESP32 пока не обрабатывается)
- `9` - `PACKET_ERROR`

## 5. Контракт payload по пакетам

### 5.1 `PACKET_UID_532` (`reg=0x01`, `len=15`)

- ESP32 читает 15 байт
- `data[0]` = длина входа
- `data[1..]` = данные UID

### 5.2 `PACKET_PIN` (`reg=0x10`, `len=13`)

- ESP32 читает 13 байт
- `data[1]` = длина
- `data[2..]` = код

### 5.3 `PACKET_WIEGAND` (`reg=0x20`, `len=15`)

- ESP32 читает 15 байт
- `data[1]` = длина
- `data[2..]` = данные

### 5.4 `PACKET_PIN_HMI` (`reg=0x40`, `len=15`)

- ESP32 читает 15 байт
- `data[1]` = длина
- `data[2..]` = ввод с клавиатуры/HMI

### 5.5 `PACKET_TIME` (`reg=0x80`, `len=8`)

STM32 -> ESP32 (чтение из `0x80`):

- используется BCD и локальное время
- порядок полей (байты `0..6`):
  - `sec` (00..59)
  - `min` (00..59)
  - `hour` (00..23)
  - `wday` (01..07), где **понедельник=1**, воскресенье=7
  - `mday` (01..31)
  - `mon` (01..12)
  - `year` (00..99, интерпретация 20YY)
- `byte[7]` сейчас не используется ESP32

ESP32 -> STM32 (запись в `0x88`, `len=7`):

- те же 7 BCD-полей, локальное время ESP32
- `wday` всегда в шкале понедельник=1 ... воскресенье=7

## 6. Правила актуальности времени

- Сравнение делается по локальному времени
- `age_sec = abs(esp_local_time - stm32_local_time)`
- Порог свежести: `5` секунд
- Если `age_sec > 5`: только лог + runtime-метрика (без блокировки обмена)
- Если NTP на ESP32 невалиден (epoch/1970):
  - допускается доверие времени STM32/DS3231M, если `stm32_time > 2026-03-20 00:00:00 local`

## 7. Ответ ESP32 по результату авторизации

### 7.1 Action result (`reg=0x70`, `len=5`)

Payload:

- `byte0`: `result`
- `byte1..4`: `res_register_1` (big-endian)

### 7.2 HMI message (`reg=0x50`, `len=msg[0]+3`)

Payload копируется из `InputAuthResult_t.msg[]`.

## 8. Служебные записи ESP32 после обработки пакета

После обработки входного пакета ESP32 пишет:

- `0x30`, `len=2`: счетчик `count1` (`{0,3}`)
- `0xE0`, `len=16`: конфигурация STM32 из NVS (`cfg_stm32`)

STM32 должен принимать эти записи в любом цикле обработки пакетов.

## 9. MQTT-диагностика времени

Для событий `HELLO_FROM_DEV` (0) и `DEV_HELTHCHECK` (44) ESP32 публикует:

- тег `339`: абсолютный age времени (`age_sec`) как **строковый int** (`"0"`, `"1"`, ...)
- если age недоступен: `"-1"`

## 10. Чек-лист STM32 (полный контракт)

- [ ] Подтвержден slave-адрес `0x11` (7-bit)
- [ ] Реализован wire-протокол заголовка `[reg,len]` для read/write
- [ ] Реализован источник типа пакета в `0x00`
- [ ] `PACKET_UID_532`: формат `len + payload` совпадает с контрактом
- [ ] `PACKET_PIN`: формат `data[1]=len, data[2..]=payload`
- [ ] `PACKET_WIEGAND`: формат `data[1]=len, data[2..]=payload`
- [ ] `PACKET_PIN_HMI`: формат `data[1]=len, data[2..]=payload`
- [ ] `PACKET_TIME`: BCD-валидация, локальное время, `wday` с понедельника
- [ ] Прием записи времени от ESP32 в `0x88` (7 BCD байт)
- [ ] Прием action-результата в `0x70` (5 байт)
- [ ] Прием HMI-сообщения в `0x50`
- [ ] Прием `counter` в `0x30` и `cfg` в `0xE0`
- [ ] IRQ-сигнал на `EVENT_INT_PIN`: данные готовы к чтению до завершения транзакции
- [ ] Поведение при `PACKET_ERROR` согласовано (в ESP32 пока TODO)
- [ ] Проверен сценарий NTP-invalid на ESP32 (fallback на STM32/DS3231M)
- [ ] Проверен сценарий `age_sec > 5` (логируется, но обмен не рвется)

## 11. Рекомендации по тестам

Минимальный набор интеграционных тестов:

1. Позитивный цикл каждого `PACKET_*` с валидными длинами
2. Невалидные длины и границы буферов
3. `PACKET_TIME` с корректным и некорректным BCD
4. Проверка weekday mapping: Monday=1, Sunday=7
5. Drift тесты по времени: `age=0..5` и `age>5`
6. Проверка после перезапуска ESP32 и STM32

## 12. Ex register (0xE0)

`Ex` is a runtime control register written by I2C master.

- bit0 (`0x01`): enable relay pulse from external button (`TCA p2 -> relay timer chain`)
- bit1 (`0x02`): reserved / auth-timeout action flag
- bit2 (`0x04`): reserved / auth-fail action flag
- bit3..bit7: reserved

Implementation notes:
- `rele_mode_flag` is derived from `Ex.bit0`.
- External button pulse runs only if `Ex.bit0 == 1`.
- Button is debounced in firmware (`EXT_BTN_DEBOUNCE_MS`) and re-armed on release (`p2` back HIGH).