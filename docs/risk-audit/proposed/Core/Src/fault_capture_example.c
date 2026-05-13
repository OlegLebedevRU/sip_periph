#include "stm32f4xx.h"

typedef struct {
    uint32_t magic;
    uint32_t exc_return;
    uint32_t r0;
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
    uint32_t r12;
    uint32_t lr;
    uint32_t pc;
    uint32_t psr;
    uint32_t cfsr;
    uint32_t hfsr;
    uint32_t mmfar;
    uint32_t bfar;
    uint32_t afsr;
    uint32_t shcsr;
} fault_dump_t;

__attribute__((section(".noinit"))) volatile fault_dump_t g_fault_dump;

#define STM32F411_SRAM_END  (SRAM_BASE + (128UL * 1024UL))

static uint8_t Fault_IsValidStackPointer(const uint32_t *stack_ptr)
{
    uint32_t addr = (uint32_t)stack_ptr;
    uint32_t end_addr = addr + (8U * sizeof(uint32_t));

    if (addr < SRAM_BASE) {
        return 0U;
    }
    if (end_addr > STM32F411_SRAM_END) {
        return 0U;
    }
    if ((addr & 0x7U) != 0U) {
        return 0U;
    }

    return 1U;
}

static void Fault_SaveAndReset(uint32_t *stack_ptr, uint32_t exc_return)
{
    g_fault_dump.magic = 0xFA17FA17U;
    g_fault_dump.exc_return = exc_return;

    if (Fault_IsValidStackPointer(stack_ptr) != 0U) {
        g_fault_dump.r0  = stack_ptr[0];
        g_fault_dump.r1  = stack_ptr[1];
        g_fault_dump.r2  = stack_ptr[2];
        g_fault_dump.r3  = stack_ptr[3];
        g_fault_dump.r12 = stack_ptr[4];
        g_fault_dump.lr  = stack_ptr[5];
        g_fault_dump.pc  = stack_ptr[6];
        g_fault_dump.psr = stack_ptr[7];
    }

    g_fault_dump.cfsr  = SCB->CFSR;
    g_fault_dump.hfsr  = SCB->HFSR;
    g_fault_dump.mmfar = SCB->MMFAR;
    g_fault_dump.bfar  = SCB->BFAR;
    g_fault_dump.afsr  = SCB->AFSR;
    g_fault_dump.shcsr = SCB->SHCSR;

    NVIC_SystemReset();
    while (1) {
    }
}

__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile(
        "tst lr, #4            \n"
        "ite eq                \n"
        "mrseq r0, msp         \n"
        "mrsne r0, psp         \n"
        "mov r1, lr            \n"
        "b Fault_SaveAndReset  \n"
    );
}

__attribute__((naked)) void MemManage_Handler(void)
{
    __asm volatile(
        "tst lr, #4            \n"
        "ite eq                \n"
        "mrseq r0, msp         \n"
        "mrsne r0, psp         \n"
        "mov r1, lr            \n"
        "b Fault_SaveAndReset  \n"
    );
}

__attribute__((naked)) void BusFault_Handler(void)
{
    __asm volatile(
        "tst lr, #4            \n"
        "ite eq                \n"
        "mrseq r0, msp         \n"
        "mrsne r0, psp         \n"
        "mov r1, lr            \n"
        "b Fault_SaveAndReset  \n"
    );
}

__attribute__((naked)) void UsageFault_Handler(void)
{
    __asm volatile(
        "tst lr, #4            \n"
        "ite eq                \n"
        "mrseq r0, msp         \n"
        "mrsne r0, psp         \n"
        "mov r1, lr            \n"
        "b Fault_SaveAndReset  \n"
    );
}
