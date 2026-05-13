# Proposed `FreeRTOSConfig.h` snippet

```c
#define configUSE_IDLE_HOOK                    1
#define configCHECK_FOR_STACK_OVERFLOW         2
#define configUSE_MALLOC_FAILED_HOOK           1

#define configASSERT(x)                                                    \
    do {                                                                   \
        if ((x) == 0) {                                                    \
            __disable_irq();                                               \
            __BKPT(0);                                                     \
            NVIC_SystemReset();                                            \
            for (;;) { }                                                   \
        }                                                                  \
    } while (0)
```

Recommended hook behavior:

- `vApplicationStackOverflowHook()` -> short diagnostic log + `NVIC_SystemReset()`
- `vApplicationMallocFailedHook()` -> short diagnostic log + `NVIC_SystemReset()`
- `vApplicationIdleHook()` -> never feed IWDG directly
