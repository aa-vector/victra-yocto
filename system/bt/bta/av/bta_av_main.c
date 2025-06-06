/******************************************************************************
 *  Copyright (c) 2016, The Linux Foundation. All rights reserved.
 *
 *  Not a contribution.
 ******************************************************************************/
/******************************************************************************
 *
 *  Copyright (C) 2004-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/******************************************************************************
 *
 *  This is the main implementation file for the BTA advanced audio/video.
 *
 ******************************************************************************/

#define LOG_TAG "bt_bta_av"

#include <assert.h>
#include <string.h>

#include "bt_target.h"
#include "osi/include/log.h"
#include <cutils/properties.h>
#include <stdlib.h>

#if defined(BTA_AV_INCLUDED) && (BTA_AV_INCLUDED == TRUE)
#include "bta_av_co.h"
#include "bta_av_int.h"
#include "l2c_api.h"
#include "l2cdefs.h"
#include "utl.h"
#include "btm_int.h"

#if( defined BTA_AR_INCLUDED ) && (BTA_AR_INCLUDED == TRUE)
#include "bta_ar_api.h"
#endif

#include "a2d_aptx.h"
#include "a2d_aptx_hd.h"

/*****************************************************************************
** Constants and types
*****************************************************************************/

/* AVDTP protocol timeout values */
#define BTIF_AVK_SERVICE_NAME "Advanced Audio Sink"

#ifndef BTA_AV_RET_TOUT
#define BTA_AV_RET_TOUT     4
#endif

#ifndef BTA_AV_SIG_TOUT
#define BTA_AV_SIG_TOUT     4
#endif

#ifndef BTA_AV_IDLE_TOUT
#define BTA_AV_IDLE_TOUT    10
#endif

/* the delay time in milliseconds to retry role switch */
#ifndef BTA_AV_RS_TIME_VAL
#define BTA_AV_RS_TIME_VAL     1000
#endif

/* offload codecs support */
enum
{
    APTX = 1,
    AAC,
    APTXHD
};

/* state machine states */
enum
{
    BTA_AV_INIT_ST,
    BTA_AV_OPEN_ST
};

/* state machine action enumeration list */
enum
{
    BTA_AV_DISABLE,
    BTA_AV_RC_OPENED,
    BTA_AV_RC_REMOTE_CMD,
    BTA_AV_RC_VENDOR_CMD,
    BTA_AV_RC_VENDOR_RSP,
    BTA_AV_RC_FREE_RSP,
    BTA_AV_RC_FREE_MSG,
    BTA_AV_RC_META_RSP,
    BTA_AV_RC_MSG,
    BTA_AV_RC_CLOSE,
    BTA_AV_NUM_ACTIONS
};

#define BTA_AV_IGNORE       BTA_AV_NUM_ACTIONS

/* type for action functions */
typedef void (*tBTA_AV_ACTION)(tBTA_AV_CB *p_cb, tBTA_AV_DATA *p_data);

/* action functions */
const tBTA_AV_ACTION bta_av_action[] =
{
    bta_av_disable,
    bta_av_rc_opened,
    bta_av_rc_remote_cmd,
    bta_av_rc_vendor_cmd,
    bta_av_rc_vendor_rsp,
    bta_av_rc_free_rsp,
    bta_av_rc_free_msg,
    bta_av_rc_meta_rsp,
    bta_av_rc_msg,
    bta_av_rc_close,
    NULL
};

/* state table information */
#define BTA_AV_ACTION_COL           0       /* position of actions */
#define BTA_AV_NEXT_STATE           1       /* position of next state */
#define BTA_AV_NUM_COLS             2       /* number of columns in state tables */

/* state table for init state */
static const UINT8 bta_av_st_init[][BTA_AV_NUM_COLS] =
{
/* Event                     Action 1               Next state */
/* API_DISABLE_EVT */       {BTA_AV_DISABLE,        BTA_AV_INIT_ST },
/* API_REMOTE_CMD_EVT */    {BTA_AV_IGNORE,         BTA_AV_INIT_ST },
/* API_VENDOR_CMD_EVT */    {BTA_AV_IGNORE,         BTA_AV_INIT_ST },
/* API_VENDOR_RSP_EVT */    {BTA_AV_IGNORE,         BTA_AV_INIT_ST },
/* API_META_RSP_EVT */      {BTA_AV_RC_FREE_RSP,    BTA_AV_INIT_ST },
/* API_RC_CLOSE_EVT */      {BTA_AV_RC_CLOSE,       BTA_AV_INIT_ST },
/* AVRC_OPEN_EVT */         {BTA_AV_RC_OPENED,      BTA_AV_OPEN_ST },
/* AVRC_MSG_EVT */          {BTA_AV_RC_FREE_MSG,    BTA_AV_INIT_ST },
/* AVRC_NONE_EVT */         {BTA_AV_IGNORE,         BTA_AV_INIT_ST },
};

/* state table for open state */
static const UINT8 bta_av_st_open[][BTA_AV_NUM_COLS] =
{
/* Event                     Action 1               Next state */
/* API_DISABLE_EVT */       {BTA_AV_DISABLE,        BTA_AV_INIT_ST },
/* API_REMOTE_CMD_EVT */    {BTA_AV_RC_REMOTE_CMD,  BTA_AV_OPEN_ST },
/* API_VENDOR_CMD_EVT */    {BTA_AV_RC_VENDOR_CMD,  BTA_AV_OPEN_ST },
/* API_VENDOR_RSP_EVT */    {BTA_AV_RC_VENDOR_RSP,  BTA_AV_OPEN_ST },
/* API_META_RSP_EVT */      {BTA_AV_RC_META_RSP,    BTA_AV_OPEN_ST },
/* API_RC_CLOSE_EVT */      {BTA_AV_RC_CLOSE,       BTA_AV_OPEN_ST },
/* AVRC_OPEN_EVT */         {BTA_AV_RC_OPENED,      BTA_AV_OPEN_ST },
/* AVRC_MSG_EVT */          {BTA_AV_RC_MSG,         BTA_AV_OPEN_ST },
/* AVRC_NONE_EVT */         {BTA_AV_IGNORE,         BTA_AV_INIT_ST },
};

/* type for state table */
typedef const UINT8 (*tBTA_AV_ST_TBL)[BTA_AV_NUM_COLS];

/* state table */
static const tBTA_AV_ST_TBL bta_av_st_tbl[] =
{
    bta_av_st_init,
    bta_av_st_open
};

typedef void (*tBTA_AV_NSM_ACT)(tBTA_AV_DATA *p_data);
static void bta_av_api_enable(tBTA_AV_DATA *p_data);
static void bta_av_api_register(tBTA_AV_DATA *p_data);
#if (BTA_AV_SINK_INCLUDED == TRUE)
static void bta_av_api_sink_enable(tBTA_AV_DATA *p_data);
#endif
static void bta_av_ci_data(tBTA_AV_DATA *p_data);
#if (AVDT_REPORTING == TRUE)
static void bta_av_rpc_conn(tBTA_AV_DATA *p_data);
#endif
static void bta_av_api_to_ssm(tBTA_AV_DATA *p_data);

static void bta_av_sco_chg_cback(tBTA_SYS_CONN_STATUS status, UINT8 id, UINT8
                                 app_id, BD_ADDR peer_addr);
static void bta_av_sys_rs_cback (tBTA_SYS_CONN_STATUS status,UINT8 id, UINT8 app_id, BD_ADDR peer_addr);

static void bta_av_api_enable_multicast(tBTA_AV_DATA *p_data);
static void bta_av_api_update_max_av_clients(tBTA_AV_DATA * p_data);
static void bta_av_api_update_supp_codecs(tBTA_AV_DATA *p_data);

/* action functions */
const tBTA_AV_NSM_ACT bta_av_nsm_act[] =
{
    bta_av_api_enable,      /* BTA_AV_API_ENABLE_EVT */
    bta_av_api_register,    /* BTA_AV_API_REGISTER_EVT */
    bta_av_api_deregister,  /* BTA_AV_API_DEREGISTER_EVT */
    bta_av_api_disconnect,  /* BTA_AV_API_DISCONNECT_EVT */
    bta_av_ci_data,         /* BTA_AV_CI_SRC_DATA_READY_EVT */
    bta_av_sig_chg,         /* BTA_AV_SIG_CHG_EVT */
    bta_av_signalling_timer, /* BTA_AV_SIGNALLING_TIMER_EVT */
    bta_av_rc_disc_done,    /* BTA_AV_SDP_AVRC_DISC_EVT */
    bta_av_rc_closed,       /* BTA_AV_AVRC_CLOSE_EVT */
    bta_av_conn_chg,        /* BTA_AV_CONN_CHG_EVT */
    bta_av_dereg_comp,      /* BTA_AV_DEREG_COMP_EVT */
#if (BTA_AV_SINK_INCLUDED == TRUE)
    bta_av_api_sink_enable,         /* BTA_AV_API_SINK_ENABLE_EVT */
#endif
#if (AVDT_REPORTING == TRUE)
    bta_av_rpc_conn,                /* BTA_AV_AVDT_RPT_CONN_EVT */
#endif
    bta_av_api_to_ssm,              /* BTA_AV_API_START_EVT */
    bta_av_api_to_ssm,              /* BTA_AV_API_STOP_EVT */
    bta_av_api_update_max_av_clients, /* BTA_AV_UPDATE_MAX_AV_CLIENTS_EVT */
    bta_av_api_update_supp_codecs, /* BTA_AV_UPDATE_SUPP_CODECS */
    bta_av_api_enable_multicast,    /* BTA_AV_ENABLE_MULTICAST_EVT */
};

