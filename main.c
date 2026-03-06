/**
 * main.c -- Application entry point and top-level task management
 *
 * AppMain initialises all subsystems and starts the RTOS scheduler.
 * Task architecture (inferred from strings and queue names):
 *
 *   AppMain              -- root task, spawns all others
 *   VerificationSerialTask -- serial port framing / checksum
 *   serialComm_task      -- higher-level serial protocol
 *   httpd task           -- HTTP server (port 80)
 *   echonet_task         -- ECHONET Lite over UDP (port 3610)
 *   cloud_task           -- HTTPS polling to Daikin cloud
 *   AppUdpCmdReceive     -- UDP command listener
 *
 * Global event queues:
 *   gQ_PKTRX             -- raw SDIO packet received
 *   gQ_PKTRX_COMPLETE    -- packet fully processed
 */

#include "daikin.h"
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  Global state                                                       */
/* ------------------------------------------------------------------ */
ac_state_t      g_ac_state;
adapter_info_t  g_adapter_info;
wifi_config_t   g_wifi_cfg;
scdl_timer_t    g_scdl;
demand_ctrl_t   g_demand;
event_queue_t   gQ_PKTRX;
event_queue_t   gQ_PKTRX_COMPLETE;
uint32_t        g_reset_count      = 0;
uint32_t        g_last_reset_time  = 0;

/* ------------------------------------------------------------------ */
/*  Forward declarations for internal helpers                         */
/* ------------------------------------------------------------------ */
static int  init_event_queue(event_queue_t *q, const char *name);
static void system_clock_init(void);
static void gpio_init(void);
static void default_ac_state(void);
static void default_adapter_info(void);
static void default_wifi_config(void);

/* ------------------------------------------------------------------ */
/*  AppMain (0x08001F5A area -- "HTTPd START.")                        */
/* ------------------------------------------------------------------ */
void AppMain(void) {
    int ret;

    /* Hardware init */
    system_clock_init();
    gpio_init();
    uart_init(115200);

    log_print(LOG_I, "ptn9:power on");
    log_print(LOG_I, "dkacstm-wlan-");    /* firmware build tag */

    /* Initialize event queues */
    ret = init_event_queue(&gQ_PKTRX, "gQ_PKTRX");
    if (ret != RET_OK) {
        log_print(LOG_E, "[ERR][%s]init_event_queue failed(gQ_PKTRX)", __func__);
    }
    ret = init_event_queue(&gQ_PKTRX_COMPLETE, "gQ_PKTRX_COMPLETE");
    if (ret != RET_OK) {
        log_print(LOG_E, "[ERR][%s]init_event_queue failed(gQ_PKTRX_COMPLETE)", __func__);
    }

    /* Load persistent settings from flash */
    default_ac_state();
    default_adapter_info();
    default_wifi_config();

    /* Bring up WiFi (Murata WLAN via SDIO) */
    ret = wifi_init();
    if (ret != RET_OK) {
        log_print(LOG_E, " Adapter Bug!");
        /* Continue anyway -- enter AP mode for provisioning */
    }

    /* Start AP for initial provisioning if no SSID configured */
    if (g_wifi_cfg.ssid[0] == '\0') {
        wifi_start_ap(DEFAULT_AP_SSID);
        log_print(LOG_I, "ptn1:%02x%02x%02x%02x%02x%02x",
                  g_adapter_info.mac[0], g_adapter_info.mac[1],
                  g_adapter_info.mac[2], g_adapter_info.mac[3],
                  g_adapter_info.mac[4], g_adapter_info.mac[5]);
    } else {
        wifi_connect(g_wifi_cfg.ssid, g_wifi_cfg.key, g_wifi_cfg.security);
        dhcp_start();
    }

    /* ECHONET Lite */
    echonet_init();

    /* Start HTTP server */
    log_print(LOG_I, "HTTPd START.");
    httpd_start();

    /* Spawn tasks */
    /* NOTE: actual RTOS task creation calls depend on the kernel API.
     * The firmware appears to use a proprietary lightweight kernel
     * (not vanilla FreeRTOS) with a similar API. */

    /* task: serial verification */
    /* create_task(VerificationSerialTask, "VerificationSerialTask", NULL, 512, 3); */

    /* task: serial communication (higher-level protocol to indoor unit) */
    /* create_task(serialComm_task, "serialComm", NULL, 1024, 3); */

    /* task: ECHONET Lite / UDP */
    /* create_task(echonet_task, "echonet", NULL, 1024, 2); */

    /* task: cloud sync */
    /* create_task(cloud_task, "cloud", NULL, 2048, 1); */

    /* Start scheduler -- does not return */
    /* scheduler_start(); */

    for (;;) {
        delay_ms(1000);

        /* Watchdog kick */
        /* WWDG->CR = WWDG_CR_WDGA | 0x7F; */

        /* Log network state periodically */
        uint32_t ip = 0, gw = 0;
        wifi_get_ip(&ip, &gw);
        log_print(LOG_V, "myip=%08x gwip=%08x pngCnt=%d gLnkOn=%d",
                  ip, gw, 0, g_wifi_cfg.link_up);
    }
}

