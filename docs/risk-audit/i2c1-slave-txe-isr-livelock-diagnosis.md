# STM32F411 I2C1 slave TXE ISR livelock — диагностика зависания

Repository: `sip_periph`  
Контекст: пост-фактум диагностика зависания, зафиксированного после PR #21 (`Harden I2C1 anti-hang path with multilayer watchdogs and staged STM32 recovery escalation`).

---

## Краткий итог

Зафиксирован **ISR-level livelock** в `I2C1_EV_IRQn`, а не HardFault и не “обычное” зависание одной задачи FreeRTOS.

Ключевое:

- `ICSR & 0x1FF = 0x2F` стабильно при повторных чтениях;
- `0x2F` соответствует активному исключению №47, т.е. `IRQn = 47 - 16 = 31 = I2C1_EV_IRQn` для STM32F411;
- `I2C1_SR1=0x0080` (`TXE=1`), `I2C1_SR2=0x0006` (`BUSY=1`, `TRA=1`, `MSL=0`);
- `GPIOB_IDR=0x06BB`, `PB6/SCL=LOW`, `PB7/SDA=HIGH` (физический clock stretching со стороны STM32 slave);
- fault-регистры нулевые: `CFSR=0`, `HFSR=0`, `SHCSR=0`.

Следствие: CPU постоянно занят в `I2C1_EV_IRQHandler`, `SysTick`/`PendSV` висят в pending и не исполняются, поэтому task-level watchdog/recovery из PR #21 не могут выполниться.

---

## Наблюдаемые симптомы на устройстве

- STM32F411 в состоянии `CPU: RUNNING` (STM32CubeProgrammer).
- I2C1 slave-шина к ESP32 “залипает”: STM32 удерживает/растягивает SCL.
- На DWIN/HMI перестаёт обновляться время.
- DWIN touch перестаёт обрабатываться.
- PN532-события не обрабатываются.
- HardFault/MemManage/BusFault/UsageFault не фиксируются.

---

## Почему debug-конфигурация не дала воспроизведение

В debug-конфигурации (STM32CubeIDE) зависание не повторялось, поэтому фиксация делалась через STM32CubeProgrammer в режиме hot-plug/live dump без остановки по fault-breakpoint.

Практический вывод: ошибка проявляется как timing-sensitive ISR livelock под реальной нагрузкой/времянками шины, а не как классический fault exception.

---

## Как снимались данные

Инструмент: STM32CubeProgrammer, hot-plug/live чтение регистров и памяти в момент уже наблюдаемого зависания (без reliance на fault handlers).

Использованные артефакты:

| Артефакт | Содержание | Назначение |
|---|---|---|
| `bak/STM32F411.txt` | Снимок системных регистров Cortex-M/NVIC/SysTick и периферии | Доказать активный IRQ, pending системных исключений и отсутствие fault |
| `bak/0xE000E000.bin` | Dump системного блока (`0xE000E000`, 3584 байта) | Побайтовая проверка состояния core/system control |
| `bak/I2C1.bin` | Dump I2C1 (`0x40005400`, 64 байта) | Подтвердить TXE/BUSY/TRA и режим slave-transmitter |
| `bak/GPIOB.bin` | Dump GPIOB (`0x40020400`, 48 байта) | Подтвердить физические уровни SCL/SDA |
| `bak/MSP.bin` | Снимок MSP/stack контекста | Дополнительная forensic-проверка контекста исполнения |

---

## Декодирование `ICSR`: CPU застрял в `I2C1_EV_IRQn`

Из `bak/STM32F411.txt`:

```text
ICSR = 0x1400E82F
PENDSVSET = 1
PENDSTSET = 1
RETTOBASE = 1
VECTACTIVE = 0x2F
```

Расшифровка:

- `VECTACTIVE = 0x2F = 47`
- `External IRQn = 47 - 16 = 31`
- на STM32F411 `IRQn 31 = I2C1_EV_IRQn`

Дополнительное подтверждение: пользовательское наблюдение показывает, что `ICSR & 0x1FF = 0x2F` остаётся стабильным при повторных чтениях (не “моментальный” снимок, а устойчивое состояние).

---

## Декодирование fault-регистров: это не HardFault

Из `bak/STM32F411.txt`:

```text
CFSR  = 0x00000000
HFSR  = 0x00000000
SHCSR = 0x00000000
```

Вывод: fault exception отсутствует. Корневая проблема — ISR lock/livelock в обработке I2C1 event IRQ.

---

## Декодирование `I2C1.bin`: slave-transmitter TXE loop

Ключевые значения из `bak/I2C1.bin`:

```text
CR2 = 0x0000071E
SR1 = 0x00000080
SR2 = 0x00000006
```

Интерпретация:

- `SR1=0x0080` → `TXE=1` (data register empty),
- `SR2=0x0006` → `BUSY=1`, `TRA=1`, `MSL=0` (slave transmitter),
- `CR2=0x071E` → прерывания событий/буфера/ошибок включены (`ITEVTEN=1`, `ITBUFEN=1`, `ITERREN=1`).