/*****************************************************************************
** Global data
*****************************************************************************/

/* AV control block */
#if BTA_DYNAMIC_MEMORY == FALSE
tBTA_AV_CB  bta_av_cb;
#endif

#if (defined(BTA_AV_DEBUG) && BTA_AV_DEBUG == TRUE)
static char *bta_av_st_code(UINT8 state);
#endif

static BOOLEAN is_multicast_enabled = FALSE;
/*******************************************************************************
**
** Function         bta_av_api_enable
**
** Description      Handle an API enable event.
**
**
** Returns          void
**
*******************************************************************************/
static void bta_av_api_enable(tBTA_AV_DATA *p_data)
{
    /* initialize control block */
    memset(&bta_av_cb, 0, sizeof(tBTA_AV_CB));

    for (int i = 0; i < BTA_AV_NUM_RCB; i++)
        bta_av_cb.rcb[i].handle = BTA_AV_RC_HANDLE_NONE;

    bta_av_cb.rc_acp_handle = BTA_AV_RC_HANDLE_NONE;

    /*
     * TODO: The "disable" event handling is missing - there we need
     * to alarm_free() the alarms below.
     */
    bta_av_cb.link_signalling_timer = alarm_new("bta_av.link_signalling_timer");
    bta_av_cb.accept_signalling_timer =
        alarm_new("bta_av.accept_signalling_timer");

    /* store parameters */
    bta_av_cb.p_cback  = p_data->api_enable.p_cback;
    bta_av_cb.features = p_data->api_enable.features;
    bta_av_cb.sec_mask = p_data->api_enable.sec_mask;

    tBTA_AV_ENABLE enable;
    enable.features = bta_av_cb.features;

    /* Register for SCO change event */
    if (!(bta_av_cb.features & BTA_AV_FEAT_NO_SCO_SSPD))
    {
        bta_sys_sco_register(bta_av_sco_chg_cback);
    }

    /* call callback with enable event */
    (*bta_av_cb.p_cback)(BTA_AV_ENABLE_EVT, (tBTA_AV *)&enable);
}

/*******************************************************************************
**
** Function         bta_av_addr_to_scb
**
** Description      find the stream control block by the peer addr
**
** Returns          void
**
*******************************************************************************/
static tBTA_AV_SCB * bta_av_addr_to_scb(BD_ADDR bd_addr)
{
    tBTA_AV_SCB * p_scb = NULL;
    int         xx;

    for(xx=0; xx<BTA_AV_NUM_STRS; xx++)
    {
        if(bta_av_cb.p_scb[xx])
        {
            if(!bdcmp(bd_addr, bta_av_cb.p_scb[xx]->peer_addr))
            {
                p_scb = bta_av_cb.p_scb[xx];
                break;
            }
        }
    }
    return p_scb;
}

/*******************************************************************************
**
** Function         bta_av_hndl_to_scb
**
** Description      find the stream control block by the handle
**
** Returns          void
**
*******************************************************************************/
tBTA_AV_SCB * bta_av_hndl_to_scb(UINT16 handle)
{
    tBTA_AV_HNDL hndl = (tBTA_AV_HNDL)handle;
    tBTA_AV_SCB * p_scb = NULL;
    UINT8 idx = (hndl & BTA_AV_HNDL_MSK);

    if(idx && (idx <= BTA_AV_NUM_STRS) )
    {
        p_scb = bta_av_cb.p_scb[idx-1];
    }
    return p_scb;
}

/*******************************************************************************
**
** Function         bta_av_alloc_scb
**
** Description      allocate stream control block,
**                  register the service to stack
**                  create SDP record
**
** Returns          void
**
*******************************************************************************/
static tBTA_AV_SCB * bta_av_alloc_scb(tBTA_AV_CHNL chnl)
{
    tBTA_AV_SCB *p_ret = NULL;
    int         xx;
    tBTA_AV_STATUS sts = BTA_AV_SUCCESS;

    if(chnl == BTA_AV_CHNL_VIDEO)
    {
        if(p_bta_av_cfg->p_act_tbl == NULL || p_bta_av_cfg->p_reg == NULL)
        {
            APPL_TRACE_ERROR("Video streaming not supported");
            sts = BTA_AV_FAIL;
        }
        else
        {
            /* allow only one Video channel */
            if(bta_av_cb.reg_video)
            {
                APPL_TRACE_ERROR("Already registered");
                sts = BTA_AV_FAIL;
            }
        }
    }
    else if(chnl != BTA_AV_CHNL_AUDIO)
    {
        APPL_TRACE_ERROR("bad channel: %d", chnl);
        sts = BTA_AV_FAIL;
    }

    if(sts == BTA_AV_SUCCESS)
    {
        for(xx=0; xx<BTA_AV_NUM_STRS; xx++)
        {
            if(bta_av_cb.p_scb[xx] == NULL)
            {
                /* found an empty spot */
                p_ret = (tBTA_AV_SCB *)osi_calloc(sizeof(tBTA_AV_SCB));
                p_ret->rc_handle = BTA_AV_RC_HANDLE_NONE;
                p_ret->chnl = chnl;
                p_ret->hndl = (tBTA_AV_HNDL)((xx + 1) | chnl);
                p_ret->hdi  = xx;
                p_ret->a2d_list = list_new(NULL);
                p_ret->avrc_ct_timer = alarm_new("bta_av.avrc_ct_timer");
                bta_av_cb.p_scb[xx] = p_ret;
                APPL_TRACE_EVENT("AV: Alloc success, handle is =%d", p_ret->hndl);
                break;
            }
        }
    }
    return p_ret;
}

/*******************************************************************************
*******************************************************************************/
void bta_av_conn_cback(UINT8 handle, BD_ADDR bd_addr, UINT8 event, tAVDT_CTRL *p_data)
{
    UINT16  evt = 0;
    tBTA_AV_SCB *p_scb = NULL;
    UNUSED(handle);

    APPL_TRACE_DEBUG("%s event :%d",__func__, event);
#if( defined BTA_AR_INCLUDED ) && (BTA_AR_INCLUDED == TRUE)
    if (event == BTA_AR_AVDT_CONN_EVT ||
        event == AVDT_CONNECT_IND_EVT || event == AVDT_DISCONNECT_IND_EVT)
#else
    if (event == AVDT_CONNECT_IND_EVT || event == AVDT_DISCONNECT_IND_EVT)
#endif
    {
        evt = BTA_AV_SIG_CHG_EVT;
        if(AVDT_DISCONNECT_IND_EVT == event)
            p_scb = bta_av_addr_to_scb(bd_addr);
#if (defined(BTA_AV_DEBUG) && BTA_AV_DEBUG == TRUE)
        else if (AVDT_CONNECT_IND_EVT == event)
        {
            APPL_TRACE_DEBUG("CONN_IND is ACP:%d", p_data->hdr.err_param);
        }
#endif

        tBTA_AV_STR_MSG *p_msg =
            (tBTA_AV_STR_MSG *)osi_malloc(sizeof(tBTA_AV_STR_MSG));
        p_msg->hdr.event = evt;
        p_msg->hdr.layer_specific = event;
        p_msg->hdr.offset = p_data->hdr.err_param;
        bdcpy(p_msg->bd_addr, bd_addr);
#if (defined(BTA_AV_DEBUG) && BTA_AV_DEBUG == TRUE)
        if(p_scb) {
            APPL_TRACE_DEBUG("scb hndl x%x, role x%x", p_scb->hndl, p_scb->role);
        }
#endif
        APPL_TRACE_DEBUG("conn_cback bd_addr:%02x-%02x-%02x-%02x-%02x-%02x",
                         bd_addr[0], bd_addr[1],
                         bd_addr[2], bd_addr[3],
                         bd_addr[4], bd_addr[5]);
        bta_sys_sendmsg(p_msg);
    }
}

