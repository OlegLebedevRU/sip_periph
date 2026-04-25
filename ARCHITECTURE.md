# ARCHITECTURE.md — Top-Level Architecture Map

> This is a starter scaffold. Sections marked **TBD** require deeper investigation and will be expanded in follow-up PRs.

---

## Hardware Target

| Property | Value |
|---|---|
| MCU | STM32F411CEU (ARM Cortex-M4, 100 MHz, 512 KB Flash, 128 KB RAM) |
| Package | UFQFPN48 |
| USB | USB OTG FS (USB Device, CDC class) |
| I²C | I²C1 (slave — host interface), I²C2 (master — on-board peripherals) |
| UART | USART1 or USART2 — DWIN display; UART for GM810 touch controller |
| GPIO | Matrix keyboard rows/columns, relay, buzzer, IRQ lines |
| Pinout reference | [`docs/chip-pinout-preliminary.md`](docs/chip-pinout-preliminary.md) |

---

## Top-Level Module Map

### Application Services (`Core/Src/service_*.c`)

| Module | File | Description |
|---|---|---|
| `service_pn532_task` | `Core/Src/service_pn532_task.c` | PN532 NFC reader — I²C2 master, task `myTask532` |
| `service_tca6408` | `Core/Src/service_tca6408.c` | TCA6408A 8-bit I/O expander — I²C2 master, task `myTask_tca6408a` |
| `service_time_sync` | `Core/Src/service_time_sync.c` | DS3231M RTC driver + BCD time sync, triggered by TCA P0 1 Hz SQW |
| `service_gm810_uart` | `Core/Src/service_gm810_uart.c` | GM810 touch controller — UART interface |
| `service_oled_task` | `Core/Src/service_oled_task.c` | SSD1306 OLED 128×64 display, task `myTaskOLED` |
| `service_matrix_kbd` | `Core/Src/service_matrix_kbd.c` | 4×3 matrix keyboard, GPIO EXTI |
| `service_relay_actuator` | `Core/Src/service_relay_actuator.c` | Relay and buzzer outputs |
| `service_runtime_config` | `Core/Src/service_runtime_config.c` | Runtime config from register space `ram[0xE0..0xE4]` |

### Infrastructure / App Modules (`Core/Src/app_*.c`)

| Module | File | Description |
|---|---|---|
| `app_i2c_slave` | `Core/Src/app_i2c_slave.c` | I²C1 slave engine — register map, outbox/inbox, IRQ callbacks |
| `app_irq_router` | `Core/Src/app_irq_router.c` | Routes GPIO EXTI events to services |
| `app_uart_dwin` | `Core/Src/app_uart_dwin.c` | DWIN serial display — RX path |
| `app_uart_dwin_tx` | `Core/Src/app_uart_dwin_tx.c` | DWIN serial display — TX path |

### HMI / Display

| Module | File | Description |
|---|---|---|
| `hmi` | `Core/Src/hmi.c` | HMI task (`myTaskHmi`) |
| `hmi_console` | `Core/Src/hmi_console.c` | USB CDC debug console, task `myTaskHmiMsg` |
| `hmi_diag_helper` | `Core/Src/hmi_diag_helper.c` | Diagnostic helpers for HMI output |
| `dwin_gfx` | `Core/Src/dwin_gfx.c` | DWIN display graphics primitives |
| `ssd1306` | `Core/Src/ssd1306.c` | SSD1306 OLED low-level driver |

### Other

| Module | File | Description |
|---|---|---|
| `pn532_com` | `Core/Src/pn532_com.c` | Low-level PN532 I²C communication primitives |
| `wiegand` | `Core/Src/wiegand.c` | Wiegand protocol decoder, task `myTaskWiegand` |

---

## FreeRTOS Tasks

Tasks are created in `main.c` using CMSIS-RTOS v1 (`osThreadDef` / `osThreadCreate`).

| Task handle | Entry function | Priority | Stack (words) | Description |
|---|---|---|---|---|
| `defaultTaskHandle` | `StartDefaultTask` | Normal | 128 | Default/init task — one-time inits then idle loop |
| `myTask532Handle` | `StartTask532` | AboveNormal | 256 | PN532 NFC reader loop (`service_pn532_task.c`) |
| `myTaskRxTxI2c1Handle` | `StartTaskRxTxI2c1` | AboveNormal | 512 | I²C1 slave master loop (`app_i2c_slave.c`) |
| `myTaskOLEDHandle` | `StartTaskOLED` | Normal | 256 | SSD1306 OLED update loop (`service_oled_task.c`) |
| `myTaskWiegandHandle` | `StartTaskWiegand` | Idle | 256 | Wiegand decoder loop (`wiegand.c`) |
| `myTaskHmiHandle` | `StartTaskHmi` | Idle | 1024 | HMI event loop (`hmi.c`) |
| `myTaskHmiMsgHandle` | `StartTaskHmiMsg` | Idle | 512 | USB CDC console loop (`hmi_console.c`) |
| `myTask_tca6408aHandle` | `StartTasktca6408a` | AboveNormal | 256 | TCA6408A I/O expander loop (`service_tca6408.c`) |
| `myTaskI2cGuardHandle` | `StartTaskI2cGuard` | High | 256 | I²C1 watchdog/guard task |
| `myTaskI2c2GuardHandle` | `StartTaskI2c2Guard` | Normal | 256 | I²C2 watchdog/guard task |

FreeRTOS hooks: `freertos.c` — `vApplicationStackOverflowHook`.

---

## I²C Bus Map

### I²C1 — Slave (host interface)

The STM32 acts as an **I²C slave** towards an external host controller. Protocol: register-based outbox/inbox with IRQ signalling.

- Contract: [`docs/i2c_global_contract.md`](docs/i2c_global_contract.md)
- Outbox flow: [`docs/i2c_slave_outbox_flow.md`](docs/i2c_slave_outbox_flow.md)
- Implementation: `app_i2c_slave.c`

### I²C2 — Master (on-board peripherals)

The STM32 is **I²C master** for all on-board chips. Bus access is protected by `i2c2_MutexHandle`.

| Device | I²C address | Contract / notes |
|---|---|---|
| PN532 NFC reader | TBD | [`docs/i2c_uid532_contract.md`](docs/i2c_uid532_contract.md) |
| TCA6408A I/O expander | `0x20` (default) | [`docs/tca6408_protocol.md`](docs/tca6408_protocol.md) |
| DS3231M RTC | `0x68` | `service_time_sync.c` |
| GM810 touch controller | via UART (not I²C) | [`docs/i2c_gm810_contract.md`](docs/i2c_gm810_contract.md) |

See also: [`docs/i2c_global_contract.md`](docs/i2c_global_contract.md) for full bus-level contract.

---

## USB

USB OTG FS configured as **USB Device, CDC class** (virtual COM port).

- Stack: `USB_DEVICE/` (generated by CubeMX; do not modify outside USER CODE blocks).
- Used by `hmi_console.c` for debug/HMI output and command reception.

---

## Pinout

See [`docs/chip-pinout-preliminary.md`](docs/chip-pinout-preliminary.md) for the preliminary pin assignment table and board photos in `docs/sip_board_*.png`.

---

## Where to Look Next

- [`docs/README.md`](docs/README.md) — full documentation index with status tags
- [`AGENTS.md`](AGENTS.md) — build instructions, coding conventions, workflow
- [`GLOSSARY.md`](GLOSSARY.md) — glossary of terms and component names
