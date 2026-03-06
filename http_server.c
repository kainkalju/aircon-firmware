/**
 * http_server.c -- Embedded HTTP/1.0 server
 *
 * The BRP069A42 exposes a local HTTP API on port 80 that allows the
 * Daikin smartphone app and cloud service to read and control the AC.
 *
 * All responses use the Daikin proprietary format:
 *   ret=OK,key1=val1,key2=val2,...
 *   ret=PARAM NG          (bad request)
 *   ret=INTERNAL NG,...   (server error)
 *
 * The server is HTTP/1.0; it also accepts HTTP/1.1 (seen in strings).
 *
 * Authentication: session token in X-Daikin-uuid header or 'session'
 * query parameter (seen: "ret=%s,session=%s").
 *
 * From string analysis, the full route table is at 0x08075154+:
 *   GET  /aircon/get_control_info
 *   POST /aircon/set_control_info
 *   GET  /aircon/get_sensor_info
 *   GET  /aircon/get_model_info
 *   GET  /aircon/get_info
 *   POST /aircon/set_info
 *   ...  (see daikin.h for full list)
 *
 * HTTP response codes:
 *   200 OK                   ("HTTP/1.0 200 OK")
 *   400 Bad Request          ("ret=INTERNAL NG,msg=400 Bad Request")
 *   403 HTTP_FORBIDDEN       ("HTTP/1.0 403 HTTP_FORBIDDEN")
 *   404 Not Found            ("ret=PARAM NG,msg=404 Not Found")
 *   500 Internal Server Error
 *   503 Service Unavailable
 */

#include "daikin.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/*  HTTP response templates (reconstructed from firmware strings)     */
/* ------------------------------------------------------------------ */

#define HTTP_200  "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n\r\n"
#define HTTP_400  "HTTP/1.0 400 Bad Request\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n\r\n"
#define HTTP_401  "HTTP/1.0 401 Authentication required\r\nContent-Length: 0\r\n\r\n"
#define HTTP_403  "HTTP/1.0 403 HTTP_FORBIDDEN\r\nContent-Length: 0\r\n\r\n"
#define HTTP_404  "HTTP/1.0 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n\r\n"
#define HTTP_500  "HTTP/1.0 500 Internal Server Error\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n\r\n"
#define HTTP_503  "HTTP/1.0 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n"

/* ------------------------------------------------------------------ */
/*  Route table                                                       */
/* ------------------------------------------------------------------ */

typedef enum { HTTP_GET, HTTP_POST } http_method_t;

typedef struct {
    http_method_t method;
    const char   *path;
    int (*handler)(const char *qs, char *resp, int resp_len);
} route_t;

