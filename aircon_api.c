/**
 * aircon_api.c — Aircon HTTP API handlers
 *
 * All response strings reconstructed verbatim from firmware strings:
 *
 *   get_control_info response (0x08075AB8):
 *     ret=%s,pow=%c,mode=%s,adv=%s,stemp=%s,shum=%s,
 *     dt1=%s,...,b_mode=%s,b_stemp=%s,b_shum=%s,alert=%d
 *
 *   get_sensor_info response (0x0800B328):
 *     ret=%s,htemp=%s,hhum=%s,otemp=%s,err=%d,cmpfreq=%d
 *
 *   get_model_info response (0x08075A74):
 *     ret=%s,model=%s,type=%c,pv=%s,cpv=%c,mid=%s,
 *     s_fdir=%d,en_scdltmr=%d
 *
 *   get_model_info extended (0x08075BE0):
 *     ret=%s,model=%s,type=%c,pv=%s,cpv=%c,cpv_minor=%02d,
 *     mid=%s,humd=%d,s_humd=%d,acled=%d,land=%d,elec=%d,
 *     temp=%d,temp_rng=%d,m_dtct=%d,ac_dst=%s,disp_dry=%d,
 *     dmnd=%d,en_scdltmr=%d,en_frate=%d,en_fdir=%d,s_fdir=%d,
 *     en_rtemp_a=%d,en_spmode=%d,en_ipw_sep=%d,en_mompow=%d
 *
 *   get_week_power / get_day_power:
 *     ret=%s,p=%d,pmode=%d,ccnt=%d,wcnt=%d
 *
 *   get_scdltimer (0x08075900):
 *     ret=OK,format=v1,f_detail=total#18;_en#1;_pow#1;_mode#1;
 *     _temp#4;_time#4;_vol#1;_dir#1;_humi#3;_spmd#2,
 *     scdl_num=3,scdl_per_day=6,...
 *
 *   get_control_info fan rates (0x080759C8):
 *     ,f_rate=%c,f_dir=%c,b_f_rate=%c,b_f_dir=%c,
 *     dfr1=%c,...,dfr7=%c,dfrh=%c,dfd1=%c,...,dfd7=%c,dfdh=%c
 *
 *   get_price (0x0800A310):
 *     ret=%s,price_int=%d,price_dec=%d
 *
 *   get_demand (0x0800B87C):
 *     ,en_demand=%d,mode=%d,max_pow=%d,scdl_per_day=%d
 *
 *   periodic stats (0x08009E64):
 *     ,RouterDisconCnt=%lu,PollingErrCnt=%lu
 *     ,ResetCount=%d,LastResetTime=...
 *
 *   moc/power stats (0x0800A928):
 *     ret=OK,moc=0,tuc=0,wec=0,thc=0,frc=0,sac=0,suc=0
 */

#include "daikin.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/*  Static model capabilities (device-specific, read-only)           */
/* ------------------------------------------------------------------ */
static const char  MODEL_STR[]   = "0000";
static const char  MODEL_TYPE    = 'N';
static const char  MODEL_PV[]    = "2";
static const char  MODEL_CPV     = 'B';
static const int   MODEL_CPV_MIN = 0;
static const char  MODEL_MID[]   = "0000000000000000";

/* Capability flags — inferred from extended model_info format string */
static const int   CAP_HUMD       = 0;
static const int   CAP_S_HUMD     = 0;
static const int   CAP_ACLED      = 1;
static const int   CAP_LAND       = 0;
static const int   CAP_ELEC       = 1;   /* power monitoring */
static const int   CAP_TEMP       = 1;
static const int   CAP_TEMP_RNG   = 0;
static const int   CAP_M_DTCT     = 0;
static const char  CAP_AC_DST[]   = "0";
static const int   CAP_DISP_DRY   = 1;
static const int   CAP_DMND       = 1;   /* demand control */
static const int   CAP_EN_SCDLTMR = 1;
static const int   CAP_EN_FRATE   = 1;
static const int   CAP_EN_FDIR    = 1;
static const int   CAP_S_FDIR     = 1;
static const int   CAP_EN_RTEMP_A = 0;
static const int   CAP_EN_SPMODE  = 1;   /* special mode (streamer) */
static const int   CAP_EN_IPW_SEP = 1;
static const int   CAP_EN_MOMPOW  = 1;  /* momentary power */

/* Power history (simple ring buffer, in Wh) */
#define POWER_HIST_WEEKS  53
#define POWER_HIST_DAYS   8
static uint32_t g_week_power[POWER_HIST_WEEKS];
static uint32_t g_day_power[POWER_HIST_DAYS];
static uint32_t g_month_power[13];

/* Network error counter */
static uint32_t g_router_disconn_cnt = 0;
static uint32_t g_polling_err_cnt    = 0;

