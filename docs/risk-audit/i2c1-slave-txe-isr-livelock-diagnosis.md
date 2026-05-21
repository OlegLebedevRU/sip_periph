# I2C1 slave TXE/TRA/BUSY ISR livelock diagnosis

Date: 2026-05-21

## Summary

A production failure was captured where STM32F411 stayed permanently in `I2C1_EV_IRQn` while acting as an I2C1 slave transmitter for the ESP32 master. FreeRTOS tasks, SysTick handling, PendSV context switching, DWIN updates, PN532 processing, and I2C2 service work all stopped. The MCU core was still `RUNNING`, but the scheduler could not execute because the active exception never returned.

## Evidence from hot-plug dump

### Active exception

`SCB->ICSR = 0x1400E82F`

- `VECTACTIVE = 0x2F` = exception 47.
- External IRQ number = `47 - 16 = 31`.
- On STM32F411, IRQn 31 is `I2C1_EV_IRQn`.

Repeated reads confirmed `ICSR & 0x1FF = 0x2F` was stable.

### Not a CPU fault

- `CFSR = 0x00000000`
- `HFSR = 0x00000000`
- `SHCSR = 0x00000000`

No HardFault/MemManage/BusFault/UsageFault was pending or active.

### Scheduler could not run

- `ICSR.PENDSTSET = 1`
- `ICSR.PENDSVSET = 1`
- `SysTick->CTRL = 0x00010007`
- `SysTick ENABLE = 1`, `TICKINT = 1`, `COUNTFLAG = 1`

SysTick and PendSV were pending, but never serviced because the core remained inside `I2C1_EV_IRQn`.

### I2C1 peripheral state

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

This is the dangerous condition: slave transmitter with `TXE=1` and buffer interrupt enabled, but no data being supplied to `DR`.

### GPIO / clock stretching

`GPIOB_IDR = 0x000006BB`, `GPIOB_ODR = 0x00000233`:

- PB6 / I2C1_SCL = LOW.
- PB7 / I2C1_SDA = HIGH.

The STM32 was physically holding SCL low via slave clock stretching.

## Root cause

The STM32F4 I2C slave can enter an unrecoverable event-interrupt loop when the ESP32 starts a read transaction while the application-level I2C slave FSM/HAL transmit state is not ready to provide a valid TX buffer. The peripheral enters slave-transmit phase with `TXE=1`, `TRA=1`, `BUSY=1`, and `ITBUFEN=1`. The peripheral stretches SCL low waiting for `DR` to be written, while the event IRQ remains active/re-enters continuously.

Task-level recovery is ineffective for this failure mode because FreeRTOS tasks do not get CPU time while the core is trapped in `I2C1_EV_IRQn`.

## Required protection model

The fix must include independent layers:

1. ISR-level detection of repeated `TXE && TRA && BUSY && ITBUFEN` in `I2C1_EV_IRQHandler`.
2. Immediate unexpected-read handling that either provides a dummy TX byte or resets, never leaving TXE without data.
3. Independent watchdog reset if interrupt livelock or any other CPU starvation prevents task-context feeding.
4. Retained `.noinit` diagnostics so the next boot can report why the previous run reset.
