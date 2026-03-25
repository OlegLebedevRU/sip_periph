# TCA6408 integration notes

## Files

- `Core/Inc/service_tca6408.h`
- `Core/Src/service_tca6408.c`
- `Core/Src/app_irq_router.c`
- `Core/Src/main.c`

## Responsibility split

### `app_irq_router`

- handles `HAL_GPIO_EXTI_Callback()` fan-out
- for `EXT_INT_Pin`:
  - disables `EXT_INT` IRQ line
  - posts one message into `myQueueTCA6408Handle`
- does not read I2C
- does not interpret TCA inputs

### `StartTasktca6408a`

- initializes `service_tca6408`
- posts bootstrap event once after init
- waits queue events
- delegates each event to `service_tca6408_process_irq_event()`

### `service_tca6408`

- configures TCA as inputs
- stores previous and current snapshots
- reads `TCA6408A_REG_INPUT`
- computes `changed` mask
- runs DS / button / PN532 dispatch rules
- re-enables `EXT_INT` after I2C read

## Notes

- `PN532` logic intentionally keeps existing active-low behavior
- matrix keyboard is intentionally untouched in this change set
- OLED time string must use synchronized `ram[0x60..0x66]`