/* Price settings */
static int g_price_int = 0;
static int g_price_dec = 0;

/* ------------------------------------------------------------------ */
/*  ac_get_control_info                                               */
/* ------------------------------------------------------------------ */
int ac_get_control_info(char *buf, int buf_len) {
    ac_state_t *ac = &g_ac_state;

    return snprintf(buf, buf_len,
        "ret=OK,pow=%c,mode=%s,adv=%s,stemp=%s,shum=%s,"
        "dt1=--,dt2=--,dt3=--,dt4=--,dt5=--,dt7=--,"
        "dh1=--,dh2=--,dh3=--,dh4=--,dh5=--,dh7=--,dhh=--,"
        "b_mode=%s,b_stemp=%s,b_shum=%s,alert=%d,"
        "f_rate=%c,f_dir=%c,b_f_rate=%c,b_f_dir=%c,"
        "dfr1=A,dfr2=A,dfr3=A,dfr4=A,dfr5=A,dfr6=A,dfr7=A,dfrh=A,"
        "dfd1=0,dfd2=0,dfd3=0,dfd4=0,dfd5=0,dfd6=0,dfd7=0,dfdh=0",
        ac->pow,
        (ac->mode == AC_MODE_AUTO ? "0" :
         ac->mode == AC_MODE_DRY  ? "2" :
         ac->mode == AC_MODE_COOL ? "3" :
         ac->mode == AC_MODE_HEAT ? "4" : "6"),
        ac->adv[0] ? ac->adv : "0",
        ac->stemp,
        ac->shum,
        ac->b_mode ? &ac->b_mode : "0",
        ac->b_stemp,
        ac->b_shum,
        ac->alert,
        ac->f_rate,
        ac->f_dir,
        ac->f_rate,   /* b_f_rate same as current */
        AC_FDIR_STOP
    );
}

/* ------------------------------------------------------------------ */
/*  ac_set_control_info                                               */
/* ------------------------------------------------------------------ */
int ac_set_control_info(const char *qs) {
    char val[32];
    ac_state_t *ac = &g_ac_state;

    if (parse_key_values(qs, "pow",    val, sizeof(val)) == RET_OK)
        ac->pow = val[0];
    if (parse_key_values(qs, "mode",   val, sizeof(val)) == RET_OK)
        ac->mode = val[0];
    if (parse_key_values(qs, "stemp",  val, sizeof(val)) == RET_OK)
        snprintf(ac->stemp,  sizeof(ac->stemp),  "%s", val);
    if (parse_key_values(qs, "shum",   val, sizeof(val)) == RET_OK)
        snprintf(ac->shum,   sizeof(ac->shum),   "%s", val);
    if (parse_key_values(qs, "f_rate", val, sizeof(val)) == RET_OK)
        ac->f_rate = val[0];
    if (parse_key_values(qs, "f_dir",  val, sizeof(val)) == RET_OK)
        ac->f_dir = val[0];
    if (parse_key_values(qs, "adv",    val, sizeof(val)) == RET_OK)
        snprintf(ac->adv,    sizeof(ac->adv),    "%s", val);
    if (parse_key_values(qs, "b_mode", val, sizeof(val)) == RET_OK)
        ac->b_mode = val[0];
    if (parse_key_values(qs, "b_stemp",val, sizeof(val)) == RET_OK)
        snprintf(ac->b_stemp,sizeof(ac->b_stemp),"%s", val);
    if (parse_key_values(qs, "b_shum", val, sizeof(val)) == RET_OK)
        snprintf(ac->b_shum, sizeof(ac->b_shum), "%s", val);

    log_print(LOG_I, ",pow=%02x,mode=%02x",
              (unsigned)ac->pow, (unsigned)ac->mode);

    /* Forward control command to indoor unit via serial */
    /* AppSerialSend(build_control_cmd(), cmd_len); */

    return RET_OK;
}

/* ------------------------------------------------------------------ */
/*  ac_get_sensor_info                                                */
/* ------------------------------------------------------------------ */
int ac_get_sensor_info(char *buf, int buf_len) {
    ac_state_t *ac = &g_ac_state;

    int len = snprintf(buf, buf_len,
        "ret=OK,htemp=%s,hhum=%s,otemp=%s,err=%d,cmpfreq=%d",
        ac->htemp, ac->hhum, ac->otemp, ac->err, ac->cmpfreq);

    if (CAP_EN_MOMPOW) {
        len += snprintf(buf + len, buf_len - len,
            ",mompow=%d", ac->mompow);
    }

    return len;
}

