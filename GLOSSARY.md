# GLOSSARY.md — Key Terms and Components

One-line definitions for terms and components used across the codebase and documentation.

| Term | Definition |
|---|---|
| **SIP periph** | The peripheral board this firmware runs on (STM32F411CEU-based). |
| **GM810** | Touch controller IC; communicated with via UART. Driver: `service_gm810_uart.c`. Contract: [`docs/i2c_gm810_contract.md`](docs/i2c_gm810_contract.md). |
| **PN532** | NFC reader IC (NXP). Communicates over I²C2 in this project. Low-level: `pn532_com.c`. Task: `service_pn532_task.c`. |
| **UID532** | NFC UID-reading service built on top of PN532. Contract: [`docs/i2c_uid532_contract.md`](docs/i2c_uid532_contract.md). |
| **TCA6408A** | 8-bit I²C I/O expander (TI). I²C2 address `0x20`. Driver: `service_tca6408.c`. Protocol: [`docs/tca6408_protocol.md`](docs/tca6408_protocol.md). |
| **DS3231M** | I²C RTC (Maxim/Analog). I²C2 address `0x68`. Provides 1 Hz SQW on TCA P0. Driver: `service_time_sync.c`. |
| **DWIN** | Serial display controller. Communicates over UART. Drivers: `app_uart_dwin.c`, `app_uart_dwin_tx.c`, `dwin_gfx.c`. |
| **SSD1306** | OLED display controller (128×64). Driver: `ssd1306.c`. Task: `service_oled_task.c`. |
| **leo4** | External host module / bus protocol. Reference code: [`docs/leo4_stm32_bus.c`](docs/leo4_stm32_bus.c), [`docs/l4_i2c_master.c`](docs/l4_i2c_master.c). |
| **outbox** | Slave-side data delivery mechanism on I²C1: the STM32 queues data into an outbox register area which the host reads. See [`docs/i2c_slave_outbox_flow.md`](docs/i2c_slave_outbox_flow.md). |
| **inbox** | Register area written by the host to the STM32 slave over I²C1. |
| **time packet** | Time synchronisation packet sent by the host to set/sync the RTC and internal clock. See archived reports in [`docs/reports/`](docs/reports/). |
| **I²C1** | I²C bus 1 — STM32 acts as **slave** (host interface). Implementation: `app_i2c_slave.c`. |
| **I²C2** | I²C bus 2 — STM32 acts as **master** (on-board peripherals). Protected by `i2c2_MutexHandle`. |
| **CubeMX user code blocks** | Sections delimited by `/* USER CODE BEGIN <name> */` … `/* USER CODE END <name> */`. These are preserved during CubeMX re-generation; all other generated code is overwritten. |
| **CMSIS-RTOS v1** | RTOS abstraction layer used over FreeRTOS (`osThreadDef`, `osThreadCreate`, `osDelay`, etc.). |
| **outbox flow** | See **outbox**. |
| **i2c2_MutexHandle** | FreeRTOS mutex protecting shared access to the I²C2 bus between multiple tasks. |
| **Wiegand** | Access-control protocol decoder. Implementation: `wiegand.c`, task `myTaskWiegand`. |
| **HMI** | Human-Machine Interface layer. USB CDC console (`hmi_console.c`) + display logic (`hmi.c`). |
| **app_irq_router** | Routes GPIO EXTI interrupt events to the appropriate service module. |
| **i2c2 soft/hard recover** | Bus recovery routines called when I²C2 encounters consecutive errors. Triggered by `service_tca6408` or `myTaskI2c2Guard`. |