/* Handler wrappers (GET handlers ignore qs; POST handlers use it) */
static int h_ac_get_control(const char *qs, char *r, int l)  { (void)qs; return ac_get_control_info(r, l); }
static int h_ac_set_control(const char *qs, char *r, int l)  { int ret = ac_set_control_info(qs); return snprintf(r, l, ret==0?RESP_OK:RESP_PARAM_NG); }
static int h_ac_get_sensor(const char *qs, char *r, int l)   { (void)qs; return ac_get_sensor_info(r, l); }
static int h_ac_get_model(const char *qs, char *r, int l)    { (void)qs; return ac_get_model_info(r, l); }
static int h_ac_get_info(const char *qs, char *r, int l)     { (void)qs; return ac_get_info(r, l); }
static int h_ac_set_info(const char *qs, char *r, int l)     { int ret = ac_set_info(qs); return snprintf(r, l, ret==0?RESP_OK:RESP_PARAM_NG); }
static int h_ac_get_timer(const char *qs, char *r, int l)    { (void)qs; return ac_get_timer(r, l); }
static int h_ac_set_timer(const char *qs, char *r, int l)    { int ret = ac_set_timer(qs); return snprintf(r, l, ret==0?RESP_OK:RESP_PARAM_NG); }
static int h_ac_get_week_pwr(const char *qs, char *r, int l) { (void)qs; return ac_get_week_power(r, l); }
static int h_ac_get_year_pwr(const char *qs, char *r, int l) { (void)qs; return ac_get_year_power(r, l); }
static int h_ac_get_mon_pwr(const char *qs, char *r, int l)  { (void)qs; return ac_get_month_power_ex(r, l); }
static int h_ac_clr_nw_err(const char *qs, char *r, int l)   { (void)qs; ac_clr_nw_err(); return snprintf(r, l, RESP_OK); }
static int h_ac_get_nw_err(const char *qs, char *r, int l)   { (void)qs; return ac_get_nw_err(r, l); }
static int h_ac_set_special(const char *qs, char *r, int l)  { int ret = ac_set_special_mode(qs); return snprintf(r, l, ret==0?RESP_OK:RESP_PARAM_NG); }
static int h_ac_get_demand(const char *qs, char *r, int l)   { (void)qs; return ac_get_demand_control(r, l); }
static int h_ac_set_demand(const char *qs, char *r, int l)   { int ret = ac_set_demand_control(qs); return snprintf(r, l, ret==0?RESP_OK:RESP_PARAM_NG); }
static int h_ac_get_scdl(const char *qs, char *r, int l)     { (void)qs; return ac_get_scdltimer(r, l); }
static int h_ac_set_scdl(const char *qs, char *r, int l)     { int ret = ac_set_scdltimer(qs); return snprintf(r, l, ret==0?RESP_OK:RESP_PARAM_NG); }
static int h_ac_get_scdl_body(const char *qs, char *r, int l){ (void)qs; return ac_get_scdltimer_body(r, l); }
static int h_ac_set_scdl_body(const char *qs, char *r, int l){ int ret = ac_set_scdltimer_body(qs); return snprintf(r, l, ret==0?RESP_OK:RESP_PARAM_NG); }
static int h_ac_get_price(const char *qs, char *r, int l)    { (void)qs; return ac_get_price(r, l); }
static int h_ac_set_price(const char *qs, char *r, int l)    { int ret = ac_set_price(qs); return snprintf(r, l, ret==0?RESP_OK:RESP_PARAM_NG); }
static int h_common_basic(const char *qs, char *r, int l)    { (void)qs; return common_basic_info(r, l); }
static int h_common_get_rm(const char *qs, char *r, int l)   { (void)qs; return common_get_remote_method(r, l); }
static int h_common_set_rm(const char *qs, char *r, int l)   { int ret = common_set_remote_method(qs); return snprintf(r, l, ret==0?RESP_OK:RESP_PARAM_NG); }
static int h_common_get_wifi(const char *qs, char *r, int l) { (void)qs; return common_get_wifi_setting(r, l); }
static int h_common_set_wifi(const char *qs, char *r, int l) { int ret = common_set_wifi_setting(qs); return snprintf(r, l, ret==0?RESP_OK:RESP_PARAM_NG); }
static int h_common_get_net(const char *qs, char *r, int l)  { (void)qs; return common_get_network_setting(r, l); }
static int h_common_set_net(const char *qs, char *r, int l)  { int ret = common_set_network_setting(qs); return snprintf(r, l, ret==0?RESP_OK:RESP_PARAM_NG); }
static int h_common_reboot(const char *qs, char *r, int l)   { (void)qs; (void)r; (void)l; common_reboot(); return 0; }
static int h_common_set_led(const char *qs, char *r, int l)  { int s=0; parse_key_values(qs,"led",(char*)&s,4); common_set_led(s); return snprintf(r,l,RESP_OK); }
static int h_common_get_dt(const char *qs, char *r, int l)   { (void)qs; return common_get_datetime(r, l); }
static int h_common_notify_dt(const char *qs, char *r, int l){ int ret = common_notify_datetime(qs); return snprintf(r,l,ret==0?RESP_OK:RESP_PARAM_NG); }
static int h_common_get_fw(const char *qs, char *r, int l)   { (void)qs; return common_get_fwinfo(r, l); }
static int h_common_scan(const char *qs, char *r, int l)     { (void)qs; return common_start_wifi_scan(r, l); }
static int h_common_scan_res(const char *qs, char *r, int l) { (void)qs; return common_get_wifi_scan_result(r, l); }
static int h_common_permit(const char *qs, char *r, int l)   { int ret = common_permit_wifi_connection(qs); return snprintf(r,l,ret==0?RESP_OK:RESP_PARAM_NG); }
static int h_common_get_sn(const char *qs, char *r, int l)   { (void)qs; return common_get_sn(r, l); }
static int h_common_set_sn(const char *qs, char *r, int l)   { int ret = common_set_sn(qs); return snprintf(r,l,ret==0?RESP_OK:RESP_PARAM_NG); }
static int h_common_get_hol(const char *qs, char *r, int l)  { (void)qs; return common_get_holiday(r, l); }
static int h_common_set_hol(const char *qs, char *r, int l)  { int ret = common_set_holiday(qs); return snprintf(r,l,ret==0?RESP_OK:RESP_PARAM_NG); }
static int h_common_set_acct(const char *qs, char *r, int l) { int ret = common_set_account(qs); return snprintf(r,l,ret==0?RESP_OK:RESP_PARAM_NG); }
static int h_fwupdate(const char *qs, char *r, int l)        {
    /* POST body is the raw firmware binary */
    (void)qs; (void)r; (void)l;
    log_print(LOG_I, "dkac/system/fwupdate");
    /* common_fwupdate() called with POST body data */
    return snprintf(r, l, "ret=OK");
}