/* ------------------------------------------------------------------ */
/*  ac_get_model_info                                                 */
/* ------------------------------------------------------------------ */
int ac_get_model_info(char *buf, int buf_len) {
    return snprintf(buf, buf_len,
        "ret=OK,model=%s,type=%c,pv=%s,cpv=%c,cpv_minor=%02d,"
        "mid=%s,humd=%d,s_humd=%d,acled=%d,land=%d,elec=%d,"
        "temp=%d,temp_rng=%d,m_dtct=%d,ac_dst=%s,disp_dry=%d,"
        "dmnd=%d,en_scdltmr=%d,en_frate=%d,en_fdir=%d,s_fdir=%d,"
        "en_rtemp_a=%d,en_spmode=%d,en_ipw_sep=%d,en_mompow=%d",
        MODEL_STR, MODEL_TYPE, MODEL_PV, MODEL_CPV, MODEL_CPV_MIN,
        MODEL_MID, CAP_HUMD, CAP_S_HUMD, CAP_ACLED, CAP_LAND,
        CAP_ELEC, CAP_TEMP, CAP_TEMP_RNG, CAP_M_DTCT, CAP_AC_DST,
        CAP_DISP_DRY, CAP_DMND, CAP_EN_SCDLTMR, CAP_EN_FRATE,
        CAP_EN_FDIR, CAP_S_FDIR, CAP_EN_RTEMP_A, CAP_EN_SPMODE,
        CAP_EN_IPW_SEP, CAP_EN_MOMPOW);
}

