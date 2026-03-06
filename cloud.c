/**
 * cloud.c -- Daikin cloud HTTPS client
 *
 * The adapter periodically polls and POSTs to the Daikin cloud service.
 * Communication is HTTPS (TLS 1.0/1.1) authenticated with a GlobalSign
 * root CA certificate (found embedded at 0x080770E0).
 *
 * Key strings:
 *   "sha2.daikinonlinecontroller.com"    -- primary cloud host
 *   "test2.daikindev.com"                -- dev/staging host
 *   "www.daikincloud.net"                -- international host
 *   "daikinsmartdb.jp"                   -- Japan legacy
 *   "BBE, GlobalSign nv-sa, Root CA, GlobalSign Root CA"  -- TLS CA
 *   "server finished"                    -- TLS handshake completion
 *   "key expansion"                      -- TLS PRF label
 *   "Wi-Fi Easy and Secure Key Derivation" -- WPS key derivation string
 *   "[err]HTTPc-BadSig"                  -- HTTPS signature error
 *   "[err]HTTPc-UnknownSigAlg"           -- unknown TLS sig algorithm
 *   "[err]HTTPc-UnknownPKAlg"            -- unknown TLS PK algorithm
 *   "XP Post Result = %s"                -- cloud POST result log
 *   "/aircon/notice"                     -- notice endpoint
 *   "/aircon/error_notice"               -- error notice endpoint
 *   "&err_info="                         -- error info param
 *   "&ver="                              -- firmware version param
 *   "&notice_ip_int="                    -- IP notice interval param
 *   "&en_hol=%d"                         -- holiday setting param
 *   "&price="                            -- price setting param
 *   "DAIKIN_UDP/debug/24h_info"          -- 24-hour statistics
 *
 * Cloud POST body format (application/x-www-form-urlencoded):
 *   ver=3.4.3&mac=XXXXXXXXXXXX&notice_ip_int=3600&...
 *
 * The "polllog" string (0x08008370) suggests the cloud task polls
 * on a configurable interval (default: notice_sync_int seconds).
 *
 * TLS implementation: custom, using embedded TLS stack with:
 *   - SHA-256 for cert verification ("sha2" in server name)
 *   - AES-128-CBC bulk cipher (inferred from key expansion size)
 *   - RSA key exchange
 */

#include "daikin.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Cloud task state                                                  */
/* ------------------------------------------------------------------ */
#define CLOUD_TIMEOUT_MS    30000
#define CLOUD_PORT          443   /* HTTPS */
#define CLOUD_PORT_HTTP     80    /* HTTP fallback */

static int g_cloud_poll_tick = 0;
static int g_router_disconn  = 0;
static int g_polling_err     = 0;
static int g_err_nopost_flg  = 0;

static int cloud_post_notice(void);  /* forward declaration */

/* ------------------------------------------------------------------ */
/*  cloud_task -- main cloud polling loop                              */
/* ------------------------------------------------------------------ */
void cloud_task(void *arg) {
    (void)arg;

    log_print(LOG_I, "polllog");

    for (;;) {
        /* Sync time via NTP if needed */
        if ((g_cloud_poll_tick % 3600) == 0) {
            cloud_sync_datetime();
        }

        /* POST notice every notice_ip_int seconds */
        /* (actual interval set via common/set_remote_method) */
        if (g_wifi_cfg.link_up) {
            int ret = cloud_post_notice();
            if (ret != RET_OK) {
                g_polling_err++;
                log_print(LOG_E, " httpc communication error");
            } else {
                log_print(LOG_I, "XP Post Result = %s", "OK");
            }
        } else {
            g_router_disconn++;
        }

        /* Energy monitoring -- post 24h data once per day */
        if ((g_cloud_poll_tick % 86400) == 0) {
            cloud_post_24h_info();
        }

        log_print(LOG_V,
            ",RouterDisconCnt=%lu,PollingErrCnt=%lu",
            (unsigned long)g_router_disconn,
            (unsigned long)g_polling_err);

        g_cloud_poll_tick++;
        delay_ms(1000);
    }
}

