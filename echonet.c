/**
 * echonet.c -- ECHONET Lite protocol implementation
 *
 * ECHONET Lite is a Japanese smart-home protocol used by Daikin for
 * local LAN communication. It runs over UDP port 3610.
 *
 * Key strings from firmware:
 *   "ECHONET Lite software version: %s"  (0x08009E94)
 *   "epc=%02x, pdc=%02x, edt=%02x, err=%d"  (0x080151C3)
 *   "DAIKIN_UDP/common/basic_info"  (0x08042FCC)
 *
 * ECHONET Lite frame structure:
 *   EHD1  = 0x10 (ECHONET Lite)
 *   EHD2  = 0x81 (Format 1)
 *   TID   = 2 bytes transaction ID
 *   SEOJ  = 3 bytes source object (class group, class, instance)
 *   DEOJ  = 3 bytes destination object
 *   ESV   = 1 byte service code
 *   OPC   = 1 byte property count
 *   [EPC + PDC + EDT] ? OPC
 *
 * Daikin AC is ECHONET class 0x01 0x30 (Home Air Conditioner).
 *
 * Key ESV codes:
 *   0x62 = Get (read request)
 *   0x72 = Get_Res (read response)
 *   0x61 = SetC (write request with response)
 *   0x71 = Set_Res (write response)
 *   0x60 = SetI (write request without response)
 *   0x73 = INF (notification)
 *   0x63 = Get_SNA (Get not allowed)
 *
 * Key EPC codes for class 0x0130 (Air Conditioner):
 *   0x80 = Operation status (on/off)
 *   0xA0 = Airflow rate setting
 *   0xA3 = Airflow direction vertical
 *   0xB0 = Operation mode setting
 *   0xB3 = Temperature setting
 *   0xBB = Measured room temperature
 *   0xBE = Measured outdoor temperature
 *   0xCA = Special state
 *   0x8F = Current date/time
 *   0x9D = Status change announcement property map
 *   0x9E = Set property map
 *   0x9F = Get property map
 */

#include "daikin.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/*  ECHONET Lite constants                                            */
/* ------------------------------------------------------------------ */
#define ECHONET_PORT        3610

#define EHD1                0x10
#define EHD2                0x81

/* Service codes */
#define ESV_SETI            0x60  /* Set, no response */
#define ESV_SETC            0x61  /* Set, response required */
#define ESV_GET             0x62  /* Get */
#define ESV_INF_REQ         0x63  /* INF request */
#define ESV_SET_RES         0x71  /* Set response */
#define ESV_GET_RES         0x72  /* Get response */
#define ESV_INF             0x73  /* Notification */
#define ESV_SETGET          0x6E  /* Set+Get */
#define ESV_SETGET_RES      0x7E  /* Set+Get response */
#define ESV_SET_SNA         0x51  /* Set not accepted */
#define ESV_GET_SNA         0x52  /* Get not accepted */
#define ESV_INF_SNA         0x53  /* INF not accepted */

/* EPC codes for Home Air Conditioner (class 0x0130) */
#define EPC_OPERATION_STATUS     0x80
#define EPC_INSTALLATION_LOC     0x81
#define EPC_FAULT_STATUS         0x88
#define EPC_FAULT_CODE           0x89
#define EPC_CURRENT_DATETIME     0x97
#define EPC_STATUS_MAP           0x9D
#define EPC_SET_MAP              0x9E
#define EPC_GET_MAP              0x9F
#define EPC_AIRFLOW_RATE         0xA0
#define EPC_AIRFLOW_DIR_V        0xA3
#define EPC_SPECIAL_STATE        0xCA
#define EPC_OPERATION_MODE       0xB0
#define EPC_TEMP_SETTING         0xB3
#define EPC_ROOM_TEMP            0xBB
#define EPC_OUTDOOR_TEMP         0xBE
#define EPC_HUMIDITY_SETTING     0xB4

/* ECHONET object classes */
#define ECHONET_NODE_PROFILE     0x0EF0  /* Node Profile class */
#define ECHONET_AC_CLASS         0x0130  /* Air Conditioner    */

/* Multicast address: 224.0.23.0 */
#define ECHONET_MULTICAST 0xE0001700UL

/* ------------------------------------------------------------------ */
/*  Frame builder                                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t ehd1;
    uint8_t ehd2;
    uint8_t tid[2];
    uint8_t seoj[3];
    uint8_t deoj[3];
    uint8_t esv;
    uint8_t opc;
    uint8_t props[128];   /* packed EPC + PDC + EDT */
    int     props_len;
} echonet_frame_t;

