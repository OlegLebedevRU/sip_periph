# I2C TIME reliability report (2026-03-24)

## Итоговый статус

На текущем состоянии после перепрошивки **мастера и slave ошибок не наблюдается** в TIME-only тесте.

Ключевой вывод: проблема не была связана с контрактом I2C slave как таковым. Контракт `[reg, len]` и EEPROM-style read/write в целом работали корректно. Основной вклад в сбои вносили:

1. кастомная правка в `HAL_I2C_ER_IRQHandler()`;
2. слишком агрессивный pacing publish chain на slave;
3. повышенная чувствительность slave-детектора `SCL low` в `LISTEN idle`;
4. слишком плотная последовательность EEPROM-style транзакций на мастере.

---

## Наблюдавшиеся симптомы до исправлений

Диагностика HMI и `0xF0..0xFF` ранее показывала повторяющийся паттерн:

- `SclSt` заметно растёт;
- `SdaSt` иногда растёт, но слабее;
- `Abort` и `HdRcv` растут почти синхронно со `SclSt`;
- `Malf = 0`, `TmOut = 0` или близко к 0.

Это означало, что:

- malformed/contract violations не являлись главной причиной;
- slave чаще всего входил в recovery по признаку stuck bus / low-level condition, а не по ошибке register map или длины.

---

## Диагностические байты `0xF0..0xFF`

Фактически использовалась следующая раскладка:

- `[0]` `progress_timeout_count`
- `[1]` `stuck_scl_count`
- `[2]` `stuck_sda_count`
- `[3]` `abort_count`
- `[4]` `relisten_count`
- `[5]` `hard_recover_count`
- `[6]` `malformed_count`
- `[7]` `recover_fail_count`
- `[8..9]` `last_recovery_ms`
- `[10..11]` `max_recovery_ms`
- `[12]` `HAL_I2C_GetState(&hi2c1)`
- `[13]` physical SCL level
- `[14]` physical SDA level
- `[15]` reserved

Промежуточный улучшенный снимок после первой итерации был таким:

- `progress_timeout_count = 0`
- `stuck_scl_count = 2`
- `stuck_sda_count = 0`
- `abort_count = 2`
- `hard_recover_count = 2`
- `malformed_count = 0`
- `recover_fail_count = 0`
- `last_recovery_ms = 2 ms`
- `max_recovery_ms = 2 ms`
- `SCL = HIGH`, `SDA = HIGH`

Это уже тогда подтвердило, что контракт работает, а recovery-триггер локализован в `SCL low`-ветке.

---

## Внесённые изменения на slave

### 1. Возврат `HAL_I2C_ER_IRQHandler()` к vendor flow

Файл:
- `Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_i2c.c`

Что сделано:
- удалена кастомная логика `CR1.SWRST` в ветке `BERR` внутри `HAL_I2C_ER_IRQHandler()`;
- обработчик возвращён к vendor-style flow.

Почему это важно:
- кастомный `SWRST` внутри error IRQ усиливал риск каскадного recovery и нестабильного поведения после bus error;
- vendor flow лучше соответствует ожидаемой логике HAL для slave repeated-start / AF / BERR path.

---

### 2. Первая итерация: глобальный Option A pacing во все publish chain

Файл:
- `Core/Src/app_i2c_slave.c`

Что сделано:
- pacing добавлен централизованно в `StartTaskRxTxI2c1()`;
- применён ко **всем** пакетам из `myQueueToMasterHandle`, не только к TIME.

Параметры:
- `I2C_SLAVE_PUBLISH_PRE_DELAY_MS = 10`
- `I2C_SLAVE_PUBLISH_POST_DELAY_MS = 10`
- `I2C_SLAVE_OUTBOX_RETRY_DELAY_MS = 20`

Что это дало:
- снизилась плотность publish/read overlap;
- заметно уменьшилось количество recovery even before second iteration.

---

### 3. Вторая итерация: смягчение idle `SCL/SDA low`-детектора

Файл:
- `Core/Src/app_i2c_slave.c`

Что сделано:
- добавлен debounce/confirmation для детектора stuck bus в `poll_bus_health()`;
- одиночный краткий `SCL low` больше не вызывает recovery;
- нужен подтверждённый low в течение окна времени и нескольких подряд опросов.

Параметры:
- `I2C_SLAVE_STUCK_CONFIRM_POLLS = 3`
- `I2C_SLAVE_STUCK_CONFIRM_MS = 15`

Дополнительно:
- debounce state сбрасывается при любом прогрессе FSM;
- debounce state сбрасывается при выходе из idle/listen-условия.

Почему это важно:
- master работает в EEPROM-style (`write header -> repeated start -> read`), и слишком чувствительный idle detector мог воспринимать короткое окно как false stuck.

---

## Что подтверждено по мастеру

Файл:
- `docs/leo4_stm32_bus.c`

После обновления master-side логики подтверждены дополнительные паузы:

1. `vTaskDelay(1 ms)` сразу после события из очереди перед началом обработки;
2. `vTaskDelay(BUS_STABILITY_DELAY)` после чтения `PACKET_TYPE`;
3. `vTaskDelay(BUS_STABILITY_DELAY)` перед записью `TIME set` (`I2C_REG_HW_TIME_ADDR + 8`);
4. `vTaskDelay(BUS_STABILITY_DELAY)` перед записью `I2C_REG_COUNTER_ADDR` (`0x30`);
5. `vTaskDelay(BUS_STABILITY_DELAY)` перед записью `I2C_REG_CFG_ADDR` (`0xE0`);
6. `vTaskDelay(BUS_STABILITY_DELAY)` в конце цикла.

Это согласуется с результатом: после перепрошивки мастера и slave ошибки перестали воспроизводиться.

---

## Почему контракт признан рабочим

Для TIME-only сценария использовалась стандартная outbox-схема:

1. master читает `I2C_PACKET_TYPE_ADDR` (`0x00`, len=1);
2. получает `PACKET_TIME`;
3. master читает `I2C_REG_HW_TIME_ADDR` (`0x80`, len=8);
4. slave считает пакет подтверждённым только после payload read.

Что важно:
- `malformed_count` не рос;
- `progress_timeout_count` не был доминирующим симптомом;
- ошибки register contract не подтверждены диагностикой.

Следовательно, register map и сам wire-contract не были корневой причиной.

---

## Итоговое инженерное заключение

Фикс оказался составным и низкорисковым:

1. возвращён vendor HAL error flow;
2. введён консервативный pacing publish chain на slave;
3. ослаблена чувствительность idle stuck detector на slave;
4. добавлены паузы между EEPROM-style транзакциями на мастере.

На текущем этапе это привело к состоянию:

- контракт I2C slave работает штатно;
- TIME-only сценарий стабилен;
- после перепрошивки master + slave ошибки не наблюдаются.

---

## Что считать за regression markers в будущем

Если проблема вернётся, первым делом смотреть:

- HMI: `SclSt`, `SdaSt`, `Abort`, `HdRcv`, `Malf`, `TmOut`;
- `0xF0..0xFF`:
  - `[1]`, `[2]`, `[3]`, `[5]`, `[6]`, `[7]`, `[13]`, `[14]`.

Особенно подозрительный паттерн:
- `SclSt` растёт,
- `Malf = 0`,
- `TmOut = 0`,
- `Abort ~= HdRcv`.

Это снова будет указывать не на контракт, а на timing/bus-level поведение.
