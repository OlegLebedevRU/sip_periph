# Risk audit bundle

This folder contains the saved results of the STM32F411CEU + FreeRTOS HardFault/watchdog audit.

## Files

- `hardfault-risk-audit.md` — audit report with concrete risks, line references, and minimal fixes.
- `cubeide-2.0-hardfault-dump-first-step.md` — first-step diagnostic procedure for capturing the current HardFault cause with ST-LINK in STM32CubeIDE 2.0.
- `proposed/Core/Inc/app_watchdog.h` — proposed watchdog API.
- `proposed/Core/Src/app_watchdog.c` — proposed watchdog implementation skeleton.
- `proposed/Core/Src/fault_capture_example.c` — proposed HardFault/MemManage/BusFault/UsageFault capture example.
- `proposed/patches/freertosconfig-snippet.md` — proposed `FreeRTOSConfig.h` changes.
- `proposed/patches/linker-noinit-snippet.ld` — proposed linker snippet for `.noinit` fault dump storage.
