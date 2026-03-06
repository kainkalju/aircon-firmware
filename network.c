/**
 * network.c -- WiFi (Murata/Broadcom) and SDIO driver
 *
 * The adapter uses a Murata WLAN module (Broadcom BCM43362 or similar)
 * connected via SDIO. The firmware includes the Broadcom SDPCMD CDC
 * driver (version 221.1.11.0, built 2013-01-24).
 *
 * Key strings recovered from firmware:
 *   "Broadcom SDPCMD CDC driver"
 *   "(unknown)/sdio-apsta-idsup-idauth-pno Version: 221.1.11.0"
 *   "il0macaddr=00:11:22:33:44:55"   -- NV RAM key for MAC address
 *   "sup_wpa2_eapver"                -- WPA2-Enterprise supplicant
 *   "Wi-Fi Easy and Secure Key Derivation"  -- WPS key derivation
 *   "WFA-SimpleConfig-Enrollee-1-0"  -- WPS enrollee string
 *   "swctrlmap_2g=0x04040404,..."    -- Broadcom RF calibration NV
 *   "tssipos2g=0x%x"                 -- TX power calibration
 *   "chiprev=%d"                     -- Broadcom chip revision
 *   "auto_ip"                        -- link-local fallback
 *   " DHCPsLC"  "DHCPs"              -- DHCP client states
 *   "macaddr=%s"                     -- MAC address format string
 *   " TCP/IP"                        -- TCP/IP stack log prefix
 *
 * SDIO CMD53 is used for bulk data transfer (proc_cmd53).
 * The "TRAP %x(%x): pc %x, lr %x, sp %x" string indicates the
 * Broadcom firmware can report its own crash context via SDIO.
 */

#include "daikin.h"
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  Broadcom SDIO function IDs                                        */
/* ------------------------------------------------------------------ */
#define SDIO_FUNC_BUS   0   /* backplane bus registers */
#define SDIO_FUNC_RADIO 1   /* WLAN radio control      */
#define SDIO_FUNC_NET   2   /* network data            */

/* Broadcom backplane addresses */
#define SBSDIO_SB_OFT_ADDR_MASK  0x7FFF
#define SBSDIO_SB_ACCESS_2_4B_FLAG 0x8000

/* ------------------------------------------------------------------ */
/*  SDIO low-level                                                    */
/* ------------------------------------------------------------------ */

static int sdio_send_cmd(uint32_t cmd, uint32_t arg, uint32_t *resp) {
    SDIO->ARG = arg;
    SDIO->CMD = cmd | (1u << 10); /* CPSMEN */

    uint32_t timeout = 100000;
    while (!(SDIO->STA & (1u << 6)) && timeout--) /* CMDREND */
        ;
    if (!timeout) return RET_TIMEOUT;

    if (resp) *resp = SDIO->RESP[0];
    SDIO->ICR = 0xFFFFFFFF;
    return RET_OK;
}

/**
 * sdio_cmd53 -- Broadcom bulk read/write (proc_cmd53 @ 0x080313DB)
 *
 * CMD53 is the SDIO multi-byte transfer command. The firmware uses it
 * to exchange Ethernet frames with the Broadcom chip.
 */
int sdio_cmd53(int write, uint32_t addr, uint8_t *buf, int len) {
    /* Build CMD53 argument:
     *   [31]    = R/W flag
     *   [30:28] = function number (2 for data)
     *   [27]    = block mode (0=byte)
     *   [26]    = auto-increment address
     *   [25:9]  = register address
     *   [8:0]   = byte count
     */
    uint32_t arg = ((write ? 1u : 0u) << 31)
                 | (SDIO_FUNC_NET << 28)
                 | (0u << 27)     /* byte mode */
                 | (1u << 26)     /* auto-increment */
                 | ((addr & 0x1FFFF) << 9)
                 | (len & 0x1FF);

    /* Configure SDIO data path */
    SDIO->DTIMER = 0xFFFFFFFF;
    SDIO->DLEN   = len;
    SDIO->DCTRL  = (7u << 4)       /* 128-byte block */
                 | (write ? 0u : (1u << 1))  /* direction */
                 | (1u << 0);      /* DTEN */

    uint32_t resp;
    int ret = sdio_send_cmd(53 | (1u << 6) | (1u << 7), arg, &resp);
    if (ret != RET_OK) {
        log_print(LOG_E, "[%s]CMD53 fail", __func__);
        return ret;
    }

    /* Transfer data via FIFO */
    uint32_t *fifo = (uint32_t *)&SDIO->FIFO;
    int words = (len + 3) / 4;
    if (write) {
        for (int i = 0; i < words; i++) {
            uint32_t w = 0;
            memcpy(&w, buf + i*4, (i*4+4 <= len) ? 4 : len - i*4);
            while (!(SDIO->STA & (1u << 14))) ; /* TXFIFOHE */
            *fifo = w;
        }
    } else {
        for (int i = 0; i < words; i++) {
            while (!(SDIO->STA & (1u << 15))) ; /* RXFIFOHF */
            uint32_t w = *fifo;
            int bytes = (i*4+4 <= len) ? 4 : len - i*4;
            memcpy(buf + i*4, &w, bytes);
        }
    }

    return len;
}

