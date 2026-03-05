/**
 * uart.c — USART1 driver and serial protocol to indoor AC unit
 *
 * The BRP069A42 talks to the indoor AC unit over a proprietary serial
 * bus (UART-based, likely S21 or P1/P2 protocol). The adapter acts as
 * master — it polls the indoor unit for sensor data and sends control
 * commands, then relays the results over WiFi.
 *
 * Hardware:
 *   USART1 @ APB2, PA9=TX, PA10=RX
 *   TIM3 — generates periodic 20 ms frames / timeouts
 *   EXTI15_10 — not UART but WLAN IRQ (PC11)
 *
 * Inferred from strings:
 *   "AppSerialReceive"     — RX handler
 *   "AppSerialSend"        — TX handler
 *   "VerificationSerialTask" — checksum/framing validation task
 *   "serialComm"           — high-level serial task
 *   "SERIAL IF FAILURE"    — no response from indoor unit
 *   "ptn9:power on"        — logged on UART after reset
 *   "proc_cmd53"           — SDIO CMD53 for WLAN data transfer
 */

#include "daikin.h"
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  USART1 register bits                                              */
/* ------------------------------------------------------------------ */
#define USART_CR1_TE    (1u << 3)
#define USART_CR1_RE    (1u << 2)

/* ------------------------------------------------------------------ */
/*  RX ring buffer                                                    */
/* ------------------------------------------------------------------ */
#define UART_RX_BUF_SIZE    256
#define UART_TX_BUF_SIZE    256

static uint8_t  rx_buf[UART_RX_BUF_SIZE];
static uint16_t rx_head = 0;
static uint16_t rx_tail = 0;

static uint8_t  tx_buf[UART_TX_BUF_SIZE];
static uint16_t tx_head = 0;
static uint16_t tx_tail = 0;

/* Serial protocol frame state */
typedef enum {
    FRAME_IDLE = 0,
    FRAME_HEADER,
    FRAME_DATA,
    FRAME_CHECKSUM,
} frame_state_t;

static frame_state_t  frame_state = FRAME_IDLE;
static uint8_t        frame_buf[64];
static int            frame_len = 0;
static int            frame_expected_len = 0;

/* ------------------------------------------------------------------ */
/*  uart_init — configure USART1 at given baud rate                  */
/* ------------------------------------------------------------------ */
void uart_init(uint32_t baud) {
    /* APB2 clock = 60 MHz (PCLK2 = SYSCLK/2) */
    uint32_t pclk2 = 60000000UL;
    uint32_t brr   = pclk2 / baud;

    USART1->BRR = brr;
    USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE |
                  USART_CR1_RXNEIE;   /* enable RX interrupt */

    /* Enable USART1 IRQ in NVIC */
    /* NVIC_EnableIRQ(USART1_IRQn) → IRQ #37, priority 5 */
    volatile uint32_t *NVIC_ISER = (volatile uint32_t *)0xE000E100;
    NVIC_ISER[1] = (1u << (37 - 32));
}

/* ------------------------------------------------------------------ */
/*  USART1_IRQHandler — runs at 0x08020A9E                           */
/*                                                                    */
/*  Disassembly shows it chains through a linked-list of RX nodes:   */
/*    if (head == NULL) → set head                                   */
/*    else walk tail links until ->next == NULL, append              */
/*  This is a simple software FIFO implemented as a singly-linked    */
/*  list of event_node_t entries.                                     */
/* ------------------------------------------------------------------ */
void USART1_IRQHandler(void) {
    uint32_t sr = USART1->SR;

    if (sr & USART_SR_RXNE) {
        uint8_t data = (uint8_t)USART1->DR;
        uint16_t next = (rx_head + 1) & (UART_RX_BUF_SIZE - 1);
        if (next != rx_tail) {
            rx_buf[rx_head] = data;
            rx_head = next;
        }
        /* Post to packet RX queue (abbreviated — real code uses
         * the event_node linked list shown in disassembly) */
        gQ_PKTRX.count++;
    }

    if (sr & USART_SR_TXE) {
        if (tx_head != tx_tail) {
            USART1->DR = tx_buf[tx_tail];
            tx_tail = (tx_tail + 1) & (UART_TX_BUF_SIZE - 1);
        } else {
            /* Disable TXE interrupt when buffer empty */
            USART1->CR1 &= ~(1u << 7); /* TXEIE */
        }
    }
}