/* ------------------------------------------------------------------ */
/*  AppTestMain -- factory/production self-test (0x08001DA3)           */
/* ------------------------------------------------------------------ */
void AppTestMain(void) {
    log_print(LOG_I, " AppTestMain");
    log_print(LOG_I, "ptn4:%d_%d_%d %s aircon",
              FW_VER_MAJOR, FW_VER_MINOR, FW_VER_PATCH, "test");

    /* Serial loopback test */
    /* Flash R/W test */
    AppFlashTest();

    /* WiFi module presence test */
    int ret = wifi_init();
    if (ret != RET_OK) {
        log_print(LOG_E, "SERIAL IF FAILURE");
        return;
    }

    /* TypeYdMusen (Murata WLAN type string) */
    log_print(LOG_I, "TypeYdMusen");

    /* All tests passed */
    log_print(LOG_I, "ptn2:%s", "OK");
}

/* ------------------------------------------------------------------ */
/*  AppFlashTest -- verify flash memory integrity (0x08001929)         */
/* ------------------------------------------------------------------ */
void AppFlashTest(void) {
    log_print(LOG_I, "@AppFlashTest");

    /* Walk through flash and verify checksum */
    const uint8_t *flash = (const uint8_t *)0x08000000;
    uint32_t csum = 0;
    for (int i = 0; i < 489248; i++) {
        csum += flash[i];
    }

    /* Compare against stored checksum at end of image */
    uint32_t stored = *((const uint32_t *)(flash + 489244));
    if (csum != stored) {
        log_print(LOG_E, "memory check error");
    } else {
        log_print(LOG_I, "ret=OK,csum=%u", csum);
    }
}

/* ------------------------------------------------------------------ */
/*  Internal: event queue init                                        */
/* ------------------------------------------------------------------ */
static int init_event_queue(event_queue_t *q, const char *name) {
    if (!q || !name) {
        log_print(LOG_E, "[ERR][%s]parameter error", __func__);
        return RET_PARAM_NG;
    }
    /* Detect duplicate init */
    if (q->name != NULL) {
        log_print(LOG_I, "event queue \"%s\" is already exists.", name);
        return RET_OK;
    }
    q->head  = NULL;
    q->tail  = NULL;
    q->count = 0;
    q->name  = name;
    return RET_OK;
}

