# STM32F411CEU + FreeRTOS HardFault / watchdog risk audit

Repository: `OlegLebedevRU/sip_periph`  
Branch reference for links: `main`

## Summary

Main conclusions:

1. The project already contains several recovery-oriented changes, especially around I2C1/I2C2.
2. The most important remaining HardFault-class risks are:
   - fault handlers that do not persist crash context;
   - missing SCB configurable-fault enable/trap setup;
   - `configASSERT()` turning failures into a permanent dead stop;
   - ISR misuse of CMSIS-RTOS timer API in `service_matrix_kbd.c`;
   - a few direct memory-corruption candidates (`strcpy`, unchecked buffer growth).
3. IWDG is not enabled yet; for a guaranteed 1-2 s restart, IWDG must be the primary watchdog.
4. The safest implementation path is: **(a)** capture the current crash as-is with ST-LINK, **(b)** add fault dump + reset, **(c)** add IWDG heartbeat supervision.

---

## Found HardFault risks

| Risk | File and lines | Why it can end in fault / unrecoverable stop | Minimal safe fix |
|---|---|---|---|
| HardFault context is lost | [`Core/Src/stm32f4xx_it.c#L92-L101`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Src/stm32f4xx_it.c#L92-L101) | `NVIC_SystemReset()` is called, but register context is not preserved; the root cause is lost | Save stacked registers + SCB fault registers into `.noinit`, then reset |
| MemManage / BusFault / UsageFault are infinite loops | [`Core/Src/stm32f4xx_it.c#L107-L147`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Src/stm32f4xx_it.c#L107-L147) | Any of these exceptions will dead-stop the device forever | Use the same dump-and-reset path as HardFault |
| SCB configurable faults and traps are not enabled | [`Core/Src/system_stm32f4xx.c#L167-L182`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Src/system_stm32f4xx.c#L167-L182) | Divide-by-zero, unaligned access, bus and memory faults are not escalated with maximum diagnostic value | Early init: enable `DIV_0_TRP`, `UNALIGN_TRP`, `MEMFAULTENA`, `BUSFAULTENA`, `USGFAULTENA` |
| `configASSERT()` hard-hangs the system | [`Core/Inc/FreeRTOSConfig.h#L135-L135`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Inc/FreeRTOSConfig.h#L135-L135) | A failed RTOS assert disables interrupts and loops forever; on target this is an opaque permanent hang | Replace with breakpoint in debug and `NVIC_SystemReset()` in production |
| Stack overflow checking is weaker than recommended | [`Core/Inc/FreeRTOSConfig.h#L73-L73`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Inc/FreeRTOSConfig.h#L73-L73) | `configCHECK_FOR_STACK_OVERFLOW = 1` is less effective than mode 2 for detecting stack damage | Set it to `2` |
| `malloc failed hook` is not explicitly enabled | [`Core/Inc/FreeRTOSConfig.h`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Inc/FreeRTOSConfig.h), FreeRTOS default [`FreeRTOS.h#L737-L738`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Middlewares/Third_Party/FreeRTOS/Source/include/FreeRTOS.h#L737-L738) | Dynamic allocation failures may not go through the intended recovery hook | Add `#define configUSE_MALLOC_FAILED_HOOK 1` |
| ISR starts RTOS timers via non-ISR CMSIS API | [`Core/Src/service_matrix_kbd.c#L91-L106`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Src/service_matrix_kbd.c#L91-L106) | `osTimerStart()` from ISR is not the safe ISR API path; depending on wrapper behavior this can assert or corrupt state | Use `xTimerStartFromISR()` or defer the request to a task |
| Keyboard buffer can overflow | [`Core/Src/service_matrix_kbd.c#L87-L89`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Src/service_matrix_kbd.c#L87-L89), buffer size [`Core/Inc/main.h#L64-L67`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Inc/main.h#L64-L67) | `s_keyb.offset` is incremented without a hard bound before writing the next byte and terminator | Guard `offset < sizeof(buf) - 1` before every write |
| Unbounded `strcpy()` on OLED task stack | [`Core/Src/service_oled_task.c#L43-L55`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Src/service_oled_task.c#L43-L55) | If the source is not safely terminated, stack corruption is possible | Replace with bounded copy and forced NUL |
| Wiegand bit collection can write past the end of `readerdata.rdata` | [`Core/Src/wiegand.c#L181-L191`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Src/wiegand.c#L181-L191), array size [`Core/Inc/main.h#L92-L100`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Inc/main.h#L92-L100) | No bound check on `bytenum`; excessive bits/noise can corrupt adjacent memory | Abort/reset state if `bytenum >= sizeof(readerdata.rdata)` |
| Early enable/disable window for EXTI15_10 | [`Core/Src/main.c#L852-L863`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Src/main.c#L852-L863) | The IRQ is enabled before it is deliberately disabled; a narrow early interrupt window still exists | Do not enable this IRQ in `MX_GPIO_Init()` until queues are ready |
| `_exit()` hangs forever | [`Core/Src/syscalls.c#L61-L65`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Src/syscalls.c#L61-L65) | Any accidental path into `_exit()` becomes a permanent dead stop | Panic log + reset instead of `while(1)` |

---

## Checks that were explicitly reviewed

### 1. Exception handlers

- `HardFault_Handler` currently resets immediately, but without preserving diagnostic context: [`stm32f4xx_it.c#L92-L101`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Src/stm32f4xx_it.c#L92-L101)
- `MemManage_Handler`, `BusFault_Handler`, `UsageFault_Handler` are still empty infinite loops: [`stm32f4xx_it.c#L107-L147`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Src/stm32f4xx_it.c#L107-L147)
- No `SCB->CCR` trap setup was found.
- No `SCB->SHCSR` enabling of configurable faults was found.

### 2. FreeRTOS configuration

Reviewed file: [`Core/Inc/FreeRTOSConfig.h`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Inc/FreeRTOSConfig.h)

Observed:

- `configCHECK_FOR_STACK_OVERFLOW = 1`: [`#L73`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Inc/FreeRTOSConfig.h#L73)
- `configTOTAL_HEAP_SIZE = 24576`: [`#L67`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Inc/FreeRTOSConfig.h#L67)
- `configUSE_TIMERS = 1`, timer stack depth `1024`: [`#L86-L89`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Inc/FreeRTOSConfig.h#L86-L89)
- `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5`: [`#L123`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Inc/FreeRTOSConfig.h#L123)
- `configASSERT()` loops forever: [`#L135`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Inc/FreeRTOSConfig.h#L135)

Hooks present in `freertos.c`:

- `vApplicationStackOverflowHook()` -> `Error_Handler()`: [`Core/Src/freertos.c#L66-L74`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Src/freertos.c#L66-L74)
- `vApplicationMallocFailedHook()` -> `Error_Handler()`: [`Core/Src/freertos.c#L76-L79`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Src/freertos.c#L76-L79)

### 3. IRQ priority vs FreeRTOS FromISR usage

Configured IRQ priorities that interact with RTOS are at priority 5:

- I2C1/I2C2/UART/TIM11: [`Core/Src/stm32f4xx_hal_msp.c#L112-L151`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Src/stm32f4xx_hal_msp.c#L112-L151), [`#L298-L299`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Src/stm32f4xx_hal_msp.c#L298-L299), [`#L387-L412`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Src/stm32f4xx_hal_msp.c#L387-L412)
- EXTI0/1/15_10: [`Core/Src/main.c#L846-L853`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Src/main.c#L846-L853)
- USB OTG FS: [`USB_DEVICE/Target/usbd_conf.c#L93-L95`](https://github.com/OlegLebedevRU/sip_periph/blob/main/USB_DEVICE/Target/usbd_conf.c#L93-L95)

This matches `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5`, so no direct priority inversion bug was found in the configured NVIC priorities.

### 4. I2C1 slave path

Reviewed file: [`Core/Src/app_i2c_slave.c`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Src/app_i2c_slave.c)

Positive findings:

- `HAL_I2C_EnableListen_IT()` is re-armed in multiple completion/error paths: [`#L239-L253`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Src/app_i2c_slave.c#L239-L253), [`#L655-L679`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Src/app_i2c_slave.c#L655-L679)
- malformed transactions are checked and recovered: [`#L725-L756`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Src/app_i2c_slave.c#L725-L756)
- bus recovery is deferred to task context, which is correct for HAL deinit/init: [`#L344-L353`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Src/app_i2c_slave.c#L344-L353)

Remaining concern:

- `StartTaskRxTxI2c1()` can wait on `s_outbox_busy` indefinitely unless higher-level recovery clears it: [`#L643-L646`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Src/app_i2c_slave.c#L643-L646)

### 5. USB CDC

Reviewed files:

- [`USB_DEVICE/App/usbd_cdc_if.c`](https://github.com/OlegLebedevRU/sip_periph/blob/main/USB_DEVICE/App/usbd_cdc_if.c)
- [`USB_DEVICE/Target/usbd_conf.c`](https://github.com/OlegLebedevRU/sip_periph/blob/main/USB_DEVICE/Target/usbd_conf.c)
- [`Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Src/usbd_cdc.c`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Src/usbd_cdc.c)

Observed:

- CDC RX/TX buffers are static 2048-byte arrays: [`usbd_cdc_if.h#L52-L53`](https://github.com/OlegLebedevRU/sip_periph/blob/main/USB_DEVICE/App/usbd_cdc_if.h#L52-L53)
- The application currently does not actively process CDC RX payloads in a custom way.
- No custom overflow-prone ring buffer was found here.

USB is therefore not one of the primary current HardFault candidates.

### 6. Linker / memory layout

- Flash linker script reserves `_Min_Heap_Size = 0x800`, `_Min_Stack_Size = 0x1000`: [`STM32F411CEUX_FLASH.ld#L39-L42`](https://github.com/OlegLebedevRU/sip_periph/blob/main/STM32F411CEUX_FLASH.ld#L39-L42)
- RAM size is 128 KB: [`STM32F411CEUX_FLASH.ld#L45-L49`](https://github.com/OlegLebedevRU/sip_periph/blob/main/STM32F411CEUX_FLASH.ld#L45-L49)
- FreeRTOS heap implementation is `heap_4.c`.

Approximate task stacks configured in `main.c` are moderate, but there are several formatting-heavy paths (`snprintf`, diagnostics, font rendering, HMI formatting), so enabling stronger overflow checking remains justified.

---

## Watchdog architecture recommendation

## Current state

- IWDG is not enabled in CubeMX or HAL config: [`Core/Inc/stm32f4xx_hal_conf.h#L58-L72`](https://github.com/OlegLebedevRU/sip_periph/blob/main/Core/Inc/stm32f4xx_hal_conf.h#L58-L72)
- No IWDG or WWDG initialization was found in the source tree.
- No `HAL_IWDG_Refresh()` usage was found.

## Recommended choice

Use **IWDG as the primary watchdog**.

Reason:

- IWDG runs from **LSI**, independent from the main clock tree and scheduler.
- WWDG depends on APB clocking and is not sufficient as the only protection against a fully wedged kernel or clock-tree-related stall.

## Recommended timeout

Target nominal timeout: about **1.5 s**.

Recommended settings:

- Prescaler = 32
- Reload = 1499

Formula:

`T = (Reload + 1) * Prescaler / fLSI`

With typical `fLSI = 32000 Hz`:

- `T = 1500 * 32 / 32000 = 1.5 s`

With ±20% LSI spread:

- fastest case (`38.4 kHz`) -> `~1.25 s`
- slowest case (`25.6 kHz`) -> `~1.875 s`

This remains within the requested 1-2 s class.

## Feed policy

Recommended policy:

1. Every critical task sets its heartbeat bit every 100-200 ms.
2. A dedicated high-priority watchdog supervisor task checks that **all expected bits** were seen inside a window of about 800 ms.
3. Only that supervisor refreshes IWDG.
4. A fast timer ISR probe tracks whether the watchdog task itself is still advancing.
5. Fault hooks and fatal traps do **not** feed the watchdog.

Critical tasks to register:

- DWIN/HMI RX task
- HMI message task
- I2C1 slave task
- I2C1 guard task
- PN532 task
- TCA6408 task
- I2C2 guard task
- OLED task
- optionally Wiegand task if considered safety-critical for system liveness

---

## Recommended minimal fixes

1. Add diagnostic fault capture + immediate reset for all fatal fault handlers.
2. Enable SCB configurable faults and divide-by-zero / unaligned traps early.
3. Upgrade FreeRTOS config:
   - stack overflow check = 2;
   - malloc failed hook enabled;
   - assert path resets instead of looping forever.
4. Fix ISR misuse in matrix keyboard code.
5. Add bound checks to keyboard and Wiegand buffers.
6. Replace unsafe `strcpy()` in OLED task.
7. Add IWDG-based heartbeat supervisor.
8. Add reset-reason capture from `RCC->CSR` at the very beginning of `main()`.

---

## Proposed code bundle saved separately

The proposed implementation is intentionally **not applied** to the existing source tree.
All suggested integration code is saved only under:

- `/home/runner/work/sip_periph/sip_periph/docs/risk-audit/proposed/`

That bundle includes:

- watchdog API and skeleton implementation;
- fault dump example;
- FreeRTOS config snippet;
- linker snippet for `.noinit`.