/* ------------------------------------------------------------------ */
/*  WiFi init -- reset Broadcom chip and load firmware via SDIO       */
/* ------------------------------------------------------------------ */

static uint8_t g_mac[6];

int wifi_init(void) {
    int ret;
    uint32_t resp;

    /* Assert nRESET low */
    GPIOC->BSRR = (1u << 16);   /* PC0 = 0 */
    delay_ms(10);
    GPIOC->BSRR = (1u << 0);    /* PC0 = 1 */
    delay_ms(50);

    /* SDIO init: CMD0 (GO_IDLE), CMD5 (IO_SEND_OP_COND) */
    sdio_send_cmd(0, 0, NULL);

    ret = sdio_send_cmd(5 | (1u << 5), 0x100000, &resp); /* CMD5-1 */
    if (ret != RET_OK) {
        log_print(LOG_E, "[%s]CMD5-1 fail", __func__);
        return ret;
    }
    ret = sdio_send_cmd(5 | (1u << 5), resp & 0xFFFFFF, &resp); /* CMD5-2 */
    if (ret != RET_OK) {
        log_print(LOG_E, "[%s]CMD5-2 fail", __func__);
        return ret;
    }

    /* CMD3: GET_RELATIVE_ADDR */
    uint32_t rca;
    ret = sdio_send_cmd(3 | (1u << 5) | (1u << 6), 0, &rca);
    if (ret != RET_OK) {
        log_print(LOG_E, "[%s]CMD3 fail", __func__);
        return ret;
    }

    /* CMD7: SELECT card */
    ret = sdio_send_cmd(7 | (1u << 6), rca, &resp);
    if (ret != RET_OK) {
        log_print(LOG_E, "[%s]CMD7 fail", __func__);
        return ret;
    }

    /* Read MAC address from Broadcom NV RAM */
    /* NV key: "il0macaddr" -- read SROM, parse "il0macaddr=xx:xx:xx:xx:xx:xx" */
    memcpy(g_mac, "\x00\x11\x22\x33\x44\x55", 6); /* default placeholder */
    memcpy(g_adapter_info.mac, g_mac, 6);

    /* Read chiprev */
    uint32_t chiprev = 0;
    log_print(LOG_I, "chiprev=%u", chiprev);
    log_print(LOG_I, "macaddr=%02x:%02x:%02x:%02x:%02x:%02x",
              g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);

    /* Log WLAN driver info (stored in firmware as a string) */
    log_print(LOG_I, "%s: Broadcom SDPCMD CDC driver", "sdpcmd");
    log_print(LOG_I, "*unknown*/sdio-apsta-idsup-idauth-pno "
                     "Version: 221.1.11.0 CRC: 9cc022d9 "
                     "Date: Thu 2013-01-24 00:19:20 KST");

    return RET_OK;
}

/* ------------------------------------------------------------------ */
/*  WiFi connect to infrastructure network                           */
/* ------------------------------------------------------------------ */
int wifi_connect(const char *ssid, const char *key, int security) {
    if (!ssid || ssid[0] == '\0') {
        log_print(LOG_E, "DUMMY_SSID");
        return RET_PARAM_NG;
    }

    log_print(LOG_I, "ptn2:%s", ssid);

    /* Set security mode */
    int wpa_auth = 0;
    switch (security) {
    case WIFI_SEC_NONE:  wpa_auth = 0x00; break;
    case WIFI_SEC_WEP:   wpa_auth = 0x01; break;
    case WIFI_SEC_WPA:   wpa_auth = 0x22; break;
    case WIFI_SEC_WPA2:  wpa_auth = 0x80; break; /* sup_wpa2_eapver */
    }
    log_print(LOG_V, "wpa_auth=%d", wpa_auth);

    /* Configure SSID and passphrase in Broadcom chip via SDIO ioctl */
    /* bcm_iovar_setbuf("ssid", ...) */
    /* bcm_iovar_set("auth", wpa_auth) */
    /* bcm_iovar_setbuf("wsec_key", key, strlen(key)) */

    /* Association request */
    /* bcm_ioctl(WLC_SET_SSID, &join_params) */

    memcpy(g_wifi_cfg.ssid, ssid, WIFI_SSID_MAX - 1);
    if (key) memcpy(g_wifi_cfg.key, key, WIFI_KEY_MAX - 1);
    g_wifi_cfg.security = security;

    return RET_OK;
}

/* ------------------------------------------------------------------ */
/*  Start SoftAP (access point mode) for provisioning                */
/* ------------------------------------------------------------------ */
int wifi_start_ap(const char *ssid) {
    log_print(LOG_I, "ptn3:%s", ssid);

    /* ap_channel = 1, security = WPA2 */
    /* bcm_iovar_set("apsta", 1) */
    /* bcm_ioctl(WLC_SET_SSID, ap_ssid) */
    /* bcm_iovar_set("wsec", WPA2_AUTH_PSK) */

    log_print(LOG_I, "macaddr=%s", DEFAULT_MAC);
    return RET_OK;
}