static uint16_t g_tid = 0;

static int echonet_build_frame(echonet_frame_t *f, uint8_t *out, int out_len) {
    if (out_len < 12 + f->props_len) return RET_NG;

    out[0] = EHD1;
    out[1] = EHD2;
    out[2] = (f->tid[0]);
    out[3] = (f->tid[1]);
    memcpy(out + 4,  f->seoj, 3);
    memcpy(out + 7,  f->deoj, 3);
    out[10] = f->esv;
    out[11] = f->opc;
    memcpy(out + 12, f->props, f->props_len);

    return 12 + f->props_len;
}

/* ------------------------------------------------------------------ */
/*  echonet_init                                                      */
/* ------------------------------------------------------------------ */
void echonet_init(void) {
    char ver[16];
    snprintf(ver, sizeof(ver), "%d.%d.%d",
             FW_VER_MAJOR, FW_VER_MINOR, FW_VER_PATCH);
    log_print(LOG_I, "ECHONET Lite software version: %s", ver);

    /* Open UDP socket on port 3610, join multicast group 224.0.23.0 */
}

/* ------------------------------------------------------------------ */
/*  echonet_task -- receive and dispatch ECHONET Lite frames           */
/* ------------------------------------------------------------------ */
void echonet_task(void *arg) {
    (void)arg;
    uint8_t buf[256];
    int     len;

    for (;;) {
        /* recvfrom(sock, buf, sizeof(buf), &src_ip, &src_port) */
        len = 0;

        if (len > 0) {
            echonet_process_command(buf, len);
        }

        delay_ms(10);
    }
}