/* ------------------------------------------------------------------ */
/*  Internal: system clock -- 120 MHz via PLL from HSE 12 MHz         */
/*  (inferred: STM32F207 or STM32F217)                               */
/* ------------------------------------------------------------------ */
static void system_clock_init(void) {
    /* Enable HSE */
    RCC->CR |= (1u << 16);          /* HSEON */
    while (!(RCC->CR & (1u << 17))) /* wait HSERDY */
        ;

    /* Configure PLL: PLLM=12, PLLN=240, PLLP=2 -> 120 MHz */
    RCC->PLLCFGR = (12u << 0)   |  /* PLLM */
                   (240u << 6)  |  /* PLLN */
                   (0u << 16)   |  /* PLLP = /2 */
                   (1u << 22)   |  /* PLLSRC = HSE */
                   (5u << 24);     /* PLLQ = /5 (48 MHz USB) */

    /* Enable PLL */
    RCC->CR |= (1u << 24);          /* PLLON */
    while (!(RCC->CR & (1u << 25))) /* wait PLLRDY */
        ;

    /* Configure flash latency for 120 MHz */
    *((volatile uint32_t *)FLASH_BASE) = 0x103; /* 3 wait states + prefetch */

    /* Switch sysclk to PLL */
    RCC->CFGR = (RCC->CFGR & ~0x3u) | 0x2u;
    while ((RCC->CFGR & 0xCu) != 0x8u)
        ;

    /* Enable peripheral clocks */
    RCC->AHB1ENR |= (1u << 0)  | /* GPIOA */
                    (1u << 1)  | /* GPIOB */
                    (1u << 2);   /* GPIOC */
    RCC->APB2ENR |= (1u << 4)  | /* USART1 */
                    (1u << 11);  /* SDIO   */
    RCC->APB1ENR |= (1u << 1);   /* TIM3   */
}

/* ------------------------------------------------------------------ */
/*  Internal: GPIO init                                               */
/*                                                                    */
/*  PA9  = USART1 TX (AF7)                                           */
/*  PA10 = USART1 RX (AF7)                                           */
/*  PC0  = WLAN nRESET (output)                                      */
/*  PC1  = WLAN IRQ   (input, EXTI10->EXTI15_10_IRQHandler)          */
/*  PB12-PB15, PC8-PC12 = SDIO (AF12)                               */
/* ------------------------------------------------------------------ */
static void gpio_init(void) {
    /* USART1 pins */
    GPIOA->MODER   = (GPIOA->MODER & ~(0xFu << 18)) | (0xAu << 18); /* PA9,PA10 AF */
    GPIOA->AFR[1] |= (7u << 4) | (7u << 8);  /* AF7 = USART1 */

    /* WLAN nRESET = PC0 output */
    GPIOC->MODER = (GPIOC->MODER & ~(0x3u << 0)) | (0x1u << 0);
    GPIOC->BSRR  = (1u << 0);   /* assert high (released) */

    /* WLAN IRQ = PC1 input with EXTI */
    GPIOC->MODER &= ~(0x3u << 2);  /* input mode */
    /* Configure EXTI line 11 for PC ... (SYSCFG EXTICR) */
}

/* ------------------------------------------------------------------ */
/*  Internal: default state initialisation                           */
/* ------------------------------------------------------------------ */
static void default_ac_state(void) {
    memset(&g_ac_state, 0, sizeof(g_ac_state));
    g_ac_state.pow    = AC_POW_OFF;
    g_ac_state.mode   = AC_MODE_AUTO;
    g_ac_state.f_rate = AC_FRATE_AUTO;
    g_ac_state.f_dir  = AC_FDIR_STOP;
    snprintf(g_ac_state.stemp,  sizeof(g_ac_state.stemp),  "25.0");
    snprintf(g_ac_state.shum,   sizeof(g_ac_state.shum),   "--");
    snprintf(g_ac_state.htemp,  sizeof(g_ac_state.htemp),  "--");
    snprintf(g_ac_state.hhum,   sizeof(g_ac_state.hhum),   "--");
    snprintf(g_ac_state.otemp,  sizeof(g_ac_state.otemp),  "--");
}

static void default_adapter_info(void) {
    memset(&g_adapter_info, 0, sizeof(g_adapter_info));
    snprintf(g_adapter_info.type,    sizeof(g_adapter_info.type),    "aircon");
    snprintf(g_adapter_info.reg,     sizeof(g_adapter_info.reg),     "--");
    snprintf(g_adapter_info.method,  sizeof(g_adapter_info.method),  "polling");
    g_adapter_info.port    = 80;
    g_adapter_info.adp_kind = 0;
    /* MAC is read from WLAN module NV at runtime */
}

static void default_wifi_config(void) {
    memset(&g_wifi_cfg, 0, sizeof(g_wifi_cfg));
    g_wifi_cfg.security = WIFI_SEC_WPA2;
    g_wifi_cfg.auto_ip  = 1;
}
