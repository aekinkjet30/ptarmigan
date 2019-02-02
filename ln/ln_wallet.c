/*
 *  Copyright (C) 2017, Nayuta, Inc. All Rights Reserved
 *  SPDX-License-Identifier: Apache-2.0
 *
 *  Licensed to the Apache Software Foundation (ASF) under one
 *  or more contributor license agreements.  See the NOTICE file
 *  distributed with this work for additional information
 *  regarding copyright ownership.  The ASF licenses this file
 *  to you under the Apache License, Version 2.0 (the
 *  "License"); you may not use this file except in compliance
 *  with the License.  You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an
 *  "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *  KIND, either express or implied.  See the License for the
 *  specific language governing permissions and limitations
 *  under the License.
 */
/** @file   ln_close.c
 *  @brief  ln_close
 */
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <assert.h>

/*
#include "utl_str.h"
#include "utl_buf.h"
#include "utl_time.h"
#include "utl_int.h"

#include "btc_crypto.h"
#include "btc_script.h"

#include "ln_db.h"
#include "ln_comtx.h"
#include "ln_derkey.h"
#include "ln_script.h"
#include "ln.h"
#include "ln_msg_close.h"
#include "ln_local.h"
#include "ln_setupctl.h"
#include "ln_anno.h"
#include "ln_close.h"
*/

#include "utl_dbg.h"

#include "btc_sw.h"

#include "ln_signer.h"
#include "ln_wallet.h"


/**************************************************************************
 * macros
 **************************************************************************/
/**************************************************************************
 * prototypes
 **************************************************************************/

static bool create_base_tx(
    btc_tx_t *pTx, uint64_t Value, const utl_buf_t *pScriptPk, uint32_t LockTime, const uint8_t *pTxid, int Index, bool bRevoked);


/**************************************************************************
 * public functions
 **************************************************************************/

bool ln_wallet_create_to_local(const ln_channel_t *pChannel, btc_tx_t *pTx, uint64_t Value, uint32_t ToSelfDelay,
                const utl_buf_t *pScript, const uint8_t *pTxid, int Index, bool bRevoked)
{
    bool ret = create_base_tx(pTx, Value,
                NULL, ToSelfDelay, pTxid, Index, bRevoked);
    if (ret) {
        btc_keys_t sigkey;
        ln_signer_to_local_key(
            &sigkey, &pChannel->keys_local, &pChannel->keys_remote, bRevoked ? pChannel->revoked_sec.buf : NULL);
        ret = ln_wallet_script_to_local_set_vin0(pTx, &sigkey, pScript, bRevoked);
    }
    return ret;
}


bool ln_wallet_create_to_remote(
            const ln_channel_t *pChannel, btc_tx_t *pTx, uint64_t Value,
            const uint8_t *pTxid, int Index)
{
    bool ret = create_base_tx(pTx, Value,
                NULL, 0, pTxid, Index, false);
    if (ret) {
        btc_keys_t sigkey;
        ln_signer_to_remote_key(&sigkey, &pChannel->keys_local, &pChannel->keys_remote);
        ln_wallet_script_to_remote_set_vin0(pTx, &sigkey);
    }

    return ret;
}


bool HIDDEN ln_wallet_script_to_local_set_vin0(
    btc_tx_t *pTx,
    const btc_keys_t *pKey,
    const utl_buf_t *pWitScript,
    bool bRevoked)
{
    // <local_delayedsig>
    // 0
    // <witness script>

    // OR

    // <revocation_sig>
    // 1
    // <witness script>

    const utl_buf_t key = { (CONST_CAST uint8_t *)pKey->priv, BTC_SZ_PRIVKEY }; //XXX: privkey not sig (original form)
    const utl_buf_t zero = UTL_BUF_INIT;
    const utl_buf_t one = { (CONST_CAST uint8_t *)"\x01", 1 };
    const utl_buf_t *wit_items[] = { &key, (bRevoked) ? &one : &zero, pWitScript };
    if (!btc_sw_set_vin_p2wsh(pTx, 0, (const utl_buf_t **)wit_items, ARRAY_SIZE(wit_items))) return false;
    return true;
}


bool HIDDEN ln_wallet_script_to_remote_set_vin0(btc_tx_t *pTx, const btc_keys_t *pKey)
{
    utl_buf_t *p_wit_items = (utl_buf_t *)UTL_DBG_MALLOC(sizeof(utl_buf_t) * 2);
    if (!utl_buf_alloccopy(&p_wit_items[0], pKey->priv, BTC_SZ_PRIVKEY)) return false; //XXX: privkey not sig (original form)
    if (!utl_buf_alloccopy(&p_wit_items[1], pKey->pub, BTC_SZ_PUBKEY)) return false;
    pTx->vin[0].wit_item_cnt = 2;
    pTx->vin[0].witness = p_wit_items;
    return true;
}


/**************************************************************************
 * private functions
 **************************************************************************/

static bool create_base_tx(btc_tx_t *pTx,
    uint64_t Value, const utl_buf_t *pScriptPk, uint32_t LockTime, const uint8_t *pTxid, int Index, bool bRevoked)
{
    //vout
    btc_vout_t* vout = btc_tx_add_vout(pTx, Value);
    if (pScriptPk != NULL) {
        utl_buf_alloccopy(&vout->script, pScriptPk->buf, pScriptPk->len);
    }

    //vin
    btc_tx_add_vin(pTx, pTxid, Index);
    if (!bRevoked) {
        pTx->vin[0].sequence = LockTime;
    }

    return true;
}