BOOLEAN bta_av_is_scb_available()
{
    tBTA_AV_CB   *p_cb = &bta_av_cb;
    int     xx;
    UINT8   mask;
    for(xx=0; xx<BTA_AV_NUM_LINKS; xx++)
    {
        mask = 1 << xx;
        APPL_TRACE_DEBUG(" %s The current conn_lcb: 0x%x index = %d", __func__, p_cb->conn_lcb, xx);

        /* look for a p_lcb with its p_scb registered */
        if((!(mask & p_cb->conn_lcb)) && (p_cb->p_scb[xx] != NULL))
        {
            /* Check if the SCB is Free before using for
             * ACP connection
             */
            if (p_cb->p_scb[xx]->state == BTA_AV_INIT_ST)
            {
                APPL_TRACE_DEBUG(" %s SCB is free @ %d", __func__, xx);
                return true;
            }
        }
    }
    APPL_TRACE_DEBUG(" %s SCB is not free ", __func__);
    return false;
}

#if AVDT_REPORTING == TRUE
/*******************************************************************************
**
** Function         bta_av_a2dp_report_cback
**
** Description      A2DP report callback.
**
** Returns          void
**
*******************************************************************************/
static void bta_av_a2dp_report_cback(UINT8 handle, AVDT_REPORT_TYPE type,
                                    tAVDT_REPORT_DATA *p_data)
{
    UNUSED(handle);
    UNUSED(type);
    UNUSED(p_data);
    /* Do not need to handle report data for now.
     * This empty function is here for conformance reasons. */
}
#endif

#if (BTA_AV_SINK_INCLUDED == TRUE)
/*******************************************************************************
**
** Function         bta_av_api_sink_enable
**
** Description      activate, deactive A2DP Sink,
**
** Returns          void
**
*******************************************************************************/