/* ------------------------------------------------------------------ */
/*  ac_get_info / ac_set_info (adapter registration info)            */
/* ------------------------------------------------------------------ */
int ac_get_info(char *buf, int buf_len) {
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

int ac_set_info(const char *qs) {
    adapter_info_t *ai = &g_adapter_info;
    char val[64];

    if (parse_key_values(qs, "name",     ai->name,     sizeof(ai->name))     != RET_OK) return RET_PARAM_NG;
    if (parse_key_values(qs, "location", ai->location, sizeof(ai->location)) != RET_OK) return RET_PARAM_NG;
    parse_key_values(qs, "icon",    ai->icon,    sizeof(ai->icon));
    parse_key_values(qs, "method",  ai->method,  sizeof(ai->method));
    if (parse_key_values(qs, "port", val, sizeof(val)) == RET_OK)
        ai->port = atoi(val);
    parse_key_values(qs, "id",  ai->id,  sizeof(ai->id));
    parse_key_values(qs, "pw",  ai->pw,  sizeof(ai->pw));
    if (parse_key_values(qs, "lpw_flag", val, sizeof(val)) == RET_OK)
        ai->lpw_flag = atoi(val);
    if (parse_key_values(qs, "led", val, sizeof(val)) == RET_OK)
        ai->led = atoi(val);

    return RET_OK;
}

/* ------------------------------------------------------------------ */
/*  Timer (on/off schedule)                                           */
/* ------------------------------------------------------------------ */
int ac_get_timer(char *buf, int buf_len) {
    /* t%d_ena, t%d_pow, t%d_time, t%d_wk format (0x0800A8E4) */
    int len = snprintf(buf, buf_len, "ret=OK");
    for (int i = 1; i <= 3; i++) {
        len += snprintf(buf + len, buf_len - len,
            ",t%d_ena=0,t%d_pow=0,t%d_time=----,t%d_wk=0000000",
            i, i, i, i);
    }
    return len;
}

int ac_set_timer(const char *qs) {
    (void)qs;
    /* Parse t1_ena, t1_pow, t1_time, t1_wk ... */
    return RET_OK;
}

/* ------------------------------------------------------------------ */
/*  Power history                                                     */
/* ------------------------------------------------------------------ */
int ac_get_week_power(char *buf, int buf_len) {
    /* "ret=%s,p=%d,pmode=%d,ccnt=%d,wcnt=%d" */
    int total = 0;
    for (int i = 0; i < 7; i++) total += g_day_power[i];
    return snprintf(buf, buf_len,
        "ret=OK,p=%u,pmode=0,ccnt=%u,wcnt=%u",
        total, 0u, 0u);
}

int ac_get_year_power(char *buf, int buf_len) {
    int total = 0;
    for (int i = 0; i < 12; i++) total += g_month_power[i];
    return snprintf(buf, buf_len,
        "ret=OK,p=%u,pmode=0,ccnt=%u,wcnt=%u",
        total, 0u, 0u);
}

int ac_get_month_power_ex(char *buf, int buf_len) {
    /* "ret=OK,moc=0,tuc=0,wec=0,thc=0,frc=0,sac=0,suc=0" */
    return snprintf(buf, buf_len,
        "ret=OK,moc=0,tuc=0,wec=0,thc=0,frc=0,sac=0,suc=0");
}

/* ------------------------------------------------------------------ */
/*  Network error tracking                                            */
/* ------------------------------------------------------------------ */
int ac_clr_nw_err(void) {
    g_router_disconn_cnt = 0;
    g_polling_err_cnt    = 0;
    return RET_OK;
}

int ac_get_nw_err(char *buf, int buf_len) {
    return snprintf(buf, buf_len,
        "ret=OK,RouterDisconCnt=%lu,PollingErrCnt=%lu",
        (unsigned long)g_router_disconn_cnt,
        (unsigned long)g_polling_err_cnt);
}

/* ------------------------------------------------------------------ */
/*  Special mode (streamer / humidifier etc.)                        */
/* ------------------------------------------------------------------ */
int ac_set_special_mode(const char *qs) {
    char val[16];
    int en_streamer = 0, set_spmode = 0, spmode_kind = 0;

    parse_key_values(qs, "en_streamer",  val, sizeof(val));
    en_streamer = atoi(val);
    parse_key_values(qs, "set_spmode",   val, sizeof(val));
    set_spmode = atoi(val);
    parse_key_values(qs, "spmode_kind",  val, sizeof(val));
    spmode_kind = atoi(val);

    log_print(LOG_V, "ret=%s,adv=%s",
              RESP_OK,
              en_streamer ? "streamer" : "0");

    (void)set_spmode; (void)spmode_kind;
    return RET_OK;
}

/* ------------------------------------------------------------------ */
/*  Demand control                                                    */
/* ------------------------------------------------------------------ */
int ac_get_demand_control(char *buf, int buf_len) {
    demand_ctrl_t *d = &g_demand;
    return snprintf(buf, buf_len,
        "ret=OK,en_demand=%d,mode=%d,max_pow=%d,scdl_per_day=%d"
        ",dmnd_run=%d",
        d->en_demand, d->mode, d->max_pow, d->scdl_per_day,
        d->dmnd_run);
}

int ac_set_demand_control(const char *qs) {
    char val[16];
    demand_ctrl_t *d = &g_demand;

    if (parse_key_values(qs, "en_demand", val, sizeof(val)) == RET_OK)
        d->en_demand = atoi(val);
    if (parse_key_values(qs, "mode", val, sizeof(val)) == RET_OK)
        d->mode = atoi(val);
    if (parse_key_values(qs, "max_pow", val, sizeof(val)) == RET_OK)
        d->max_pow = atoi(val);

    return RET_OK;
}

/* ------------------------------------------------------------------ */
/*  Schedule timer                                                    */
/* ------------------------------------------------------------------ */
int ac_get_scdltimer(char *buf, int buf_len) {
    scdl_timer_t *s = &g_scdl;
    return snprintf(buf, buf_len,
        "ret=OK,en_scdltimer=%d,active_no=%d,en_oldtimer=%d",
        s->en_scdltimer, s->active_no, 0);
}

int ac_set_scdltimer(const char *qs) {
    char val[16];
    scdl_timer_t *s = &g_scdl;

    if (parse_key_values(qs, "en_scdltimer", val, sizeof(val)) == RET_OK)
        s->en_scdltimer = atoi(val);
    if (parse_key_values(qs, "active_no", val, sizeof(val)) == RET_OK)
        s->active_no = atoi(val);

    return RET_OK;
}

int ac_get_scdltimer_body(char *buf, int buf_len) {
    /* "ret=OK,format=v1,f_detail=total#18;_en#1;..." */
    return snprintf(buf, buf_len,
        "ret=OK,format=v1,"
        "f_detail=total#18;_en#1;_pow#1;_mode#1;_temp#4;"
        "_time#4;_vol#1;_dir#1;_humi#3;_spmd#2,"
        "scdl_num=%d,scdl_per_day=%d",
        SCDL_MAX, SCDL_PER_DAY);
}

int ac_set_scdltimer_body(const char *qs) {
    (void)qs;
    /* Parse schedule body data — %s%d_en, %s%d_pow, %s%d_mod,
     * %s%d_tmp, %s%d_time format (from 0x0800A95C) */
    return RET_OK;
}

/* ------------------------------------------------------------------ */
/*  Price                                                             */
/* ------------------------------------------------------------------ */
int ac_get_price(char *buf, int buf_len) {
    return snprintf(buf, buf_len,
        "ret=OK,price_int=%d,price_dec=%d",
        g_price_int, g_price_dec);
}

int ac_set_price(const char *qs) {
    char val[16];
    if (parse_key_values(qs, "price_int", val, sizeof(val)) == RET_OK)
        g_price_int = atoi(val);
    if (parse_key_values(qs, "price_dec", val, sizeof(val)) == RET_OK)
        g_price_dec = atoi(val);
    return RET_OK;
}

/* ------------------------------------------------------------------ */
/*  Error notice POST to cloud                                        */
/* ------------------------------------------------------------------ */
void ac_error_notice_post(void) {
    char info[64];
    snprintf(info, sizeof(info), "err=%d", g_ac_state.err);
    log_print(LOG_I, "/aircon/error_notice");
    cloud_post_error_notice(g_ac_state.err, info);
}
