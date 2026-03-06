/**
 * daikin.h -- Reversed header for Daikin BRP069A42 WiFi Adapter Firmware
 *
 * Target: STM32F2xx (ARM Cortex-M3), flash @ 0x08000000, SRAM @ 0x20000000
 * WiFi:   Murata WLAN module (Broadcom BCM43xx) via SDIO
 * RTOS:   Lightweight cooperative/preemptive task kernel (FreeRTOS-style)
 * Stack:  0x20020000 (128 KB SRAM)
 *
 * Firmware version: 3.4.3  (filename: dkacstm-wlan-F-3_4_3.bin)
 * Date string:      Thu 2013-01-24  (Broadcom WLAN driver build)
 */

#ifndef DAIKIN_H
#define DAIKIN_H

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/*  Compiler / MCU shims                                               */
/* ------------------------------------------------------------------ */
#define __weak      __attribute__((weak))
#define __irq       __attribute__((interrupt))
#define __packed    __attribute__((packed))
#define UNUSED(x)   ((void)(x))

typedef unsigned int   uint;
typedef unsigned char  uchar;

/* ------------------------------------------------------------------ */
/*  STM32 peripheral base addresses (F2xx / F4xx family)              */
/* ------------------------------------------------------------------ */
#define PERIPH_BASE     0x40000000UL
#define APB1_BASE       (PERIPH_BASE + 0x00000000)
#define APB2_BASE       (PERIPH_BASE + 0x00010000)
#define AHB1_BASE       (PERIPH_BASE + 0x00020000)

#define RCC_BASE        (AHB1_BASE  + 0x3800)
#define GPIOA_BASE      (AHB1_BASE  + 0x0000)
#define GPIOB_BASE      (AHB1_BASE  + 0x0400)
#define GPIOC_BASE      (AHB1_BASE  + 0x0800)
#define USART1_BASE     (APB2_BASE  + 0x1000)
#define TIM3_BASE       (APB1_BASE  + 0x0400)
#define EXTI_BASE       (APB2_BASE  + 0x3C00)
#define SDIO_BASE       (APB2_BASE  + 0xC000)
#define FLASH_BASE      0x40023C00UL

/* ------------------------------------------------------------------ */
/*  RCC register map                                                   */
/* ------------------------------------------------------------------ */
typedef struct {
    volatile uint32_t CR;        /* 0x00 clock control */
    volatile uint32_t PLLCFGR;   /* 0x04 PLL config    */
    volatile uint32_t CFGR;      /* 0x08 clock config  */
    volatile uint32_t CIR;       /* 0x0C clock irq     */
    volatile uint32_t AHB1RSTR;
    volatile uint32_t AHB2RSTR;
    volatile uint32_t AHB3RSTR;
    uint32_t RESERVED0;
    volatile uint32_t APB1RSTR;
    volatile uint32_t APB2RSTR;
    uint32_t RESERVED1[2];
    volatile uint32_t AHB1ENR;   /* 0x30 AHB1 clock enable */
    volatile uint32_t AHB2ENR;
    volatile uint32_t AHB3ENR;
    uint32_t RESERVED2;
    volatile uint32_t APB1ENR;   /* 0x40 APB1 clock enable */
    volatile uint32_t APB2ENR;
} RCC_TypeDef;

/* ------------------------------------------------------------------ */
/*  USART register map                                                 */
/* ------------------------------------------------------------------ */
typedef struct {
    volatile uint32_t SR;   /* status         */
    volatile uint32_t DR;   /* data           */
    volatile uint32_t BRR;  /* baud rate      */
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t CR3;
    volatile uint32_t GTPR;
} USART_TypeDef;

#define USART_SR_RXNE   (1u << 5)
#define USART_SR_TC     (1u << 6)
#define USART_SR_TXE    (1u << 7)
#define USART_CR1_UE    (1u << 13)
#define USART_CR1_RXNEIE (1u << 5)

/* ------------------------------------------------------------------ */
/*  GPIO register map                                                  */
/* ------------------------------------------------------------------ */
typedef struct {
    volatile uint32_t MODER;
    volatile uint32_t OTYPER;
    volatile uint32_t OSPEEDR;
    volatile uint32_t PUPDR;
    volatile uint32_t IDR;
    volatile uint32_t ODR;
    volatile uint32_t BSRR;
    volatile uint32_t LCKR;
    volatile uint32_t AFR[2];
} GPIO_TypeDef;