/* ------------------------------------------------------------------ */
/*  cloud_post_notice -- POST current AC state to cloud               */
/* ------------------------------------------------------------------ */
static int cloud_post_notice(void) {
    char body[512];
    adapter_info_t *ai = &g_adapter_info;
    ac_state_t     *ac = &g_ac_state;

    /* Build POST body */
    int len = snprintf(body, sizeof(body),
        "ver=%d.%d.%d"
        "&mac=%02x%02x%02x%02x%02x%02x"
        "&notice_ip_int=%d"
        "&en_hol=%d"
        "&err_info=%d",
        FW_VER_MAJOR, FW_VER_MINOR, FW_VER_PATCH,
        ai->mac[0], ai->mac[1], ai->mac[2],
        ai->mac[3], ai->mac[4], ai->mac[5],
        3600,   /* notice_ip_int */
        ai->en_hol,
        ac->err);

    log_print(LOG_V, " POST %s", "/aircon/notice");

    /* HTTPS POST to cloud server */
    return https_post("/aircon/notice", body, len);
}

/* ------------------------------------------------------------------ */
/*  cloud_post_error_notice                                           */
/* ------------------------------------------------------------------ */
int cloud_post_error_notice(int err_code, const char *info) {
    if (g_err_nopost_flg) {
        log_print(LOG_V, "err_nopost_flg");
        return RET_OK;
    }

    char body[256];
    int len = snprintf(body, sizeof(body),
        "&err_info=%s&err=%d",
        info ? info : "", err_code);

    log_print(LOG_I, "/aircon/error_notice");
    log_print(LOG_I, " Error History Post #S");

    int ret = https_post("/aircon/error_notice", body, len);

    log_print(LOG_I, " Error History Post #E");
    log_print(LOG_I, "ErrorNotice Result=%d", ret);

    return ret;
}

/* ------------------------------------------------------------------ */
/*  cloud_post_24h_info -- energy statistics                          */
/* ------------------------------------------------------------------ */
int cloud_post_24h_info(void) {
    char body[256];
    int len = snprintf(body, sizeof(body),
        "ver=%d.%d.%d&kind=24h",
        FW_VER_MAJOR, FW_VER_MINOR, FW_VER_PATCH);

    log_print(LOG_I, "DAIKIN_UDP/debug/24h_info");
    return https_post("/aircon/24h_info", body, len);
}

/* ------------------------------------------------------------------ */
/*  cloud_sync_datetime -- retrieve time from cloud / NTP             */
/* ------------------------------------------------------------------ */
int cloud_sync_datetime(void) {
    char buf[64];
    int ret = https_get("/common/get_date", buf, sizeof(buf));
    if (ret != RET_OK) {
        log_print(LOG_E, "[err]t sync");
        return ret;
    }
    log_print(LOG_V, "/common/get_date");
    /* Parse ret=OK,YYYY/MM/DD HH:MM:SS and call common_notify_datetime */
    return RET_OK;
}

/* ------------------------------------------------------------------ */
/*  https_post / https_get -- TLS HTTP client                         */
/*                                                                    */
/*  The firmware implements a compact TLS 1.0 client. Key strings:  */
/*    "server finished"   -- PRF label for Finished message           */
/*    "key expansion"     -- PRF label for key material derivation    */
/*    "[err]HTTPc-BadSig" -- certificate signature verification fail  */
/*    "GlobalSign Root CA" -- embedded trust anchor                   */
/*    "%s:80"             -- HTTP fallback URL format                 */
/* ------------------------------------------------------------------ */
int https_post(const char *path, const char *body, int body_len) {
    const char *host = cloud_get_server_name();
    char request[1024];
    int req_len;

    req_len = snprintf(request, sizeof(request),
        "POST /%s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s",
        path, host, body_len, body);

    /* Open TLS connection */
    /* tls_connect(host, CLOUD_PORT) */

    /* TLS handshake uses:
     *   - RSA key exchange
     *   - AES-128-CBC cipher
     *   - SHA-1 / SHA-256 MAC
     *   - GlobalSign Root CA for server cert verification
     */

    /* Send request and receive response */
    /* ret = tls_send(request, req_len) */
    /* ret = tls_recv(response, sizeof(response)) */

    log_print(LOG_V, " POST %s", path);
    (void)req_len;

    /* Parse response */
    /* if (strncmp(response, "HTTP/1.0 200", 12) == 0) -> OK */

    return RET_OK;
}

int https_get(const char *path, char *buf, int buf_len) {
    const char *host = cloud_get_server_name();
    char request[256];

    snprintf(request, sizeof(request),
        "GET /%s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "\r\n",
        path, host);

    /* tls_connect, send, recv ... */
    (void)buf; (void)buf_len;

    return RET_OK;
}