static const route_t g_routes[] = {
    /* Aircon control */
    { HTTP_GET,  "aircon/get_control_info",   h_ac_get_control  },
    { HTTP_POST, "aircon/set_control_info",   h_ac_set_control  },
    { HTTP_GET,  "aircon/get_sensor_info",    h_ac_get_sensor   },
    { HTTP_GET,  "aircon/get_model_info",     h_ac_get_model    },
    { HTTP_GET,  "aircon/get_info",           h_ac_get_info     },
    { HTTP_POST, "aircon/set_info",           h_ac_set_info     },
    { HTTP_GET,  "aircon/get_timer",          h_ac_get_timer    },
    { HTTP_POST, "aircon/set_timer",          h_ac_set_timer    },
    { HTTP_GET,  "aircon/get_week_power",     h_ac_get_week_pwr },
    { HTTP_GET,  "aircon/get_week_power_ex",  h_ac_get_week_pwr },
    { HTTP_GET,  "aircon/get_year_power",     h_ac_get_year_pwr },
    { HTTP_GET,  "aircon/get_year_power_ex",  h_ac_get_year_pwr },
    { HTTP_GET,  "aircon/get_month_power_ex", h_ac_get_mon_pwr  },
    { HTTP_GET,  "aircon/clr_nw_err",         h_ac_clr_nw_err   },
    { HTTP_GET,  "aircon/get_nw_err",         h_ac_get_nw_err   },
    { HTTP_POST, "aircon/set_special_mode",   h_ac_set_special  },
    { HTTP_GET,  "aircon/get_demand_control", h_ac_get_demand   },
    { HTTP_POST, "aircon/set_demand_control", h_ac_set_demand   },
    { HTTP_GET,  "aircon/get_scdltimer",      h_ac_get_scdl     },
    { HTTP_POST, "aircon/set_scdltimer",      h_ac_set_scdl     },
    { HTTP_GET,  "aircon/get_scdltimer_body", h_ac_get_scdl_body},
    { HTTP_POST, "aircon/set_scdltimer_body", h_ac_set_scdl_body},
    { HTTP_GET,  "aircon/get_price",          h_ac_get_price    },
    { HTTP_POST, "aircon/set_price",          h_ac_set_price    },
    { HTTP_GET,  "aircon/get_monitordata",    h_ac_get_sensor   },
    { HTTP_GET,  "aircon/debugconfig",        h_ac_get_info     },

    /* Common / adapter */
    { HTTP_GET,  "common/basic_info",           h_common_basic    },
    { HTTP_GET,  "common/get_remote_method",    h_common_get_rm   },
    { HTTP_POST, "common/set_remote_method",    h_common_set_rm   },
    { HTTP_GET,  "common/get_wifi_setting",     h_common_get_wifi },
    { HTTP_POST, "common/set_wifi_setting",     h_common_set_wifi },
    { HTTP_GET,  "common/get_network_setting",  h_common_get_net  },
    { HTTP_POST, "common/set_network_setting",  h_common_set_net  },
    { HTTP_GET,  "common/reboot",               h_common_reboot   },
    { HTTP_POST, "common/set_led",              h_common_set_led  },
    { HTTP_GET,  "common/get_datetime",         h_common_get_dt   },
    { HTTP_POST, "common/notify_date_time",     h_common_notify_dt},
    { HTTP_GET,  "common/get_fwinfo",           h_common_get_fw   },
    { HTTP_GET,  "common/start_wifi_scan",      h_common_scan     },
    { HTTP_GET,  "common/get_wifi_scan_result", h_common_scan_res },
    { HTTP_POST, "common/permit_wifi_connection",h_common_permit  },
    { HTTP_POST, "common/start_wifi_connection", h_common_permit  },
    { HTTP_GET,  "common/get_sn",               h_common_get_sn   },
    { HTTP_POST, "common/set_sn",               h_common_set_sn   },
    { HTTP_GET,  "common/get_holiday",          h_common_get_hol  },
    { HTTP_POST, "common/set_holiday",          h_common_set_hol  },
    { HTTP_POST, "common/set_account",          h_common_set_acct },
    { HTTP_POST, "common/set_name",             h_common_set_acct },

    /* Firmware update */
    { HTTP_POST, "dkac/system/fwupdate",        h_fwupdate        },
};