/* ------------------------------------------------------------------ */
/*  SDIO register map (for Broadcom WLAN via SDIO)                    */
/* ------------------------------------------------------------------ */
typedef struct {
    volatile uint32_t POWER;
    volatile uint32_t CLKCR;
    volatile uint32_t ARG;
    volatile uint32_t CMD;
    volatile uint32_t RESPCMD;
    volatile uint32_t RESP[4];
    volatile uint32_t DTIMER;
    volatile uint32_t DLEN;
    volatile uint32_t DCTRL;
    volatile uint32_t DCOUNT;
    volatile uint32_t STA;
    volatile uint32_t ICR;
    volatile uint32_t MASK;
    uint32_t RESERVED[2];
    volatile uint32_t FIFOCNT;
    uint32_t RESERVED2[13];
    volatile uint32_t FIFO;
} SDIO_TypeDef;

/* ------------------------------------------------------------------ */
/*  Peripheral instances                                               */
/* ------------------------------------------------------------------ */
#define RCC     ((RCC_TypeDef *)  RCC_BASE)
#define USART1  ((USART_TypeDef *)USART1_BASE)
#define GPIOA   ((GPIO_TypeDef *) GPIOA_BASE)
#define GPIOB   ((GPIO_TypeDef *) GPIOB_BASE)
#define GPIOC   ((GPIO_TypeDef *) GPIOC_BASE)
#define SDIO    ((SDIO_TypeDef *) SDIO_BASE)

/* ------------------------------------------------------------------ */
/*  Return codes used throughout the firmware                         */
/* ------------------------------------------------------------------ */
#define RET_OK          0
#define RET_NG          (-1)
#define RET_PARAM_NG    (-2)
#define RET_TIMEOUT     (-3)
#define RET_BUSY        (-4)
#define RET_NOT_SUPPORT (-5)

/* HTTP response body prefixes */
#define RESP_OK          "ret=OK"
#define RESP_PARAM_NG    "ret=PARAM NG"
#define RESP_INTERNAL_NG "ret=INTERNAL NG"

/* ------------------------------------------------------------------ */
/*  Network / WiFi                                                     */
/* ------------------------------------------------------------------ */
#define WIFI_SSID_MAX       64
#define WIFI_KEY_MAX        64
#define WIFI_BSSID_LEN      6
#define DEFAULT_AP_SSID     "@DaikinAP"
#define DEFAULT_MAC         "00:11:22:33:44:55"   /* placeholder */

/* Security modes */
#define WIFI_SEC_NONE   0
#define WIFI_SEC_WEP    1
#define WIFI_SEC_WPA    2
#define WIFI_SEC_WPA2   3

typedef struct {
    char    ssid[WIFI_SSID_MAX];
    char    key[WIFI_KEY_MAX];
    uint8_t bssid[WIFI_BSSID_LEN];
    int     security;
    int     radio_enabled;
    uint32_t ip;
    uint32_t gw;
    uint32_t netmask;
    int     auto_ip;
    int     link_up;
    int     ping_count;
} wifi_config_t;

/* ------------------------------------------------------------------ */
/*  Aircon control state                                               */
/* ------------------------------------------------------------------ */
#define AC_POW_OFF      '0'
#define AC_POW_ON       '1'

#define AC_MODE_AUTO    '0'
#define AC_MODE_DRY     '2'
#define AC_MODE_COOL    '3'
#define AC_MODE_HEAT    '4'
#define AC_MODE_FAN     '6'

#define AC_FRATE_AUTO   'A'
#define AC_FRATE_SILENT 'B'
#define AC_FRATE_1      '3'
#define AC_FRATE_2      '4'
#define AC_FRATE_3      '5'
#define AC_FRATE_4      '6'
#define AC_FRATE_5      '7'

#define AC_FDIR_STOP    '0'
#define AC_FDIR_VD1     '1'
#define AC_FDIR_VD2     '2'
#define AC_FDIR_VD3     '3'
#define AC_FDIR_VD4     '4'
#define AC_FDIR_VD5     '5'
#define AC_FDIR_SWING   'S'