static void bta_av_api_sink_enable(tBTA_AV_DATA *p_data)
{
    UINT16 activate_sink = 0;
    activate_sink = p_data->hdr.layer_specific;
    APPL_TRACE_DEBUG("bta_av_api_sink_enable %d ", activate_sink)
    char p_service_name[BTA_SERVICE_NAME_LEN+1];
    strlcpy(p_service_name, BTIF_AVK_SERVICE_NAME, BTA_SERVICE_NAME_LEN);

    if (activate_sink)
    {
        AVDT_SINK_Activate();
        if (bta_av_cb.sdp_a2d_snk_handle == 0)
        {
            bta_av_cb.sdp_a2d_snk_handle = SDP_CreateRecord();
            A2D_AddRecord(UUID_SERVCLASS_AUDIO_SINK, p_service_name, NULL,
                          A2D_SUPF_PLAYER, bta_av_cb.sdp_a2d_snk_handle);
            bta_sys_add_uuid(UUID_SERVCLASS_AUDIO_SINK);
        }
    }
    else
    {
        AVDT_SINK_Deactivate();
        if (bta_av_cb.sdp_a2d_snk_handle != 0)
        {
            SDP_DeleteRecord(bta_av_cb.sdp_a2d_snk_handle);
            bta_av_cb.sdp_a2d_snk_handle = 0;
            bta_sys_remove_uuid(UUID_SERVCLASS_AUDIO_SINK);
        }
    }
}
#endif
/*******************************************************************************
**
** Function         bta_av_api_register
**
** Description      allocate stream control block,
**                  register the service to stack
**                  create SDP record
**
** Returns          void
**
*******************************************************************************/
static void bta_av_api_register(tBTA_AV_DATA *p_data)
{
    tBTA_AV_REGISTER    registr;
    tBTA_AV_SCB         *p_scb;    /* stream control block */
    tAVDT_REG       reg;
    tAVDT_CS        cs;
    char            *p_service_name;
    tBTA_AV_CODEC   codec_type;
    tBTA_UTL_COD    cod;
    UINT8           startIndex = 0;
    UINT8           endIndex = 0;
    UINT8           index;
    UINT16          profile_initialized;

    memset(&cs,0,sizeof(tAVDT_CS));

    registr.status = BTA_AV_FAIL_RESOURCES;
    registr.app_id = p_data->api_reg.app_id;
    registr.chnl   = (tBTA_AV_CHNL)p_data->hdr.layer_specific;

    APPL_TRACE_DEBUG("bta_av_api_register : channel %d", registr.chnl);

    profile_initialized = p_data->api_reg.service_uuid;
    if (profile_initialized == UUID_SERVCLASS_AUDIO_SOURCE)
    {
        p_bta_av_cfg  = (tBTA_AV_CFG *) &bta_av_cfg;
    }

    do
    {
        p_scb = bta_av_alloc_scb(registr.chnl);
        if(p_scb == NULL)
        {
            APPL_TRACE_ERROR("failed to alloc SCB");
            break;
        }
        cs.registration_id = p_scb->hdi;
        registr.hndl    = p_scb->hndl;
        p_scb->app_id   = registr.app_id;

        /* initialize the stream control block */
        registr.status = BTA_AV_SUCCESS;

        if((bta_av_cb.reg_audio + bta_av_cb.reg_video) == 0)
        {
            /* the first channel registered. register to AVDTP */
            reg.ctrl_mtu = p_bta_av_cfg->sig_mtu;
            reg.ret_tout = BTA_AV_RET_TOUT;
            reg.sig_tout = BTA_AV_SIG_TOUT;
            reg.idle_tout = BTA_AV_IDLE_TOUT;
            reg.sec_mask = bta_av_cb.sec_mask;
#if( defined BTA_AR_INCLUDED ) && (BTA_AR_INCLUDED == TRUE)
            bta_ar_reg_avdt(&reg, bta_av_conn_cback, BTA_ID_AV);
#endif
            bta_sys_role_chg_register(&bta_av_sys_rs_cback);

            /* create remote control TG service if required */
            if (bta_av_cb.features & (BTA_AV_FEAT_RCTG))
            {
                /* register with no authorization; let AVDTP use authorization instead */
#if( defined BTA_AR_INCLUDED ) && (BTA_AR_INCLUDED == TRUE)
#if (BTA_AV_WITH_AVCTP_AUTHORIZATION == TRUE)
                bta_ar_reg_avct(p_bta_av_cfg->avrc_mtu, p_bta_av_cfg->avrc_br_mtu,
                                bta_av_cb.sec_mask, BTA_ID_AV);
#else
                bta_ar_reg_avct(p_bta_av_cfg->avrc_mtu, p_bta_av_cfg->avrc_br_mtu,
                                (UINT8)(bta_av_cb.sec_mask & (~BTA_SEC_AUTHORIZE)), BTA_ID_AV);
#endif
                if (profile_initialized == UUID_SERVCLASS_AUDIO_SOURCE)
                {
#if (defined(SDP_AVRCP_1_6) && (SDP_AVRCP_1_6 == TRUE))
                    bta_ar_reg_avrc(UUID_SERVCLASS_AV_REM_CTRL_TARGET, "AV Remote Control Target",
                                   NULL, p_bta_av_cfg->avrc_tg_cat, BTA_ID_AV,
                                   (bta_av_cb.features & BTA_AV_FEAT_BROWSE), AVRC_REV_1_6);
#else
#if (defined(SDP_AVRCP_1_5) && (SDP_AVRCP_1_5 == TRUE))
            /* Downgraded AVRCP version to have symmetry in case both src and sink are enableda */
                    bta_ar_reg_avrc(UUID_SERVCLASS_AV_REM_CTRL_TARGET, "AV Remote Control Target",
                                   NULL, p_bta_av_cfg->avrc_tg_cat, BTA_ID_AV,
                                   (bta_av_cb.features & BTA_AV_FEAT_BROWSE), AVRC_REV_1_4);
#else
                    bta_ar_reg_avrc(UUID_SERVCLASS_AV_REM_CTRL_TARGET, "AV Remote Control Target",
                                   NULL, p_bta_av_cfg->avrc_tg_cat, BTA_ID_AV,
                                   (bta_av_cb.features & BTA_AV_FEAT_BROWSE), AVRC_REV_1_3);
#endif
#endif
                }
                else if (profile_initialized == UUID_SERVCLASS_AUDIO_SINK)
                {
                    // Initialize AVRCP1.4 to provide Absolute Volume control.
                    bta_ar_reg_avrc(UUID_SERVCLASS_AV_REM_CTRL_TARGET, "AV Remote Control Target",
                                   NULL, p_bta_av_cfg->avrc_tg_cat, BTA_ID_AV,
                                   (bta_av_cb.features & BTA_AV_FEAT_BROWSE), AVRC_REV_1_4);
                }
#endif
            }

            /* Set the Capturing service class bit */
            if (profile_initialized == UUID_SERVCLASS_AUDIO_SOURCE)
                cod.service = BTM_COD_SERVICE_CAPTURING;
            else if (profile_initialized == UUID_SERVCLASS_AUDIO_SINK)
                cod.service = BTM_COD_SERVICE_RENDERING;
            utl_set_device_class(&cod, BTA_UTL_SET_COD_SERVICE_CLASS);
        } /* if 1st channel */

        /* get stream configuration and create stream */
        /* memset(&cs.cfg,0,sizeof(tAVDT_CFG)); */
        cs.cfg.num_codec = 1;


        /*
         * memset of cs takes care setting call back pointers to null.
        cs.p_data_cback = NULL;
        cs.p_report_cback = NULL;
        */
        cs.nsc_mask = AVDT_NSC_RECONFIG |
              ((bta_av_cb.features & BTA_AV_FEAT_PROTECT) ? 0 : AVDT_NSC_SECURITY);
        APPL_TRACE_DEBUG("nsc_mask: 0x%x", cs.nsc_mask);

        if (p_data->api_reg.p_service_name[0] == 0)
        {
            p_service_name = NULL;
        }
        else
        {
            p_service_name = p_data->api_reg.p_service_name;
        }

        p_scb->suspend_sup  = TRUE;
        p_scb->recfg_sup    = TRUE;
        p_scb->skip_sdp     = FALSE;

        cs.p_ctrl_cback  = bta_av_dt_cback[p_scb->hdi];
        if(registr.chnl == BTA_AV_CHNL_AUDIO)
        {
            /* set up the audio stream control block */
            APPL_TRACE_EVENT("AV: set up the audio stream control block ");
            p_scb->p_act_tbl = (const tBTA_AV_ACT *)bta_av_a2d_action;
            p_scb->p_cos     = &bta_av_a2d_cos;
            p_scb->media_type= AVDT_MEDIA_AUDIO;
            cs.cfg.psc_mask  = AVDT_PSC_TRANS;
            cs.media_type    = AVDT_MEDIA_AUDIO;
            cs.mtu           = p_bta_av_cfg->audio_mtu;
            cs.flush_to      = L2CAP_DEFAULT_FLUSH_TO;
#if AVDT_REPORTING == TRUE
            if(bta_av_cb.features & BTA_AV_FEAT_REPORT)
            {
                cs.cfg.psc_mask |= AVDT_PSC_REPORT;
                cs.p_report_cback = bta_av_a2dp_report_cback;
#if AVDT_MULTIPLEXING == TRUE
                cs.cfg.mux_tsid_report = 2;
#endif
            }
#endif
            if(bta_av_cb.features & BTA_AV_FEAT_DELAY_RPT)
                cs.cfg.psc_mask |= AVDT_PSC_DELAY_RPT;

            if (profile_initialized == UUID_SERVCLASS_AUDIO_SOURCE)
            {
                cs.tsep = AVDT_TSEP_SRC;
                startIndex = BTIF_SV_AV_AA_SBC_INDEX;
                endIndex = BTIF_SV_AV_AA_SRC_SEP_INDEX;
            }
            else if (profile_initialized == UUID_SERVCLASS_AUDIO_SINK)
            {
                cs.tsep = AVDT_TSEP_SNK;
                cs.p_data_cback = bta_av_stream_data_cback;
                startIndex = BTIF_SV_AV_AA_SBC_SINK_INDEX;
                endIndex = BTIF_SV_AV_AA_SNK_SEP_INDEX;
            }

            /* Initialize Handles to zero */
            for(index = 0; index < (endIndex - startIndex); index++)
            {
                p_scb->seps[index].av_handle = 0;
            }

            /* keep the configuration in the stream control block */
            memcpy(&p_scb->cfg, &cs.cfg, sizeof(tAVDT_CFG));
            index = startIndex;
            while (index < endIndex &&
                   (*bta_av_a2d_cos.init)(&codec_type, cs.cfg.codec_info,
                    &cs.cfg.num_protect, cs.cfg.protect_info, index) == TRUE)
            {
                UINT8* ptr = cs.cfg.codec_info;
                tA2D_APTX_CIE* codecInfo = (tA2D_APTX_CIE*) &ptr[BTA_AV_CFG_START_IDX];
                UINT32 vendorId = codecInfo->vendorId;
                UINT16 codecId = codecInfo->codecId;

                if ((*bta_av_a2d_cos.offload)() ==  TRUE)
                {
                    if(codec_type == A2D_NON_A2DP_MEDIA_CT)
                    {
                       if (vendorId == A2D_APTX_VENDOR_ID &&
                           codecId == A2D_APTX_CODEC_ID_BLUETOOTH)
                       {
                           if((*bta_av_a2d_cos.cap)(APTX) != TRUE)
                           {
                               APPL_TRACE_DEBUG("%s: aptx-Classic offload codec not supported",__func__);
                               index++;
                               continue;
                           }
                           else
                               APPL_TRACE_DEBUG("%s:aptx-Classic offload codec supported",__func__)
                       } else {
                           if (codecId == A2D_APTX_HD_CODEC_ID_BLUETOOTH &&
                                  vendorId == A2D_APTX_HD_VENDOR_ID ) {
                               if((*bta_av_a2d_cos.cap)(APTXHD) != TRUE)
                               {
                                   APPL_TRACE_DEBUG("%s: aptx-HD offload codec not supported",__func__)
                                   index++;
                                   continue;
                               }
                               else
                                   APPL_TRACE_DEBUG("%s: aptx-HD offload codec supported",__func__)

                           }
                       }
                    }
                    else if (codec_type == AAC)
                    {
                        if ((*bta_av_a2d_cos.cap)(AAC) != TRUE)
                        {
                            APPL_TRACE_DEBUG("%s: AAC offload codec not supported",__func__);
                            index++;
                            continue;
                        } else {
                            APPL_TRACE_DEBUG("%s: AAC offload codec supported",__func__);
                        }
                    }
                } else if (codec_type == A2D_NON_A2DP_MEDIA_CT) {
                    if ((codecId == A2D_APTX_CODEC_ID_BLUETOOTH && vendorId == A2D_APTX_VENDOR_ID)
                        && (A2D_check_and_init_aptX() == false)) {
                        APPL_TRACE_WARNING("%s aptX not available ", __func__);
                        index++;
                        continue;

                    } else {
                        char value[PROPERTY_VALUE_MAX];
                        bool enableAptXHD = false;
                        if (property_get("persist.bt.enableAptXHD", value, "false") && strcmp(value, "true") == 0)
                            enableAptXHD = true;
                        else
                            APPL_TRACE_WARNING("%s enableAptXHD property is not set", __func__);

                        if ((codecId == A2D_APTX_HD_CODEC_ID_BLUETOOTH && vendorId == A2D_APTX_HD_VENDOR_ID) &&
                            ((enableAptXHD == false) || (A2D_check_and_init_aptX_HD() == false))) {
                             APPL_TRACE_WARNING("%s aptX-HD not available", __func__);
                             index++;
                             continue;
                        }
                    }
                } else if (codec_type == AAC) {
                    //Don't add AAC in Non split mode
                    index++;
                    continue;
                }

                /* Fix for below klockwork Issues
                 * Array 'seps' of size 2 may use index value(s) 2 */
                if (((index - startIndex) < BTA_AV_MAX_SEPS) &&
                    (AVDT_CreateStream(&p_scb->seps[index - startIndex].av_handle, &cs) ==
                                                                             AVDT_SUCCESS))
                {
                   if ((profile_initialized == UUID_SERVCLASS_AUDIO_SOURCE) &&
                       ((index == BTIF_SV_AV_AA_APTX_INDEX) || (index == BTIF_SV_AV_AA_APTX_HD_INDEX)))
                   {
                       p_scb->seps[index - startIndex].vendorId = vendorId;
                       p_scb->seps[index - startIndex].codecId = codecId;
                       APPL_TRACE_DEBUG("%s audio[%x] vendorId: %x codecId: %x", __func__,
                            index, p_scb->seps[index - startIndex].vendorId,
                            p_scb->seps[index - startIndex].codecId);
                    }
                    p_scb->seps[index - startIndex].codec_type = codec_type;
                    p_scb->seps[index - startIndex].tsep = cs.tsep;
                    p_scb->seps[index - startIndex].p_app_data_cback =
                        p_data->api_reg.p_app_data_cback;
                    index++;
                } else
                    break;
            }

            if(!bta_av_cb.reg_audio)
            {
                bta_av_cb.sdp_a2d_handle = 0;
#if (BTA_AV_SINK_INCLUDED == TRUE)
                bta_av_cb.sdp_a2d_snk_handle = 0;
#endif
                if (profile_initialized == UUID_SERVCLASS_AUDIO_SOURCE)
                {
                    /* create the SDP records on the 1st audio channel */
                    bta_av_cb.sdp_a2d_handle = SDP_CreateRecord();
                    A2D_AddRecord(UUID_SERVCLASS_AUDIO_SOURCE, p_service_name, NULL,
                                  A2D_SUPF_PLAYER, bta_av_cb.sdp_a2d_handle);
                    bta_sys_add_uuid(UUID_SERVCLASS_AUDIO_SOURCE);
                }
                else if (profile_initialized == UUID_SERVCLASS_AUDIO_SINK)
                {
#if (BTA_AV_SINK_INCLUDED == TRUE)
                    bta_av_cb.sdp_a2d_snk_handle = SDP_CreateRecord();
                    A2D_AddRecord(UUID_SERVCLASS_AUDIO_SINK, p_service_name, NULL,
                                  A2D_SUPF_PLAYER, bta_av_cb.sdp_a2d_snk_handle);
                    bta_sys_add_uuid(UUID_SERVCLASS_AUDIO_SINK);
#endif
                }

                /* start listening when A2DP is registered */
                if (bta_av_cb.features & BTA_AV_FEAT_RCTG)
                    bta_av_rc_create(&bta_av_cb, AVCT_ACP, p_scb->hdi, BTA_AV_NUM_LINKS + 1);

                /* if the AV and AVK are both supported, it cannot support the CT role */
                if (bta_av_cb.features & (BTA_AV_FEAT_RCCT))
                {
                    /* if TG is not supported, we need to register to AVCT now */
                    if ((bta_av_cb.features & (BTA_AV_FEAT_RCTG)) == 0)
                    {
#if( defined BTA_AR_INCLUDED ) && (BTA_AR_INCLUDED == TRUE)
#if (BTA_AV_WITH_AVCTP_AUTHORIZATION == TRUE)
                        bta_ar_reg_avct(p_bta_av_cfg->avrc_mtu, p_bta_av_cfg->avrc_br_mtu,
                                        bta_av_cb.sec_mask, BTA_ID_AV);
#else
                        bta_ar_reg_avct(p_bta_av_cfg->avrc_mtu, p_bta_av_cfg->avrc_br_mtu,
                                        (UINT8)(bta_av_cb.sec_mask & (~BTA_SEC_AUTHORIZE)), BTA_ID_AV);
#endif
#endif
                        bta_av_rc_create(&bta_av_cb, AVCT_ACP, 0, BTA_AV_NUM_LINKS + 1);
                    }
#if( defined BTA_AR_INCLUDED ) && (BTA_AR_INCLUDED == TRUE)
                    /* create an SDP record as AVRC CT. We create 1.3 for SOURCE
                     * because we rely on feature bits being scanned by external
                     * devices more than the profile version itself.
                     */
                    if (profile_initialized == UUID_SERVCLASS_AUDIO_SOURCE)
                    {
                        bta_ar_reg_avrc(UUID_SERVCLASS_AV_REMOTE_CONTROL, NULL, NULL,
                        p_bta_av_cfg->avrc_ct_cat, BTA_ID_AV,
                        (bta_av_cb.features & BTA_AV_FEAT_BROWSE), AVRC_REV_1_4);
                    }
                    if (profile_initialized == UUID_SERVCLASS_AUDIO_SINK)
                    {
                        bta_ar_reg_avrc(UUID_SERVCLASS_AV_REMOTE_CONTROL, NULL, NULL,
                        p_bta_av_cfg->avrc_ct_cat, BTA_ID_AV,
                        (bta_av_cb.features & BTA_AV_FEAT_BROWSE), AVRC_REV_1_3);
                    }
#endif
                }
            }
            bta_av_cb.reg_audio |= BTA_AV_HNDL_TO_MSK(p_scb->hdi);
            APPL_TRACE_DEBUG("reg_audio: 0x%x", bta_av_cb.reg_audio);
        }
        else
        {
            bta_av_cb.reg_video = BTA_AV_HNDL_TO_MSK(p_scb->hdi);
            bta_av_cb.sdp_vdp_handle = SDP_CreateRecord();
            /* register the video channel */
            /* no need to verify the function pointer here. it's verified prior */
            (*p_bta_av_cfg->p_reg)(&cs, p_service_name, p_scb);
        }
    } while (0);

    /* call callback with register event */
    (*bta_av_cb.p_cback)(BTA_AV_REGISTER_EVT, (tBTA_AV *)&registr);
}

