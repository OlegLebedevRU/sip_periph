# Диагностические сообщения I2C Slave

## Актуальный статус на 2026-03-24

После двух итераций правок slave и добавления пауз на master-side в EEPROM-style последовательности текущий TIME-only тест проходит в состоянии **без наблюдаемых ошибок**.

Что подтверждено по результатам анализа:
- register contract `[reg, len]` и общая EEPROM-style модель обмена работают корректно;
- основная проблема была не в контракте, а в сочетании error-handler behavior + слишком плотного publish/read timing + слишком чувствительного idle stuck-detector;
- полезными оказались четыре изменения:
  1. возврат `HAL_I2C_ER_IRQHandler()` к vendor flow;
  2. глобальный консервативный pacing для всех publish chain на slave;
  3. debounce/confirmation для `SCL/SDA low` в `LISTEN idle`;
  4. дополнительные паузы между транзакциями на мастере.

## Смысл диагностических полей (I2C RAM `0xF0..0xFF`)

Эти 16 байт — диагностический снимок состояния I2C1 slave, доступный мастеру (ESP) для удалённого мониторинга здоровья шины.

### Счётчики ошибок (saturating uint8, макс. 255)

| Смещение | Поле | Что означает |
|----------|------|-------------|
| `[0]` | `progress_timeout_count` | Сколько раз FSM «завис» — не было прогресса в рамках дедлайна (`s_phase_deadline_tick` истёк). Slave начал транзакцию, но она не завершилась вовремя. |
| `[1]` | `stuck_scl_count` | Сколько раз обнаружен залипший SCL (линия тактирования прижата к LOW в состоянии LISTEN). Обычно указывает на проблему у мастера или аппаратный сбой. |
| `[2]` | `stuck_sda_count` | Аналогично для SDA (линия данных). Может означать, что slave или мастер не отпустил шину после транзакции. |
| `[3]` | `abort_count` | Сколько раз был запрошен аварийный сброс транзакции (через `schedule_abort_recovery`), т.е. сколько раз FSM попадал в нештатную ситуацию. |
| `[4]` | `relisten_count` | Сколько раз slave успешно перезапустил прослушивание (`HAL_I2C_EnableListen_IT`). Нормально, что растёт — каждая завершённая транзакция требует re-listen. Полезно для оценки общего трафика. |
| `[5]` | `hard_recover_count` | Сколько раз выполнялся полный сброс периферии (`HAL_I2C_DeInit` + `HAL_I2C_Init`). Это самая тяжёлая мера восстановления. Ненулевое значение — повод для беспокойства. |
| `[6]` | `malformed_count` | Сколько раз получен некорректный запрос: неизвестный регистр, несовпадение длины с контрактом, выход за границы `s_ram`. |
| `[7]` | `recover_fail_count` | Сколько раз `EnableListen_IT` или `Init` вернули ошибку после восстановления. Критичный показатель — шина не восстановилась. |

### Тайминги восстановления

| Смещение | Поле | Что означает |
|----------|------|-------------|
| `[8..9]` | `last_recovery_ms` | Длительность **последнего** восстановления в мс (uint16 LE). Показывает, как быстро slave вернулся в строй. |
| `[10..11]` | `max_recovery_ms` | **Максимальная** длительность восстановления за всё время работы. Помогает выявить worst-case задержки. |

### Мгновенное состояние (snapshot)

| Смещение | Поле | Что означает |
|----------|------|-------------|
| `[12]` | HAL I2C1 state | Текущее состояние HAL-автомата (`HAL_I2C_STATE_LISTEN`, `BUSY_TX`, `BUSY_RX` и т.д.). Позволяет мастеру понять, не завис ли slave в промежуточной фазе. |
| `[13]` | SCL pin level | Физический уровень SCL (0=LOW, 1=HIGH). В покое должен быть HIGH. |
| `[14]` | SDA pin level | Физический уровень SDA. В покое должен быть HIGH. |
| `[15]` | reserved | Зарезервировано, всегда 0. |

### Как использовать

Мастер (ESP) периодически читает регистр `0xF0` (16 байт). Если `[0]`,`[1]`,`[2]`,`[5]`,`[6]`,`[7]` ненулевые — на шине были проблемы. Если `[13]` или `[14]` показывают LOW при `[12]` = LISTEN — шина залипла прямо сейчас.

## Документация модуля `app_i2c_slave.c`

### Назначение

I2C1 slave-обёртка протокола с антизалипающими мерами для связи STM32 ↔ ESP по шине I2C.

---

### Ключевые меры против залипания / clock stretching

1. Жёсткий контроль длины для известных входных регистров
2. Любая аномалия → reset FSM + re-enable LISTEN
3. После error/abort/listen complete гарантированно отпускается EVENT pin, сбрасывается outbox state
4. При BERR/AF — быстрая реинициализация `hi2c1` + LISTEN

---

### Константы восстановления

