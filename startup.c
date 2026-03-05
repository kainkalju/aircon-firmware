/**
 * startup.c — STM32F2xx vector table and fault handlers
 *
 * Flash layout (from vector table analysis):
 *   0x08000000  Vector table (256 bytes)
 *   0x08000100  Application code
 *   0x08036000  Reset_Handler  (0x080369C8)
 *   0x08038000  Fault handlers (0x080380CC)
 *
 * Active IRQs (non-default handlers):
 *   TIM3_IRQHandler        @ 0x08024BB8  — serial framing timer
 *   USART1_IRQHandler      @ 0x08020A9E  — UART receive/transmit
 *   EXTI15_10_IRQHandler   @ 0x080240DC  — GPIO edge (WLAN IRQ pin)
 *   SysTick_Handler        @ 0x080380A4  — 1 ms tick counter
 *   HardFault_Handler      @ 0x080380CC  — crash dump
 */

#include "daikin.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Linker symbols (provided by linker script)                        */
/* ------------------------------------------------------------------ */
extern uint32_t _sidata;   /* start of .data in flash  */
extern uint32_t _sdata;    /* start of .data in SRAM   */
extern uint32_t _edata;    /* end   of .data in SRAM   */
extern uint32_t _sbss;     /* start of .bss  in SRAM   */
extern uint32_t _ebss;     /* end   of .bss  in SRAM   */
extern uint32_t _estack;   /* top of stack  (0x20020000) */

/* ------------------------------------------------------------------ */
/*  Weak default handler — spins forever, triggers watchdog reset     */
/* ------------------------------------------------------------------ */
__weak void Default_Handler(void) {
    /* All unused IRQ vectors point here (0x0802023C).
     * Logging the fault is not possible at this point since we do
     * not know which IRQ fired; just loop and let the WWDG expire. */
    for (;;) {}
}

/* ------------------------------------------------------------------ */
/*  Vector table (Cortex-M3 format, placed at 0x08000000)            */
/* ------------------------------------------------------------------ */
__attribute__((section(".isr_vector")))
const void * const g_pfnVectors[] = {
    /* Stack pointer initial value */
    (void *)0x20020000,          /* [0]  Initial SP           */

    /* Core exception handlers */
    Reset_Handler,               /* [1]  Reset                */
    Default_Handler,             /* [2]  NMI                  */
    HardFault_Handler,           /* [3]  HardFault            */
    HardFault_Handler,           /* [4]  MemManage            */
    HardFault_Handler,           /* [5]  BusFault             */
    HardFault_Handler,           /* [6]  UsageFault           */
    0, 0, 0, 0,                  /* [7-10] Reserved           */
    Default_Handler,             /* [11] SVCall               */
    Default_Handler,             /* [12] DebugMon             */
    0,                           /* [13] Reserved             */
    Default_Handler,             /* [14] PendSV               */
    SysTick_Handler,             /* [15] SysTick              */

    /* External IRQ handlers */
    Default_Handler,             /* [16] WWDG                 */
    Default_Handler,             /* [17] PVD                  */
    Default_Handler,             /* [18] TAMP_STAMP           */
    Default_Handler,             /* [19] RTC_WKUP             */
    Default_Handler,             /* [20] FLASH                */
    Default_Handler,             /* [21] RCC                  */
    Default_Handler,             /* [22] EXTI0                */
    Default_Handler,             /* [23] EXTI1                */
    Default_Handler,             /* [24] EXTI2                */
    Default_Handler,             /* [25] EXTI3                */
    Default_Handler,             /* [26] EXTI4                */
    Default_Handler,             /* [27] DMA1_Stream0         */
    Default_Handler,             /* [28] DMA1_Stream1         */
    Default_Handler,             /* [29] DMA1_Stream2         */
    Default_Handler,             /* [30] DMA1_Stream3         */
    Default_Handler,             /* [31] DMA1_Stream4         */
    Default_Handler,             /* [32] DMA1_Stream5         */
    Default_Handler,             /* [33] DMA1_Stream6         */
    Default_Handler,             /* [34] ADC                  */
    Default_Handler,             /* [35] CAN1_TX              */
    Default_Handler,             /* [36] CAN1_RX0             */
    Default_Handler,             /* [37] CAN1_RX1             */
    Default_Handler,             /* [38] CAN1_SCE             */
    Default_Handler,             /* [39] EXTI9_5              */
    Default_Handler,             /* [40] TIM1_BRK_TIM9        */
    Default_Handler,             /* [41] TIM1_UP_TIM10        */
    Default_Handler,             /* [42] TIM1_TRG_COM_TIM11   */
    Default_Handler,             /* [43] TIM1_CC              */
    Default_Handler,             /* [44] TIM2                 */
    TIM3_IRQHandler,             /* [45] TIM3 — serial timer  */
    Default_Handler,             /* [46] TIM4                 */
    Default_Handler,             /* [47] I2C1_EV              */
    Default_Handler,             /* [48] I2C1_ER              */
    Default_Handler,             /* [49] I2C2_EV              */
    Default_Handler,             /* [50] I2C2_ER              */
    Default_Handler,             /* [51] SPI1                 */
    Default_Handler,             /* [52] SPI2                 */
    USART1_IRQHandler,           /* [53] USART1 — serial comm */
    Default_Handler,             /* [54] USART2               */
    Default_Handler,             /* [55] USART3               */
    EXTI15_10_IRQHandler,        /* [56] EXTI15_10 — WLAN IRQ */
    Default_Handler,             /* [57] RTC_Alarm            */
    Default_Handler,             /* [58] OTG_FS_WKUP          */
    Default_Handler,             /* [59] TIM8_BRK_TIM12       */
    Default_Handler,             /* [60] TIM8_UP_TIM13        */
    Default_Handler,             /* [61] TIM8_TRG_COM_TIM14   */
};