/*******************************************************************************
**
** Function         bta_av_api_deregister
**
** Description      de-register a channel
**
**
** Returns          void
**
*******************************************************************************/
void bta_av_api_deregister(tBTA_AV_DATA *p_data)
{
    tBTA_AV_SCB *p_scb = bta_av_hndl_to_scb(p_data->hdr.layer_specific);

    A2D_close_aptX();

    if(p_scb)
    {
        p_scb->deregistring = TRUE;
        bta_av_ssm_execute(p_scb, BTA_AV_API_CLOSE_EVT, p_data);
    }
    else
    {
        bta_av_dereg_comp(p_data);
    }
}

/*******************************************************************************
**
** Function         bta_av_ci_data
**
** Description      forward the BTA_AV_CI_SRC_DATA_READY_EVT to stream state machine
**
**
** Returns          void
**
*******************************************************************************/
static void bta_av_ci_data(tBTA_AV_DATA *p_data)
{
    tBTA_AV_SCB *p_scb;
    int     i;
    UINT8   chnl = (UINT8)p_data->hdr.layer_specific;
    for( i=0; i < BTA_AV_NUM_STRS; i++ )
    {
        p_scb = bta_av_cb.p_scb[i];
        //Check if the Stream is in Started state before sending data
        //in Dual Handoff mode, get SCB where START is done.
        if(p_scb && (p_scb->chnl == chnl) && (p_scb->started))
        {
            bta_av_ssm_execute(p_scb, BTA_AV_SRC_DATA_READY_EVT, p_data);
        }
    }
}

/*******************************************************************************
**
** Function         bta_av_rpc_conn
**
** Description      report report channel open
**
** Returns          void
**
*******************************************************************************/
#if (AVDT_REPORTING == TRUE)
static void bta_av_rpc_conn(tBTA_AV_DATA *p_data)
{
    UNUSED(p_data);
}
#endif

BOOLEAN bta_av_multiple_streams_started(void)
{
    int xx, stream_count = 0;

    for(xx = 0; xx < BTA_AV_NUM_STRS; xx++)
    {
        if((bta_av_cb.p_scb[xx] != NULL) && bta_av_cb.p_scb[xx]->started == TRUE)
        {
            stream_count++;
        }
    }
    return (stream_count > 1);
}

/*******************************************************************************
**
** Function         bta_av_api_to_ssm
**
** Description      forward the API request to stream state machine
**
**
** Returns          void
**
*******************************************************************************/
static void bta_av_api_to_ssm(tBTA_AV_DATA *p_data)
{
    int xx;
    UINT16 event = p_data->hdr.event - BTA_AV_FIRST_A2S_API_EVT + BTA_AV_FIRST_A2S_SSM_EVT;

    /* Multicast: Corner case handling for multicast state getting
     * updated for ACL connected during the stream start where both
     * streams are not yet started. We need to take care of this
     * during suspend to ensure we suspend both streams.
     */
    if ((is_multicast_enabled == TRUE) ||
        ((event == BTA_AV_AP_STOP_EVT) && (bta_av_multiple_streams_started() == TRUE)))
    {
        /* Send START request to all Open Stream connections.*/
        for(xx=0; xx<BTA_AV_NUM_STRS; xx++)
        {
            bta_av_ssm_execute(bta_av_cb.p_scb[xx], event, p_data);
        }
    }
    else
    {
        /*In Dual A2dp Handoff, process this fucntion on specific handles.*/
        APPL_TRACE_DEBUG("bta_av_api_to_ssm: on Handle 0x%x",p_data->hdr.layer_specific);
        bta_av_ssm_execute(bta_av_hndl_to_scb(p_data->hdr.layer_specific), event, p_data);
    }
}

/*******************************************************************************
**
** Function         bta_av_api_enable_multicast
**
** Description      Enable/Disable Avdtp multicast
**
**
** Returns          void
**
*******************************************************************************/
static void bta_av_api_enable_multicast(tBTA_AV_DATA *p_data)
{
    is_multicast_enabled = p_data->multicast_state.is_multicast_enabled;
    APPL_TRACE_DEBUG("is_multicast_enabled :%d", is_multicast_enabled);
}

/*******************************************************************************
**
** Function         bta_av_api_update_supp_codecs
**
** Description      Update Avdtp supported codecs
**
**
** Returns          void
**
*******************************************************************************/
static void bta_av_api_update_supp_codecs(tBTA_AV_DATA *p_data)
{
    APPL_TRACE_DEBUG("bta_av_api_update_supp_codecs: num_codec_configs : %d",
        p_data->update_supp_codecs.num_codec_configs);
    avdt_scb_update_supported_codecs(p_data->update_supp_codecs.codec_type,
        p_data->update_supp_codecs.vnd_id, p_data->update_supp_codecs.codec_id,
        p_data->update_supp_codecs.num_codec_configs,
        p_data->update_supp_codecs.codec_info, AVDT_TSEP_SRC);
}

/*******************************************************************************
**
** Function         bta_av_api_update_max_av_client
**
** Description      Update max simultaneous AV connections supported
**
** Returns          void
**
*******************************************************************************/
static void bta_av_api_update_max_av_clients(tBTA_AV_DATA *p_data)
{
    int bta_av_max_clients = p_data->max_av_clients.max_clients;
    APPL_TRACE_DEBUG("bta_av_max_clients:%d",bta_av_max_clients);
    AVDT_UpdateMaxAvClients(bta_av_max_clients);
}