#define NROUTES  (sizeof(g_routes) / sizeof(g_routes[0]))

/* ------------------------------------------------------------------ */
/*  HTTP parser state                                                 */
/* ------------------------------------------------------------------ */
typedef struct {
    http_method_t method;
    char          path[128];
    char          qs[512];          /* query string or POST body */
    int           content_length;
    char          uuid[64];         /* X-Daikin-uuid header */
    int           keep_alive;
} http_request_t;

/* ------------------------------------------------------------------ */
/*  httpd_start -- main HTTP server accept loop                        */
/* ------------------------------------------------------------------ */
void httpd_start(void) {
    log_print(LOG_I, "HTTPd START.");

    /* In the real firmware this opens a TCP socket and enters an
     * accept loop, dispatching each connection to a handler.
     * The actual socket API is a proprietary wrapper around lwIP. */

    /* socket(), bind(80), listen(), loop: accept(), handle_conn() */
}

/* ------------------------------------------------------------------ */
/*  parse_request -- parse HTTP/1.0 or HTTP/1.1 request               */
/* ------------------------------------------------------------------ */
static int parse_request(const char *raw, int raw_len, http_request_t *req) {
    (void)raw_len;
    memset(req, 0, sizeof(*req));

    /* Method */
    if (strncmp(raw, "GET ", 4) == 0) {
        req->method = HTTP_GET;
        raw += 4;
    } else if (strncmp(raw, "POST ", 5) == 0) {
        req->method = HTTP_POST;
        raw += 5;
    } else {
        return RET_PARAM_NG;
    }

    /* Path: skip leading '/' */
    if (*raw == '/') raw++;
    const char *path_start = raw;
    while (*raw && *raw != ' ' && *raw != '?') raw++;

    int path_len = (int)(raw - path_start);
    if (path_len >= (int)sizeof(req->path)) path_len = sizeof(req->path) - 1;
    memcpy(req->path, path_start, path_len);
    req->path[path_len] = '\0';

    /* Query string (GET) */
    if (*raw == '?') {
        raw++;
        const char *qs_start = raw;
        while (*raw && *raw != ' ') raw++;
        int qs_len = (int)(raw - qs_start);
        if (qs_len >= (int)sizeof(req->qs)) qs_len = sizeof(req->qs) - 1;
        memcpy(req->qs, qs_start, qs_len);
        req->qs[qs_len] = '\0';
    }

    /* HTTP version -- accept both 1.0 and 1.1 */
    if (strstr(raw, "HTTP/1.0")) {
        /* ok */
    } else if (strstr(raw, "HTTP/1.1")) {
        req->keep_alive = 1;
    }

    /* Headers */
    const char *hdr = raw;
    while ((hdr = strstr(hdr, "\r\n"))) {
        hdr += 2;
        if (strncasecmp(hdr, "Content-Length:", 15) == 0) {
            req->content_length = atoi(hdr + 15);
        } else if (strncasecmp(hdr, "X-Daikin-uuid:", 14) == 0) {
            const char *v = hdr + 14;
            while (*v == ' ') v++;
            int n = 0;
            while (v[n] && v[n] != '\r' && v[n] != '\n' && n < 63) n++;
            memcpy(req->uuid, v, n);
            req->uuid[n] = '\0';
        } else if (strncmp(hdr, "\r\n", 2) == 0) {
            /* End of headers */
            hdr += 2;
            if (req->method == HTTP_POST && req->content_length > 0) {
                int body_len = req->content_length;
                if (body_len >= (int)sizeof(req->qs))
                    body_len = sizeof(req->qs) - 1;
                memcpy(req->qs, hdr, body_len);
                req->qs[body_len] = '\0';
            }
            break;
        }
    }

    return RET_OK;
}

