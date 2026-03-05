/**
 * common_api.c — Common adapter HTTP API handlers
 *
 * Response formats reconstructed from firmware strings:
 *
 *   basic_info (0x08075CF8):
 *     ret=%s,type=%s,reg=%s,dst=%d,ver=%d_%d_%d,
 *     pow=%s,err=%d,location=%s,name=%s,icon=%s,
 *     method=%s,port=%d,id=%s,pw=%s,lpw_flag=%d,
 *     adp_kind=%d,pv=%s,cpv=%c,cpv_minor=%02d,led=%d,
 *     en_setzone=%d,mac=%02X%02X%02X%02X%02X%02X,
 *     adp_mode=%s,en_hol=%d,enlver=%s
 *
 *   get_wifi_setting (0x0800BE10):
 *     ret=%s,ssid=%s,security=%s,key=%s
 *
 *   get_network_setting (0x0800BDB0):
 *     ret=%s,method=%s,notice_ip_int=%d,notice_sync_int=%d
 *
 *   get_version (0x08009D44):
 *     ret=OK,ver=%d.%d.%d,fwtm=%d/%d/%d/%d:%d,lorw=%d,kind=%d
 *
 *   get_servername (0x08009DF0):
 *     ret=OK,ServerName=%s
 *
 *   DKAC_V03 / DKAC_V02 strings indicate protocol version support
 *
 * Cloud servers found in firmware (0x080750A4+):
 *   sha2.daikinonlinecontroller.com  (production)
 *   test2.daikindev.com              (development)
 *   DS-AIRmini.daikin-china.com.cn   (China)
 *   daikinsmartdb.jp                 (Japan legacy)
 *   www.daikincloud.net              (international)
 *   Ubiquitous server                (label string)
 *
 * SSL/TLS certificate CA:
 *   "BBE, GlobalSign nv-sa, Root CA, GlobalSign Root CA" (0x080770E0)
 */

#include "daikin.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/*  Network notification settings                                     */
/* ------------------------------------------------------------------ */
static int   g_notice_ip_int   = 3600;   /* IP change notify interval (s) */
static int   g_notice_sync_int = 3600;   /* NTP sync interval (s)         */
static char  g_server_name[64] = "sha2.daikinonlinecontroller.com";

/* Product / serial */
static char  g_product_code[32] = "";
static char  g_region_code[8]   = "jp";
static char  g_serial_num[32]   = "";

/* WiFi scan results */
#define WIFI_SCAN_MAX   10
typedef struct {
    char ssid[64];
    int  security;
    int  radio;
    int  rssi;
} scan_result_t;
static scan_result_t g_scan_results[WIFI_SCAN_MAX];
static int           g_scan_count = 0;

/* Firmware update timestamp */
static int g_fwtm[5] = { 2013, 1, 24, 0, 0 };  /* year/mon/day/hour/min */

/* ------------------------------------------------------------------ */
/*  common_basic_info                                                 */
/* ------------------------------------------------------------------ */
int common_basic_info(char *buf, int buf_len) {
    adapter_info_t *ai = &g_adapter_info;

    return snprintf(buf, buf_len,
        "ret=OK,type=%s,reg=%s,dst=%d,ver=%d_%d_%d,"
        "pow=%s,err=%d,location=%s,name=%s,icon=%s,"
        "method=%s,port=%d,id=%s,pw=%s,lpw_flag=%d,"
        "adp_kind=%d,pv=%s,cpv=%c,cpv_minor=%02d,led=%d,"
        "en_setzone=%d,"
        "mac=%02X%02X%02X%02X%02X%02X,"
        "adp_mode=%s,en_hol=%d,enlver=%s",
        ai->type, ai->reg, ai->dst,
        FW_VER_MAJOR, FW_VER_MINOR, FW_VER_PATCH,
        (g_ac_state.pow == AC_POW_ON ? "on" : "off"),
        g_ac_state.err,
        ai->location, ai->name, ai->icon,
        ai->method, ai->port, ai->id, ai->pw, ai->lpw_flag,
        ai->adp_kind, ai->pv, ai->cpv, ai->cpv_minor, ai->led,
        ai->en_setzone,
        ai->mac[0], ai->mac[1], ai->mac[2],
        ai->mac[3], ai->mac[4], ai->mac[5],
        ai->adp_mode, ai->en_hol, ai->enlver);
}

