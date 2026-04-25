# docs/README.md — Documentation Index

This file is the central index for all project documentation. For the architecture overview start with [`../ARCHITECTURE.md`](../ARCHITECTURE.md).

---

## Contracts (active)

Current, normative device-level communication contracts.

| File | Description | Status |
|---|---|---|
| [`i2c_global_contract.md`](i2c_global_contract.md) | Master I²C bus contract — addresses, register layout, timing | **active** |
| [`i2c_gm810_contract.md`](i2c_gm810_contract.md) | GM810 touch controller protocol contract | **active** |
| [`i2c_uid532_contract.md`](i2c_uid532_contract.md) | PN532 / UID532 NFC reader I²C contract | **active** |
| [`tca6408_protocol.md`](tca6408_protocol.md) | TCA6408A I/O expander protocol and pin map | **active** |
| [`i2c_slave_outbox_flow.md`](i2c_slave_outbox_flow.md) | I²C1 slave outbox data-delivery flow | **active** |

---

## Plans

| File | Description | Status |
|---|---|---|
| [`gm810_integration_plan_2026-04-23.md`](gm810_integration_plan_2026-04-23.md) | GM810 touch integration plan (2026-04-23) | **plan** |
| [`dwin_tx_stage3_plan_2026-03-26.md`](dwin_tx_stage3_plan_2026-03-26.md) | DWIN TX stage 3 implementation plan (2026-03-26) | **plan** |

---

## Reference / Hardware

| File | Description | Status |
|---|---|---|
| [`chip-pinout-preliminary.md`](chip-pinout-preliminary.md) | STM32F411CEU preliminary pin assignment | **reference** |
| [`sip_board_main_left.png`](sip_board_main_left.png) | Board photo — main left view | **reference** |
| [`sip_board_pn532.png`](sip_board_pn532.png) | Board photo — PN532 area | **reference** |
| [`sip_board_audio.png`](sip_board_audio.png) | Board photo — audio area | **reference** |
| [`sip_board_ethernet.png`](sip_board_ethernet.png) | Board photo — ethernet area | **reference** |
| [`DisplayConfig.xls`](DisplayConfig.xls) | DWIN display configuration spreadsheet | **reference** |
| [`TouchConfig.xls`](TouchConfig.xls) | GM810 touch configuration spreadsheet | **reference** |

---

## Reports / Analyses

| File | Description | Status |
|---|---|---|
| [`i2c_main_buffer_summary_2026-03-26.md`](i2c_main_buffer_summary_2026-03-26.md) | I²C main buffer analysis (2026-03-26) | **archived** |
| [`i2c_master_recommendations_2026-03-24.md`](i2c_master_recommendations_2026-03-24.md) | I²C master recommendations (2026-03-24) | **archived** |
| [`i2c_slave_diag_err_overvie.md`](i2c_slave_diag_err_overvie.md) | I²C slave diagnostic error overview | **archived** |
| [`i2c_time_reliability_report_2026-03-24.md`](i2c_time_reliability_report_2026-03-24.md) | I²C time reliability report (2026-03-24) | **archived** |
| [`stm32_i2c_bus_checkup.md`](stm32_i2c_bus_checkup.md) | STM32 I²C bus health check procedure | **archived** |
| [`tca6408_integration.md`](tca6408_integration.md) | TCA6408A integration notes | **archived** |

---

## Code References / Examples

Reference C code used to understand the host-side (leo4) bus protocol.

| File | Description | Status |
|---|---|---|
| [`leo4_stm32_bus.c`](leo4_stm32_bus.c) | leo4 host-side STM32 bus implementation | **reference** |
| [`l4_i2c_master.c`](l4_i2c_master.c) | leo4 I²C master reference code | **reference** |
| [`l4_input_processing.c`](l4_input_processing.c) | leo4 input processing reference code | **reference** |

---

## Archived Reports (moved from repository root)

Historical reports relocated from the repository root. Kept for audit trail.

| File | Description | Status |
|---|---|---|
| [`reports/BUGFIX_TIME_PACKET.md`](reports/BUGFIX_TIME_PACKET.md) | Bug-fix notes for time packet handling | **archived** |
| [`reports/FIX_SUMMARY.md`](reports/FIX_SUMMARY.md) | Fix summary report | **archived** |
| [`reports/TESTING_TIME_PACKET.md`](reports/TESTING_TIME_PACKET.md) | Testing notes for time packet | **archived** |
| [`reports/CHECKLIST.md`](reports/CHECKLIST.md) | Development checklist | **archived** |
| [`reports/REFACTORING_PLAN.md`](reports/REFACTORING_PLAN.md) | Refactoring plan | **archived** |
| [`reports/GM810_DIAGNOSTICS_2026-04-24.md`](reports/GM810_DIAGNOSTICS_2026-04-24.md) | GM810 diagnostics (2026-04-24) | **archived** |
| [`reports/GM810_DIAGNOSTIC_PLAN_2026-04-24.md`](reports/GM810_DIAGNOSTIC_PLAN_2026-04-24.md) | GM810 diagnostic plan (2026-04-24) | **archived** |
| [`reports/GM810_DOCUMENTATION_INDEX_2026-04-24.md`](reports/GM810_DOCUMENTATION_INDEX_2026-04-24.md) | GM810 documentation index (2026-04-24) | **archived** |
| [`reports/GM810_EXECUTIVE_SUMMARY_2026-04-24.md`](reports/GM810_EXECUTIVE_SUMMARY_2026-04-24.md) | GM810 executive summary (2026-04-24) | **archived** |
| [`reports/GM810_FINAL_REPORT_2026-04-24.md`](reports/GM810_FINAL_REPORT_2026-04-24.md) | GM810 final report (2026-04-24) | **archived** |
| [`reports/GM810_FIX_SUMMARY_2026-04-24.md`](reports/GM810_FIX_SUMMARY_2026-04-24.md) | GM810 fix summary (2026-04-24) | **archived** |
| [`reports/GM810_IMPLEMENTATION_STATUS_2026-04-24.md`](reports/GM810_IMPLEMENTATION_STATUS_2026-04-24.md) | GM810 implementation status (2026-04-24) | **archived** |
| [`reports/GM810_QUICK_FIX_2026-04-24.md`](reports/GM810_QUICK_FIX_2026-04-24.md) | GM810 quick fix notes (2026-04-24) | **archived** |
| [`reports/GM810_QUICK_REFERENCE_2026-04-24.md`](reports/GM810_QUICK_REFERENCE_2026-04-24.md) | GM810 quick reference card (2026-04-24) | **archived** |
