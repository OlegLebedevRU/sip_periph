# TCA6408 protocol in project logic

## Scope

This document fixes the **actual project protocol** for `TCA6408A` processing.
Current working polarity from code is the source of truth.

- `~INT` from `TCA6408A` is connected to `STM32 EXT_INT` (`PC13`)
- IRQ routing stays in `app_irq_router`
- business logic lives in `service_tca6408`
- matrix keyboard flow is out of scope for this refactoring

## Input map

- `P0` = `DS3231M SQW 1Hz`
- `P2` = `BTN_OPEN` external mechanical button
- `P3` = `PN532 IRQ`

## Signal polarity used by firmware

The project keeps the **working polarity already used in code**:

- `P0 / DS3231 SQW` — active `LOW`
- `P2 / BTN_OPEN` — active `LOW`
- `P3 / PN532 IRQ` — active `LOW`

This means the physical text description may differ, but the firmware contract is defined by current validated behavior.

## IRQ model

1. `TCA6408A` asserts `~INT`
2. `STM32 EXTI` fires on falling edge
3. ISR only disables `EXT_INT` and posts a queue event
4. `StartTasktca6408a()` calls `service_tca6408_process_irq_event()`
5. service reads `TCA6408A reg[0]`
6. service re-enables `EXT_INT`
7. business routing is decided from `prev_inputs` + `curr_inputs`

## Edge and level detection

## `P3 / PN532 IRQ`

Firmware keeps the current working behavior:

- readiness is accepted when `P3 == LOW`
- no polarity inversion is introduced during refactoring
- service releases `pn532SemaphoreHandle`

This preserves the existing correct external behavior of PN532.

## `P0 / DS3231 1Hz`

`SQW` produces two changes per period, but only one project event must be generated.

Rules:

- active phase is treated as `LOW`
- only one time-sync event is generated per active-low cycle
- repeated IRQs while the same low phase is still latched are ignored
- if `DS` is mixed with `BTN` or `PN532` in the same change set, `DS` is dropped

## `P2 / BTN_OPEN`

Button processing is two-stage:

1. any change on `P2` starts debounce window
2. after debounce timeout the service reads `reg[0]` again
3. decision is made only from the stable reread level

Rules:

- if stable level becomes `HIGH`, button release is propagated to relay service
- if `RELE1` is active, stable press is dropped
- if external-button flow is already active, new button events are dropped
- after accepted stable press, new button events are suppressed for `200 ms`

## Arbitration

Priority is logical, not queue-based:

1. keep working `PN532` ready behavior
2. process button with debounce/suppress rules
3. process `DS3231` only if not mixed with other changed inputs

## Integration points

- `Core/Src/app_irq_router.c` — ISR routing only
- `Core/Src/main.c` — thin task wrapper only
- `Core/Src/service_tca6408.c` — full TCA state machine
- `Core/Src/service_relay_actuator.c` — relay activity status for button suppression
- `Core/Src/service_time_sync.c` — DS tick consumer