| Макрос | Значение | Описание |
|--------|----------|----------|
| `I2C_SLAVE_HEADER_TIMEOUT_MS` | 50 мс | Таймаут ожидания заголовка транзакции |
| `I2C_SLAVE_PAYLOAD_TIMEOUT_MS` | 100 мс | Таймаут ожидания полезной нагрузки |
| `I2C_SLAVE_IDLE_STUCK_TIMEOUT_MS` | 500 мс | Таймаут входа в проверку stuck bus в состоянии LISTEN |
| `I2C_SLAVE_RECOVERY_COOLDOWN_MS` | 2 мс | Пауза между DeInit и Init при жёстком восстановлении |
| `I2C_SLAVE_PUBLISH_PRE_DELAY_MS` | 10 мс | Пауза перед `app_i2c_slave_publish()` для всех publish chain |
| `I2C_SLAVE_PUBLISH_POST_DELAY_MS` | 10 мс | Пауза сразу после `app_i2c_slave_publish()` |
| `I2C_SLAVE_OUTBOX_RETRY_DELAY_MS` | 20 мс | Шаг ожидания при занятом outbox |
| `I2C_SLAVE_STUCK_CONFIRM_POLLS` | 3 | Минимум подряд опросов LOW перед фиксацией stuck |
| `I2C_SLAVE_STUCK_CONFIRM_MS` | 15 мс | Минимальное окно LOW перед фиксацией stuck |

---

### Важное обновление по HAL

В `Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_i2c.c` обработчик `HAL_I2C_ER_IRQHandler()` был возвращён к vendor flow:
- удалена кастомная установка `CR1.SWRST` в ветке `BERR`;
- это уменьшило риск каскадных recovery после bus error.

---

### Локальное состояние

| Переменная | Тип | Описание |
|-----------|-----|----------|
| `s_ram[256]` | `uint8_t[]` | Общая RAM-область, доступная мастеру по I2C (регистровая модель) |
| `s_i2c_test` | `struct i2c_test_s` | Счётчики адресных/приёмных событий (отладка) |
| `s_i2c_sec_ctrl_h` | `struct i2c_seq_ctrl_s` | FSM-состояние текущей транзакции (first/second/final фазы) |
| `s_outbox_busy` | `uint8_t` | Флаг: исходящий пакет ожидает чтения мастером |
| `s_outbox_type` | `uint8_t` | Тип пакета в outbox (`I2cPacketType_t`) |
| `s_outbox_reg` | `uint8_t` | Адрес регистра outbox-пакета |
| `s_outbox_len` | `uint8_t` | Длина outbox-пакета |
| `s_last_tx_reg` | `uint8_t` | Регистр последней TX-передачи (для ACK-сверки) |
| `s_last_tx_len` | `uint8_t` | Длина последней TX-передачи |
| `s_event_latched` | `uint8_t` | EVENT-линия прижата (ожидается чтение мастером) |
| `s_phase_deadline_tick` | `uint32_t` | Абсолютный tick дедлайна текущей фазы |
| `s_last_progress_tick` | `uint32_t` | Tick последнего прогресса FSM |
| `s_recovery_pending` | `uint8_t` | Флаг отложенного жёсткого восстановления |
| `s_recovery_started_tick` | `uint32_t` | Tick начала текущего восстановления (для замера длительности) |
| `s_idle_scl_low_polls` | `uint8_t` | Сколько подряд guard-опросов наблюдался LOW на SCL в idle/listen |
| `s_idle_sda_low_polls` | `uint8_t` | Сколько подряд guard-опросов наблюдался LOW на SDA в idle/listen |
| `s_idle_scl_low_since_tick` | `uint32_t` | Tick начала подтверждаемого LOW на SCL |
| `s_idle_sda_low_since_tick` | `uint32_t` | Tick начала подтверждаемого LOW на SDA |
| `s_diag` | `app_i2c_slave_diag_t` | Структура диагностических счётчиков |

---

### Вспомогательные функции

#### Тайминг и watchdog

| Функция | Описание |
|---------|----------|
| `tick_now()` | Возвращает `HAL_GetTick()` |
| `tick_expired(now, deadline)` | Проверяет, истёк ли дедлайн (с учётом переполнения int32) |
| `note_progress(timeout_ms)` | Обновляет `s_last_progress_tick` и устанавливает новый дедлайн |
| `clear_progress_watchdog()` | Сбрасывает дедлайн (транзакция завершена/idle) |

#### Диагностика

| Функция | Описание |
|---------|----------|
| `i2c1_line_is_low(pin)` | Читает физический уровень SCL/SDA |
| `note_recovery_duration()` | Вычисляет длительность восстановления, обновляет `last/max_recovery_ms` |
| `mark_malformed_and_recover()` | Инкрементирует `malformed_count`, ставит флаг recovery |
| `poll_bus_health()` | Проверяет timeout прогресса и подтверждённый stuck bus в `LISTEN idle`; для LOW теперь требуется debounce/confirmation |

#### Протокол