/* ------------------------------------------------------------------ */
/*  common_get_remote_method / common_set_remote_method               */
/* ------------------------------------------------------------------ */
int common_get_remote_method(char *buf, int buf_len) {
    adapter_info_t *ai = &g_adapter_info;
    return snprintf(buf, buf_len,
        "ret=OK,method=%s,notice_ip_int=%d,notice_sync_int=%d",
        ai->method, g_notice_ip_int, g_notice_sync_int);
}

int common_set_remote_method(const char *qs) {
    char val[32];
    if (parse_key_values(qs, "method", g_adapter_info.method,
                         sizeof(g_adapter_info.method)) != RET_OK)
        return RET_PARAM_NG;
    if (parse_key_values(qs, "notice_ip_int", val, sizeof(val)) == RET_OK)
        g_notice_ip_int = atoi(val);
    if (parse_key_values(qs, "notice_sync_int", val, sizeof(val)) == RET_OK)
        g_notice_sync_int = atoi(val);
    return RET_OK;
}

/* ------------------------------------------------------------------ */
/*  common_get_wifi_setting / common_set_wifi_setting                 */
/* ------------------------------------------------------------------ */
int common_get_wifi_setting(char *buf, int buf_len) {
    wifi_config_t *wc = &g_wifi_cfg;
    const char *sec_str;
    switch (wc->security) {
    case WIFI_SEC_NONE: sec_str = "unsecured";  break;
    case WIFI_SEC_WEP:  sec_str = "wep";        break;
    case WIFI_SEC_WPA:  sec_str = "wpa";        break;
    default:            sec_str = "wpa2";       break;
    }

    /* Multiple SSIDs supported: ssid1=, sec1=, radio1= ... */
    int len = snprintf(buf, buf_len, "ret=OK,ssid=%s,security=%s,key=%s",
                       wc->ssid, sec_str, wc->key);

    /* Additional scan slots */
    for (int i = 0; i < g_scan_count && i < 3; i++) {
        len += snprintf(buf + len, buf_len - len,
            ",ssid%d=%s,sec%d=%d,radio%d=%d",
            i+1, g_scan_results[i].ssid,
            i+1, g_scan_results[i].security,
            i+1, g_scan_results[i].radio);
    }
    return len;
}

int common_set_wifi_setting(const char *qs) {
    wifi_config_t *wc = &g_wifi_cfg;
    char sec_str[16];

    if (parse_key_values(qs, "ssid", wc->ssid, sizeof(wc->ssid)) != RET_OK)
        return RET_PARAM_NG;
    parse_key_values(qs, "key",      wc->key,  sizeof(wc->key));
    parse_key_values(qs, "security", sec_str,  sizeof(sec_str));

    if (strcmp(sec_str, "unsecured") == 0) wc->security = WIFI_SEC_NONE;
    else if (strcmp(sec_str, "wep")  == 0) wc->security = WIFI_SEC_WEP;
    else if (strcmp(sec_str, "wpa")  == 0) wc->security = WIFI_SEC_WPA;
    else                                    wc->security = WIFI_SEC_WPA2;

    log_print(LOG_I, "ptn3:%s", wc->ssid);
    /* Reconnect to new AP */
    /* wifi_connect(wc->ssid, wc->key, wc->security); */

    return RET_OK;
}

/* ------------------------------------------------------------------ */
/*  common_get_network_setting / common_set_network_setting           */
/* ------------------------------------------------------------------ */
int common_get_network_setting(char *buf, int buf_len) {
    return snprintf(buf, buf_len,
        "ret=OK,method=%s,notice_ip_int=%d,notice_sync_int=%d",
        g_adapter_info.method, g_notice_ip_int, g_notice_sync_int);
}

