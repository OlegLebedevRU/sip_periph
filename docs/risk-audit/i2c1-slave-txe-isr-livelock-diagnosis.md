# STM32F411 I2C1 slave TXE/TRA/BUSY ISR livelock — diagnosis

Repository: `sip_periph`
Date: 2026-05-21
Context: post-mortem diagnosis of a hang first captured after PR #21
(`Harden I2C1 anti-hang path with multilayer watchdogs and staged STM32 recovery escalation`)
and observed again on firmware matching the current repository state.

---

## Summary (EN)

A production failure was captured where STM32F411 stayed permanently in `I2C1_EV_IRQn`
while acting as an I2C1 slave transmitter for the ESP32 master. FreeRTOS tasks, SysTick
handling, PendSV context switching, DWIN updates, PN532 processing, and I2C2 service
work all stopped. The MCU core was still `RUNNING`, but the scheduler could not execute
because the active exception never returned.

### Evidence from hot-plug dump

**Active exception**

`SCB->ICSR = 0x1400E82F`

- `VECTACTIVE = 0x2F` = exception 47.
- External IRQ number = `47 - 16 = 31`.
- On STM32F411, IRQn 31 is `I2C1_EV_IRQn`.

Repeated reads confirmed `ICSR & 0x1FF = 0x2F` was stable.

**Not a CPU fault**

- `CFSR = 0x00000000`
- `HFSR = 0x00000000`
- `SHCSR = 0x00000000`

No HardFault/MemManage/BusFault/UsageFault was pending or active.

**Scheduler could not run**

- `ICSR.PENDSTSET = 1`
- `ICSR.PENDSVSET = 1`
- `SysTick->CTRL = 0x00010007`
- `SysTick ENABLE = 1`, `TICKINT = 1`, `COUNTFLAG = 1`

SysTick and PendSV were pending, but never serviced because the core remained inside
`I2C1_EV_IRQn`.

**I2C1 peripheral state**

Base `0x40005400`:

```text
CR1   = 0x00000401
CR2   = 0x0000071E
OAR1  = 0x00004022
OAR2  = 0x00000000
DR    = 0x00000001
SR1   = 0x00000080
SR2   = 0x00000006
CCR   = 0x00006019
TRISE = 0x0000000A
FLTR  = 0x00000000
```

Interpretation:

- `SR1.TXE = 1` — transmit data register empty.
- `SR2.BUSY = 1` — bus busy.
- `SR2.TRA = 1` — slave transmitter.
- `SR2.MSL = 0` — slave mode.
- `CR2.ITBUFEN = 1`, `ITEVTEN = 1`, `ITERREN = 1` — buffer/event/error IRQs enabled.

This is the dangerous condition: slave transmitter with `TXE=1` and buffer interrupt
enabled, but no data being supplied to `DR`.

**GPIO / clock stretching**

`GPIOB_IDR = 0x000006BB`, `GPIOB_ODR = 0x00000233`:

- PB6 / I2C1_SCL = LOW.
- PB7 / I2C1_SDA = HIGH.

The STM32 was physically holding SCL low via slave clock stretching.

### Root cause

The STM32F4 I2C slave can enter an unrecoverable event-interrupt loop when the ESP32
starts a read transaction while the application-level I2C slave FSM/HAL transmit state
is not ready to provide a valid TX buffer. The peripheral enters slave-transmit phase
with `TXE=1`, `TRA=1`, `BUSY=1`, and `ITBUFEN=1`. The peripheral stretches SCL low
waiting for `DR` to be written, while the event IRQ remains active/re-enters
continuously.

Task-level recovery is ineffective for this failure mode because FreeRTOS tasks do not
get CPU time while the core is trapped in `I2C1_EV_IRQn`.

### Required protection model

The fix must include independent layers:

1. ISR-level detection of repeated `TXE && TRA && BUSY && ITBUFEN` in
   `I2C1_EV_IRQHandler`.
2. Immediate unexpected-read handling that either provides a dummy TX byte or resets,
   never leaving TXE without data.
3. Independent watchdog reset if interrupt livelock or any other CPU starvation
   prevents task-context feeding.
4. Retained `.noinit` diagnostics so the next boot can report why the previous run
   reset.

---

## Подробная диагностика (RU)

Зафиксирован **ISR-level livelock** в `I2C1_EV_IRQn`, а не HardFault и не «обычное»
зависание одной задачи FreeRTOS.

Ключевое:

- `ICSR & 0x1FF = 0x2F` стабильно при повторных чтениях;
- `0x2F` соответствует активному исключению №47, т.е. `IRQn = 47 - 16 = 31 = I2C1_EV_IRQn` для STM32F411;
- `I2C1_SR1=0x0080` (`TXE=1`), `I2C1_SR2=0x0006` (`BUSY=1`, `TRA=1`, `MSL=0`);
- `GPIOB_IDR=0x06BB`, `PB6/SCL=LOW`, `PB7/SDA=HIGH` (физический clock stretching со стороны STM32 slave);
- fault-регистры нулевые: `CFSR=0`, `HFSR=0`, `SHCSR=0`.

Следствие: CPU постоянно занят в `I2C1_EV_IRQHandler`, `SysTick`/`PendSV` висят в pending
и не исполняются, поэтому task-level watchdog/recovery из PR #21 не могут выполниться.

### Наблюдаемые симптомы на устройстве

- STM32F411 в состоянии `CPU: RUNNING` (STM32CubeProgrammer).
- I2C1 slave-шина к ESP32 «залипает»: STM32 удерживает/растягивает SCL.
- На DWIN/HMI перестаёт обновляться время.
- DWIN touch перестаёт обрабатываться.
- PN532-события не обрабатываются.
- HardFault/MemManage/BusFault/UsageFault не фиксируются.

