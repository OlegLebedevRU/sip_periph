# I2C1 ISR watchdog recovery plan

Date: 2026-05-21

## Goal

Prevent STM32F411 from remaining indefinitely in the I2C1 slave event interrupt while holding PB6/SCL low. Liveness has priority over preserving a partial I2C transaction or a pending outbox packet.

## Implemented recovery layers

### 1. ISR-level I2C1 EV livelock guard

Location:

- `Core/Src/stm32f4xx_it.c`
- `Core/Src/app_i2c_slave.c`
- `Core/Inc/app_i2c_slave.h`

`I2C1_EV_IRQHandler()` now samples the peripheral state before and after `HAL_I2C_EV_IRQHandler(&hi2c1)`:

```c
app_i2c_slave_i2c1_ev_irq_guard_before_hal();
HAL_I2C_EV_IRQHandler(&hi2c1);
app_i2c_slave_i2c1_ev_irq_guard_after_hal();
```

The guard detects the captured failure signature:

- `I2C1->SR1 & I2C_SR1_TXE`
- `I2C1->SR2 & I2C_SR2_TRA`
- `I2C1->SR2 & I2C_SR2_BUSY`
- `I2C1->CR2 & I2C_CR2_ITBUFEN`

It avoids reading `SR2` while `ADDR` is pending, because on STM32F4 reading `SR2` participates in ADDR clearing.

Threshold: `I2C1_ISR_TXE_TRA_BUSY_SPIN_LIMIT = 256` consecutive guard samples.

Emergency action:

1. Write retained diagnostics into `.noinit`.
2. Disable `I2C1_EV_IRQn` and `I2C1_ER_IRQn`.
3. Disable I2C1 event/buffer/error interrupt bits.
4. Clear `I2C_CR1_PE`.
5. Execute `NVIC_SystemReset()`.

No RTOS API, queue, mutex, delay, or blocking HAL recovery is used in this ISR emergency path.

### 2. Unexpected master-read hardening

Location: `app_i2c_slave_addr_callback()` in `Core/Src/app_i2c_slave.c`.

If the ESP32 starts a read transaction while the app-level FSM does not have a validated offset/count preamble, the code no longer only schedules task-level recovery. It now:

1. Records `I2C1_UNEXPECTED_READ_DUMMY_TX` in retained event diagnostics.
2. Immediately calls `HAL_I2C_Slave_Seq_Transmit_IT()` with a single static dummy byte (`0x00`) and `I2C_LAST_FRAME`.
3. Marks the transaction malformed and schedules the existing task-level recovery as an additional layer.
4. If dummy TX cannot be armed, or if unexpected reads repeat `I2C1_UNEXPECTED_READ_RESET_LIMIT = 16` times consecutively, it records `I2C1_UNEXPECTED_READ_RESET` and resets immediately.

This prevents the slave transmitter state from being left with `TXE=1` and no TX data.

### 3. Hardware IWDG supervisor

Location: `Core/Src/main.c` user-code sections.

The project does not currently include `stm32f4xx_hal_iwdg.c/.h`, so the IWDG is configured directly through CMSIS registers instead of enabling a missing HAL module.

Configuration:

- Prescaler code: `APP_IWDG_PRESCALER_DIV32 = 3` (`/32`).
- Reload: `APP_IWDG_RELOAD_2S = 1999`.
- Expected timeout: approximately 2 seconds at nominal 32 kHz LSI.
- Refresh period: `APP_IWDG_REFRESH_MS = 250`.

Refresh policy:

- Only `StartTaskWatchdog()` refreshes `IWDG->KR = 0xAAAA`.
- The watchdog is not refreshed from I2C/UART/EXTI/SysTick/any ISR.
- If the core is trapped in `I2C1_EV_IRQn`, this task cannot run and IWDG resets the MCU.
- If the I2C guard task heartbeat becomes stale for `APP_IWDG_I2C_GUARD_STALE_MS = 750`, the supervisor records `IWDG_SUPERVISOR_TIMEOUT` and intentionally stops refreshing IWDG.

### 4. Retained `.noinit` diagnostics

Location:

- `Core/Src/app_i2c_slave.c`
- `STM32F411CEUX_FLASH.ld`
- `STM32F411CEUX_RAM.ld`

The retained structure `s_noinit_recovery` now includes:

- `magic`
- `boot_count`
- `last_reset_reason`
- `system_reset_count`
- `last_event_reason`
- `i2c1_isr_txe_tra_busy_count`
- `i2c1_unexpected_read_count`
- `iwdg_supervisor_timeout_count`
- `last_i2c1_sr1`
- `last_i2c1_sr2`
- `last_i2c1_cr2`
- `last_gpiob_idr`

Both linker scripts now define `.noinit (NOLOAD)` after `.bss`, outside the startup zero-init range.

## Reset/event reason codes

Existing reason codes `0..11` are preserved. New codes:

| Code | Name |
|---:|---|
| 12 | `I2C1_ISR_TXE_TRA_BUSY_SPIN` |
| 13 | `I2C1_UNEXPECTED_READ_DUMMY_TX` |
| 14 | `I2C1_UNEXPECTED_READ_RESET` |
| 15 | `IWDG_SUPERVISOR_TIMEOUT` |

`last_reset_reason` is used for reset-causing conditions. `last_event_reason` can record non-reset events such as dummy TX.

## Diagnostics exposure

On boot, retained fields are copied into `app_i2c_slave_diag_t`. Existing diagnostic RAM export at `I2C_REG_STM32_ERROR_ADDR` still exposes the low byte of `last_recovery_reason`; richer retained fields are available in RAM through `s_noinit_recovery` during hot-plug/debug and through `app_i2c_slave_get_diag()` for future UI/export extensions.

## Test plan

1. Normal operation:
   - ESP32 write/read register protocol still returns valid payloads.
   - `PACKET_TIME` still updates.
   - DWIN touch/time display still works.
   - PN532/TCA6408/DS3231 on I2C2 still operate.

2. Unexpected read injection:
   - Start ESP32 read without a valid offset/count preamble.
   - Expected: STM32 returns dummy `0x00` or resets after repeated invalid reads; PB6/SCL must not remain low indefinitely.

3. Aggressive polling / TXE livelock stress:
   - Force repeated read starts while the FSM is not ready.
   - Expected: ISR guard trips before permanent lock; next boot has `last_reset_reason = 12` and snapshots of SR1/SR2/CR2/GPIOB.

4. Scheduler starvation test:
   - Artificially prevent tasks from running after IWDG starts.
   - Expected: IWDG resets after approximately 2 seconds.

5. Post-reset recovery:
   - `boot_count` increments.
   - I2C1 returns to listen state.
   - ESP32 no longer sees permanent clock stretching.

## Remaining risks

- IWDG uses the LSI oscillator; actual timeout varies with LSI tolerance.
- CubeMX regeneration may not know about the manual IWDG setup unless equivalent `.ioc` settings are added later.
- Reset diagnostics in `.noinit` survive software/IWDG reset but not power loss or RAM corruption.
- The ISR guard intentionally prioritizes liveness over completing the current I2C transaction; a packet/outbox item may be lost.
