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
/** @file   btc_local.h
 *  @brief  libbtc内インターフェース
 */
#ifndef BTC_LOCAL_H__
#define BTC_LOCAL_H__

#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>

#include <stdio.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define LOG_TAG "BTC"
#include "utl_log.h"
#include "utl_common.h"

#include "btc.h"
#include "btc_tx.h"


/**************************************************************************
 * macros
 **************************************************************************/

/**************************************************************************
 * macro functions
 **************************************************************************/

/**************************************************************************
 * package variables
 **************************************************************************/

extern uint8_t  HIDDEN mPref[BTC_PREF_MAX];
extern bool     HIDDEN mNativeSegwit;


/**************************************************************************
 * prototypes
 **************************************************************************/

/** 圧縮された公開鍵をkeypairに展開する
 *
 * @param[in]       pPubKey     圧縮された公開鍵
 * @return      0   成功
 * @note
 *      - https://bitcointalk.org/index.php?topic=644919.0
 *      - https://gist.github.com/flying-fury/6bc42c8bb60e5ea26631
 */
int HIDDEN btcl_util_set_keypair(void *pKeyPair, const uint8_t *pPubKey);


/** Hash(PKH/SH/WPKH/WSH)をBitcoinアドレスに変換
 *
 * @param[out]      pAddr           変換後データ(#BTC_SZ_ADDR_STR_MAX+1 以上のサイズを想定)
 * @param[in]       pHash           対象データ(最大#BTC_SZ_HASH_MAX)
 * @param[in]       Prefix          BTC_PREF_xxx
 * @note
 *      - if Prefix == #BTC_PREF_P2PKH then pHash is PKH(#BTC_SZ_HASH160)
 *      - if Prefix == #BTC_PREF_P2SH then pHash is SH(#BTC_SZ_HASH160)
 *      - if Prefix == #BTC_PREF_P2WPKH then pHash is WPKH(#BTC_SZ_HASH160)
 *      - if Prefix == #BTC_PREF_P2WSH then pHash is WSH(#BTC_SZ_HASH256)
 */
bool HIDDEN btcl_util_keys_hash2addr(char *pAddr, const uint8_t *pHash, uint8_t Prefix);


/** トランザクションデータ作成
 *
 * @param[out]      pBuf            変換後データ
 * @param[in]       pTx             対象データ
 * @param[in]       enableSegWit    false:pTxがsegwitでも、witnessを作らない(TXID計算用)
 *
 * @note
 *      - 動的にメモリ確保するため、pBufは使用後 #utl_buf_free()で解放すること
 *      - vin cntおよびvout cntは 252までしか対応しない(varint型の1byteまで)
 */
bool HIDDEN btcl_util_create_tx(utl_buf_t *pBuf, const btc_tx_t *pTx, bool enableSegWit);


/** vout追加(pubkey)
 *
 */
bool HIDDEN btcl_util_add_vout_pub(btc_tx_t *pTx, uint64_t Value, const uint8_t *pPubKey, uint8_t Pref);


/** vout追加(pubkeyhash)
 *
 */
bool HIDDEN btcl_util_add_vout_pkh(btc_tx_t *pTx, uint64_t Value, const uint8_t *pPubKeyHash, uint8_t Pref);


#endif /* BTC_LOCAL_H__ */