/*******************************************************************************
**
** Function         bta_av_chk_start
**
** Description      if this is audio channel, check if more than one audio
**                  channel is connected & already started.
**
** Returns          TRUE, if need api_start
**
*******************************************************************************/
BOOLEAN bta_av_chk_start(tBTA_AV_SCB *p_scb)
{
    BOOLEAN start = FALSE;
    tBTA_AV_SCB *p_scbi;
    int i;
    APPL_TRACE_DEBUG("bta_av_chk_start: Audio open count: 0x%x",bta_av_cb.audio_open_cnt);
    if(p_scb->chnl == BTA_AV_CHNL_AUDIO)
    {
        if ((bta_av_cb.audio_open_cnt >= 2) &&
            ((0 == (p_scb->role & BTA_AV_ROLE_AD_ACP)) ||       /* Outgoing connection or   */
             (bta_av_cb.features & BTA_AV_FEAT_ACP_START)))     /* auto-starting option     */
        {
            /* more than one audio channel is connected */
            /* if this is the 2nd stream as ACP, give INT a chance to issue the START command */
            for(i=0; i<BTA_AV_NUM_STRS; i++)
            {
                p_scbi = bta_av_cb.p_scb[i];
                if(p_scbi && p_scbi->chnl == BTA_AV_CHNL_AUDIO && p_scbi->co_started)
                {
                    if (is_multicast_enabled == TRUE)
                    {
                        start = TRUE;
                    }
                    else
                    {
                        start = FALSE;
                        APPL_TRACE_DEBUG("bta_av_chk_start: Already playing");
                        break;
                    }
                    /* may need to update the flush timeout of this already started stream */
                    if(p_scbi->co_started != bta_av_cb.audio_open_cnt)
                    {
                        p_scbi->co_started = bta_av_cb.audio_open_cnt;
                        L2CA_SetFlushTimeout(p_scbi->peer_addr, p_bta_av_cfg->p_audio_flush_to[p_scbi->co_started - 1] );
                    }
                }
            }
        }
    }
    return start;
}

/*******************************************************************************
**
** Function         bta_av_restore_switch
**
** Description      assume that the caller of this function already makes
**                  sure that there's only one ACL connection left
**
** Returns          void
**
*******************************************************************************/
void bta_av_restore_switch (void)
{
    tBTA_AV_CB   *p_cb = &bta_av_cb;
    int     i;
    UINT8   mask;

    APPL_TRACE_DEBUG("reg_audio: 0x%x",bta_av_cb.reg_audio);
    for(i=0; i<BTA_AV_NUM_STRS; i++)
    {
        mask = BTA_AV_HNDL_TO_MSK(i);
        if (p_cb->conn_audio == mask)
        {
            if (p_cb->p_scb[i])
            {
                bta_sys_set_policy(BTA_ID_AV, HCI_ENABLE_MASTER_SLAVE_SWITCH, p_cb->p_scb[i]->peer_addr);
            }
            break;
        }
    }
}

/*******************************************************************************
**
** Function         bta_av_sys_rs_cback
**
** Description      Receives the role change event from dm
**
** Returns          (BTA_SYS_ROLE_CHANGE, new_role, hci_status, p_bda)
**
*******************************************************************************/
static void bta_av_sys_rs_cback (tBTA_SYS_CONN_STATUS status,UINT8 id, UINT8 app_id, BD_ADDR peer_addr)
{
    int         i;
    tBTA_AV_SCB *p_scb = NULL;
    UINT8       cur_role;
    UINT8       peer_idx = 0;
    UNUSED(status);

    APPL_TRACE_DEBUG("bta_av_sys_rs_cback: %d", bta_av_cb.rs_idx);
    for(i=0; i<BTA_AV_NUM_STRS; i++)
    {
        /* loop through all the SCBs to find matching peer addresses and report the role change event */
        /* note that more than one SCB (a2dp & vdp) maybe waiting for this event */
        p_scb = bta_av_cb.p_scb[i];
        if (p_scb && (bdcmp (peer_addr, p_scb->peer_addr) == 0)) {
            tBTA_AV_ROLE_RES *p_buf =
                (tBTA_AV_ROLE_RES *)osi_malloc(sizeof(tBTA_AV_ROLE_RES));
            APPL_TRACE_DEBUG("new_role:%d, hci_status:x%x hndl: x%x", id, app_id, p_scb->hndl);
            /*
            if ((id != BTM_ROLE_MASTER) && (app_id != HCI_SUCCESS))
            {
                bta_sys_set_policy(BTA_ID_AV, (HCI_ENABLE_MASTER_SLAVE_SWITCH|HCI_ENABLE_SNIFF_MODE), p_scb->peer_addr);
            }
            */
            p_buf->hdr.event = BTA_AV_ROLE_CHANGE_EVT;
            p_buf->hdr.layer_specific = p_scb->hndl;
            p_buf->new_role = id;
            p_buf->hci_status = app_id;
            bta_sys_sendmsg(p_buf);

            peer_idx = p_scb->hdi + 1;  /* Handle index for the peer_addr */
        }
    }

    /* restore role switch policy, if role switch failed */
    if ((HCI_SUCCESS != app_id) &&
        (BTM_GetRole (peer_addr, &cur_role) == BTM_SUCCESS) &&
        (cur_role == BTM_ROLE_SLAVE) )
    {
        bta_sys_set_policy(BTA_ID_AV, HCI_ENABLE_MASTER_SLAVE_SWITCH, peer_addr);
    }

    /* if BTA_AvOpen() was called for other device, which caused the role switch of the peer_addr,  */
    /* we need to continue opening process for the BTA_AvOpen().                                    */
    if ((bta_av_cb.rs_idx != 0) && (bta_av_cb.rs_idx != peer_idx))
    {
        if ((bta_av_cb.rs_idx -1) < BTA_AV_NUM_STRS)
        {
            p_scb = bta_av_cb.p_scb[bta_av_cb.rs_idx - 1];
        }
        if (p_scb && p_scb->q_tag == BTA_AV_Q_TAG_OPEN)
        {
            APPL_TRACE_DEBUG ("bta_av_sys_rs_cback: rs_idx(%d), hndl:x%x q_tag: %d",
                bta_av_cb.rs_idx, p_scb->hndl, p_scb->q_tag);
            /* Multicast:
             * As per Multicast feature implementation, fallback
             * happens to soft hand-off when DUT is in scatternet
             * scenario. Hence, don't fail the connection if
             * role switch fails because of remote disallowing.
             * Set switch_res to BTA_AV_RS_DONE on failure.
             */
            if(HCI_SUCCESS == app_id || HCI_ERR_NO_CONNECTION == app_id)
                p_scb->q_info.open.switch_res = BTA_AV_RS_OK;
            else
                p_scb->q_info.open.switch_res = BTA_AV_RS_DONE;

            /* Continue av open process */
            bta_av_do_disc_a2d (p_scb, (tBTA_AV_DATA *)&(p_scb->q_info.open));
        }

        bta_av_cb.rs_idx = 0;
    }
}

/*******************************************************************************
**
** Function         bta_av_sco_chg_cback
**
** Description      receive & process the SCO connection up/down event from sys.
**                  call setup also triggers this callback, to suspend av before sco
**                  activity happens, or to resume av once call ends.
**
** Returns          void
**
*******************************************************************************/
static void bta_av_sco_chg_cback(tBTA_SYS_CONN_STATUS status, UINT8 id, UINT8
                                 app_id, BD_ADDR peer_addr)
{
    tBTA_AV_SCB *p_scb;
    int     i;
    tBTA_AV_API_STOP stop;
    UNUSED(app_id);
    UNUSED(peer_addr);

    APPL_TRACE_DEBUG("bta_av_sco_chg_cback:%d status:%d", id, status);
    if(id)
    {
        bta_av_cb.sco_occupied = TRUE;

        /* either BTA_SYS_SCO_OPEN or BTA_SYS_SCO_CLOSE with remaining active SCO */
        for(i=0; i<BTA_AV_NUM_STRS; i++)
        {
            p_scb = bta_av_cb.p_scb[i];

            if( p_scb && p_scb->co_started && (p_scb->sco_suspend == FALSE))
            {
                APPL_TRACE_DEBUG("suspending scb:%d", i);
                /* scb is used and started, not suspended automatically */
                p_scb->sco_suspend = TRUE;
                stop.flush   = FALSE;
                stop.suspend = TRUE;
                bta_av_ssm_execute(p_scb, BTA_AV_AP_STOP_EVT, (tBTA_AV_DATA *)&stop);
            }
        }
    }
    else
    {
        bta_av_cb.sco_occupied = FALSE;

        for(i=0; i<BTA_AV_NUM_STRS; i++)
        {
            p_scb = bta_av_cb.p_scb[i];

            if( p_scb && p_scb->sco_suspend ) /* scb is used and suspended for SCO */
            {
                APPL_TRACE_DEBUG("starting scb:%d", i);
                bta_av_ssm_execute(p_scb, BTA_AV_AP_START_EVT, NULL);
            }
        }
    }
}