То есть мастер ESP32 начал read transaction, STM32 вошёл в slave-transmit фазу, но следующий байт в `DR` не подан. При `TXE=1` и `ITBUFEN=1` event IRQ остаётся активным/переактивируется, формируя бесконечный ISR цикл.

---

## Декодирование `GPIOB.bin`: подтверждение clock stretching

Ключевое из `bak/GPIOB.bin`:

```text
GPIOB_IDR = 0x000006BB
```

Для I2C1:

- `PB6 = I2C1_SCL`,
- `PB7 = I2C1_SDA`.

По `GPIOB_IDR=0x06BB`:

- `PB6/SCL = LOW`,
- `PB7/SDA = HIGH`.

Это соответствует clock stretching со стороны STM32 slave в transmit phase.

---

## Root cause (зафиксированный вывод)

**EN:**  
`STM32F411 I2C1 slave can enter an unrecoverable slave-transmit TXE interrupt loop. When ESP32 starts a read transaction while the app-level I2C slave FSM/HAL state is not ready to provide a transmit buffer, I2C1 enters TRA/BUSY with TXE=1 and ITBUFEN enabled. The peripheral stretches SCL low waiting for DR to be written. Since the I2C1 event IRQ remains active/re-enters continuously, SysTick and PendSV remain pending and FreeRTOS tasks never run. Therefore task-level watchdog/recovery added in PR #21 cannot execute.`

**RU:**  
Причина зависания — не HardFault и не зависание одной задачи FreeRTOS, а ISR-level livelock в `I2C1_EV_IRQn`. Периферия I2C1 в состоянии slave transmitter (`TXE=1`, `BUSY=1`, `TRA=1`) с включённым buffer interrupt; STM32 удерживает SCL в LOW. CPU постоянно обслуживает `I2C1_EV_IRQn`, поэтому `SysTick/PendSV` и задачи FreeRTOS не выполняются. По этой причине task-level watchdog-и и staged recovery из PR #21 не могут сработать.

---

## Почему PR #21 не мог восстановить систему в этом кейсе

PR #21 добавил полезные task-level watchdog/recovery слои, но они исполняются только когда scheduler получает квант CPU.

В зафиксированном состоянии:

- `SysTick` уже pending (`STCSR=0x00010007`, `COUNTFLAG=1`),
- `PendSV` pending (`ICSR.PENDSVSET=1`),
- но поток исполнения непрерывно занят в `I2C1_EV_IRQn`.

Итог: task-контекст не выполняется, поэтому recovery-логика из задач не может стартовать.

---

## Следующие инженерные меры (рекомендации для будущего PR, не реализация в этом документе)

1. Включить аппаратный `IWDG`; выполнять `IWDG refresh` только из task/supervisor context (не из ISR), в соответствии с правилом ISR-safe API (в ISR использовать только `*FromISR` и не выполнять task-level recovery прямо в обработчике).
2. Добавить ISR-level guard в `I2C1_EV_IRQHandler` на патологическое состояние `TXE + TRA + BUSY`:
   - guard должен быть lightweight и не выполнять тяжёлое восстановление внутри IRQ;
   - сигнал в отложенный контекст передавать через `xTaskNotifyFromISR` (supervisor task) или recovery queue event;
   - при использовании notify-path учитывать `portYIELD_FROM_ISR` для минимизации задержки обработки;
   - само восстановление выполнять вне IRQ;
   - дополнительно добавить pre-condition проверки перед включением `ITBUFEN`.
3. В `unexpected-read` path не оставлять slave “без байта” и явно задать policy выбора реакции:
   - порядок по умолчанию: `NACK/abort` → `dummy TX` → `controlled reset`;
   - аварийный `NACK/abort` — применять первым при недопустимом состоянии FSM до начала выдачи полезных данных;
   - `dummy TX` (предопределённый fail-safe байт, например `0xFF`) — применять, если транзакцию нужно быстро и детерминированно завершить без немедленного reset;
   - `controlled reset` I2C1 state machine (через `I2C_CR1_SWRST` либо RCC peripheral reset) в отложенном контексте — применять, если состояние не нормализуется после abort/dummy path или повторяется в пределах контрольного окна.
   После любой из веток обязателен гарантированный re-init/listen re-arm.
   Тяжёлый reset-sequence не выполнять прямо внутри IRQ, кроме явно оговорённого last-resort сценария.
4. Перед `NVIC_SystemReset()` сохранять код причины reset (и диагностический reason code) в `.noinit`.
5. Сохранить task-level watchdog-и как вторичный слой защиты, явно учитывая, что при ISR lock они не исполняются.
6. Пересмотреть использование `osPriorityRealtime` для I2C-задач (профилактика task-level starvation), но считать это вторичным фактором, не первопричиной данного ISR-зависания.

---

## Scope заметки

Этот документ фиксирует результаты диагностики и границы следующего PR.  
Он **не** вносит изменений в firmware code path и не является реализацией recovery-механизмов.