/* ------------------------------------------------------------------ */
/*  Get current IP address                                            */
/* ------------------------------------------------------------------ */
int wifi_get_ip(uint32_t *ip, uint32_t *gw) {
    if (ip) *ip = g_wifi_cfg.ip;
    if (gw) *gw = g_wifi_cfg.gw;
    return RET_OK;
}

/* ------------------------------------------------------------------ */
/*  DHCP client                                                       */
/* ------------------------------------------------------------------ */
int dhcp_start(void) {
    log_print(LOG_V, " DHCPsLC");
    log_print(LOG_V, "DHCPs");

    /* Send DHCP DISCOVER -> OFFER -> REQUEST -> ACK */
    /* On success, update g_wifi_cfg.ip, .gw, .netmask */
    /* On failure, fall back to auto_ip (link-local 169.254.x.x) */

    g_wifi_cfg.link_up = 1;
    log_print(LOG_I, "notice_ip_int");

    return RET_OK;
}

/* ------------------------------------------------------------------ */
/*  UDP send/receive for DAIKIN_UDP protocol and ECHONET Lite        */
/* ------------------------------------------------------------------ */

/* UDP "DAIKIN_UDP/" command dispatcher */
void AppUdpCmdReceive(const uint8_t *buf, int len,
                      uint32_t src_ip, uint16_t src_port) {
    if (!buf || len < 10) return;

    log_print(LOG_V, "AppUdpCmdReceive");

    const char *cmd = (const char *)buf;

    /* Route to handler based on URI path */
    if (strncmp(cmd, "DAIKIN_UDP/common/basic_info", 28) == 0) {
        char resp[256];
        int rlen = common_basic_info(resp, sizeof(resp));
        AppUdpCmdSend(src_ip, src_port, (uint8_t *)resp, rlen);

    } else if (strncmp(cmd, "DAIKIN_UDP/debug/timeinfo", 25) == 0) {
        char resp[128];
        int rlen = common_get_datetime(resp, sizeof(resp));
        AppUdpCmdSend(src_ip, src_port, (uint8_t *)resp, rlen);

    } else if (strncmp(cmd, "DAIKIN_UDP/debug/s_debug_on", 27) == 0) {
        log_print(LOG_I, "Demand Debug Log is ON");
        AppUdpCmdSend(src_ip, src_port, (uint8_t *)"OK", 2);

    } else if (strncmp(cmd, "DAIKIN_UDP/debug/s_debug_off", 28) == 0) {
        log_print(LOG_I, "Demand Debug Log is OFF");
        AppUdpCmdSend(src_ip, src_port, (uint8_t *)"OK", 2);

    } else if (strncmp(cmd, "DAIKIN_UDP/debug/myconsole", 26) == 0) {
        log_print(LOG_I, "MyConsoleOK");
        AppUdpCmdSend(src_ip, src_port, (uint8_t *)"OK", 2);

    } else if (strncmp(cmd, "DAIKIN_UDP/debug/loglevel=e", 27) == 0) {
        /* Set log level: errors only */
        AppUdpCmdSend(src_ip, src_port, (uint8_t *)"OK", 2);

    } else if (strncmp(cmd, "DAIKIN_UDP/debug/loglevel=i", 27) == 0) {
        /* Set log level: info */
        AppUdpCmdSend(src_ip, src_port, (uint8_t *)"OK", 2);

    } else if (strncmp(cmd, "DAIKIN_UDP/debug/loglevel=v", 27) == 0) {
        /* Set log level: verbose */
        AppUdpCmdSend(src_ip, src_port, (uint8_t *)"OK", 2);

    } else if (strncmp(cmd, "DAIKIN_UDP/debug/demandON", 25) == 0) {
        g_demand.dmnd_run = 1;
        log_print(LOG_I, "Demand Debug Log is ON");
        AppUdpCmdSend(src_ip, src_port, (uint8_t *)"OK", 2);

    } else if (strncmp(cmd, "DAIKIN_UDP/debug/demandOFF", 26) == 0) {
        g_demand.dmnd_run = 0;
        log_print(LOG_I, "Demand Debug Log is OFF");
        AppUdpCmdSend(src_ip, src_port, (uint8_t *)"OK", 2);

    } else if (strncmp(cmd, "DAIKIN_UDP/debug/24h_info", 25) == 0) {
        cloud_post_24h_info();
        AppUdpCmdSend(src_ip, src_port, (uint8_t *)"OK", 2);

    } else {
        log_print(LOG_E, "INVALID URL");
        AppUdpCmdSend(src_ip, src_port,
                      (uint8_t *)"ret=PARAM NG", 12);
    }
}

void AppUdpCmdSend(uint32_t dst_ip, uint16_t dst_port,
                   const uint8_t *buf, int len) {
    log_print(LOG_V, " AppUdpCmdSend");
    /* Build UDP packet and send via TCP/IP stack / lwIP */
    (void)dst_ip; (void)dst_port; (void)buf; (void)len;
}