int common_set_network_setting(const char *qs) {
    char val[16];
    parse_key_values(qs, "method",          g_adapter_info.method,
                     sizeof(g_adapter_info.method));
    if (parse_key_values(qs, "notice_ip_int", val, sizeof(val)) == RET_OK)
        g_notice_ip_int = atoi(val);
    if (parse_key_values(qs, "notice_sync_int", val, sizeof(val)) == RET_OK)
        g_notice_sync_int = atoi(val);
    return RET_OK;
}

/* ------------------------------------------------------------------ */
/*  common_reboot                                                     */
/* ------------------------------------------------------------------ */
int common_reboot(void) {
    log_print(LOG_I, "common/reboot");
    delay_ms(500);
    /* Trigger system reset via SCB AIRCR */
    *((volatile uint32_t *)0xE000ED0C) = 0x05FA0004;
    for (;;) {}
}

/* ------------------------------------------------------------------ */
/*  common_set_led                                                    */
/* ------------------------------------------------------------------ */
int common_set_led(int state) {
    /* Toggle LED via GPIO */
    if (state) {
        GPIOC->BSRR = (1u << 2);   /* PC2 = LED on */
    } else {
        GPIOC->BSRR = (1u << 18);  /* PC2 = LED off */
    }
    g_adapter_info.led = state;
    return RET_OK;
}

/* ------------------------------------------------------------------ */
/*  common_get_datetime / common_notify_datetime                      */
/* ------------------------------------------------------------------ */
static int g_year = 2013, g_mon = 1, g_day = 1;
static int g_hour = 0, g_min = 0, g_sec = 0;

int common_get_datetime(char *buf, int buf_len) {
    return snprintf(buf, buf_len,
        "ret=OK,%04d/%02d/%02d %02d:%02d:%02d",
        g_year, g_mon, g_day, g_hour, g_min, g_sec);
}

int common_notify_datetime(const char *qs) {
    char val[32];
    /* Format: date=YYYY/MM/DD&time=HH:MM:SS */
    if (parse_key_values(qs, "date", val, sizeof(val)) == RET_OK) {
        sscanf(val, "%d/%d/%d", &g_year, &g_mon, &g_day);
    }
    if (parse_key_values(qs, "time", val, sizeof(val)) == RET_OK) {
        sscanf(val, "%d:%d:%d", &g_hour, &g_min, &g_sec);
    }
    log_print(LOG_I, "/common/get_date");
    return RET_OK;
}

/* ------------------------------------------------------------------ */
/*  common_get_fwinfo                                                 */
/* ------------------------------------------------------------------ */
int common_get_fwinfo(char *buf, int buf_len) {
    return snprintf(buf, buf_len,
        "ret=OK,ver=%d.%d.%d,fwtm=%d/%d/%d/%d:%d,lorw=0,kind=0",
        FW_VER_MAJOR, FW_VER_MINOR, FW_VER_PATCH,
        g_fwtm[0], g_fwtm[1], g_fwtm[2], g_fwtm[3], g_fwtm[4]);
}

/* ------------------------------------------------------------------ */
/*  Firmware update (dkac/system/fwupdate)                           */
/*                                                                    */
/*  The update is received as multipart/form-data POST (observed in  */
/*  the wire capture). Binary image is written to the second flash   */
/*  bank and the bootloader swaps banks on reboot.                   */
/* ------------------------------------------------------------------ */
int common_fwupdate(const uint8_t *data, int len) {
    log_print(LOG_I, "dkac/system/fwupdate");
    log_print(LOG_I, "ret=OK,ver=%d.%d.%d",
              FW_VER_MAJOR, FW_VER_MINOR, FW_VER_PATCH);

    /* Validate image header */
    if (len < 256) {
        log_print(LOG_E, "INVALID URL");
        return RET_NG;
    }

    /* Write to flash (simplified — real code uses flash HAL) */
    /* flash_erase_sector(SECTOR_2); */
    /* flash_write(FLASH_BANK2_BASE, data, len); */
    /* flash_set_boot_bank(2); */

    /* Reboot into new firmware */
    delay_ms(1000);
    common_reboot();
    return RET_OK;
}