/* ------------------------------------------------------------------ */
/*  TIM3_IRQHandler — 20 ms serial frame timer (0x08024BB8)          */
/*                                                                    */
/*  TIM3 fires every ~20 ms to drive the serial polling loop.        */
/*  Disassembly shows it reads a control structure, decrements        */
/*  a counter, and calls into the frame-send path when the count     */
/*  reaches zero (periodic polling).                                 */
/* ------------------------------------------------------------------ */
static uint32_t tim3_period_ms  = 20;
static uint32_t tim3_tick_count = 0;

void TIM3_IRQHandler(void) {
    /* Clear TIM3 update interrupt flag */
    volatile uint32_t *TIM3_SR = (volatile uint32_t *)(TIM3_BASE + 0x10);
    *TIM3_SR &= ~1u;

    tim3_tick_count++;

    /* Every 20 ticks (= 400 ms) send a polling request to indoor unit */
    if ((tim3_tick_count % 20) == 0) {
        /* Trigger serial send of polling command */
        /* AppSerialSend(poll_cmd, poll_cmd_len); */
    }
}

/* ------------------------------------------------------------------ */
/*  EXTI15_10_IRQHandler — WLAN module interrupt line (0x080240DC)   */
/*                                                                    */
/*  The WLAN chip asserts an interrupt when an RX packet is ready.   */
/*  The handler reads the packet from the Broadcom SDIO chip via     */
/*  CMD53, then posts to gQ_PKTRX for the WiFi task to process.     */
/* ------------------------------------------------------------------ */
void EXTI15_10_IRQHandler(void) {
    /* Clear EXTI pending bit for line 11 (PC11 = WLAN IRQ) */
    volatile uint32_t *EXTI_PR = (volatile uint32_t *)(EXTI_BASE + 0x14);
    *EXTI_PR = (1u << 11);

    /* Read packet from WLAN chip over SDIO */
    static uint8_t wlan_rx_buf[2048];
    int len = sdio_cmd53(0 /*read*/, 0x0000, wlan_rx_buf, sizeof(wlan_rx_buf));
    if (len < 0) {
        log_print(LOG_E, " [SDIO] proc_cmd53 ret=%d", len);
        return;
    }

    /* Forward raw packet to RX complete queue */
    gQ_PKTRX_COMPLETE.count++;
}