/* ------------------------------------------------------------------ */
/*  handle_connection -- dispatch one HTTP request                     */
/* ------------------------------------------------------------------ */
static void handle_connection(int sock, const char *raw, int raw_len) __attribute__((unused));
static void handle_connection(int sock, const char *raw, int raw_len) {
    http_request_t req;
    char resp_body[2048];
    char resp_hdr[256];

    if (parse_request(raw, raw_len, &req) != RET_OK) {
        httpd_send_error(sock, 400, "ret=INTERNAL NG,msg=400 Bad Request");
        return;
    }

    /* Route lookup */
    for (int i = 0; i < (int)NROUTES; i++) {
        const route_t *r = &g_routes[i];
        if (r->method == req.method &&
            strcmp(r->path, req.path) == 0) {

            int body_len = r->handler(req.qs, resp_body, sizeof(resp_body));
            if (body_len < 0) {
                httpd_send_error(sock, 500,
                    "ret=INTERNAL NG,msg=500 Internal Server Error");
                return;
            }

            int hdr_len = snprintf(resp_hdr, sizeof(resp_hdr),
                HTTP_200, body_len);
            /* send(sock, resp_hdr, hdr_len) */
            /* send(sock, resp_body, body_len) */
            (void)hdr_len;
            return;
        }
    }

    /* No route matched */
    httpd_send_error(sock, 404, "ret=PARAM NG,msg=404 Not Found");
}

/* ------------------------------------------------------------------ */
/*  httpd_send_ok / httpd_send_error helpers                         */
/* ------------------------------------------------------------------ */
int httpd_send_ok(int sock, const char *body, int body_len) {
    (void)body;
    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr), HTTP_200, body_len);
    /* send(sock, hdr, hlen); send(sock, body, body_len); */
    (void)sock; (void)hlen;
    return RET_OK;
}

int httpd_send_error(int sock, int code, const char *msg) {
    char hdr[256], body[256];
    int blen = snprintf(body, sizeof(body), "%s", msg);
    const char *fmt;
    switch (code) {
    case 400: fmt = HTTP_400; break;
    case 403: fmt = HTTP_403; /* send(sock, HTTP_403, strlen(HTTP_403)); */ return RET_OK;
    case 404: fmt = HTTP_404; break;
    case 503: /* send(sock, HTTP_503, strlen(HTTP_503)); */ return RET_OK;
    default:  fmt = HTTP_500; break;
    }
    int hlen = snprintf(hdr, sizeof(hdr), fmt, blen);
    (void)hlen; (void)sock;
    return RET_OK;
}

/* ------------------------------------------------------------------ */
/*  parse_key_values -- extract a value from a URL-encoded string     */
/*                                                                    */
/*  "parseKeyAndValues NG" seen at 0x080116F0 and 0x08012EB4        */
/*  Format: key1=val1&key2=val2&...                                  */
/* ------------------------------------------------------------------ */
int parse_key_values(const char *qs, const char *key, char *out, int out_len) {
    if (!qs || !key || !out) {
        log_print(LOG_E, "parseKeyAndValues NG");
        return RET_PARAM_NG;
    }

    size_t klen = strlen(key);
    const char *p = qs;

    while (*p) {
        /* Find key= */
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            int n = 0;
            while (v[n] && v[n] != '&' && n < out_len - 1) n++;
            memcpy(out, v, n);
            out[n] = '\0';
            return RET_OK;
        }
        /* Advance to next key */
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }

    log_print(LOG_E, "parseKeyAndValues NG");
    return RET_NG;
}