/*******************************************************************************
**
** Function         bta_av_switch_if_needed
**
** Description      This function checks if there is another existing AV
**                  channel that is local as slave role.
**                  If so, role switch and remove it from link policy.
**
** Returns          TRUE, if role switch is done
**
*******************************************************************************/
BOOLEAN bta_av_switch_if_needed(tBTA_AV_SCB *p_scb)
{
    UINT8 role;
    BOOLEAN needed = FALSE;
    tBTA_AV_SCB *p_scbi;
    tBTM_STATUS ret;
    int i;
    UINT8       mask;

    for(i=0; i<BTA_AV_NUM_STRS; i++)
    {
        mask = BTA_AV_HNDL_TO_MSK(i);
        p_scbi = bta_av_cb.p_scb[i];
        if( p_scbi && (p_scb->hdi != i) && /* not the original channel */
            ((bta_av_cb.conn_audio & mask) ||/* connected audio */
            (bta_av_cb.conn_video & mask)) ) /* connected video */
        {
            BTM_GetRole(p_scbi->peer_addr, &role);
            /* this channel is open - clear the role switch link policy for this link */
            if(BTM_ROLE_MASTER != role)
            {
                if (bta_av_cb.features & BTA_AV_FEAT_MASTER)
                    bta_sys_clear_policy(BTA_ID_AV, HCI_ENABLE_MASTER_SLAVE_SWITCH, p_scbi->peer_addr);
                ret = BTM_SwitchRole(p_scbi->peer_addr, BTM_ROLE_MASTER, NULL);
                if ((ret == BTM_REPEATED_ATTEMPTS) ||
                   ((ret == BTM_NO_RESOURCES) && btm_is_sco_active_by_bdaddr(p_scbi->peer_addr)))
                    return FALSE;

                if (BTM_CMD_STARTED != ret)
                {
                    /* can not switch role on SCBI
                     * start the timer on SCB - because this function is ONLY called when SCB gets API_OPEN */
                    bta_sys_start_timer(p_scb->avrc_ct_timer,
                                        BTA_AV_RS_TIME_VAL,
                                        BTA_AV_AVRC_TIMER_EVT, p_scb->hndl);
                }
                needed = TRUE;
                /* mark the original channel as waiting for RS result */
                bta_av_cb.rs_idx = p_scb->hdi + 1;
                break;
            }
        }
    }
    return needed;
}

/*******************************************************************************
**
** Function         bta_av_link_role_ok
**
** Description      This function checks if the SCB has existing ACL connection
**                  If so, check if the link role fits the requirements.
**
** Returns          TRUE, if role is ok
**
*******************************************************************************/
BOOLEAN bta_av_link_role_ok(tBTA_AV_SCB *p_scb, UINT8 bits)
{
    UINT8 role;
    BOOLEAN is_ok = TRUE;
    tBTM_STATUS ret;

    if (BTM_GetRole(p_scb->peer_addr, &role) == BTM_SUCCESS)
    {
        LOG_INFO("%s hndl:x%x role:%d conn_audio:x%x bits:%d features:x%x",
                __func__, p_scb->hndl, role, bta_av_cb.conn_audio, bits,
                bta_av_cb.features);
        if (BTM_ROLE_MASTER != role && (A2D_BitsSet(bta_av_cb.conn_audio) > bits || (bta_av_cb.features & BTA_AV_FEAT_MASTER)))
        {
            if (bta_av_cb.features & BTA_AV_FEAT_MASTER)
                bta_sys_clear_policy(BTA_ID_AV, HCI_ENABLE_MASTER_SLAVE_SWITCH, p_scb->peer_addr);
            ret = BTM_SwitchRole(p_scb->peer_addr, BTM_ROLE_MASTER, NULL);
            /* We have already reached maximum attempts,
             * If we try again it will anyways fail
             * return from here
             * */
            if ((ret == BTM_REPEATED_ATTEMPTS) ||
                ((ret == BTM_NO_RESOURCES) && btm_is_sco_active_by_bdaddr(p_scb->peer_addr)))
                return TRUE;

            if (BTM_CMD_STARTED != ret)
            {
                /* can not switch role on SCB - start the timer on SCB */
            }
            is_ok = FALSE;
            p_scb->wait |= BTA_AV_WAIT_ROLE_SW_RES_START;

        }
    }

    return is_ok;
}

/*******************************************************************************
**
** Function         bta_av_chk_mtu
**
** Description      if this is audio channel, check if more than one audio
**                  channel is connected.
**
** Returns          The smallest mtu of the connected audio channels
**
*******************************************************************************/
UINT16 bta_av_chk_mtu(tBTA_AV_SCB *p_scb, UINT16 mtu)
{
    UINT16 ret_mtu = BTA_AV_MAX_A2DP_MTU;
    tBTA_AV_SCB *p_scbi;
    int i;
    UINT8   mask;
    UNUSED(mtu);

    /* TODO_MV mess with the mtu according to the number of EDR/non-EDR headsets */
    if(p_scb->chnl == BTA_AV_CHNL_AUDIO)
    {
        if(bta_av_cb.audio_open_cnt >= 2)
        {
            /* more than one audio channel is connected */
            for(i=0; i<BTA_AV_NUM_STRS; i++)
            {
                p_scbi = bta_av_cb.p_scb[i];
                if((p_scb != p_scbi) && p_scbi && (p_scbi->chnl == BTA_AV_CHNL_AUDIO) )
                {
                    mask = BTA_AV_HNDL_TO_MSK(i);
                    APPL_TRACE_DEBUG("[%d] mtu: %d, mask:0x%x",
                        i, p_scbi->stream_mtu, mask);
                    if(bta_av_cb.conn_audio & mask)
                    {
                        if(ret_mtu > p_scbi->stream_mtu)
                            ret_mtu = p_scbi->stream_mtu;
                    }
                }
            }
        }
        APPL_TRACE_DEBUG("bta_av_chk_mtu audio count:%d, conn_audio:0x%x, ret:%d",
            bta_av_cb.audio_open_cnt, bta_av_cb.conn_audio, ret_mtu);
    }
    return ret_mtu;
}

/*******************************************************************************
**
** Function         bta_av_dup_audio_buf
**
** Description      dup the audio data to the q_info.a2d of other audio channels
**
** Returns          void
**
*******************************************************************************/
void bta_av_dup_audio_buf(tBTA_AV_SCB *p_scb, BT_HDR *p_buf)
{
    /* Test whether there is more than one audio channel connected */
    if ((p_buf == NULL) || (bta_av_cb.audio_open_cnt < 2))
        return;

    UINT16 copy_size = BT_HDR_SIZE + p_buf->len + p_buf->offset;
    for (int i = 0; i < BTA_AV_NUM_STRS; i++) {
        tBTA_AV_SCB *p_scbi = bta_av_cb.p_scb[i];

        if (i == p_scb->hdi)
            continue;           /* Ignore the original channel */
        if ((p_scbi == NULL) || !p_scbi->co_started)
            continue;           /* Ignore if SCB is not used or started */
        if (!(bta_av_cb.conn_audio & BTA_AV_HNDL_TO_MSK(i)))
            continue;           /* Audio is not connected */

        /* Enqueue the data */
        BT_HDR *p_new = (BT_HDR *)osi_malloc(copy_size);
        memcpy(p_new, p_buf, copy_size);
        list_append(p_scbi->a2d_list, p_new);

        if (list_length(p_scbi->a2d_list) > p_bta_av_cfg->audio_mqs) {
            // Drop the oldest packet
            bta_av_co_audio_drop(p_scbi->hndl);
            BT_HDR *p_buf_drop = list_front(p_scbi->a2d_list);
            list_remove(p_scbi->a2d_list, p_buf_drop);
            osi_free(p_buf_drop);
        }
    }
}

/*******************************************************************************
**
** Function         bta_av_sm_execute
**
** Description      State machine event handling function for AV
**
**
** Returns          void
**
*******************************************************************************/
void bta_av_sm_execute(tBTA_AV_CB *p_cb, UINT16 event, tBTA_AV_DATA *p_data)
{
    tBTA_AV_ST_TBL      state_table;
    UINT8               action;

#if (defined(BTA_AV_DEBUG) && BTA_AV_DEBUG == TRUE)
    APPL_TRACE_EVENT("AV event=0x%x(%s) state=%d(%s)",
        event, bta_av_evt_code(event), p_cb->state, bta_av_st_code(p_cb->state));
#else
    APPL_TRACE_EVENT("AV event=0x%x state=%d", event, p_cb->state);
#endif

    /* look up the state table for the current state */
    state_table = bta_av_st_tbl[p_cb->state];

    event &= 0x00FF;

    /* set next state */
    p_cb->state = state_table[event][BTA_AV_NEXT_STATE];
    APPL_TRACE_EVENT("next state=%d", p_cb->state);

    /* execute action functions */
    if ((action = state_table[event][BTA_AV_ACTION_COL]) != BTA_AV_IGNORE)
    {
        (*bta_av_action[action])(p_cb, p_data);
    }
}