/* ------------------------------------------------------------------ */
/*  AppSerialReceive — process bytes received from indoor unit        */
/*  (referenced at 0x0800E4D4)                                        */
/* ------------------------------------------------------------------ */
void AppSerialReceive(uint8_t *buf, int len) {
    for (int i = 0; i < len; i++) {
        uint8_t b = buf[i];

        switch (frame_state) {
        case FRAME_IDLE:
            /* Start-of-frame byte (0x02 in S21-style protocol) */
            if (b == 0x02) {
                frame_len = 0;
                frame_state = FRAME_HEADER;
            }
            break;

        case FRAME_HEADER:
            frame_buf[frame_len++] = b;
            if (frame_len >= 3) {
                /* Byte 2 encodes payload length */
                frame_expected_len = frame_buf[2];
                frame_state = FRAME_DATA;
            }
            break;

        case FRAME_DATA:
            frame_buf[frame_len++] = b;
            if (frame_len >= 3 + frame_expected_len) {
                frame_state = FRAME_CHECKSUM;
            }
            break;

        case FRAME_CHECKSUM:
            /* Verify checksum — sum of all bytes must equal received byte */
            {
                uint8_t csum = 0;
                for (int j = 0; j < frame_len; j++) csum += frame_buf[j];
                csum = (uint8_t)(~csum + 1);
                if (csum == b) {
                    /* Valid frame — parse and update g_ac_state */
                    serialComm_task(NULL); /* simplified call */
                } else {
                    log_print(LOG_E, "err=%d", RET_NG);
                }
            }
            frame_len   = 0;
            frame_state = FRAME_IDLE;
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  AppSerialSend — transmit a command frame to the indoor unit       */
/*  (referenced at 0x0800E534)                                        */
/* ------------------------------------------------------------------ */
void AppSerialSend(const uint8_t *buf, int len) {
    for (int i = 0; i < len; i++) {
        uint16_t next = (tx_head + 1) & (UART_TX_BUF_SIZE - 1);
        while (next == tx_tail) { /* spin if full — should use semaphore */ }
        tx_buf[tx_head] = buf[i];
        tx_head = next;
    }
    /* Enable TXE interrupt to drain the buffer */
    USART1->CR1 |= (1u << 7); /* TXEIE */
}

/* ------------------------------------------------------------------ */
/*  VerificationSerialTask — validates serial frames (0x08002218)     */
/*                                                                    */
/*  Disassembly shows this function accesses GPIO registers (BSRR)   */
/*  with masks 0x100, 0x200, 0x400, 0x800, 0x1000 — these correspond */
/*  to GPIOx ODR bits 8-12, toggled based on the incoming frame      */
/*  content. This is likely status LED control or handshake lines.   */
/* ------------------------------------------------------------------ */
void VerificationSerialTask(void *arg) {
    (void)arg;
    log_print(LOG_I, "VerificationSerialTask");

    uint8_t buf[64];
    int     len;

    for (;;) {
        /* Wait for data in RX buffer */
        len = 0;
        while (rx_head != rx_tail) {
            buf[len++] = rx_buf[rx_tail];
            rx_tail = (rx_tail + 1) & (UART_RX_BUF_SIZE - 1);
            if (len >= (int)sizeof(buf)) break;
        }

        if (len > 0) {
            AppSerialReceive(buf, len);
        }

        delay_ms(5);
    }
}

/* ------------------------------------------------------------------ */
/*  serialComm_task — high-level serial protocol state machine        */
/*  (referenced at 0x08010B20)                                        */
/*                                                                    */
/*  Polls the indoor unit for:                                        */
/*   - Current sensor readings (htemp, otemp, hhum, cmpfreq)         */
/*   - Operating state (pow, mode, stemp, shum, f_rate, f_dir)       */
/*   - Error codes                                                    */
/* ------------------------------------------------------------------ */

/* S21-like command codes (inferred from protocol usage) */
#define CMD_GET_STATUS  0x62   /* 'b' — request operating status   */
#define CMD_GET_SENSOR  0x63   /* 'c' — request sensor readings    */
#define CMD_SET_CONTROL 0x64   /* 'd' — set control parameters     */

static const uint8_t POLL_STATUS_CMD[] = { 0x02, CMD_GET_STATUS, 0x00, 0xAC, 0x03 };
static const uint8_t POLL_SENSOR_CMD[] = { 0x02, CMD_GET_SENSOR, 0x00, 0xAB, 0x03 };

void serialComm_task(void *arg) {
    (void)arg;
    log_print(LOG_I, "serialComm");

    int poll_tick = 0;

    for (;;) {
        /* Every 400 ms: poll operating status */
        if ((poll_tick % 20) == 0) {
            AppSerialSend(POLL_STATUS_CMD, sizeof(POLL_STATUS_CMD));
        }
        /* Every 2 s: poll sensor readings */
        if ((poll_tick % 100) == 0) {
            AppSerialSend(POLL_SENSOR_CMD, sizeof(POLL_SENSOR_CMD));
        }

        poll_tick++;
        delay_ms(20);
    }
}