/* ------------------------------------------------------------------ */
/*  SysTick — 1 ms system tick (reconstructed from 0x080380A4)       */
/* ------------------------------------------------------------------ */
uint32_t g_ms_ticks = 0;

void SysTick_Handler(void) {
    g_ms_ticks++;
}

/* ------------------------------------------------------------------ */
/*  HardFault — crash dump via UART then reboot (0x080380CC)         */
/*                                                                    */
/*  The disassembly shows it performs integer division (udiv) on the  */
/*  fault address to convert to decimal for the log string:           */
/*    "#### %s(): Unrecoverable error (ms_time=%d) ####"             */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t r0, r1, r2, r3;
    uint32_t r12, lr, pc, xpsr;
} fault_frame_t;

static void fault_dump(const fault_frame_t *f, const char *type) {
    /* Print via UART — low-level because scheduler may be dead */
    log_print(LOG_E, "#### %s(): Unrecoverable error (ms_time=%u) ####",
              type, g_ms_ticks);
    log_print(LOG_E, "PC=0x%08X LR=0x%08X SP=0x%08X XPSR=0x%08X",
              f->pc, f->lr, (uint32_t)f, f->xpsr);
    log_print(LOG_E, "R0=0x%08X R1=0x%08X R2=0x%08X R3=0x%08X",
              f->r0, f->r1, f->r2, f->r3);
}

__attribute__((naked)) void HardFault_Handler(void) {
    __asm volatile(
        "tst   lr, #4          \n"
        "ite   eq              \n"
        "mrseq r0, msp         \n"
        "mrsne r0, psp         \n"
        "b     hard_fault_impl \n"
    );
}

void hard_fault_impl(fault_frame_t *frame) {
    fault_dump(frame, "HardFault");
    /* Trigger system reset via AIRCR */
    *((volatile uint32_t *)0xE000ED0C) = 0x05FA0004;
    for (;;) {}
}

void BusFault_Handler(void)   { log_print(LOG_E, "BusFault");   for (;;) {} }
void UsageFault_Handler(void) { log_print(LOG_E, "UsageFault"); for (;;) {} }

/* ------------------------------------------------------------------ */
/*  Reset_Handler — C runtime init then jump to AppMain (0x080369C8) */
/* ------------------------------------------------------------------ */
void Reset_Handler(void) {
    uint32_t *src, *dst;

    /* Copy .data section from flash to SRAM */
    src = &_sidata;
    dst = &_sdata;
    while (dst < &_edata) {
        *dst++ = *src++;
    }

    /* Zero-fill .bss section */
    dst = &_sbss;
    while (dst < &_ebss) {
        *dst++ = 0;
    }

    /* Configure SysTick for 1 ms at 120 MHz (STM32F2xx max clock) */
    *((volatile uint32_t *)0xE000E014) = 120000 - 1; /* LOAD */
    *((volatile uint32_t *)0xE000E018) = 0;           /* VAL  */
    *((volatile uint32_t *)0xE000E010) = 0x07;        /* CTRL: enable, tickint, sysclk */

    /* Enable FPU (Cortex-M4 variant only) */
    *((volatile uint32_t *)0xE000ED88) |= (0xF << 20);

    /* Log power-on */
    log_print(LOG_I, "ptn9:power on");

    /* Jump to application */
    AppMain();

    /* Should never reach here */
    for (;;) {}
}

/* ------------------------------------------------------------------ */
/*  panic_main — structured crash handler (0x08074E38)               */
/* ------------------------------------------------------------------ */
void panic_main(int type) {
    log_print(LOG_E, "panic_main(): unknown type=%d", type);
    log_print(LOG_E, "#### %s(): Unrecoverable error ####", "panic_main");

    /* Spin — let watchdog reset the system */
    for (;;) {}
}