/* ------------------------------------------------------------------ */
/*  echonet_process_command -- parse and respond to one frame         */
/* ------------------------------------------------------------------ */
int echonet_process_command(const uint8_t *frame, int len) {
    if (len < 12) return RET_PARAM_NG;
    if (frame[0] != EHD1 || frame[1] != EHD2) return RET_PARAM_NG;

    uint8_t esv   = frame[10];
    uint8_t opc   = frame[11];
    /* SEOJ and DEOJ */
    /* uint16_t seoj_class = (frame[4] << 8) | frame[5]; */
    uint16_t deoj_class = (frame[7] << 8) | frame[8];

    if (deoj_class != ECHONET_AC_CLASS &&
        deoj_class != ECHONET_NODE_PROFILE) {
        return RET_PARAM_NG;
    }

    uint8_t resp_props[128];
    int     resp_props_len = 0;

    const uint8_t *p = frame + 12;

    for (int i = 0; i < opc; i++) {
        uint8_t epc = *p++;
        uint8_t pdc = *p++;

        int err = 0;

        if (esv == ESV_GET || esv == ESV_SETGET) {
            /* Build response property */
            resp_props[resp_props_len++] = epc;

            switch (epc) {
            case EPC_OPERATION_STATUS:
                resp_props[resp_props_len++] = 1;
                resp_props[resp_props_len++] =
                    (g_ac_state.pow == AC_POW_ON) ? 0x30 : 0x31;
                break;

            case EPC_OPERATION_MODE:
                resp_props[resp_props_len++] = 1;
                {
                    uint8_t mode;
                    switch (g_ac_state.mode) {
                    case AC_MODE_AUTO: mode = 0x41; break;
                    case AC_MODE_COOL: mode = 0x42; break;
                    case AC_MODE_HEAT: mode = 0x43; break;
                    case AC_MODE_DRY:  mode = 0x44; break;
                    case AC_MODE_FAN:  mode = 0x45; break;
                    default:           mode = 0x41; break;
                    }
                    resp_props[resp_props_len++] = mode;
                }
                break;

            case EPC_TEMP_SETTING:
                resp_props[resp_props_len++] = 1;
                resp_props[resp_props_len++] =
                    (uint8_t)((int)atof(g_ac_state.stemp));
                break;

            case EPC_ROOM_TEMP:
                resp_props[resp_props_len++] = 1;
                {
                    int t = (g_ac_state.htemp[0] == '-') ? 0x7F :
                            (int)atof(g_ac_state.htemp);
                    resp_props[resp_props_len++] = (uint8_t)t;
                }
                break;

            case EPC_OUTDOOR_TEMP:
                resp_props[resp_props_len++] = 1;
                {
                    int t = (g_ac_state.otemp[0] == '-') ? 0x7F :
                            (int)atof(g_ac_state.otemp);
                    resp_props[resp_props_len++] = (uint8_t)t;
                }
                break;

            case EPC_FAULT_STATUS:
                resp_props[resp_props_len++] = 1;
                resp_props[resp_props_len++] =
                    (g_ac_state.err != 0) ? 0x41 : 0x42;
                break;

            case EPC_AIRFLOW_RATE:
                resp_props[resp_props_len++] = 1;
                resp_props[resp_props_len++] =
                    (g_ac_state.f_rate == AC_FRATE_AUTO) ? 0x41 :
                    (uint8_t)(g_ac_state.f_rate - '0' + 0x31);
                break;

            default:
                /* Property not supported */
                resp_props[resp_props_len++] = 0;  /* PDC=0 -> SNA */
                err = 1;
                break;
            }

            log_print(LOG_V, " epc=%02x, pdc=%02x, edt=%02x, err=%d",
                      epc, 1, resp_props[resp_props_len - 1], err);

        } else if (esv == ESV_SETC || esv == ESV_SETI) {
            /* Write property */
            const uint8_t *edt = p;
            p += pdc;

            switch (epc) {
            case EPC_OPERATION_STATUS:
                g_ac_state.pow = (edt[0] == 0x30) ? AC_POW_ON : AC_POW_OFF;
                break;
            case EPC_OPERATION_MODE:
                switch (edt[0]) {
                case 0x41: g_ac_state.mode = AC_MODE_AUTO; break;
                case 0x42: g_ac_state.mode = AC_MODE_COOL; break;
                case 0x43: g_ac_state.mode = AC_MODE_HEAT; break;
                case 0x44: g_ac_state.mode = AC_MODE_DRY;  break;
                case 0x45: g_ac_state.mode = AC_MODE_FAN;  break;
                }
                break;
            case EPC_TEMP_SETTING:
                snprintf(g_ac_state.stemp, sizeof(g_ac_state.stemp),
                         "%d", (int)edt[0]);
                break;
            default:
                break;
            }

            if (esv == ESV_SETC) {
                resp_props[resp_props_len++] = epc;
                resp_props[resp_props_len++] = 0;
            }
        }
    }

    /* Send response frame */
    uint8_t resp_buf[256];
    echonet_frame_t resp = {
        .ehd1 = EHD1, .ehd2 = EHD2,
        .tid  = { frame[2], frame[3] },
        .seoj = { 0x01, 0x30, 0x01 },   /* AC class */
        .deoj = { frame[4], frame[5], frame[6] },
        .esv  = (esv == ESV_GET)  ? ESV_GET_RES :
                (esv == ESV_SETC) ? ESV_SET_RES : ESV_INF,
        .opc  = opc,
        .props_len = resp_props_len,
    };
    memcpy(resp.props, resp_props, resp_props_len);

    int resp_len = echonet_build_frame(&resp, resp_buf, sizeof(resp_buf));
    /* sendto(udp_sock, resp_buf, resp_len, src_ip, src_port) */
    (void)resp_len;

    return RET_OK;
}

/* ------------------------------------------------------------------ */
/*  echonet_send_basic_info -- unsolicited INF to multicast            */
/* ------------------------------------------------------------------ */
int echonet_send_basic_info(uint32_t dst_ip) {
    uint8_t buf[64];
    echonet_frame_t f = {
        .ehd1 = EHD1, .ehd2 = EHD2,
        .tid  = { (uint8_t)(g_tid >> 8), (uint8_t)g_tid },
        .seoj = { 0x01, 0x30, 0x01 },
        .deoj = { 0x0E, 0xF0, 0x01 },   /* Node Profile */
        .esv  = ESV_INF,
        .opc  = 1,
        .props_len = 0,
    };
    g_tid++;

    /* EPC 0xD5: instance list notification */
    f.props[f.props_len++] = 0xD5;
    f.props[f.props_len++] = 4;
    f.props[f.props_len++] = 1;    /* one instance */
    f.props[f.props_len++] = 0x01;
    f.props[f.props_len++] = 0x30;
    f.props[f.props_len++] = 0x01;
    f.opc = 1;

    int len = echonet_build_frame(&f, buf, sizeof(buf));
    (void)dst_ip;
    /* sendto(udp_sock, buf, len, dst_ip ? dst_ip : ECHONET_MULTICAST, ECHONET_PORT) */
    (void)len;

    return RET_OK;
}