/*******************************************************************************
**
** Function         bta_av_hdl_event
**
** Description      Advanced audio/video main event handling function.
**
**
** Returns          BOOLEAN
**
*******************************************************************************/
BOOLEAN bta_av_hdl_event(BT_HDR *p_msg)
{
    UINT16 event = p_msg->event;
    UINT16 first_event = BTA_AV_FIRST_NSM_EVT;

    if (event > BTA_AV_LAST_EVT)
    {
        return TRUE; /* to free p_msg */
    }

    if(event >= first_event)
    {
#if (defined(BTA_AV_DEBUG) && BTA_AV_DEBUG == TRUE)
        APPL_TRACE_VERBOSE("AV nsm event=0x%x(%s)", event, bta_av_evt_code(event));
#else
        APPL_TRACE_VERBOSE("AV nsm event=0x%x", event);
#endif
        /* non state machine events */

        (*bta_av_nsm_act[event - BTA_AV_FIRST_NSM_EVT]) ((tBTA_AV_DATA *) p_msg);
    }
    else if (event >= BTA_AV_FIRST_SM_EVT && event <= BTA_AV_LAST_SM_EVT)
    {
#if (defined(BTA_AV_DEBUG) && BTA_AV_DEBUG == TRUE)
        APPL_TRACE_VERBOSE("AV sm event=0x%x(%s)", event, bta_av_evt_code(event));
#else
        APPL_TRACE_VERBOSE("AV sm event=0x%x", event);
#endif
        /* state machine events */
        bta_av_sm_execute(&bta_av_cb, p_msg->event, (tBTA_AV_DATA *) p_msg);
    }
    else
    {
        APPL_TRACE_VERBOSE("handle=0x%x", p_msg->layer_specific);
        /* stream state machine events */
        bta_av_ssm_execute( bta_av_hndl_to_scb(p_msg->layer_specific),
                                p_msg->event, (tBTA_AV_DATA *) p_msg);
    }
    return TRUE;
}

/*******************************************************************************
**
** Function         bta_av_is_multicast_enabled
**
** Description      return status of Avdtp multicast
**
**
** Returns          BOOLEAN
**
*******************************************************************************/
BOOLEAN bta_av_is_multicast_enabled()
{
    return is_multicast_enabled;
}
/*****************************************************************************
**  Debug Functions
*****************************************************************************/
#if (defined(BTA_AV_DEBUG) && BTA_AV_DEBUG == TRUE)
/*******************************************************************************
**
** Function         bta_av_st_code
**
** Description
**
** Returns          char *
**
*******************************************************************************/
static char *bta_av_st_code(UINT8 state)
{
    switch(state)
    {
    case BTA_AV_INIT_ST: return "INIT";
    case BTA_AV_OPEN_ST: return "OPEN";
    default:             return "unknown";
    }
}
/*******************************************************************************
**
** Function         bta_av_evt_code
**
** Description
**
** Returns          char *
**
*******************************************************************************/
char *bta_av_evt_code(UINT16 evt_code)
{
    switch(evt_code)
    {
    case BTA_AV_API_DISABLE_EVT: return "API_DISABLE";
    case BTA_AV_API_REMOTE_CMD_EVT: return "API_REMOTE_CMD";
    case BTA_AV_API_VENDOR_CMD_EVT: return "API_VENDOR_CMD";
    case BTA_AV_API_VENDOR_RSP_EVT: return "API_VENDOR_RSP";
    case BTA_AV_API_META_RSP_EVT: return "API_META_RSP_EVT";
    case BTA_AV_API_RC_CLOSE_EVT: return "API_RC_CLOSE";
    case BTA_AV_AVRC_OPEN_EVT: return "AVRC_OPEN";
    case BTA_AV_AVRC_MSG_EVT: return "AVRC_MSG";
    case BTA_AV_AVRC_NONE_EVT: return "AVRC_NONE";

    case BTA_AV_API_OPEN_EVT: return "API_OPEN";
    case BTA_AV_API_CLOSE_EVT: return "API_CLOSE";
    case BTA_AV_AP_START_EVT: return "AP_START";
    case BTA_AV_AP_STOP_EVT: return "AP_STOP";
    case BTA_AV_API_RECONFIG_EVT: return "API_RECONFIG";
    case BTA_AV_API_PROTECT_REQ_EVT: return "API_PROTECT_REQ";
    case BTA_AV_API_PROTECT_RSP_EVT: return "API_PROTECT_RSP";
    case BTA_AV_API_RC_OPEN_EVT: return "API_RC_OPEN";
    case BTA_AV_SRC_DATA_READY_EVT: return "SRC_DATA_READY";
    case BTA_AV_CI_SETCONFIG_OK_EVT: return "CI_SETCONFIG_OK";
    case BTA_AV_CI_SETCONFIG_FAIL_EVT: return "CI_SETCONFIG_FAIL";
    case BTA_AV_SDP_DISC_OK_EVT: return "SDP_DISC_OK";
    case BTA_AV_SDP_DISC_FAIL_EVT: return "SDP_DISC_FAIL";
    case BTA_AV_STR_DISC_OK_EVT: return "STR_DISC_OK";
    case BTA_AV_STR_DISC_FAIL_EVT: return "STR_DISC_FAIL";
    case BTA_AV_STR_GETCAP_OK_EVT: return "STR_GETCAP_OK";
    case BTA_AV_STR_GETCAP_FAIL_EVT: return "STR_GETCAP_FAIL";
    case BTA_AV_STR_OPEN_OK_EVT: return "STR_OPEN_OK";
    case BTA_AV_STR_OPEN_FAIL_EVT: return "STR_OPEN_FAIL";
    case BTA_AV_STR_START_OK_EVT: return "STR_START_OK";
    case BTA_AV_STR_START_FAIL_EVT: return "STR_START_FAIL";
    case BTA_AV_STR_CLOSE_EVT: return "STR_CLOSE";
    case BTA_AV_STR_CONFIG_IND_EVT: return "STR_CONFIG_IND";
    case BTA_AV_STR_SECURITY_IND_EVT: return "STR_SECURITY_IND";
    case BTA_AV_STR_SECURITY_CFM_EVT: return "STR_SECURITY_CFM";
    case BTA_AV_STR_WRITE_CFM_EVT: return "STR_WRITE_CFM";
    case BTA_AV_STR_SUSPEND_CFM_EVT: return "STR_SUSPEND_CFM";
    case BTA_AV_STR_RECONFIG_CFM_EVT: return "STR_RECONFIG_CFM";
    case BTA_AV_AVRC_TIMER_EVT: return "AVRC_TIMER";
    case BTA_AV_AVDT_CONNECT_EVT: return "AVDT_CONNECT";
    case BTA_AV_AVDT_DISCONNECT_EVT: return "AVDT_DISCONNECT";
    case BTA_AV_ROLE_CHANGE_EVT: return "ROLE_CHANGE";
    case BTA_AV_AVDT_DELAY_RPT_EVT: return "AVDT_DELAY_RPT";
    case BTA_AV_ACP_CONNECT_EVT: return "ACP_CONNECT";

    case BTA_AV_API_ENABLE_EVT: return "API_ENABLE";
    case BTA_AV_API_REGISTER_EVT: return "API_REG";
    case BTA_AV_API_DEREGISTER_EVT: return "API_DEREG";
    case BTA_AV_API_DISCONNECT_EVT: return "API_DISCNT";
    case BTA_AV_CI_SRC_DATA_READY_EVT: return "CI_DATA_READY";
    case BTA_AV_SIG_CHG_EVT: return "SIG_CHG";
    case BTA_AV_SIGNALLING_TIMER_EVT: return "SIGNALLING_TIMER";
    case BTA_AV_SDP_AVRC_DISC_EVT: return "SDP_AVRC_DISC";
    case BTA_AV_AVRC_CLOSE_EVT: return "AVRC_CLOSE";
    case BTA_AV_CONN_CHG_EVT: return "CONN_CHG";
    case BTA_AV_DEREG_COMP_EVT: return "DEREG_COMP";
#if (BTA_AV_SINK_INCLUDED == TRUE)
    case BTA_AV_API_SINK_ENABLE_EVT: return "SINK_ENABLE";
#endif
#if (AVDT_REPORTING == TRUE)
    case BTA_AV_AVDT_RPT_CONN_EVT: return "RPT_CONN";
#endif
    case BTA_AV_API_START_EVT: return "API_START";
    case BTA_AV_API_STOP_EVT: return "API_STOP";
    case BTA_AV_UPDATE_SUPP_CODECS: return "UPDATE_SUPPORTED_CODECS";
    case BTA_AV_ENABLE_MULTICAST_EVT: return "MULTICAST_ENABLE";
    default:             return "unknown";
    }
}
#endif

#endif /* BTA_AV_INCLUDED */