typedef struct {
    char    pow;            /* '0'=off, '1'=on        */
    char    mode;           /* AC_MODE_*              */
    char    stemp[8];       /* set temp, e.g. "25.0"  */
    char    shum[8];        /* set humidity           */
    char    f_rate;         /* fan rate AC_FRATE_*    */
    char    f_dir;          /* fan direction          */
    char    b_mode;         /* backup mode            */
    char    b_stemp[8];     /* backup temperature     */
    char    b_shum[8];      /* backup humidity        */
    char    adv[8];         /* advanced settings      */
    int     alert;
    /* sensor readings */
    char    htemp[8];       /* home (indoor) temp     */
    char    hhum[8];        /* home humidity          */
    char    otemp[8];       /* outdoor temp           */
    int     err;            /* error code             */
    int     cmpfreq;        /* compressor frequency   */
    int     mompow;         /* momentary power (W)    */
} ac_state_t;

/* ------------------------------------------------------------------ */
/*  Adapter / device info                                             */
/* ------------------------------------------------------------------ */
#define FW_VER_MAJOR    3
#define FW_VER_MINOR    4
#define FW_VER_PATCH    3

typedef struct {
    char    type[16];
    char    reg[4];         /* region code            */
    int     dst;            /* daylight saving        */
    int     ver[3];         /* version [maj,min,pat]  */
    char    name[64];
    char    location[64];
    char    icon[16];
    char    method[16];     /* remote method          */
    int     port;
    char    id[64];
    char    pw[64];
    int     lpw_flag;
    int     adp_kind;
    char    pv[8];
    char    cpv;
    int     cpv_minor;
    int     led;
    int     en_setzone;
    uint8_t mac[6];
    char    adp_mode[16];
    int     en_hol;
    char    enlver[16];
} adapter_info_t;

/* ------------------------------------------------------------------ */
/*  Scheduling timer                                                   */
/* ------------------------------------------------------------------ */
#define SCDL_MAX        3
#define SCDL_PER_DAY    6

typedef struct {
    int     en;
    int     pow;
    int     mode;
    char    temp[8];
    int     time;       /* minutes from midnight */
    int     vol;
    int     dir;
    char    humi[8];
    int     spmd;
} scdl_slot_t;

typedef struct {
    int         en_scdltimer;
    int         active_no;
    int         en_oldtimer;
    char        scdl_name[SCDL_MAX][32];
    scdl_slot_t body[SCDL_MAX][SCDL_PER_DAY];
} scdl_timer_t;

/* ------------------------------------------------------------------ */
/*  Demand control                                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    int     en_demand;
    int     mode;
    int     max_pow;
    int     scdl_per_day;
    int     dmnd_run;
} demand_ctrl_t;

/* ------------------------------------------------------------------ */
/*  Event queue (lightweight message passing)                         */
/* ------------------------------------------------------------------ */
#define QUEUE_MAX_DEPTH     16

typedef struct event_node {
    struct event_node *next;
    uint32_t           data;
} event_node_t;

typedef struct {
    event_node_t  *head;
    event_node_t  *tail;
    int            count;
    const char    *name;
} event_queue_t;

/* ------------------------------------------------------------------ */
/*  Global state                                                       */
/* ------------------------------------------------------------------ */
extern ac_state_t       g_ac_state;
extern adapter_info_t   g_adapter_info;
extern wifi_config_t    g_wifi_cfg;
extern scdl_timer_t     g_scdl;
extern demand_ctrl_t    g_demand;
extern event_queue_t    gQ_PKTRX;
extern event_queue_t    gQ_PKTRX_COMPLETE;

/* reset / uptime */
extern uint32_t         g_reset_count;
extern uint32_t         g_last_reset_time;
extern uint32_t         g_ms_ticks;         /* SysTick counter */

/* ------------------------------------------------------------------ */
/*  Function prototypes -- see individual .c files                     */
/* ------------------------------------------------------------------ */

/* startup.c */
void Reset_Handler(void);
void Default_Handler(void);
void HardFault_Handler(void);
void SysTick_Handler(void);
void USART1_IRQHandler(void);
void TIM3_IRQHandler(void);
void EXTI15_10_IRQHandler(void);

/* main.c */
void AppMain(void);
void AppTestMain(void);
void AppFlashTest(void);
void panic_main(int type);

/* uart.c */
void uart_init(uint32_t baud);
void AppSerialReceive(uint8_t *buf, int len);
void AppSerialSend(const uint8_t *buf, int len);
void VerificationSerialTask(void *arg);
void serialComm_task(void *arg);