| Функция | Описание |
|---------|----------|
| `packet_reg_for_type(type)` | Маппинг типа пакета → адрес регистра в `s_ram` |
| `packet_len_for_type(type, len)` | Маппинг типа пакета → контрактная длина |
| `strict_rx_len_for_register(base)` | Строгая ожидаемая длина записи для известных входных регистров (0 = переменная) |
| `is_known_register(base)` | Проверяет, является ли адрес известным регистром |
| `validate_read_request(reg, len)` | Валидация запроса чтения: длина > 0, регистр известен, не выходит за `s_ram` |

#### Управление состоянием

| Функция | Описание |
|---------|----------|
| `force_idle_event_line()` | Отпускает EVENT-линию (HIGH), сбрасывает `s_event_latched` |
| `reset_fsm_state()` | Сброс FSM и тестовых счётчиков в начальное состояние |
| `outbox_complete_ack()` | Полный сброс outbox: busy=0, type=NULL, EVENT=idle |
| `recover_after_error()` | outbox\_complete\_ack + reset\_fsm\_state |

#### Восстановление

| Функция | Описание |
|---------|----------|
| `restart_listen_if_needed()` | Если HAL-состояние ≠ LISTEN → reset FSM + EnableListen\_IT |
| `hard_recover_bus()` | Полный цикл: DeInit → delay → Init → EnableListen\_IT |
| `schedule_abort_recovery()` | Инкрементирует `abort_count`, ставит `s_recovery_pending = 1` |
| `poll_bus_health()` | Проверяет дедлайн прогресса и залипание SCL/SDA в LISTEN |
| `execute_pending_recovery()` | Если `s_recovery_pending` → вызывает `hard_recover_bus()` |

---

### Публичный API

| Функция | Описание |
|---------|----------|
| `app_i2c_slave_init()` | Инициализация: очистка RAM, диаг, сброс FSM |
| `app_i2c_slave_get_ram()` | Возвращает указатель на `s_ram[256]` |
| `app_i2c_slave_get_test_state()` | Возвращает указатель на `s_i2c_test` |
| `app_i2c_slave_get_seq_state()` | Возвращает указатель на `s_i2c_sec_ctrl_h` |
| `app_i2c_slave_get_diag()` | Возвращает указатель на `s_diag` (read-only) |
| `app_i2c_slave_poll_recovery()` | Вызов `poll_bus_health()` + `execute_pending_recovery()` |
| `app_i2c_slave_publish(pckt)` | Публикация пакета в outbox: заполняет `s_ram`, прижимает EVENT-линию |
| `app_i2c_slave_has_errors()` | Возвращает 1, если хотя бы один счётчик ошибок ненулевой |

---

### HMI-диагностика (LCD-строки)

`app_i2c_slave_format_diag_line(index, buf, buflen)` — форматирует 8 строк по ≤10 символов:

| index | Формат | Поле |
|-------|--------|------|
| 0 | `TmOut:XXXX` | `progress_timeout_count` |
| 1 | `SclSt:XXXX` | `stuck_scl_count` |
| 2 | `SdaSt:XXXX` | `stuck_sda_count` |
| 3 | `Abort:XXXX` | `abort_count` |
| 4 | `HdRcv:XXXX` | `hard_recover_count` |
| 5 | `Malf :XXXX` | `malformed_count` |
| 6 | `RcFai:XXXX` | `recover_fail_count` |
| 7 | `MaxMs:XXXXX` | `max_recovery_ms` |

---

### Задачи RTOS

#### `StartTaskI2cGuard`

Периодическая задача (period ≈ 5 мс):
1. `app_i2c_slave_poll_recovery()` — проверка здоровья шины, исполнение отложенного восстановления
2. `process_deferred_actions()` — выполнение действий, отложенных из ISR (time sync, auth result)
3. `app_i2c_slave_sync_diag_to_ram()` — обновление диагностического снимка в RAM

#### `StartTaskRxTxI2c1`

Основная задача обработки исходящих пакетов:
1. Запускает Listen при старте
2. Ждёт пакет из `myQueueToMasterHandle`
3. Дожидается состояния LISTEN (с recovery при зацикливании >20 попыток)
4. Дожидается освобождения outbox
5. Делает `I2C_SLAVE_PUBLISH_PRE_DELAY_MS`
6. Публикует пакет через `app_i2c_slave_publish()`
7. Делает `I2C_SLAVE_PUBLISH_POST_DELAY_MS`

Важно: этот pacing применяется ко **всем** publish chain, а не только к TIME.

---

## Практическая интерпретация диагностики после исправлений

Если при будущих тестах наблюдается паттерн:
- `Malf = 0`
- `TmOut = 0`
- растут в основном `SclSt`, `Abort`, `HdRcv`

то это указывает скорее на timing/bus-level поведение, а не на нарушение контракта регистров.

Если одновременно:
- `SCL = HIGH`
- `SDA = HIGH`
- `recover_fail_count = 0`

то recovery отрабатывает штатно и шина в момент снимка уже отпущена.

---

## Сопутствующие документы

Для краткого итогового отчёта и master-side рекомендаций см. также:
- `docs/i2c_time_reliability_report_2026-03-24.md`
- `docs/i2c_master_recommendations_2026-03-24.md`