### Почему debug-конфигурация не дала воспроизведение

В debug-конфигурации (STM32CubeIDE) зависание не повторялось, поэтому фиксация делалась
через STM32CubeProgrammer в режиме hot-plug/live dump без остановки по fault-breakpoint.

Практический вывод: ошибка проявляется как timing-sensitive ISR livelock под реальной
нагрузкой/времянками шины, а не как классический fault exception.

### Как снимались данные

Инструмент: STM32CubeProgrammer, hot-plug/live чтение регистров и памяти в момент уже
наблюдаемого зависания (без reliance на fault handlers).

Использованные артефакты:

| Артефакт | Содержание | Назначение |
|---|---|---|
| `bak/STM32F411.txt` | Снимок системных регистров Cortex-M/NVIC/SysTick и периферии | Доказать активный IRQ, pending системных исключений и отсутствие fault |
| `bak/0xE000E000.bin` | Dump системного блока (`0xE000E000`, 3584 байта) | Побайтовая проверка состояния core/system control |
| `bak/I2C1.bin` | Dump I2C1 (`0x40005400`, 64 байта) | Подтвердить TXE/BUSY/TRA и режим slave-transmitter |
| `bak/GPIOB.bin` | Dump GPIOB (`0x40020400`, 48 байта) | Подтвердить физические уровни SCL/SDA |
| `bak/MSP.bin` | Снимок MSP/stack контекста | Дополнительная forensic-проверка контекста исполнения |

### Декодирование `ICSR`: CPU застрял в `I2C1_EV_IRQn`

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

### Декодирование `I2C1.bin`: slave-transmitter TXE loop

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

Мастер ESP32 начал read transaction, STM32 вошёл в slave-transmit фазу, но следующий
байт в `DR` не подан. При `TXE=1` и `ITBUFEN=1` event IRQ остаётся
активным/переактивируется, формируя бесконечный ISR цикл.

### Декодирование `GPIOB.bin`: подтверждение clock stretching

`GPIOB_IDR = 0x000006BB` → `PB6/SCL = LOW`, `PB7/SDA = HIGH`. Это соответствует clock
stretching со стороны STM32 slave в transmit phase.

### Почему PR #21 не мог восстановить систему в этом кейсе

PR #21 добавил полезные task-level watchdog/recovery слои, но они исполняются только
когда scheduler получает квант CPU. В зафиксированном состоянии `SysTick` и `PendSV`
уже pending, но поток исполнения непрерывно занят в `I2C1_EV_IRQn`, поэтому
recovery-логика из задач не может стартовать.

---

## Resolution (this PR)

This PR implements the first two protection layers from "Required protection model"
and prepares the codebase for the third (IWDG):

| # | Layer | Status | Files |
|---|---|---|---|
| 1 | ISR-level detection of `TXE && TRA && BUSY && ITBUFEN` in `I2C1_EV_IRQHandler` | **Implemented** — `app_i2c_slave_i2c1_ev_irq_guard_before_hal()` / `..._after_hal()` are now invoked around `HAL_I2C_EV_IRQHandler(&hi2c1)`; the spin threshold (`I2C1_ISR_TXE_TRA_BUSY_SPIN_LIMIT`) has been tightened from 256 to 64 entries to trigger within ~1 ms. On firing, `I2C1_EV/ER_IRQn` are masked, `CR2.ITEVTEN/ITBUFEN/ITERREN` and `CR1.PE` are cleared, the IRQ pending bit is cleared with `NVIC_ClearPendingIRQ` + `__DSB/__ISB`, so the core exits the vector and PendSV can finally run. | `Core/Src/stm32f4xx_it.c`, `Core/Src/app_i2c_slave.c` |
| 2 | Unexpected-read handling that never leaves `TXE=1` with no data | **Implemented** — `app_i2c_slave_addr_callback()` now follows the policy `fast NACK/abort → dummy TX → controlled reset`. The fast path synchronously preloads `I2C1->DR` via the new `pre_arm_tx_buffer()` helper, guaranteeing TXE is cleared before the ISR exits. Repeated unexpected reads escalate through `unexpected_read_dummy_or_reset()` and finally to a controlled bus reset (`I2C_RECOVERY_REASON_I2C1_UNEXPECTED_READ_RESET`) once `I2C1_UNEXPECTED_READ_CONTROLLED_RESET_LIMIT` is hit, instead of staying in dummy mode forever. | `Core/Src/app_i2c_slave.c` |
| 3 | Independent hardware watchdog (IWDG) | **Deferred** — the direct-register IWDG bring-up code is present (`APP_IWDG_Init()` in `Core/Src/main.c`) and the supervisor task `StartTaskWatchdog` already gates refreshes on `app_i2c_slave_watchdog_can_refresh()`. Enabling it requires flipping `APP_IWDG_ENABLE=1` and validating reset behaviour on the target — tracked as a follow-up PR. | (none in this PR) |
| 4 | `.noinit` reset-reason diagnostics | **Reused** — all new ISR-driven paths persist via `persist_event_reason()` / `persist_i2c1_snapshot()` and bump the existing counters `i2c1_isr_txe_tra_busy_count` / `i2c1_unexpected_read_count`. `init_noinit_diag()` already preserves them across resets and `app_i2c_slave_init()` mirrors them into the boot-visible `s_diag` mirror. | `Core/Src/app_i2c_slave.c` |

### Out of scope

- Migration of I2C tasks away from `osPriorityRealtime` (point 6 of the diagnosis —
  explicitly secondary).
- Enabling `IWDG` in production (point 3) — follow-up PR.
- Any changes to `Drivers/`, `Middlewares/ST/`, linker scripts, or `sip_periph.ioc`
  (forbidden by `AGENTS.md`).
