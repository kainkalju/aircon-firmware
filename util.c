/**
 * util.c -- Utility functions
 */

#include "daikin.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/*  Tick counter (incremented by SysTick_Handler every 1 ms)         */
/* ------------------------------------------------------------------ */
uint32_t get_ms_ticks(void) {
    return g_ms_ticks;
}

void delay_ms(uint32_t ms) {
    uint32_t end = g_ms_ticks + ms;
    while (g_ms_ticks < end)
        __asm volatile("nop");
}

/* ------------------------------------------------------------------ */
/*  log_print -- UART debug output with level filter                  */
/*                                                                    */
/*  Log level is set via DAIKIN_UDP/debug/loglevel=[e|i|v]           */
/* ------------------------------------------------------------------ */
static int g_log_level = LOG_I;

void log_print(int level, const char *fmt, ...) {
    if (level > g_log_level) return;

    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    /* Append CRLF and send via USART1 */
    int len = strlen(buf);
    AppSerialSend((uint8_t *)buf,    len);
    AppSerialSend((uint8_t *)"\r\n", 2);
}

/* ------------------------------------------------------------------ */
/*  IP address helpers                                                */
/* ------------------------------------------------------------------ */
int snprintf_ip(char *buf, int len, uint32_t ip) {
    /* IP stored in network byte order */
    return snprintf(buf, len, "%u.%u.%u.%u",
        (unsigned)((ip >> 24) & 0xFF), (unsigned)((ip >> 16) & 0xFF),
        (unsigned)((ip >>  8) & 0xFF), (unsigned)((ip >>  0) & 0xFF));
}

int parse_ip(const char *s, uint32_t *ip) {
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return RET_PARAM_NG;
    *ip = ((a & 0xFF) << 24) | ((b & 0xFF) << 16) |
          ((c & 0xFF) <<  8) |  (d & 0xFF);
    return RET_OK;
}

/* ------------------------------------------------------------------ */
/*  URL decode                                                        */
/* ------------------------------------------------------------------ */
int url_decode(const char *in, char *out, int out_len) {
    int o = 0;
    while (*in && o < out_len - 1) {
        if (*in == '%' && in[1] && in[2]) {
            unsigned c;
            char hex[3] = { in[1], in[2], 0 };
            sscanf(hex, "%x", &c);
            out[o++] = (char)c;
            in += 3;
        } else if (*in == '+') {
            out[o++] = ' ';
            in++;
        } else {
            out[o++] = *in++;
        }
    }
    out[o] = '\0';
    return o;
}