/* ------------------------------------------------------------------ */
/*  WiFi scan                                                         */
/* ------------------------------------------------------------------ */
int common_start_wifi_scan(char *buf, int buf_len) {
    log_print(LOG_I, "common/start_wifi_scan");
    g_scan_count = 0;
    /* bcm_ioctl(WLC_SCAN, &scan_params) */
    return snprintf(buf, buf_len, "ret=OK");
}

int common_get_wifi_scan_result(char *buf, int buf_len) {
    int len = snprintf(buf, buf_len, "ret=OK,ssidnum=%d", g_scan_count);
    for (int i = 0; i < g_scan_count; i++) {
        len += snprintf(buf + len, buf_len - len,
            ",ssid%d=%s,sec%d=%d,radio%d=%d",
            i+1, g_scan_results[i].ssid,
            i+1, g_scan_results[i].security,
            i+1, g_scan_results[i].radio);
    }
    return len;
}

/* ------------------------------------------------------------------ */
/*  WiFi connection permission (WPS-like provisioning)               */
/* ------------------------------------------------------------------ */
int common_permit_wifi_connection(const char *qs) {
    char ssid[WIFI_SSID_MAX], key[WIFI_KEY_MAX];
    char sec_str[16];

    if (parse_key_values(qs, "ssid", ssid, sizeof(ssid)) != RET_OK)
        return RET_PARAM_NG;
    parse_key_values(qs, "key",      key,     sizeof(key));
    parse_key_values(qs, "security", sec_str, sizeof(sec_str));

    /* Apply and connect */
    return common_set_wifi_setting(qs);
}

/* ------------------------------------------------------------------ */
/*  Serial number                                                     */
/* ------------------------------------------------------------------ */
int common_get_sn(char *buf, int buf_len) {
    return snprintf(buf, buf_len, "ret=OK,sn=%s", g_serial_num);
}

int common_set_sn(const char *qs) {
    return parse_key_values(qs, "sn", g_serial_num, sizeof(g_serial_num));
}

/* ------------------------------------------------------------------ */
/*  Holiday setting                                                   */
/* ------------------------------------------------------------------ */
int common_get_holiday(char *buf, int buf_len) {
    return snprintf(buf, buf_len,
        "ret=OK,en_hol=%d", g_adapter_info.en_hol);
}

int common_set_holiday(const char *qs) {
    char val[8];
    if (parse_key_values(qs, "en_hol", val, sizeof(val)) != RET_OK)
        return RET_PARAM_NG;
    g_adapter_info.en_hol = atoi(val);
    return RET_OK;
}

/* ------------------------------------------------------------------ */
/*  Account settings (cloud credentials)                             */
/* ------------------------------------------------------------------ */
int common_set_account(const char *qs) {
    adapter_info_t *ai = &g_adapter_info;
    parse_key_values(qs, "id",  ai->id,  sizeof(ai->id));
    parse_key_values(qs, "pw",  ai->pw,  sizeof(ai->pw));
    return RET_OK;
}

/* ------------------------------------------------------------------ */
/*  Server name (cloud endpoint selection)                           */
/* ------------------------------------------------------------------ */
const char *cloud_get_server_name(void) {
    return g_server_name;
}

int cloud_set_server_name(const char *name) {
    /* Validate against known server list */
    static const char *valid_servers[] = {
        "sha2.daikinonlinecontroller.com",
        "test2.daikindev.com",
        "DS-AIRmini.daikin-china.com.cn",
        "daikinsmartdb.jp",
        "www.daikincloud.net",
        NULL
    };
    for (int i = 0; valid_servers[i]; i++) {
        if (strcmp(name, valid_servers[i]) == 0) {
            snprintf(g_server_name, sizeof(g_server_name), "%s", name);
            log_print(LOG_I, "ret=OK,ServerName=%s", g_server_name);
            return RET_OK;
        }
    }
    return RET_PARAM_NG;
}