/* network.c */
int  wifi_init(void);
int  wifi_connect(const char *ssid, const char *key, int security);
int  wifi_start_ap(const char *ssid);
int  wifi_get_ip(uint32_t *ip, uint32_t *gw);
void AppUdpCmdReceive(const uint8_t *buf, int len, uint32_t src_ip, uint16_t src_port);
void AppUdpCmdSend(uint32_t dst_ip, uint16_t dst_port, const uint8_t *buf, int len);
int  sdio_cmd53(int write, uint32_t addr, uint8_t *buf, int len);
int  dhcp_start(void);

/* http_server.c */
void httpd_start(void);
int  httpd_send_ok(int sock, const char *body, int body_len);
int  httpd_send_error(int sock, int code, const char *msg);
int  parse_key_values(const char *qs, const char *key, char *out, int out_len);

/* aircon_api.c */
int  ac_get_control_info(char *buf, int buf_len);
int  ac_set_control_info(const char *qs);
int  ac_get_sensor_info(char *buf, int buf_len);
int  ac_get_model_info(char *buf, int buf_len);
int  ac_get_info(char *buf, int buf_len);
int  ac_set_info(const char *qs);
int  ac_get_timer(char *buf, int buf_len);
int  ac_set_timer(const char *qs);
int  ac_get_week_power(char *buf, int buf_len);
int  ac_get_year_power(char *buf, int buf_len);
int  ac_get_month_power_ex(char *buf, int buf_len);
int  ac_clr_nw_err(void);
int  ac_get_nw_err(char *buf, int buf_len);
int  ac_set_special_mode(const char *qs);
int  ac_get_demand_control(char *buf, int buf_len);
int  ac_set_demand_control(const char *qs);
int  ac_get_scdltimer(char *buf, int buf_len);
int  ac_set_scdltimer(const char *qs);
int  ac_get_scdltimer_body(char *buf, int buf_len);
int  ac_set_scdltimer_body(const char *qs);
int  ac_get_price(char *buf, int buf_len);
int  ac_set_price(const char *qs);
void ac_error_notice_post(void);

/* common_api.c */
int  common_basic_info(char *buf, int buf_len);
int  common_get_remote_method(char *buf, int buf_len);
int  common_set_remote_method(const char *qs);
int  common_get_wifi_setting(char *buf, int buf_len);
int  common_set_wifi_setting(const char *qs);
int  common_get_network_setting(char *buf, int buf_len);
int  common_set_network_setting(const char *qs);
int  common_reboot(void);
int  common_fwupdate(const uint8_t *data, int len);
int  common_get_datetime(char *buf, int buf_len);
int  common_notify_datetime(const char *qs);
int  common_set_led(int state);
int  common_get_fwinfo(char *buf, int buf_len);
int  common_start_wifi_scan(char *buf, int buf_len);
int  common_get_wifi_scan_result(char *buf, int buf_len);
int  common_permit_wifi_connection(const char *qs);
int  common_set_account(const char *qs);
int  common_get_sn(char *buf, int buf_len);
int  common_set_sn(const char *qs);
int  common_get_holiday(char *buf, int buf_len);
int  common_set_holiday(const char *qs);

/* echonet.c */
void echonet_init(void);
void echonet_task(void *arg);
int  echonet_send_basic_info(uint32_t dst_ip);
int  echonet_process_command(const uint8_t *frame, int len);

/* cloud.c */
void cloud_task(void *arg);
int  cloud_post_error_notice(int err_code, const char *info);
int  cloud_post_24h_info(void);
int  cloud_sync_datetime(void);
const char *cloud_get_server_name(void);
int  cloud_set_server_name(const char *name);
int  https_post(const char *path, const char *body, int body_len);
int  https_get(const char *path, char *buf, int buf_len);

/* util.c */
uint32_t get_ms_ticks(void);
void     delay_ms(uint32_t ms);
int      snprintf_ip(char *buf, int len, uint32_t ip);
int      parse_ip(const char *s, uint32_t *ip);
int      url_decode(const char *in, char *out, int out_len);
void     log_print(int level, const char *fmt, ...);

#define LOG_E   0   /* error   */
#define LOG_I   1   /* info    */
#define LOG_V   2   /* verbose */

#endif /* DAIKIN_H */
