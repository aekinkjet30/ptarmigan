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
/** @file   ln_db_lmdb.c
 *  @brief  DB access(LMDB)
 */
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>
#include <ftw.h>
#include <stddef.h>

#include "utl_str.h"
#include "utl_dbg.h"
#include "utl_time.h"

#include "btc_crypto.h"
#include "btc_sw.h"
#include "btc_script.h"
#include "btc_dbg.h"

#include "ln_local.h"
#include "ln_msg_anno.h"
#include "ln_node.h"
#include "ln_signer.h"
#include "ln_db.h"
#include "ln_db_lmdb.h"


//#define M_DB_DEBUG

/********************************************************************
 * macros
 ********************************************************************/

//INIT_PARAM[]の添字
#define M_INITPARAM_CHANNEL     (0)
#define M_INITPARAM_NODE        (1)
#define M_INITPARAM_ANNO        (2)
#define M_INITPARAM_WALT        (3)

#define M_MAPSIZE_REMAIN        (2)                         ///< DB compactionを実施する残りpage

#define M_LMDB_CHANNEL_MAXDBS   (12 * 2 * MAX_CHANNELS)     ///< 同時オープンできるDB数
#define M_LMDB_CHANNEL_MAPSIZE  ((size_t)10485760)          // DB最大長[byte](LMDBのデフォルト値)

#define M_LMDB_NODE_MAXDBS      (50)                        ///< 同時オープンできるDB数
#define M_LMDB_NODE_MAPSIZE     ((size_t)10485760)          // DB最大長[byte](LMDBのデフォルト値)

#define M_LMDB_ANNO_MAXDBS      (50)                        ///< 同時オープンできるDB数
//#define M_LMDB_ANNO_MAPSIZE   ((size_t)4294963200)        // DB最大長[byte] Ubuntu 18.04(64bit)で使用できたサイズ
#define M_LMDB_ANNO_MAPSIZE     ((size_t)1073741824)        // DB最大長[byte] Raspberry Piで使用できたサイズ
                                                            // 32bit環境ではsize_tが4byteになるため、32bitの範囲内にすること

#define M_LMDB_WALT_MAXDBS      (MAX_CHANNELS)              ///< 同時オープンできるDB数
#define M_LMDB_WALT_MAPSIZE     ((size_t)10485760)          // DB最大長[byte](LMDBのデフォルト値)

#define M_DBPATH_MAX            (256)
#define M_DBDIR                 "dbptarm"
#define M_CHANNELENV_DIR        "dbptarm_chnl"              ///< channel
#define M_NODEENV_DIR           "dbptarm_node"              ///< node
#define M_ANNOENV_DIR           "dbptarm_anno"              ///< announcement
#define M_WALTENV_DIR           "dbptarm_walt"              ///< 1st layer wallet


#define M_CHANNEL_BUFS          (3)             ///< DB保存する可変長データ数
                                                //      funding
                                                //      local shutdown scriptPubKeyHash
                                                //      remote shutdown scriptPubKeyHash

#define M_PREFIX_LEN            (2)
#define M_PREF_CHANNEL          "CN"            ///< channel
#define M_PREF_SECRET           "SE"            ///< secret
#define M_PREF_ADDHTLC          "HT"            ///< update_add_htlc関連
#define M_PREF_REVOKED          "RV"            ///< revoked transaction用
#define M_PREF_BAKCHANNEL       "cn"            ///< closed channel

#define M_DBI_ANNO_CNL          "channel_anno"          ///< 受信したchannel_announcement/channel_update
#define M_DBI_ANNOINFO_CNL      "channel_annoinfo"      ///< channel_announcement/channel_updateの受信元・送信先
#define M_DBI_ANNO_NODE         "node_anno"             ///< 受信したnode_announcement
#define M_DBI_ANNOINFO_NODE     "node_annoinfo"         ///< node_announcementの受信元・送信先
#define M_DBI_ANNOCHAN_RECV     "chananno_recv"         ///< channel_announcementのnode_id
#define M_DBI_ANNOOWN           "annoown"               ///< 自分の持つchannel
#define M_DBI_ROUTE_SKIP        LNDB_DBI_ROUTE_SKIP     ///< 送金失敗short_channel_id
#define M_DBI_INVOICE           "route_invoice"         ///< 送金中invoice一時保存
#define M_DBI_PREIMAGE          "preimage"              ///< preimage
#define M_DBI_PAYHASH           "payhash"               ///< revoked transaction close用
#define M_DBI_WALLET            "wallet"                ///< wallet
#define M_DBI_VERSION           "version"               ///< verion

#define M_SZ_DBNAME_LEN         (M_PREFIX_LEN + LN_SZ_CHANNEL_ID * 2)
#define M_SZ_HTLC_STR           (3)     // "%03d" 0〜482
#define M_SZ_ANNOINFO_CNL       (sizeof(uint64_t))
#define M_SZ_ANNOINFO_NODE      (BTC_SZ_PUBKEY)

#define M_KEY_PREIMAGE          "preimage"
#define M_SZ_PREIMAGE           (sizeof(M_KEY_PREIMAGE) - 1)
#define M_KEY_ONIONROUTE        "onion_route"
#define M_SZ_ONIONROUTE         (sizeof(M_KEY_ONIONROUTE) - 1)
#define M_KEY_SHAREDSECRET      "shared_secret"
#define M_SZ_SHAREDSECRET       (sizeof(M_KEY_SHAREDSECRET) - 1)

#define M_DB_VERSION_VAL        ((int32_t)(-54))     ///< DB version
/*
    -1 : first
    -2 : ln_update_add_htlc_t変更
    -3 : ln_funding_remote_data_t変更
    -4 : ln_funding_local_data_t, ln_funding_remote_data_t変更
    -5 : backup_self_tにln_node_info_t追加
    -6 : self.min_depth追加
    -7 : ln_commit_tx_tにtxid追加
    -8 : ln_commit_tx_tにhtlc_num追加
    -9 : self.shutdown_scriptpk_localを対象に追加
    -10: htlckey対応
    -11: self.shutdown_scriptpk_remoteを対象に追加, LOCALKEY削除, funding_local/remote整理
    -12: revoked transaction用データ追加
    -13: self.anno_flag追加
    -14: announcementの送信管理追加
    -15: node.conf情報をversionに追加
    -16: selfはmpDbEnv、それ以外はmpDbNodeEnvにする
    -17: selfの構造体を個別に保存する
         selfのsecret情報をself.priv_dataに集約
    -18: node_announcement除外用DB追加(annoinfo_chan)
    -18: [SPVのみ]funding_txのblock hash追加
    -19: revocation_number追加
    -20: current_commit_num追加、scriptpubkeys削除
    -21: fix: alias length
    -22: onion route
    -23: announcement dbを分離
    -24: self.cnl_add_htlc[].flag変更
    -25: self.close_type追加
    -26: DB_COPYにhtlc_num, htld_id_num追加
    -27: self.close_type変更
    -28: self.htlc_num削除
    -29: self.statusとself.close_typeのマージ
    -30: bitcoindとSPVを同じにする
    -31: include peer_storage_index in ln_derkey_storage_t
    -32: exchange the values of commit_tx_local.to_self_delay and commit_tx_remote.to_self_delay
    -33: change the format of pub/priv keys
    -34: change the size of ln_derkey_local_privkeys_t::per_commitment_secret
         BTC_SZ_PUBKEY -> BTC_SZ_PRIVKEY
    -35: change the order of internal members in ln_derkey_local_privkeys_t
    -36: change self->peer_storage -> self->privkeys_remote
    -37: funding_local -> pubkeys_local, funding_remote -> pubkeys_remote
    -38: rename db name, dbparam_self -> dbptarm_chnl
         rename self -> channel
    -39: DBCHANNEL_SECRET:
             ln_channel_t::privkeys_local ->
                 ln_channel_t::keys_local.ln_derkey_local_keys_t::secrets
                 ln_channel_t::keys_local.ln_derkey_local_keys_t::storage_seed
                 ln_channel_t::keys_local.ln_derkey_local_keys_t::next_storage_index
         DBCHANNEL_VALUES:
             ln_channel_t::privkeys_remote
             ln_channel_t::pubkeys_remote ->
                 ln_channel_t::keys_remote.ln_derkey_remote_keys_t::basepoints
                 ln_channel_t::keys_remote.ln_derkey_remote_keys_t::next_storage_index
                 ln_channel_t::keys_remote.ln_derkey_remote_keys_t::storage
                 ln_channel_t::keys_remote.ln_derkey_remote_keys_t::per_commitment_point
                 ln_channel_t::keys_remote.ln_derkey_remote_keys_t::prev_per_commitment_point
             ln_channel_t::pubkeys_local -> removed
         and the local public keys and the script pubkeys are restored after loading
    -40: save only txid and txindex in ln_funding_tx_t
    -41: add `funding_tx_t::funding_satoshis`
         rm `ln_channel_t::funding_sat`
    -42: rename `our_msat` -> `local_msat` and `their_msat` -> `remote_msat`
    -43: rename `ln_update_add_htlc_t::stat` -> `ln_update_add_htlc_t::flags`
    -44: rm `ln_channel_t::local_msat`
         rm `ln_channel_t::remote_msat`
         add `ln_commit_tx_t::local_msat`
         add `ln_commit_tx_t::remote_msat`
    -45: rename `htlc_id_num` -> `num_htlc_ids`
         rename `htlc_output_num` -> `num_htlc_outputs`
    -46: separate `ln_update_add_htlc_t` into `ln_update_t` and `ln_htlc_t`
         rename `num_htlc_ids` -> `next_htlc_id`
    -47: the size of `ln_update_t` gets smaller
         rm `ln_update_t::prev_short_channel_id`
         rm `ln_update_t::prev_idx`
         rm `ln_update_t::next_short_channel_id`
         rm `ln_update_t::next_idx`
         add `ln_update_t::neighbor_short_channel_id`
         add `ln_update_t::neighbor_idx`
    -48: fix ln_update_t::enabled
    -49: update `ln_update_t` and `ln_htlc_t`
    -50: rename `ln_funding_tx_t` -> `ln_funding_info_t`
         rename `ln_commit_t` -> `ln_commit_info_t`
    -51: `ln_channel_t::updates` -> `ln_channel_t::update_info.updates`
         `ln_channel_t::htlcs` -> `ln_channel_t::update_info.htlcs`
         `ln_channel_t::next_htlc_id` -> `ln_channel_t::update_info.next_htlc_id`
    -52: rm `ln_channel_t::feerate_per_kw`
         add `ln_commit_info_t::feerate_per_kw`
    -53: add `ln_update_info_t::fee_updates`
    -54: update `ln_update_info_t::updates`
 */


/********************************************************************
 * macro functions
 ********************************************************************/

#define M_ANNOINFO_CNL_SET(keydata, key, short_channel_id, type) {\
    key.mv_size = sizeof(keydata);\
    key.mv_data = keydata;\
    memcpy(keydata, &short_channel_id, LN_SZ_SHORT_CHANNEL_ID);\
    keydata[LN_SZ_SHORT_CHANNEL_ID] = type;\
}

#define M_ANNOINFO_NODE_SET(keydata, key, node_id) {\
    key.mv_size = sizeof(keydata);\
    key.mv_data = keydata;\
    memcpy(keydata, node_id, BTC_SZ_PUBKEY);\
}

#define M_SIZE(type, mem)       (sizeof(((type *)0)->mem))
#define M_ITEM(type, mem)       { #mem, M_SIZE(type, mem), offsetof(type, mem) }
#define MM_ITEM(type1, mem1, type2, mem2) \
                                { #mem1 "." #mem2, M_SIZE(type2, mem2), offsetof(type1, mem1) + offsetof(type2, mem2) }
#define MMN_ITEM(type1, mem1, n, type2, mem2) \
                                { #mem1 "." #mem2 ":" #n, M_SIZE(type2, mem2), offsetof(type1, mem1) + sizeof(type2) * n + offsetof(type2, mem2) }
#define M_BUF_ITEM(idx, mem)    { p_dbscript_keys[idx].name = #mem; p_dbscript_keys[idx].p_buf = (CONST_CAST utl_buf_t*)&pChannel->mem; }

#ifndef M_DB_DEBUG
#define MDB_TXN_BEGIN(a,b,c,d)      my_mdb_txn_begin(a, b, c, d, __LINE__)
#define MDB_TXN_ABORT(a)            mdb_txn_abort(a)
#define MDB_TXN_COMMIT(a)           my_mdb_txn_commit(a, __LINE__)
#define MDB_DBI_OPEN(a,b,c,d)       my_mdb_dbi_open(a,b,c,d, __LINE__)

#define MDB_TXN_CHECK_CHANNEL(a)    //none
#define MDB_TXN_CHECK_NODE(a)       //none
#define MDB_TXN_CHECK_ANNO(a)       //none
#define MDB_TXN_CHECK_WALT(a)       //none
#else
static volatile int g_cnt[2];
#define MDB_TXN_BEGIN(a,b,c,d)      my_mdb_txn_begin(a, b, c, d, __LINE__);
#define MDB_TXN_ABORT(a)            my_mdb_txn_abort(a, __LINE__)
#define MDB_TXN_COMMIT(a)           my_mdb_txn_commit(a, __LINE__)
#define MDB_DBI_OPEN(a,b,c,d)       my_mdb_dbi_open(a,b,c,d, __LINE__)

#define MDB_TXN_CHECK_CHANNEL(a)    if (mdb_txn_env(a) != mpDbChannel) { LOGE("ERR: txn not CHANNEL\n"); abort(); }
#define MDB_TXN_CHECK_NODE(a)       if (mdb_txn_env(a) != mpDbNode) { LOGE("ERR: txn not NODE\n"); abort(); }
#define MDB_TXN_CHECK_ANNO(a)       if (mdb_txn_env(a) != mpDbAnno) { LOGE("ERR: txn not ANNO\n"); abort(); }
#define MDB_TXN_CHECK_WALT(a)       if (mdb_txn_env(a) != mpDbWalt) { LOGE("ERR: txn not WALT\n"); abort(); }
#endif

#define M_DEBUG_KEYS
#define M_SIZE(type, mem)           (sizeof(((type *)0)->mem))


/********************************************************************
 * typedefs
 ********************************************************************/

/**
 * @typedef backup_param_t
 * @brief   ln_channel_tのバックアップ
 */
typedef struct backup_param_t {
    const char  *name;
    size_t      datalen;
    size_t      offset;
} backup_param_t;


/**
 * @typedef backup_buf_t
 * @brief   backup buffer
 */
typedef struct backup_buf_t {
    const char  *name;
    utl_buf_t *p_buf;
} backup_buf_t;


/**
 * @typedef init_param_t
 * @brief   DB初期化パラメータ
 */
typedef struct {
    MDB_env         **pp_env;
    const char      *path;
    MDB_dbi         maxdbs;         //mdb_env_set_maxdbs()
    size_t          mapsize;        //mdb_env_set_mapsize()
    unsigned int    openflag;       //mdb_env_open()
} init_param_t;


/** @typedef    nodeinfo_t
 *  @brief      [version]に保存するnode情報
 */
typedef struct {
    uint8_t     genesis[BTC_SZ_HASH256];
    char        wif[BTC_SZ_WIF_STR_MAX + 1];
    char        name[LN_SZ_ALIAS_STR + 1];
    uint16_t    port;
    uint8_t     create_bhash[BTC_SZ_HASH256];
} nodeinfo_t;


/** @typedef    preimage_info_t
 *  @brief      [preimage]に保存するpreimage情報
 */
typedef struct {
    uint64_t amount;            ///< amount[satoshi]
    uint64_t creation;          ///< invoice creation epoch
    uint32_t expiry;            ///< expiry[sec]
                                //      0: 3600s=1h(BOLT#11のデフォルト値)
                                //      UINT32_MAX: expiryによる自動削除禁止
} preimage_info_t;


/** #ln_db_channel_del_param()用(ln_db_preimage_search)
 *
 */
typedef struct {
    const ln_htlc_t  *p_htlcs;
} preimage_close_t;


/********************************************************************
 * static variables
 ********************************************************************/

//LMDB
static MDB_env      *mpDbChannel = NULL;        // channel
static MDB_env      *mpDbNode = NULL;           // node
static MDB_env      *mpDbAnno = NULL;           // announcement
static MDB_env      *mpDbWalt = NULL;           // wallt
static char         mPath[M_DBPATH_MAX];
static char         mPathChannel[M_DBPATH_MAX];
static char         mPathNode[M_DBPATH_MAX];
static char         mPathAnno[M_DBPATH_MAX];
static char         mPathWalt[M_DBPATH_MAX];

static pthread_mutex_t  mMuxAnno;
static MDB_txn          *mTxnAnno;


/**
 *  @var    DBCHANNEL_SECRET
 *  @brief  ln_channel_tのsecret
 */
static const backup_param_t DBCHANNEL_SECRET[] = {
    MM_ITEM(ln_channel_t, keys_local, ln_derkey_local_keys_t, secrets),             //[KEYS_01]
    MM_ITEM(ln_channel_t, keys_local, ln_derkey_local_keys_t, storage_seed),        //[KEYS_01]
    MM_ITEM(ln_channel_t, keys_local, ln_derkey_local_keys_t, next_storage_index),  //[KEYS_01]
};


/**
 *  @var    DBCHANNEL_VALUES
 *  @brief  ln_channel_tのほぼすべて
 */
static const backup_param_t DBCHANNEL_VALUES[] = {
    //
    //conn
    //
    M_ITEM(ln_channel_t, peer_node_id),             //[CONN_01]
    M_ITEM(ln_channel_t, last_connected_addr),      //[CONN_02]
    M_ITEM(ln_channel_t, status),                   //[CONN_03]

    //
    //keys
    //
    //[KEYS_01]keys_local --> secret
    MM_ITEM(ln_channel_t, keys_remote, ln_derkey_remote_keys_t, basepoints),                //[KEYS_02]
    MM_ITEM(ln_channel_t, keys_remote, ln_derkey_remote_keys_t, next_storage_index),        //[KEYS_02]
    MM_ITEM(ln_channel_t, keys_remote, ln_derkey_remote_keys_t, storage),                   //[KEYS_02]
    MM_ITEM(ln_channel_t, keys_remote, ln_derkey_remote_keys_t, per_commitment_point),      //[KEYS_02]
    MM_ITEM(ln_channel_t, keys_remote, ln_derkey_remote_keys_t, prev_per_commitment_point), //[KEYS_02]

    //
    //fund
    //
    MM_ITEM(ln_channel_t, funding_info, ln_funding_info_t, role),                             //[FUND_01]
    MM_ITEM(ln_channel_t, funding_info, ln_funding_info_t, state),                            //[FUND_01]
    MM_ITEM(ln_channel_t, funding_info, ln_funding_info_t, txid),                             //[FUND_01]
    MM_ITEM(ln_channel_t, funding_info, ln_funding_info_t, txindex),                          //[FUND_01]
    MM_ITEM(ln_channel_t, funding_info, ln_funding_info_t, funding_satoshis),                 //[FUND_01]
    MM_ITEM(ln_channel_t, funding_info, ln_funding_info_t, minimum_depth),                    //[FUND_01]
    M_ITEM(ln_channel_t, funding_blockhash),    //[FUNDSPV_01]
    M_ITEM(ln_channel_t, funding_last_confirm), //[FUNDSPV_02]

    //
    //anno
    //
    M_ITEM(ln_channel_t, anno_flag),       //[ANNO_01]
    //[ANNO_02]anno_param
    //[ANNO_03]cnl_anno

    //
    //init
    //
    //[INIT_01]init_flag
    //[INIT_02]lfeature_local
    //[INIT_03]lfeature_remote
    //[INIT_04]reest_commit_num
    //[INIT_05]reest_revoke_num

    //
    //clse
    //
    //[CLSE_01]---
    //[CLSE_02]tx_closing
    M_ITEM(ln_channel_t, shutdown_flag),   //[CLSE_03]shutdown_flag
    //[CLSE_04]close_fee_sat
    //[CLSE_05]close_last_fee_sat
    //[CLSE_06]shutdown_scriptpk_local --> script
    //[CLSE_07]shutdown_scriptpk_remote --> script

    //
    //revk
    //
    //[REVK_01]p_revoked_vout --> revoked db
    //[REVK_02]p_revoked_wit  --> revoked db
    //[REVK_03]p_revoked_type --> revoked db
    //[REVK_04]revoked_sec --> revoked db
    //[REVK_05]revoked_num --> revoked db
    //[REVK_06]revoked_cnt --> revoked db
    //[REVK_07]revoked_chk --> revoked db

    //
    //norm
    //
    M_ITEM(ln_channel_t, channel_id),           //[NORM_01]
    M_ITEM(ln_channel_t, short_channel_id),     //[NORM_02]
    MM_ITEM(ln_channel_t, update_info, ln_update_info_t, updates),      //[NORM_03]
    //[NORM_03]htlcs --> HTLC
    MM_ITEM(ln_channel_t, update_info, ln_update_info_t, next_htlc_id), //[NORM_03]
    MM_ITEM(ln_channel_t, update_info, ln_update_info_t, fee_updates),  //[NORM_03]

    //
    //comm
    //
    MM_ITEM(ln_channel_t, commit_info_local, ln_commit_info_t, dust_limit_sat),                   //[COMM_01]commit_info_local
    MM_ITEM(ln_channel_t, commit_info_local, ln_commit_info_t, max_htlc_value_in_flight_msat),    //[COMM_01]
    MM_ITEM(ln_channel_t, commit_info_local, ln_commit_info_t, channel_reserve_sat),              //[COMM_01]
    MM_ITEM(ln_channel_t, commit_info_local, ln_commit_info_t, htlc_minimum_msat),                //[COMM_01]
    MM_ITEM(ln_channel_t, commit_info_local, ln_commit_info_t, to_self_delay),                    //[COMM_01]
    MM_ITEM(ln_channel_t, commit_info_local, ln_commit_info_t, max_accepted_htlcs),               //[COMM_01]
    MM_ITEM(ln_channel_t, commit_info_local, ln_commit_info_t, remote_sig),                       //[COMM_01]
    MM_ITEM(ln_channel_t, commit_info_local, ln_commit_info_t, txid),                             //[COMM_01]
    MM_ITEM(ln_channel_t, commit_info_local, ln_commit_info_t, num_htlc_outputs),                 //[COMM_01]
    MM_ITEM(ln_channel_t, commit_info_local, ln_commit_info_t, commit_num),                       //[COMM_01]
    MM_ITEM(ln_channel_t, commit_info_local, ln_commit_info_t, revoke_num),                       //[COMM_01]
    MM_ITEM(ln_channel_t, commit_info_local, ln_commit_info_t, local_msat),                       //[COMM_01]
    MM_ITEM(ln_channel_t, commit_info_local, ln_commit_info_t, remote_msat),                      //[COMM_01]
    MM_ITEM(ln_channel_t, commit_info_local, ln_commit_info_t, feerate_per_kw),                   //[COMM_01]
    MM_ITEM(ln_channel_t, commit_info_local, ln_commit_info_t, obscured_commit_num_mask),         //[COMM_01]
    MM_ITEM(ln_channel_t, commit_info_remote, ln_commit_info_t, dust_limit_sat),                  //[COMM_02]commit_info_remote
    MM_ITEM(ln_channel_t, commit_info_remote, ln_commit_info_t, max_htlc_value_in_flight_msat),   //[COMM_02]
    MM_ITEM(ln_channel_t, commit_info_remote, ln_commit_info_t, channel_reserve_sat),             //[COMM_02]
    MM_ITEM(ln_channel_t, commit_info_remote, ln_commit_info_t, htlc_minimum_msat),               //[COMM_02]
    MM_ITEM(ln_channel_t, commit_info_remote, ln_commit_info_t, to_self_delay),                   //[COMM_02]
    MM_ITEM(ln_channel_t, commit_info_remote, ln_commit_info_t, max_accepted_htlcs),              //[COMM_02]
    MM_ITEM(ln_channel_t, commit_info_remote, ln_commit_info_t, remote_sig),                      //[COMM_02]
    MM_ITEM(ln_channel_t, commit_info_remote, ln_commit_info_t, txid),                            //[COMM_02]
    MM_ITEM(ln_channel_t, commit_info_remote, ln_commit_info_t, num_htlc_outputs),                //[COMM_02]
    MM_ITEM(ln_channel_t, commit_info_remote, ln_commit_info_t, commit_num),                      //[COMM_02]
    MM_ITEM(ln_channel_t, commit_info_remote, ln_commit_info_t, revoke_num),                      //[COMM_02]
    MM_ITEM(ln_channel_t, commit_info_remote, ln_commit_info_t, local_msat),                      //[COMM_02]
    MM_ITEM(ln_channel_t, commit_info_remote, ln_commit_info_t, remote_msat),                     //[COMM_02]
    MM_ITEM(ln_channel_t, commit_info_remote, ln_commit_info_t, feerate_per_kw),                  //[COMM_02]
    MM_ITEM(ln_channel_t, commit_info_remote, ln_commit_info_t, obscured_commit_num_mask),        //[COMM_02]

    //
    //nois
    //
    //[NOIS_01]noise

    //
    //erro
    //
    //[ERRO_01]err
    //[ERRO_02]err_msg

    //
    //apps
    //
    //[APPS_01]p_callback
    //[APPS_02]p_param
};


/**
 *  @var    DBCHANNEL_COPY
 *  @brief  値コピー用
 *  @note
 *      - DBCHANNEL_COPY[]とDBCHANNEL_COPYIDX[]を同時に更新すること
 */
static const backup_param_t DBCHANNEL_COPY[] = {
    M_ITEM(ln_channel_t, peer_node_id),
    M_ITEM(ln_channel_t, channel_id),
    M_ITEM(ln_channel_t, short_channel_id),
    MM_ITEM(ln_channel_t, update_info, ln_update_info_t, next_htlc_id),
    MM_ITEM(ln_channel_t, funding_info, ln_funding_info_t, txid),
    MM_ITEM(ln_channel_t, funding_info, ln_funding_info_t, txindex),
    M_ITEM(ln_channel_t, keys_local),
    M_ITEM(ln_channel_t, keys_remote),
    MM_ITEM(ln_channel_t, commit_info_local, ln_commit_info_t, commit_num),
    MM_ITEM(ln_channel_t, commit_info_local, ln_commit_info_t, revoke_num),
    MM_ITEM(ln_channel_t, commit_info_local, ln_commit_info_t, local_msat),
    MM_ITEM(ln_channel_t, commit_info_local, ln_commit_info_t, remote_msat),
    MM_ITEM(ln_channel_t, commit_info_remote, ln_commit_info_t, commit_num),
    MM_ITEM(ln_channel_t, commit_info_remote, ln_commit_info_t, revoke_num),
    MM_ITEM(ln_channel_t, commit_info_remote, ln_commit_info_t, local_msat),
    MM_ITEM(ln_channel_t, commit_info_remote, ln_commit_info_t, remote_msat),
};


/**
 *  @var    DBCHANNEL_COPYIDX
 *  @brief  値コピー用(index)
 *  @note
 *      - DBCHANNEL_COPY[]とDBCHANNEL_COPYIDX[]を同時に更新すること
 */
static const struct {
    enum {
        ETYPE_BYTEPTR,      //const uint8_t*
        ETYPE_UINT64U,      //uint64_t(unsigned decimal)
        ETYPE_UINT64X,      //uint64_t(unsigned hex)
        ETYPE_UINT16,       //uint16_t
        ETYPE_TXID,         //txid
        ETYPE_FUNDTXID,     //funding_local.txid
        ETYPE_FUNDTXIDX,    //funding_local.txindex
        ETYPE_LOCALKEYS,    //keys_local
        ETYPE_REMOTEKEYS,   //keys_remote
        //ETYPE_REMOTECOMM,   //funding_remote.prev_percommit
    } type;
    int length;
    bool disp;      //true: showdbで表示する
} DBCHANNEL_COPYIDX[] = {
    { ETYPE_BYTEPTR,    BTC_SZ_PUBKEY, true },      // peer_node_id
    { ETYPE_BYTEPTR,    LN_SZ_CHANNEL_ID, true },   // channel_id
    { ETYPE_UINT64X,    1, true },                  // short_channel_id
    { ETYPE_UINT64U,    1, true },                  // num_htlc_ids
    { ETYPE_FUNDTXID,   BTC_SZ_TXID, true },        // funding_txid
    { ETYPE_FUNDTXIDX,  1, true },                  // funding_txindex
    { ETYPE_LOCALKEYS,  1, false },                 // keys_local
    { ETYPE_REMOTEKEYS, 1, false },                 // keys_remote
    { ETYPE_UINT64U,    1, true },                  // commit_info_local.commit_num
    { ETYPE_UINT64U,    1, true },                  // commit_info_local.revoke_num
    { ETYPE_UINT64U,    1, true },                  // commit_info_local.local_msat
    { ETYPE_UINT64U,    1, true },                  // commit_info_local.remote_msat
    { ETYPE_UINT64U,    1, true },                  // commit_info_remote.commit_num
    { ETYPE_UINT64U,    1, true },                  // commit_info_remote.revoke_num
    { ETYPE_UINT64U,    1, true },                  // commit_info_remote.local_msat
    { ETYPE_UINT64U,    1, true },                  // commit_info_remote.remote_msat
};


/**
 *  @var    DBHTLC_VALUES
 *  @brief  HTLC
 */
static const backup_param_t DBHTLC_VALUES[] = {
    M_ITEM(ln_htlc_t, enabled),
    M_ITEM(ln_htlc_t, id),
    M_ITEM(ln_htlc_t, amount_msat),
    M_ITEM(ln_htlc_t, cltv_expiry),
    M_ITEM(ln_htlc_t, payment_hash),
    //buf_payment_preimage --> HTLC buf
    //buf_onion_reason --> HTLC buf
    M_ITEM(ln_htlc_t, remote_sig),
    //buf_shared_secret --> HTLC buf
};


// LMDB initialize parameter
static const init_param_t INIT_PARAM[] = {
    //M_INITPARAM_CHANNEL
    { &mpDbChannel, mPathChannel, M_LMDB_CHANNEL_MAXDBS, M_LMDB_CHANNEL_MAPSIZE, 0 },
    //M_INITPARAM_NODE
    { &mpDbNode, mPathNode, M_LMDB_NODE_MAXDBS, M_LMDB_NODE_MAPSIZE, 0 },
    //M_INITPARAM_ANNO
    { &mpDbAnno, mPathAnno, M_LMDB_ANNO_MAXDBS, M_LMDB_ANNO_MAPSIZE, MDB_NOSYNC },
    //M_INITPARAM_WALT
    { &mpDbWalt, mPathWalt, M_LMDB_WALT_MAXDBS, M_LMDB_WALT_MAPSIZE, 0 },
};


/********************************************************************
 * prototypes
 ********************************************************************/

static int channel_db_open(ln_lmdb_db_t *pDb, const char *pDbName, int OptTxn, int OptDb);
static int channel_htlc_load(ln_channel_t *pChannel, ln_lmdb_db_t *pDb);
static int channel_htlc_save(const ln_channel_t *pChannel, ln_lmdb_db_t *pDb);
static int channel_save(const ln_channel_t *pChannel, ln_lmdb_db_t *pDb);
static int channel_item_load(ln_channel_t *pChannel, const backup_param_t *pBackupParam, ln_lmdb_db_t *pDb);
static int channel_item_save(const ln_channel_t *pChannel, const backup_param_t *pBackupParam, ln_lmdb_db_t *pDb);
static int channel_secret_load(ln_channel_t *pChannel, ln_lmdb_db_t *pDb);
static int channel_secret_restore(ln_channel_t *pChannel);
static int channel_cursor_open(lmdb_cursor_t *pCur, bool bWritable);
static void channel_cursor_close(lmdb_cursor_t *pCur, bool bWritable);
static void channel_htlc_dbname(char *pDbName, int num);
static bool channel_comp_func_cnldel(ln_channel_t *pChannel, void *p_db_param, void *p_param);
static bool channel_search(ln_db_func_cmp_t pFunc, void *pFuncParam, bool bWritable, bool bRestore);

static int node_db_open(ln_lmdb_db_t *pDb, const char *pDbName, int OptTxn, int OptDb);

static int annocnl_load(ln_lmdb_db_t *pDb, utl_buf_t *pCnlAnno, uint64_t ShortChannelId);
static int annocnl_save(ln_lmdb_db_t *pDb, const utl_buf_t *pCnlAnno, uint64_t ShortChannelId);
static int annocnlupd_load(ln_lmdb_db_t *pDb, utl_buf_t *pCnlUpd, uint32_t *pTimeStamp, uint64_t ShortChannelId, uint8_t Dir);
static int annocnlupd_save(ln_lmdb_db_t *pDb, const utl_buf_t *pCnlUpd, const ln_msg_channel_update_t *pUpd);
static int annocnl_cur_load(MDB_cursor *cur, uint64_t *pShortChannelId, char *pType, uint32_t *pTimeStamp, utl_buf_t *pBuf, MDB_cursor_op op);
static int annonod_load(ln_lmdb_db_t *pDb, utl_buf_t *pNodeAnno, uint32_t *pTimeStamp, const uint8_t *pNodeId);
static int annonod_save(ln_lmdb_db_t *pDb, const utl_buf_t *pNodeAnno, const uint8_t *pNodeId, uint32_t Timestamp);
static bool annoinfo_add(ln_lmdb_db_t *pDb, MDB_val *pMdbKey, MDB_val *pMdbData, const uint8_t *pNodeId);
static bool annoinfo_search(MDB_val *pMdbData, const uint8_t *pNodeId);
static void annoinfo_cur_trim(MDB_cursor *pCursor, const uint8_t *pNodeId);
static void anno_del_prune(void);

static bool preimage_open(ln_lmdb_db_t *p_db, MDB_txn *txn);
static void preimage_close(ln_lmdb_db_t *p_db, MDB_txn *txn);
static bool preimage_del_func(const uint8_t *pPreimage, uint64_t Amount, uint32_t Expiry, void *p_db_param, void *p_param);
static bool preimage_close_func(const uint8_t *pPreimage, uint64_t Amount, uint32_t Expiry, void *p_db_param, void *p_param);

static int wallet_db_open(ln_lmdb_db_t *pDb, const char *pDbName, int OptTxn, int OptDb);

static int ver_write(ln_lmdb_db_t *pDb, const char *pWif, const char *pNodeName, uint16_t Port);
static int ver_check(ln_lmdb_db_t *pDb, int32_t *pVer, char *pWif, char *pNodeName, uint16_t *pPort, uint8_t *pGenesis);

static int backup_param_load(void *pData, ln_lmdb_db_t *pDb, const backup_param_t *pParam, size_t Num);
static int backup_param_save(const void *pData, ln_lmdb_db_t *pDb, const backup_param_t *pParam, size_t Num);

static int init_dbenv(int InitParamIdx);
static int rm_files(const char *pathname, const struct stat *sbuf, int type, struct FTW *ftwb);
static int lmdb_init(int InitParamIdx);
static int lmdb_compaction(int InitParamIdx);


#ifndef M_DB_DEBUG
static inline int my_mdb_txn_begin(MDB_env *env, MDB_txn *parent, unsigned int flags, MDB_txn **txn, int line) {
    int retval = mdb_txn_begin(env, parent, flags, txn);
    if ((retval != 0) && (retval != MDB_NOTFOUND)) {
        LOGE("ERR(%d): %s\n", line, mdb_strerror(retval));
    }
    return retval;
}

static inline int my_mdb_txn_commit(MDB_txn *txn, int line) {
    int txn_retval = mdb_txn_commit(txn);
    if (txn_retval) {
        LOGE("ERR(%d): %s\n", line, mdb_strerror(txn_retval));
        if (txn_retval == MDB_BAD_TXN) {
            mdb_txn_abort(txn);
            LOGE("FATAL: FATAL DB ERROR!\n");
            fprintf(stderr, "FATAL DB ERROR!\n");
            exit(EXIT_FAILURE);
        }
    }
    return txn_retval;
}

static inline int my_mdb_dbi_open(MDB_txn *txn, const char *name, unsigned int flags, MDB_dbi *dbi, int line) {
    int retval = mdb_dbi_open(txn, name, flags, dbi);
    if ((retval != 0) && (retval != MDB_NOTFOUND)) {
        LOGE("ERR(%d): %s\n", line, mdb_strerror(retval));
    }
    return retval;
}
#else
static inline int my_mdb_txn_begin(MDB_env *env, MDB_txn *parent, unsigned int flags, MDB_txn **txn, int line) {
    int ggg = (env == mpDbChannel) ? 0 : 1;
    g_cnt[ggg]++;
    LOGD("mdb_txn_begin:%d:[%d]opens=%d(%d)\n", line, ggg, g_cnt[ggg], (int)flags);
    MDB_envinfo stat;
    if (mdb_env_info(env, &stat) == 0) {
        LOGD("  last txnid=%lu\n", stat.me_last_txnid);
    }
    int retval = mdb_txn_begin(env, parent, flags, txn);
    if (retval == 0) {
        LOGD("  txnid=%lu\n", (unsigned long)mdb_txn_id(*txn));
    }
    return retval;
}

static inline int my_mdb_txn_commit(MDB_txn *txn, int line) {
    int ggg = (mdb_txn_env(txn) == mpDbChannel) ? 0 : 1;
    g_cnt[ggg]--;
    LOGD("mdb_txn_commit:%d:[%d]opend=%d\n", line, ggg, g_cnt[ggg]);
    int txn_retval = mdb_txn_commit(txn);
    if (txn_retval) {
        LOGE("ERR: %s\n", mdb_strerror(txn_retval));
    }
    return txn_retval;
}

static inline void my_mdb_txn_abort(MDB_txn *txn, int line) {
    int ggg = (mdb_txn_env(txn) == mpDbChannel) ? 0 : 1;
    g_cnt[ggg]--;
    LOGD("mdb_txn_abort:%d:[%d]opend=%d\n", line, ggg, g_cnt[ggg]);
    mdb_txn_abort(txn);
}


static inline int my_mdb_dbi_open(MDB_txn *txn, const char *name, unsigned int flags, MDB_dbi *dbi, int line) {
    int retval = mdb_dbi_open(txn, name, flags, dbi);
    LOGD("mdb_dbi_open(%d): retval=%d\n", line, retval);
    if ((retval != 0) && (retval != MDB_NOTFOUND)) {
        LOGE("ERR(%d): %s\n", line, mdb_strerror(retval));
    }
    return retval;
}
#endif  //M_DB_DEBUG


/** copy MDB_val
 *
 * @param[out]      pDst        destination data
 * @param[in]       pSrc        source data
 * @retval  (uint8_t *)pDst->mv_data
 * @note
 *      - mdb_cursor_put() sometime change `key`/`data` from mdb_cursor_get().
 *          For safety use, mdb_cursor_put() use copied `key`.
 */
static inline uint8_t *mdb_val_alloccopy(MDB_val *pDst, const MDB_val *pSrc) {
    void *p = UTL_DBG_MALLOC(pSrc->mv_size);
    memcpy(p, pSrc->mv_data, pSrc->mv_size);
    pDst->mv_size = pSrc->mv_size;
    pDst->mv_data = p;
    return (uint8_t *)p;
}


/********************************************************************
 * public functions
 ********************************************************************/

void ln_lmdb_set_path(const char *pPath)
{
    char path[M_DBPATH_MAX];

    strcpy(path, pPath);
    size_t len = strlen(path);
    if (path[len - 1] == '/') {
        path[len - 1] = '\0';
    }
    snprintf(mPath, M_DBPATH_MAX, "%s/%s", path, M_DBDIR);
    snprintf(mPathChannel, M_DBPATH_MAX, "%s/%s", mPath, M_CHANNELENV_DIR);
    snprintf(mPathNode, M_DBPATH_MAX, "%s/%s", mPath, M_NODEENV_DIR);
    snprintf(mPathAnno, M_DBPATH_MAX, "%s/%s", mPath, M_ANNOENV_DIR);
    snprintf(mPathWalt, M_DBPATH_MAX, "%s/%s", mPath, M_WALTENV_DIR);

    LOGD("db dir: %s\n", mPath);
    LOGD("  chnl: %s\n", mPathChannel);
    LOGD("  node: %s\n", mPathNode);
    LOGD("  anno: %s\n", mPathAnno);
    LOGD("  walt: %s\n", mPathWalt);
}


bool ln_db_have_dbdir(void)
{
    struct stat buf;
    int retval = stat(M_DBDIR, &buf);
    return (retval == 0) && S_ISDIR(buf.st_mode);
}


const char *ln_lmdb_get_chnlpath(void)
{
    return mPathChannel;
}


const char *ln_lmdb_get_nodepath(void)
{
    return mPathNode;
}


const char *ln_lmdb_get_annopath(void)
{
    return mPathAnno;
}


const char *ln_lmdb_get_waltpath(void)
{
    return mPathWalt;
}


bool ln_db_init(char *pWif, char *pNodeName, uint16_t *pPort, bool bStdErr)
{
    int         retval;
    ln_lmdb_db_t   db;

    if (mPath[0] == '\0') {
        ln_lmdb_set_path(".");
    }
    mkdir(mPath, 0755);

    if (mpDbChannel == NULL) {
        for (size_t lp = 0; lp < ARRAY_SIZE(INIT_PARAM); lp++) {
            retval = init_dbenv(lp);
            if (retval != 0) {
                LOGE("ERR: %s\n", mdb_strerror(retval));
                goto LABEL_EXIT;
            }
        }
    } else {
        LOGE("fail: already initialized\n");
        abort();
    }

    if (bStdErr) fprintf(stderr, "DB checking: open...");

    retval = MDB_TXN_BEGIN(mpDbChannel, NULL, 0, &db.txn);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }
    retval = MDB_DBI_OPEN(db.txn, M_DBI_VERSION, 0, &db.dbi);
    if (retval != 0) {
        //新規の場合は作成/保存する
        //      node_id : 生成
        //      alias : 指定が無ければ生成
        //      port : 指定された値
        LOGD("create node DB\n");
        uint8_t pub[BTC_SZ_PUBKEY];
        ln_node_create_key(pWif, pub);

        if (strlen(pNodeName) == 0) {
            sprintf(pNodeName, "node_%02x%02x%02x%02x%02x%02x",
                        pub[0], pub[1], pub[2], pub[3], pub[4], pub[5]);
        }
        if (*pPort == 0) {
            *pPort = LN_PORT_DEFAULT;
        }
        //LOGD("wif=%s\n", pWif);
        LOGD("alias=%s\n", pNodeName);
        LOGD("port=%d\n", *pPort);
        retval = ver_write(&db, pWif, pNodeName, *pPort);
        if (retval != 0) {
            if (bStdErr) fprintf(stderr, "create version db\n");
            MDB_TXN_ABORT(db.txn);
            goto LABEL_EXIT;
        }
    }

    if (bStdErr) fprintf(stderr, "done!\nDB checking: version...");

    int32_t ver;
    uint8_t genesis[BTC_SZ_HASH256];
    retval = ver_check(&db, &ver, pWif, pNodeName, pPort, genesis);
    MDB_TXN_COMMIT(db.txn);
    if (retval == 0) {
        //LOGD("wif=%s\n", pWif);
        LOGD("alias=%s\n", pNodeName);
        LOGD("port=%d\n", *pPort);
    } else {
        if (bStdErr) fprintf(stderr, "invalid version\n");
        goto LABEL_EXIT;
    }
    LOGD("DB genesis hash:\n");
    btc_block_chain_t dbtype = btc_block_get_chain(genesis);
    LOGD("node genesis hash:\n");
    btc_block_chain_t bctype = btc_block_get_chain(ln_genesishash_get());
    if (dbtype != bctype) {
        LOGE("fail: genesis hash(%d != %d)\n", dbtype, bctype);
        if (bStdErr) fprintf(stderr, "genesis hash not match\n");
        retval = -1;
        goto LABEL_EXIT;
    }
    fprintf(stderr, "done!\n");


    //ln_db_invoice_drop();               //送金を再開する場合があるが、その場合は再入力させるか？
    anno_del_prune();          //channel_updateだけの場合でも保持しておく

LABEL_EXIT:
    if (retval == 0) {
        pthread_mutex_init(&mMuxAnno, NULL);
    } else {
        ln_db_term();
    }

    return retval == 0;
}


void ln_db_term(void)
{
    if (mpDbChannel != NULL) {
        pthread_mutex_destroy(&mMuxAnno);

        mdb_env_close(mpDbWalt);
        mpDbWalt = NULL;
        mdb_env_close(mpDbAnno);
        mpDbAnno = NULL;
        mdb_env_close(mpDbNode);
        mpDbNode = NULL;
        mdb_env_close(mpDbChannel);
        mpDbChannel = NULL;
    }
}


/********************************************************************
 * channel
 ********************************************************************/

int ln_lmdb_channel_load(ln_channel_t *pChannel, MDB_txn *txn, MDB_dbi dbi, bool bRestore)
{
    int         retval;
    MDB_val     key, data;
    ln_lmdb_db_t db;

    //固定サイズ
    db.txn = txn;
    db.dbi = dbi;
    retval = backup_param_load(pChannel, &db, DBCHANNEL_VALUES, ARRAY_SIZE(DBCHANNEL_VALUES));
    if (retval != 0) {
        goto LABEL_EXIT;
    }

    for (uint16_t idx = 0; idx < LN_HTLC_RECEIVED_MAX; idx++) {
        utl_buf_init(&pChannel->update_info.htlcs[idx].buf_payment_preimage);
        utl_buf_init(&pChannel->update_info.htlcs[idx].buf_onion_reason);
        utl_buf_init(&pChannel->update_info.htlcs[idx].buf_shared_secret);
    }

    //可変サイズ
    utl_buf_t buf_funding = UTL_BUF_INIT;
    //
    backup_buf_t *p_dbscript_keys = (backup_buf_t *)UTL_DBG_MALLOC(sizeof(backup_buf_t) * M_CHANNEL_BUFS);
    int index = 0;
    p_dbscript_keys[index].name = "buf_funding";
    p_dbscript_keys[index].p_buf = &buf_funding;
    index++;
    M_BUF_ITEM(index, shutdown_scriptpk_local);
    index++;
    M_BUF_ITEM(index, shutdown_scriptpk_remote);
    //index++;

    for (size_t lp = 0; lp < M_CHANNEL_BUFS; lp++) {
        key.mv_size = strlen(p_dbscript_keys[lp].name);
        key.mv_data = (CONST_CAST char*)p_dbscript_keys[lp].name;
        retval = mdb_get(txn, dbi, &key, &data);
        if (retval == 0) {
            utl_buf_alloccopy(p_dbscript_keys[lp].p_buf, data.mv_data, data.mv_size);
        } else {
            LOGE("fail: %s\n", p_dbscript_keys[lp].name);
        }
    }

    btc_tx_read(&pChannel->funding_info.tx_data, buf_funding.buf, buf_funding.len);
    utl_buf_free(&buf_funding);
    UTL_DBG_FREE(p_dbscript_keys);

    //add_htlc
    retval = channel_htlc_load(pChannel, &db);
    if (retval != 0) {
        LOGE("ERR\n");
        goto LABEL_EXIT;
    }

    //secret
    retval = channel_secret_load(pChannel, &db);
    if (retval != 0) {
        LOGE("ERR\n");
        goto LABEL_EXIT;
    }

    if (bRestore) {
        //復元データからさらに復元
        retval = channel_secret_restore(pChannel);
    }

LABEL_EXIT:
    if (retval == 0) {
        LOGD("loaded: short_channel_id=0x%016" PRIx64 "\n", pChannel->short_channel_id);
    }
    return retval;
}


bool ln_db_channel_save(const ln_channel_t *pChannel)
{
    int             retval = -1;
    ln_lmdb_db_t    db;
    char            dbname[M_SZ_DBNAME_LEN + 1];

    for (int lp = 0; lp < LN_SZ_CHANNEL_ID; lp++) {
        if (pChannel->channel_id[lp] != 0) {
            retval = 0;
            break;
        }
    }
    if (retval != 0) {
        LOGD("through: channel_id is 0\n");
        return true;
    }

    utl_str_bin2str(dbname + M_PREFIX_LEN, pChannel->channel_id, LN_SZ_CHANNEL_ID);
    memcpy(dbname, M_PREF_CHANNEL, M_PREFIX_LEN);
    retval = channel_db_open(&db, dbname, 0, MDB_CREATE);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }
    retval = channel_save(pChannel, &db);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }
    retval = channel_htlc_save(pChannel, &db);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }

    MDB_TXN_COMMIT(db.txn);
    db.txn = NULL;

LABEL_EXIT:
    if (db.txn) {
        LOGE("fail: save\n");
        MDB_TXN_ABORT(db.txn);
    }
    return retval == 0;
}


bool ln_db_channel_del(const uint8_t *pChannelId)
{
    return ln_db_channel_search(channel_comp_func_cnldel, (CONST_CAST void *)pChannelId);
}


bool ln_db_channel_del_param(const ln_channel_t *pChannel, void *p_db_param)
{
    int         retval;
    MDB_dbi     dbi;
    char        dbname[M_SZ_DBNAME_LEN + M_SZ_HTLC_STR + 1];
    lmdb_cursor_t *p_cur = (lmdb_cursor_t *)p_db_param;

    MDB_TXN_CHECK_CHANNEL(p_cur->txn);

    //htlcsと関連するpreimage削除
    preimage_close_t param;
    param.p_htlcs = pChannel->update_info.htlcs;
    ln_db_preimage_search(preimage_close_func, &param);

    //add_htlc
    utl_str_bin2str(dbname + M_PREFIX_LEN, pChannel->channel_id, LN_SZ_CHANNEL_ID);
    memcpy(dbname, M_PREF_ADDHTLC, M_PREFIX_LEN);

    for (int lp = 0; lp < LN_HTLC_RECEIVED_MAX; lp++) {
        channel_htlc_dbname(dbname, lp);
        //LOGD("[%d]dbname: %s\n", lp, dbname);
        retval = MDB_DBI_OPEN(p_cur->txn, dbname, 0, &dbi);
        if (retval == 0) {
            retval = mdb_drop(p_cur->txn, dbi, 1);
        } else {
            LOGE("ERR: %s\n", mdb_strerror(retval));
        }
        if (retval == 0) {
            LOGD("drop: %s\n", dbname);
        } else {
            LOGE("ERR: %s\n", mdb_strerror(retval));
        }
    }

    //revoked transaction用データ
    utl_str_bin2str(dbname + M_PREFIX_LEN, pChannel->channel_id, LN_SZ_CHANNEL_ID);
    memcpy(dbname, M_PREF_REVOKED, M_PREFIX_LEN);

    retval = MDB_DBI_OPEN(p_cur->txn, dbname, 0, &dbi);
    if (retval == 0) {
        retval = mdb_drop(p_cur->txn, dbi, 1);
    }
    if (retval == 0) {
        LOGD("drop: %s\n", dbname);
    } else {
        LOGE("ERR: %s(dbname=%s)\n", mdb_strerror(retval), dbname);
    }

    //channel削除
    memcpy(dbname, M_PREF_CHANNEL, M_PREFIX_LEN);
    retval = MDB_DBI_OPEN(p_cur->txn, dbname, 0, &dbi);
    if (retval == 0) {
        retval = mdb_drop(p_cur->txn, dbi, 1);
    }
    if (retval == 0) {
        LOGD("drop: %s\n", dbname);
    } else {
        LOGE("ERR: %s\n", mdb_strerror(retval));
    }

    //記録として残す
    memcpy(dbname, M_PREF_BAKCHANNEL, M_PREFIX_LEN);
    retval = MDB_DBI_OPEN(p_cur->txn, dbname, MDB_CREATE, &dbi);
    if (retval == 0) {
        ln_lmdb_db_t db;

        db.txn = p_cur->txn;
        db.dbi = dbi;
        retval = backup_param_save(pChannel, &db, DBCHANNEL_COPY, ARRAY_SIZE(DBCHANNEL_COPY));
        if (retval != 0) {
            LOGE("fail\n");
        }
    } else {
        if (retval != MDB_NOTFOUND) {
            LOGE("ERR: %s\n", mdb_strerror(retval));
        }
    }

    return true;
}


bool ln_db_channel_search(ln_db_func_cmp_t pFunc, void *pFuncParam)
{
    return channel_search(pFunc, pFuncParam, true, true);
}


bool ln_db_channel_search_readonly(ln_db_func_cmp_t pFunc, void *pFuncParam)
{
#warning NOT READONLY
    return channel_search(pFunc, pFuncParam, true, true);
}


bool ln_db_channel_search_nk_readonly(ln_db_func_cmp_t pFunc, void *pFuncParam)
{
#warning NOT READONLY
    return channel_search(pFunc, pFuncParam, true, false);
}


bool ln_db_channel_load_status(ln_channel_t *pChannel)
{
    int             retval = -1;
    ln_lmdb_db_t    db;
    char            dbname[M_SZ_DBNAME_LEN + 1];
    const backup_param_t DBCHANNEL_KEY = M_ITEM(ln_channel_t, status);

    for (int lp = 0; lp < LN_SZ_CHANNEL_ID; lp++) {
        if (pChannel->channel_id[lp] != 0) {
            retval = 0;
            break;
        }
    }
    if (retval != 0) {
        LOGE("fail: channel_id is 0\n");
        return false;
    }

    utl_str_bin2str(dbname + M_PREFIX_LEN, pChannel->channel_id, LN_SZ_CHANNEL_ID);
    memcpy(dbname, M_PREF_CHANNEL, M_PREFIX_LEN);
    retval = channel_db_open(&db, dbname, MDB_RDONLY, 0);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }

    retval = channel_item_load(pChannel, &DBCHANNEL_KEY, &db);

LABEL_EXIT:
    if (db.txn) {
        MDB_TXN_ABORT(db.txn);
    }
    return retval == 0;
}


bool ln_db_channel_save_status(const ln_channel_t *pChannel, void *pDbParam)
{
    const backup_param_t DBCHANNEL_KEY = M_ITEM(ln_channel_t, status);
    ln_lmdb_db_t *p_db = (ln_lmdb_db_t *)pDbParam;
    int retval = channel_item_save(pChannel, &DBCHANNEL_KEY, p_db);

    LOGD("status=%02x, retval=%d\n", pChannel->status, retval);
    return retval == 0;
}


bool ln_db_channel_save_last_confirm(const ln_channel_t *pChannel, void *pDbParam)
{
    const backup_param_t DBCHANNEL_KEY = M_ITEM(ln_channel_t, funding_last_confirm);
    ln_lmdb_db_t *p_db = (ln_lmdb_db_t *)pDbParam;
    int retval = channel_item_save(pChannel, &DBCHANNEL_KEY, p_db);

    LOGD("last_confirm=%" PRIu32 ", retval=%d\n", pChannel->funding_last_confirm, retval);
    return retval == 0;
}


void ln_lmdb_bkchannel_show(MDB_txn *txn, MDB_dbi dbi)
{
    MDB_val         key, data;
#ifdef M_DEBUG_KEYS
    ln_funding_info_t funding_info;
    ln_derkey_local_keys_t keys_local;
    ln_derkey_remote_keys_t keys_remote;
    memset(&funding_info, 0x00, sizeof(funding_info));
    memset(&keys_local, 0x00, sizeof(keys_local));
    memset(&keys_remote, 0x00, sizeof(keys_remote));
#endif  //M_DEBUG_KEYS

    for (size_t lp = 0; lp < ARRAY_SIZE(DBCHANNEL_COPY); lp++) {
        key.mv_size = strlen(DBCHANNEL_COPY[lp].name);
        key.mv_data = (CONST_CAST char*)DBCHANNEL_COPY[lp].name;
        int retval = mdb_get(txn, dbi, &key, &data);
        if (retval == 0) {
            const uint8_t *p = (const uint8_t *)data.mv_data;
            if ((lp != 0) && (DBCHANNEL_COPYIDX[lp].disp)) {
                printf(",\n");
            }
            if (DBCHANNEL_COPYIDX[lp].disp) {
                printf("      \"%s\": ", DBCHANNEL_COPY[lp].name);
            }
            switch (DBCHANNEL_COPYIDX[lp].type) {
            case ETYPE_BYTEPTR: //const uint8_t*
            //case ETYPE_REMOTECOMM:
                if (DBCHANNEL_COPYIDX[lp].disp) {
                    printf("\"");
                    utl_dbg_dump(stdout, p, DBCHANNEL_COPYIDX[lp].length, false);
                    printf("\"");
                }
                break;
            case ETYPE_UINT64U:
                if (DBCHANNEL_COPYIDX[lp].disp) {
                    printf("%" PRIu64 "", *(const uint64_t *)p);
                }
                break;
            case ETYPE_UINT64X:
                if (DBCHANNEL_COPYIDX[lp].disp) {
                    printf("\"%016" PRIx64 "\"", *(const uint64_t *)p);
                }
                break;
            case ETYPE_UINT16:
            case ETYPE_FUNDTXIDX:
                if (DBCHANNEL_COPYIDX[lp].disp) {
                    printf("%" PRIu16, *(const uint16_t *)p);
                }
#ifdef M_DEBUG_KEYS
                if (DBCHANNEL_COPYIDX[lp].type == ETYPE_FUNDTXIDX) {
                    funding_info.txindex = *(const uint16_t *)p;
                }
#endif  //M_DEBUG_KEYS
                break;
            case ETYPE_TXID: //txid
            case ETYPE_FUNDTXID:
                if (DBCHANNEL_COPYIDX[lp].disp) {
                    printf("\"");
                    btc_dbg_dump_txid(stdout, p);
                    printf("\"");
                }
#ifdef M_DEBUG_KEYS
                if (DBCHANNEL_COPYIDX[lp].type == ETYPE_FUNDTXID) {
                    memcpy(funding_info.txid, p, DBCHANNEL_COPYIDX[lp].length);
                }
#endif  //M_DEBUG_KEYS
                break;
            case ETYPE_LOCALKEYS: //keys_local
#ifdef M_DEBUG_KEYS
                {
                    memcpy(&keys_local, p, sizeof(ln_derkey_local_keys_t));
                }
#endif  //M_DEBUG_KEYS
                break;
            case ETYPE_REMOTEKEYS: //keys_remote
#ifdef M_DEBUG_KEYS
                {
                    memcpy(&keys_remote, p, sizeof(ln_derkey_remote_keys_t));
                }
#endif  //M_DEBUG_KEYS
                break;
            default:
                break;
            }
        } else {
            LOGE("fail: %s\n", DBCHANNEL_COPY[lp].name);
        }
    }
#ifdef M_DEBUG_KEYS
    if ( ((keys_local.basepoints[0][0] == 0x02) || (keys_local.basepoints[0][0] == 0x03)) &&
         ((keys_remote.basepoints[0][0] == 0x02) || (keys_remote.basepoints[0][0] == 0x03))) {
        printf("\n");
        //ln_update_script_pubkeys(&local, &remote);
        //ln_print_keys(&local, &remote);
    }
#endif  //M_DEBUG_KEYS
}


bool ln_db_channel_chk_mynode(uint64_t ShortChannelId)
{
    int ret;
    bool mynode = false;
    MDB_val     key;
    lmdb_cursor_t   cur;

    LOGD("channel cursor open\n");
    ret = channel_cursor_open(&cur, false);
    if (ret != 0) {
        LOGE("fail: channel open\n");
        ln_db_anno_commit(false);
        return true;        //falseにすると削除されてしまうため
    }

    char name[M_SZ_DBNAME_LEN + 1];
    name[sizeof(name) - 1] = '\0';
    ln_channel_t *p_channel = (ln_channel_t *)UTL_DBG_MALLOC(sizeof(ln_channel_t));

    while ((ret = mdb_cursor_get(cur.cursor, &key, NULL, MDB_NEXT_NODUP)) == 0) {
        memcpy(name, key.mv_data, M_SZ_DBNAME_LEN);
        name[M_SZ_DBNAME_LEN] = '\0';
        if ((key.mv_size == M_SZ_DBNAME_LEN) && (memcmp(key.mv_data, M_PREF_CHANNEL, M_PREFIX_LEN) == 0)) {
            memcpy(name, key.mv_data, M_SZ_DBNAME_LEN);
            ret = MDB_DBI_OPEN(cur.txn, name, 0, &cur.dbi);
            if (ret == 0) {
                memset(p_channel, 0, sizeof(ln_channel_t));
                int retval = ln_lmdb_channel_load(p_channel, cur.txn, cur.dbi, true);
                ln_term(p_channel);
                if ((retval == 0) && (ShortChannelId == p_channel->short_channel_id)) {
                    //LOGD("own channel: %016" PRIx64 "\n", ShortChannelId);
                    mynode = true;
                    break;
                }
            } else {
                LOGE("fail: dbi_open\n");
            }
        }
    }
    UTL_DBG_FREE(p_channel);
    channel_cursor_close(&cur, false);
    return mynode;
}


bool ln_db_secret_save(ln_channel_t *pChannel)
{
    int             retval;
    ln_lmdb_db_t    db;
    char            dbname[M_SZ_DBNAME_LEN + 1];

    utl_str_bin2str(dbname + M_PREFIX_LEN, pChannel->channel_id, LN_SZ_CHANNEL_ID);
    memcpy(dbname, M_PREF_SECRET, M_PREFIX_LEN);
    retval = channel_db_open(&db, dbname, 0, MDB_CREATE);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }

    retval = backup_param_save(pChannel, &db, DBCHANNEL_SECRET, ARRAY_SIZE(DBCHANNEL_SECRET));
    if (retval == 0) {
        MDB_TXN_COMMIT(db.txn);
    } else {
        MDB_TXN_ABORT(db.txn);
    }

LABEL_EXIT:
    return retval == 0;
}


/********************************************************************
 * anno用DB
 ********************************************************************/

bool ln_db_anno_transaction(void)
{
    int retval;

    //LOGD("anno_transaction\n");
    pthread_mutex_lock(&mMuxAnno);
    //LOGD("anno_transaction -- in\n");
    retval = MDB_TXN_BEGIN(mpDbAnno, NULL, 0, &mTxnAnno);
    if (retval != 0) {
        pthread_mutex_unlock(&mMuxAnno);
    }

    return retval == 0;
}


void ln_db_anno_commit(bool bCommit)
{
    if (mTxnAnno != NULL) {
        if (bCommit) {
            MDB_TXN_COMMIT(mTxnAnno);
        } else {
            MDB_TXN_ABORT(mTxnAnno);
        }
        mTxnAnno = NULL;
    }
    pthread_mutex_unlock(&mMuxAnno);
    //LOGD("anno_transaction -- out\n");
}


/********************************************************************
 * [anno]channel_announcement / channel_update
 ********************************************************************/

/*-------------------------------------------------------------------
 *  dbi: "channel_anno" (M_DBI_ANNO_CNL)
 *      key:  [channel_announcement]short_channel_id + 'A'
 *            [channel_update dir0]short_channel_id + 'B'
 *            [channel_update dir1]short_channel_id + 'C'
 *      data:
 *          - [channel_announcement]
 *              - `channel_announcement` packet
 *          - [channel_update dir0/1]
 *              - timestamp: uint32_t
 *              - `channel_update` packet
 *      note:
 *          - `key` is structed like below:
 *              ``` uint8_t key[9];
 *                  memcpy(key, &short_channel_id, 8);
 *                  key[8] = 'A';   ```
 *-------------------------------------------------------------------
 *  dbi: "channel_annoinfo" (M_DBI_ANNOINFO_CNL, LN_DB_CUR_INFOCNL)
 *      key:  [channel_announcement]short_channel_id + 'A'
 *            [channel_update dir0]short_channel_id + 'B'
 *            [channel_update dir1]short_channel_id + 'C'
 *      data:
 *          - node_ids sending to or receiving from(uint8_t[33] * n)
 *      note:
 *          - `key` same as "channel_anno"
 *-------------------------------------------------------------------
 *  dbi: "chananno_recv" (M_DBI_ANNOCHAN_RECV)
 *      key:  node_id(uint8_t[33])
 *      data: -
 *      note:
 *          - `key` `channel_announcement` both side node_id
 *-------------------------------------------------------------------
 *-------------------------------------------------------------------
 *  dbi: "node_anno" (M_DBI_ANNO_NODE)
 *      key:  node_id(uint8_t[33])
 *      data:
 *          - timestamp: uint32_t
 *          - `node_announcement` packet
 *-------------------------------------------------------------------
 *  dbi: "node_annoinfo" (M_DBI_ANNOINFO_NODE, LN_DB_CUR_INFONODE)
 *      key:  node_id(uint8_t[33])
 *      data:
 *          - node_ids sending to or receiving from(uint8_t[33] * n)
 *-------------------------------------------------------------------
 */

/* [channel_announcement]load
 *  dbi: "channel_anno"
 */
bool ln_db_annocnl_load(utl_buf_t *pCnlAnno, uint64_t ShortChannelId)
{
    int         retval;
    ln_lmdb_db_t   db;

    bool ret = ln_db_anno_transaction();
    if (!ret) {
        LOGE("ERR: anno transaction\n");
        retval = -1;
        goto LABEL_EXIT;
    }
    retval = MDB_DBI_OPEN(mTxnAnno, M_DBI_ANNO_CNL, 0, &db.dbi);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        ln_db_anno_commit(false);
        goto LABEL_EXIT;
    }

    retval = annocnl_load(&db, pCnlAnno, ShortChannelId);

    ln_db_anno_commit(false);

LABEL_EXIT:
    return retval == 0;
}


/* [channel_announcement]save
 *  パケット保存と、その送信元node_idの保存を行う。
 *  また、channel_announcementの両端node_idを保存する(node_announcement送信判定用)。
 *
 *  dbi: "channel_anno"
 *  dbi: "channel_annoinfo"
 *  dbi: "chananno_recv"
 */
bool ln_db_annocnl_save(const utl_buf_t *pCnlAnno, uint64_t ShortChannelId, const uint8_t *pSendId,
                        const uint8_t *pNodeId1, const uint8_t *pNodeId2)
{
    int         retval;
    ln_lmdb_db_t   db, db_info, db_aichan;
    MDB_val     key, data;

    bool ret = ln_db_anno_transaction();
    if (!ret) {
        LOGE("ERR: anno transaction\n");
        retval = -1;
        goto LABEL_EXIT;
    }
    retval = MDB_DBI_OPEN(mTxnAnno, M_DBI_ANNO_CNL, MDB_CREATE, &db.dbi);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        ln_db_anno_commit(false);
        goto LABEL_EXIT;
    }
    retval = MDB_DBI_OPEN(mTxnAnno, M_DBI_ANNOINFO_CNL, MDB_CREATE, &db_info.dbi);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        ln_db_anno_commit(false);
        goto LABEL_EXIT;
    }

    //BOLT#07
    //  * if node_id is NOT previously known from a channel_announcement message, OR if timestamp is NOT greater than the last-received node_announcement from this node_id:
    //    * SHOULD ignore the message.
    //  channel_announcementで受信していないnode_idは無視する
    retval = MDB_DBI_OPEN(mTxnAnno, M_DBI_ANNOCHAN_RECV, MDB_CREATE, &db_aichan.dbi);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        ln_db_anno_commit(false);
        goto LABEL_EXIT;
    }
    data.mv_size = 0;
    data.mv_data = NULL;
    key.mv_size = BTC_SZ_PUBKEY;
    key.mv_data = (CONST_CAST uint8_t *)pNodeId1;
    retval = mdb_put(mTxnAnno, db_aichan.dbi, &key, &data, 0);
    if (retval != 0) {
        LOGE("ERR: channel_announcement node_id 1\n");
        ln_db_anno_commit(false);
        goto LABEL_EXIT;
    }
    key.mv_data = (CONST_CAST uint8_t *)pNodeId2;
    retval = mdb_put(mTxnAnno, db_aichan.dbi, &key, &data, 0);
    if (retval != 0) {
        LOGE("ERR: channel_announcement node_id 2\n");
        ln_db_anno_commit(false);
        goto LABEL_EXIT;
    }

    //channel_announcement
    utl_buf_t buf_ann = UTL_BUF_INIT;
    retval = annocnl_load(&db, &buf_ann, ShortChannelId);
    if (retval != 0) {
        //DB保存されていない＝新規channel
        retval = annocnl_save(&db, pCnlAnno, ShortChannelId);
    } else {
        LOGV("exist channel_announcement: %016" PRIx64 "\n", ShortChannelId);
        if (!utl_buf_equal(&buf_ann, pCnlAnno)) {
            LOGE("fail: different channel_announcement\n");
            retval = -1;
        }
    }
    utl_buf_free(&buf_ann);
    //annoinfo channel
    if ((retval == 0) && (pSendId != NULL)) {
        bool ret = ln_db_annocnlinfo_add_nodeid(&db_info, ShortChannelId, LN_DB_CNLANNO_ANNO, false, pSendId);
        if (!ret) {
            retval = -1;
        }
    }

    ln_db_anno_commit(true);

LABEL_EXIT:
    return retval == 0;
}


/* [channel_update]load
 *
 *  dbi: "channel_anno"
 */
bool ln_db_annocnlupd_load(utl_buf_t *pCnlUpd, uint32_t *pTimeStamp, uint64_t ShortChannelId, uint8_t Dir, void *pDbParam)
{
    int         retval;
    ln_lmdb_db_t   db;
    ln_lmdb_db_t   *p_db;

    if (pDbParam == NULL) {
        bool ret = ln_db_anno_transaction();
        if (!ret) {
            LOGE("ERR: anno transaction\n");
            retval = -1;
            goto LABEL_EXIT;
        }
        retval = MDB_DBI_OPEN(mTxnAnno, M_DBI_ANNO_CNL, 0, &db.dbi);
        if (retval != 0) {
            LOGE("ERR: %s\n", mdb_strerror(retval));
            ln_db_anno_commit(false);
            goto LABEL_EXIT;
        }
        p_db = &db;
    } else {
        p_db = (ln_lmdb_db_t *)pDbParam;
    }

    retval = annocnlupd_load(p_db, pCnlUpd, pTimeStamp, ShortChannelId, Dir);

    if (pDbParam == NULL) {
        ln_db_anno_commit(false);
    }

LABEL_EXIT:
    return retval == 0;
}


/* [channel_update]save
 *  パケット保存と、その送信元node_idの保存を行う。
 *
 *  dbi: "channel_anno"
 *  dbi: "channel_annoinfo"
 */
bool ln_db_annocnlupd_save(const utl_buf_t *pCnlUpd, const ln_msg_channel_update_t *pUpd, const uint8_t *pSendId)
{
    int             retval;
    ln_lmdb_db_t    db, db_info;

    bool ret = ln_db_anno_transaction();
    if (!ret) {
        LOGE("ERR: anno transaction\n");
        retval = -1;
        goto LABEL_EXIT;
    }
    retval = MDB_DBI_OPEN(mTxnAnno, M_DBI_ANNO_CNL, MDB_CREATE, &db.dbi);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        ln_db_anno_commit(false);
        goto LABEL_EXIT;
    }
    retval = MDB_DBI_OPEN(mTxnAnno, M_DBI_ANNOINFO_CNL, MDB_CREATE, &db_info.dbi);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        ln_db_anno_commit(false);
        goto LABEL_EXIT;
    }

    utl_buf_t     buf_upd = UTL_BUF_INIT;
    uint32_t        timestamp;
    bool            upddb = false;
    bool            clr = false;

    retval = annocnlupd_load(&db, &buf_upd, &timestamp, pUpd->short_channel_id, ln_cnlupd_direction(pUpd));
    if (retval == 0) {
        if (timestamp > pUpd->timestamp) {
            //BOLT07
            //  if timestamp is NOT greater than that of the last-received channel_update for this short_channel_id AND for node_id:
            //      SHOULD ignore the message.
            //自分の方が新しければ、スルー
            //LOGD("my channel_update is newer\n");
        } else if (timestamp < pUpd->timestamp) {
            //自分の方が古いので、更新
            LOGD("update: short_channel_id=%016" PRIx64 "(dir=%d)\n", pUpd->short_channel_id, ln_cnlupd_direction(pUpd));
            upddb = true;

            //announceし直す必要があるため、クリアする
            clr = true;
        } else {
            if (utl_buf_equal(&buf_upd, pCnlUpd)) {
                //LOGD("same channel_update: %d\n", ln_cnlupd_direction(pUpd));
            } else {
                //日時が同じなのにデータが異なる
                LOGE("ERR: channel_update %d mismatch !\n", ln_cnlupd_direction(pUpd));
                LOGE("  db: ");
                DUMPE(buf_upd.buf, buf_upd.len);
                LOGE("  rv: ");
                DUMPE(pCnlUpd->buf, pCnlUpd->len);
                retval = -1;
                utl_buf_free(&buf_upd);
                ln_db_anno_commit(false);
                goto LABEL_EXIT;
            }
        }
    } else {
        //新規
        LOGD("new: short_channel_id=%016" PRIx64 "(dir=%d)\n", pUpd->short_channel_id, ln_cnlupd_direction(pUpd));
        upddb = true;
    }
    utl_buf_free(&buf_upd);

    if (upddb) {
        retval = annocnlupd_save(&db, pCnlUpd, pUpd);
    }
    if ((retval == 0) && (pSendId != NULL)) {
        char type = ln_cnlupd_direction(pUpd) ?  LN_DB_CNLANNO_UPD1 : LN_DB_CNLANNO_UPD0;
        bool ret = ln_db_annocnlinfo_add_nodeid(&db_info, pUpd->short_channel_id, type, clr, pSendId);
        if (!ret) {
            retval = -1;
        }
    }

    ln_db_anno_commit(true);

LABEL_EXIT:
    return retval == 0;
}


bool ln_db_annocnlupd_is_prune(uint64_t Now, uint32_t TimesStamp)
{
    //BOLT#7: Pruning the Network View
    //  if a channel's latest channel_updates timestamp is older than two weeks (1209600 seconds):
    //      MAY prune the channel.
    //  https://github.com/lightningnetwork/lightning-rfc/blob/master/07-routing-gossip.md#recommendation-on-pruning-stale-entries
    return (uint64_t)TimesStamp + (uint64_t)1209600 < Now;
}


/* [channel_announcement]delete
 *
 *
 *  dbi: "channel_anno"
 *  dbi: "channel_annoinfo"
 */
bool ln_db_annocnlall_del(uint64_t ShortChannelId)
{
    int         retval;
    MDB_dbi     dbi, dbi_info;
    MDB_val     key;
    uint8_t     keydata[M_SZ_ANNOINFO_CNL + 1];

    bool ret = ln_db_anno_transaction();
    if (!ret) {
        LOGE("ERR: anno transaction\n");
        retval = -1;
        goto LABEL_EXIT;
    }

    retval = MDB_DBI_OPEN(mTxnAnno, M_DBI_ANNO_CNL, MDB_CREATE, &dbi);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        ln_db_anno_commit(false);
        goto LABEL_EXIT;
    }
    retval = MDB_DBI_OPEN(mTxnAnno, M_DBI_ANNOINFO_CNL, MDB_CREATE, &dbi_info);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        ln_db_anno_commit(false);
        goto LABEL_EXIT;
    }

    M_ANNOINFO_CNL_SET(keydata, key, ShortChannelId, 0);

    char POSTFIX[] = { LN_DB_CNLANNO_ANNO, LN_DB_CNLANNO_UPD0, LN_DB_CNLANNO_UPD1 };
    for (size_t lp = 0; lp < ARRAY_SIZE(POSTFIX); lp++) {
        keydata[LN_SZ_SHORT_CHANNEL_ID] = POSTFIX[lp];
        retval = mdb_del(mTxnAnno, dbi, &key, NULL);
        if ((retval != 0) && (retval != MDB_NOTFOUND)) {
            LOGE("ERR[%c]: %s\n", POSTFIX[lp], mdb_strerror(retval));
        }
        retval = mdb_del(mTxnAnno, dbi_info, &key, NULL);
        if ((retval != 0) && (retval != MDB_NOTFOUND)) {
            LOGE("ERR[%c]: %s\n", POSTFIX[lp], mdb_strerror(retval));
        }
    }

    ln_db_anno_commit(true);
    retval = 0;

LABEL_EXIT:
    return retval == 0;
}


/********************************************************************
 * node_announcement
 ********************************************************************/

// dbi: "node_anno"
bool ln_db_annonod_load(utl_buf_t *pNodeAnno, uint32_t *pTimeStamp, const uint8_t *pNodeId)
{
    int         retval;
    ln_lmdb_db_t   db;

    bool ret = ln_db_anno_transaction();
    if (!ret) {
        LOGE("ERR: anno transaction\n");
        retval = -1;
        goto LABEL_EXIT;
    }
    retval = MDB_DBI_OPEN(mTxnAnno, M_DBI_ANNO_NODE, 0, &db.dbi);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        ln_db_anno_commit(false);
        goto LABEL_EXIT;
    }
    retval = annonod_load(&db, pNodeAnno, pTimeStamp, pNodeId);

    ln_db_anno_commit(false);

LABEL_EXIT:
    return retval == 0;
}


// dbi: "node_anno"
// dbi: "node_annoinfo"
bool ln_db_annonod_save(const utl_buf_t *pNodeAnno, const ln_msg_node_announcement_t *pAnno, const uint8_t *pSendId)
{
    int             retval;
    ln_lmdb_db_t    db, db_info, db_aichan;
    utl_buf_t buf_node = UTL_BUF_INIT;
    uint32_t    timestamp;
    bool        upddb = false;
    bool        clr = false;
    MDB_val     key, data;

    bool ret = ln_db_anno_transaction();
    if (!ret) {
        LOGE("ERR: anno transaction\n");
        retval = -1;
        goto LABEL_EXIT;
    }
    retval = MDB_DBI_OPEN(mTxnAnno, M_DBI_ANNO_NODE, MDB_CREATE, &db.dbi);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        ln_db_anno_commit(false);
        goto LABEL_EXIT;
    }
    retval = MDB_DBI_OPEN(mTxnAnno, M_DBI_ANNOINFO_NODE, MDB_CREATE, &db_info.dbi);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        ln_db_anno_commit(false);
        goto LABEL_EXIT;
    }

    if (memcmp(pAnno->p_node_id, ln_node_getid(), BTC_SZ_PUBKEY) != 0) {
        //BOLT#07
        //  * if node_id is NOT previously known from a channel_announcement message, OR if timestamp is NOT greater than the last-received node_announcement from this node_id:
        //    * SHOULD ignore the message.
        //  channel_announcementで受信していないnode_idは無視する
        retval = MDB_DBI_OPEN(mTxnAnno, M_DBI_ANNOCHAN_RECV, 0, &db_aichan.dbi);
        if (retval != 0) {
            LOGE("ERR: %s\n", mdb_strerror(retval));
            ln_db_anno_commit(false);
            goto LABEL_EXIT;
        }
        if (retval == 0) {
            key.mv_size = BTC_SZ_PUBKEY;
            key.mv_data = (CONST_CAST uint8_t *)pAnno->p_node_id;
            retval = mdb_get(mTxnAnno, db_aichan.dbi, &key, &data);
            if (retval != 0) {
                LOGD("skip: not have channel_announcement node_id\n");
                ln_db_anno_commit(false);
                goto LABEL_EXIT;
            }
        }
    }

    retval = annonod_load(&db, &buf_node, &timestamp, pAnno->p_node_id);
    if (retval == 0) {
        if (timestamp > pAnno->timestamp) {
            //自分の方が新しければ、スルー
            LOGV("my node_announcement is newer\n");
            retval = 0;
        } else if (timestamp < pAnno->timestamp) {
            //自分の方が古いので、更新
            LOGV("gotten node_announcement is newer\n");
            upddb = true;

            //announceし直す必要があるため、クリアする
            clr = true;
        } else {
            if (utl_buf_equal(&buf_node, pNodeAnno)) {
                LOGV("same node_announcement\n");
            } else {
                //日時が同じなのにデータが異なる
                LOGE("ERR: node_announcement mismatch !\n");
                retval = -1;
                utl_buf_free(&buf_node);
                ln_db_anno_commit(false);
                goto LABEL_EXIT;
            }
        }
    } else {
        //新規
        LOGV("new node_announcement\n");
        upddb = true;
    }
    utl_buf_free(&buf_node);

    if (upddb) {
        retval = annonod_save(&db, pNodeAnno, pAnno->p_node_id, pAnno->timestamp);
        if ((retval == 0) && ((pSendId != NULL) || (clr && (pSendId == NULL)))) {
            // if (pSendId != NULL) {
            //     LOGD("  nodeinfo: ");
            //     DUMPD(pAnno->p_node_id, BTC_SZ_PUBKEY);
            //     LOGD("  sender: ");
            //     DUMPD(pSendId, BTC_SZ_PUBKEY);
            // }
            bool ret = ln_db_annonodinfo_add_nodeid(&db_info, pAnno->p_node_id, clr, pSendId);
            if (!ret) {
                retval = -1;
            }
        } else {
            //LOGD("  retval=%d, pSendId=%d, clr=%d\n", retval, pSendId != NULL, clr);
        }
    }

    ln_db_anno_commit(true);

LABEL_EXIT:
    return retval == 0;
}


/********************************************************************
 * [anno]cursor
 ********************************************************************/

bool ln_db_anno_cur_open(void **ppCur, ln_db_cur_t Type)
{
    int retval;
    MDB_dbi dbi;
    MDB_cursor *cursor;

    const char *p_name;
    switch (Type) {
    case LN_DB_CUR_CNL:
        p_name = M_DBI_ANNO_CNL;
        break;
    case LN_DB_CUR_NODE:
        p_name = M_DBI_ANNO_NODE;
        break;
    case LN_DB_CUR_INFOCNL:
        p_name = M_DBI_ANNOINFO_CNL;
        break;
    case LN_DB_CUR_INFONODE:
        p_name = M_DBI_ANNOINFO_NODE;
        break;
    default:
        LOGE("fail: unknown CUR: %02x\n", Type);
        return false;
    }
    retval = MDB_DBI_OPEN(mTxnAnno, p_name, MDB_CREATE, &dbi);
    if (retval == 0) {
        retval = mdb_cursor_open(mTxnAnno, dbi, &cursor);
    }
    if (retval == 0) {
        lmdb_cursor_t *p_cur = (lmdb_cursor_t *)UTL_DBG_MALLOC(sizeof(lmdb_cursor_t));
        p_cur->txn = (MDB_txn *)p_name;
        p_cur->dbi = dbi;
        p_cur->cursor = cursor;
        *ppCur = p_cur;
    } else {
        LOGE("ERR(%s): %s\n", p_name, mdb_strerror(retval));
        *ppCur = NULL;
    }

    return retval == 0;
}


void ln_db_anno_cur_close(void *pCur)
{
    if (pCur != NULL) {
        UTL_DBG_FREE(pCur);
    }
}


/* channel_announcement/channel_updateの送信元/送信先登録
 *
 * 既にchannel_announcement/channel_updateを送信したノードや、
 * その情報をもらったノードへはannoundementを送信したくないため、登録しておく。
 *
 * #ln_db_annocnlinfo_search_nodeid()で、送信不要かどうかをチェックする。

 *  dbi: "channel_annoinfo"
 */
bool ln_db_annocnlinfo_add_nodeid(void *pCur, uint64_t ShortChannelId, char Type, bool bClr, const uint8_t *pSendId)
{
    bool ret = true;
    lmdb_cursor_t *p_cur = (lmdb_cursor_t *)pCur;

    MDB_val key, data;
    uint8_t keydata[M_SZ_ANNOINFO_CNL + 1];
    bool detect = false;

    M_ANNOINFO_CNL_SET(keydata, key, ShortChannelId, Type);
    if (!bClr) {
        int retval = mdb_get(mTxnAnno, p_cur->dbi, &key, &data);
        if (retval == 0) {
            detect = annoinfo_search(&data, pSendId);
        } else {
            LOGV("new reg[%016" PRIx64 ":%c] ", ShortChannelId, Type);
            DUMPV(pSendId, BTC_SZ_PUBKEY);
            data.mv_size = 0;
        }
    } else {
        data.mv_size = 0;
    }
    if (!detect) {
        ret = annoinfo_add((ln_lmdb_db_t *)p_cur, &key, &data, pSendId);
    }

    return ret;
}


/* [channel_announcement / channel_update]search received/sent DB
 *
 *  dbi: "channel_annoinfo"
 */
bool ln_db_annocnlinfo_search_nodeid(void *pCur, uint64_t ShortChannelId, char Type, const uint8_t *pSendId)
{
    bool ret = false;
    lmdb_cursor_t *p_cur = (lmdb_cursor_t *)pCur;

    MDB_val key, data;
    uint8_t keydata[M_SZ_ANNOINFO_CNL + 1];

    M_ANNOINFO_CNL_SET(keydata, key, ShortChannelId, Type);
    int retval = mdb_get(mTxnAnno, p_cur->dbi, &key, &data);
    if (retval == 0) {
        // LOGD("short_channel_id[%c]= %016" PRIx64 "\n", Type, ShortChannelId);
        // LOGD("send_id= ");
        // DUMPD(pSendId, BTC_SZ_PUBKEY);
        ret = annoinfo_search(&data, pSendId);
    } else {
        //LOGE("ERR: %s\n", mdb_strerror(retval));
    }

    return ret;
}


/* [channel_announcement / channel_update]cursor
 *
 *  dbi: "channel_anno"
 */
bool ln_db_annocnl_cur_get(void *pCur, uint64_t *pShortChannelId, char *pType, uint32_t *pTimeStamp, utl_buf_t *pBuf)
{
    lmdb_cursor_t *p_cur = (lmdb_cursor_t *)pCur;

    int retval = annocnl_cur_load(p_cur->cursor, pShortChannelId, pType, pTimeStamp, pBuf, MDB_NEXT_NODUP);

    return retval == 0;
}


bool ln_db_annocnl_cur_back(void *pCur)
{
    lmdb_cursor_t *p_cur = (lmdb_cursor_t *)pCur;

    uint64_t short_channel_id;
    char type;
    uint32_t timestamp;
    utl_buf_t buf = UTL_BUF_INIT;
    int retval = annocnl_cur_load(p_cur->cursor, &short_channel_id, &type, &timestamp, &buf, MDB_PREV_NODUP);
    utl_buf_free(&buf);
    return retval == 0;
}


/* [channel_announcement / channel_update]
 *
 *  dbi: "channel_anno"
 */
bool ln_db_annocnl_cur_del(void *pCur)
{
    lmdb_cursor_t *p_cur = (lmdb_cursor_t *)pCur;

    int retval = mdb_cursor_del(p_cur->cursor, 0);
    if ((retval != 0) && (retval != MDB_NOTFOUND)) {
        LOGE("fail: mdb_cursor_get(): %s\n", mdb_strerror(retval));
    }

    return retval == 0;
}


/* [channel_announcement / channel_update]
 *
 *  dbi: "channel_anno"
 */
int ln_lmdb_annocnl_cur_load(MDB_cursor *cur, uint64_t *pShortChannelId, char *pType, uint32_t *pTimeStamp, utl_buf_t *pBuf)
{
    return annocnl_cur_load(cur, pShortChannelId, pType, pTimeStamp, pBuf, MDB_NEXT_NODUP);
}


/* [node_announcement]
 *
 *  dbi: "node_anno"
 */
bool ln_db_annonod_cur_load(void *pCur, utl_buf_t *pNodeAnno, uint32_t *pTimeStamp, const uint8_t *pNodeId)
{
    int         retval;
    lmdb_cursor_t   *p_cur = (lmdb_cursor_t *)pCur;

    retval = annonod_load((ln_lmdb_db_t *)p_cur, pNodeAnno, pTimeStamp, pNodeId);

    return retval == 0;
}


/* [node_announcement]
 *
 *  dbi: "node_annoinfo"
 */
bool ln_db_annonodinfo_search_nodeid(void *pCur, const uint8_t *pNodeId, const uint8_t *pSendId)
{
    //LOGD("node_id= ");
    //DUMPD(pNodeId, BTC_SZ_PUBKEY);
    //LOGD("send_id= ");
    //DUMPD(pSendId, BTC_SZ_PUBKEY);

    bool ret = false;
    lmdb_cursor_t *p_cur = (lmdb_cursor_t *)pCur;

    MDB_val key, data;
    uint8_t keydata[M_SZ_ANNOINFO_NODE];

    M_ANNOINFO_NODE_SET(keydata, key, pNodeId);
    int retval = mdb_get(mTxnAnno, p_cur->dbi, &key, &data);
    if (retval == 0) {
        //LOGD("search...\n");
        ret = annoinfo_search(&data, pSendId);
    } else {
        if (retval != MDB_NOTFOUND) {
            LOGE("ERR: %s\n", mdb_strerror(retval));
        }
    }

    return ret;
}


/* [node_announcement]
 *
 *  dbi: "node_annoinfo"
 */
bool ln_db_annonodinfo_add_nodeid(void *pCur, const uint8_t *pNodeId, bool bClr, const uint8_t *pSendId)
{
    if ((pSendId == NULL) && !bClr) {
        //更新する必要がないため、何もしない
        LOGD("do nothing\n");
        return true;
    }

    bool ret = true;
    lmdb_cursor_t *p_cur = (lmdb_cursor_t *)pCur;

    MDB_val key, data;
    uint8_t keydata[M_SZ_ANNOINFO_NODE];
    bool detect = false;

    M_ANNOINFO_NODE_SET(keydata, key, pNodeId);
    if (!bClr) {
        int retval = mdb_get(mTxnAnno, p_cur->dbi, &key, &data);
        if (retval == 0) {
            detect = annoinfo_search(&data, pSendId);
        } else {
            LOGV("new from ");
            if (pSendId != NULL) {
                DUMPV(pSendId, BTC_SZ_PUBKEY);
            } else {
                LOGV(": only clear\n");
            }
            data.mv_size = 0;
        }
    } else {
        data.mv_size = 0;
    }
    if (!detect) {
        ret = annoinfo_add((ln_lmdb_db_t *)p_cur, &key, &data, pSendId);
    }

    return ret;
}


/* [node_announcement]
 *
 *  dbi: "node_anno"
 */
bool ln_db_annonod_cur_get(void *pCur, utl_buf_t *pBuf, uint32_t *pTimeStamp, uint8_t *pNodeId)
{
    lmdb_cursor_t *p_cur = (lmdb_cursor_t *)pCur;

    int retval = ln_lmdb_annonod_cur_load(p_cur->cursor, pBuf, pTimeStamp, pNodeId);

    return retval == 0;
}


/* [node_announcement]
 *
 *  dbi: "node_anno"
 */
int ln_lmdb_annonod_cur_load(MDB_cursor *cur, utl_buf_t *pBuf, uint32_t *pTimeStamp, uint8_t *pNodeId)
{
    MDB_val key, data;

    int retval = mdb_cursor_get(cur, &key, &data, MDB_NEXT_NODUP);
    if (retval == 0) {
        // LOGD("key:  ");
        // DUMPD(key.mv_data, key.mv_size);
        // LOGD("data: ");
        // DUMPD(data.mv_data, data.mv_size);
        if (pNodeId) {
            memcpy(pNodeId, key.mv_data, key.mv_size);
        }
        memcpy(pTimeStamp, data.mv_data, sizeof(uint32_t));
        utl_buf_alloccopy(pBuf, (const uint8_t *)data.mv_data + sizeof(uint32_t), data.mv_size - sizeof(uint32_t));
    } else {
        if (retval != MDB_NOTFOUND) {
            LOGE("ERR: %s\n", mdb_strerror(retval));
        } else {
            //end of cursor
        }
    }

    return retval;
}


/********************************************************************
 * [anno]own channel
 ********************************************************************/

/*
 * dbi: "annoown"
 */
bool ln_db_annoown_save(uint64_t ShortChannelId)
{
    int         retval;
    ln_lmdb_db_t   db;
    MDB_val     key, data;

    bool ret = ln_db_anno_transaction();
    if (!ret) {
        LOGE("ERR: anno transaction\n");
        retval = -1;
        goto LABEL_EXIT;
    }
    retval = MDB_DBI_OPEN(mTxnAnno, M_DBI_ANNOOWN, MDB_CREATE, &db.dbi);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        ln_db_anno_commit(false);
        goto LABEL_EXIT;
    }
    data.mv_size = 0;
    data.mv_data = NULL;
    key.mv_size = sizeof(uint64_t);
    key.mv_data = (uint8_t *)&ShortChannelId;
    retval = mdb_put(mTxnAnno, db.dbi, &key, &data, 0);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        ln_db_anno_commit(false);
        goto LABEL_EXIT;
    }

    ln_db_anno_commit(true);

LABEL_EXIT:
    return retval == 0;
}


/*
 * dbi: "annoown"
 */
bool ln_db_annoown_check(uint64_t ShortChannelId)
{
    int         retval;
    ln_lmdb_db_t   db;

    if (mTxnAnno == NULL) {
        LOGE("fail: no txn\n");
        return false;
    }

    retval = MDB_DBI_OPEN(mTxnAnno, M_DBI_ANNOOWN, 0, &db.dbi);
    if (retval == 0) {
        MDB_val     key, data;

        key.mv_size = sizeof(uint64_t);
        key.mv_data = (uint8_t *)&ShortChannelId;
        retval = mdb_get(mTxnAnno, db.dbi, &key, &data);
    } else {
        if (retval != MDB_NOTFOUND) {
            LOGE("ERR: %s\n", mdb_strerror(retval));
        }
    }

    return retval == 0;
}


/*
 * dbi: "annoown"
 */
bool ln_db_annoown_del(uint64_t ShortChannelId)
{
    int         retval;
    ln_lmdb_db_t   db;
    MDB_val     key;

    bool ret = ln_db_anno_transaction();
    if (!ret) {
        LOGE("ERR: anno transaction\n");
        retval = -1;
        goto LABEL_EXIT;
    }
    retval = MDB_DBI_OPEN(mTxnAnno, M_DBI_ANNOOWN, 0, &db.dbi);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        ln_db_anno_commit(false);
        goto LABEL_EXIT;
    }
    key.mv_size = sizeof(uint64_t);
    key.mv_data = (uint8_t *)&ShortChannelId;
    retval = mdb_del(mTxnAnno, db.dbi, &key, NULL);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
    }

    ln_db_anno_commit(retval == 0);

LABEL_EXIT:
    return retval == 0;
}


/********************************************************************
 * [anno]annocnl, annonod共通
 ********************************************************************/

bool ln_db_annoinfos_del(const uint8_t *pNodeId)
{
    int         retval;
    MDB_dbi     dbi_cnl;
    MDB_dbi     dbi_nod;

    bool ret = ln_db_anno_transaction();
    if (!ret) {
        LOGE("ERR: anno transaction\n");
        retval = -1;
        goto LABEL_EXIT;
    }

    retval = MDB_DBI_OPEN(mTxnAnno, M_DBI_ANNOINFO_CNL, 0, &dbi_cnl);
    if (retval != 0) {
        if (retval != MDB_NOTFOUND) {
            LOGE("ERR: %s\n", mdb_strerror(retval));
        }
        goto LABEL_EXIT;
    }
    retval = MDB_DBI_OPEN(mTxnAnno, M_DBI_ANNOINFO_NODE, 0, &dbi_nod);
    if (retval != 0) {
        if (retval != MDB_NOTFOUND) {
            LOGE("ERR: %s\n", mdb_strerror(retval));
        }
        goto LABEL_EXIT;
    }

    if (pNodeId != NULL) {
        LOGD("del annoinfo: ");
        DUMPD(pNodeId, BTC_SZ_PUBKEY);
        MDB_cursor  *cursor;

        //annocnl info
        retval = mdb_cursor_open(mTxnAnno, dbi_cnl, &cursor);
        if (retval == 0) {
            annoinfo_cur_trim(cursor, pNodeId);
            mdb_cursor_close(cursor);
        } else {
            LOGE("ERR: %s\n", mdb_strerror(retval));
        }

        //annonode info
        retval = mdb_cursor_open(mTxnAnno, dbi_nod, &cursor);
        if (retval == 0) {
            annoinfo_cur_trim(cursor, pNodeId);
            mdb_cursor_close(cursor);
        } else {
            LOGE("ERR: %s\n", mdb_strerror(retval));
        }
    } else {
        LOGD("del annoinfo: ALL\n");
        //annocnl info
        retval = mdb_drop(mTxnAnno, dbi_cnl, 1);
        if (retval != 0) {
            LOGE("ERR: %s\n", mdb_strerror(retval));
            //エラーでも継続
        }

        //annonod info
        retval = mdb_drop(mTxnAnno, dbi_nod, 1);
        if (retval != 0) {
            LOGE("ERR: %s\n", mdb_strerror(retval));
            //エラーでも継続
        }
    }

    ln_db_anno_commit(true);
    retval = 0;

    if (pNodeId != NULL) {
        LOGD("remove annoinfo: ");
        DUMPD(pNodeId, BTC_SZ_PUBKEY);
    } else {
        LOGD("remove annoinfo: ALL\n");
    }

LABEL_EXIT:
    if (retval != 0) {
        ln_db_anno_commit(false);
    }

    return retval == 0;
}


/********************************************************************
 * [node]skip routing list
 ********************************************************************/

bool ln_db_routeskip_save(uint64_t ShortChannelId, bool bTemp)
{
    LOGD("short_channel_id=%016" PRIx64 ", bTemp=%d\n", ShortChannelId, bTemp);

    int         retval;
    MDB_val key, data;
    ln_lmdb_db_t db;

    retval = node_db_open(&db, M_DBI_ROUTE_SKIP, 0, MDB_CREATE);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }

    key.mv_size = sizeof(ShortChannelId);
    key.mv_data = &ShortChannelId;
    uint8_t data_temp = LNDB_ROUTE_SKIP_TEMP;
    if (bTemp) {
        data.mv_size = sizeof(data_temp);
        data.mv_data = &data_temp;
    } else {
        data.mv_size = 0;
    }
    retval = mdb_put(db.txn, db.dbi, &key, &data, 0);
    if (retval == 0) {
        LOGD("add skip[%d]: %016" PRIx64 "\n", bTemp, ShortChannelId);
    } else {
        LOGE("ERR: %s\n", mdb_strerror(retval));
    }

    MDB_TXN_COMMIT(db.txn);

LABEL_EXIT:
    return retval == 0;
}


bool ln_db_routeskip_work(bool bWork)
{
    LOGD("bWork=%d\n", bWork);

    int         retval;
    MDB_val     key, data;
    MDB_cursor  *cursor;
    ln_lmdb_db_t db;

    retval = node_db_open(&db, M_DBI_ROUTE_SKIP, 0, MDB_CREATE);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }
    retval = mdb_cursor_open(db.txn, db.dbi, &cursor);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        MDB_TXN_ABORT(db.txn);
        goto LABEL_EXIT;
    }
    while ((retval = mdb_cursor_get(cursor, &key, &data, MDB_NEXT)) == 0) {
        const uint8_t *p_data = (const uint8_t *)data.mv_data;
        if (data.mv_size == sizeof(uint8_t)) {
            mdb_val_alloccopy(&key, &key);

            uint64_t short_channel_id = *(uint64_t *)key.mv_data;
            uint8_t wk = 0;
            if (bWork && (p_data[0] == LNDB_ROUTE_SKIP_TEMP)) {
                LOGD("TEMP-->WORK: %016" PRIx64 "\n", short_channel_id);
                wk = LNDB_ROUTE_SKIP_WORK;
            } else if (!bWork && (p_data[0] == LNDB_ROUTE_SKIP_WORK)) {
                LOGD("WORK-->TEMP: %016" PRIx64 "\n", short_channel_id);
                wk = LNDB_ROUTE_SKIP_TEMP;
            }
            if (wk != 0) {
                data.mv_data = &wk;
                int retval_put = mdb_cursor_put(cursor, &key, &data, MDB_CURRENT);
                UTL_DBG_FREE(key.mv_data);
                if (retval_put != 0) {
                    LOGD("through: put(%s)\n", mdb_strerror(retval_put));
                }
            }
        }
    }
    retval = 0;

    MDB_TXN_COMMIT(db.txn);

LABEL_EXIT:
    return retval == 0;
}


/* open DB cursor(M_DBI_ROUTE_SKIP)
 *
 *  dbi: "route_skip"
 */
ln_db_routeskip_t ln_db_routeskip_search(uint64_t ShortChannelId)
{
    ln_db_routeskip_t result = LN_DB_ROUTESKIP_NONE;
    int         retval;
    MDB_val     key, data;
    ln_lmdb_db_t db;

    retval = node_db_open(&db, M_DBI_ROUTE_SKIP, 0, 0);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }

    key.mv_size = sizeof(ShortChannelId);
    key.mv_data = &ShortChannelId;
    retval = mdb_get(db.txn, db.dbi, &key, &data);
    if (retval == 0) {
        const uint8_t *p_data = (const uint8_t *)data.mv_data;
        if (data.mv_size == 0) {
            result = LN_DB_ROUTESKIP_PERM;
        } else if (data.mv_size == sizeof(uint8_t)) {
            switch (p_data[0]) {
            case LNDB_ROUTE_SKIP_TEMP:
                result = LN_DB_ROUTESKIP_TEMP;
                break;
            case LNDB_ROUTE_SKIP_WORK:
                result = LN_DB_ROUTESKIP_WORK;
                break;
            default:
                LOGE("fail: unknown value: %02x\n", p_data[0]);
                break;
            }
        } else {
            LOGE("fail\n");
        }
    } else {
        result = LN_DB_ROUTESKIP_NONE;
    }
    MDB_TXN_ABORT(db.txn);

LABEL_EXIT:
    if ((retval != 0) && (retval != MDB_NOTFOUND)) {
        LOGE("fail: %s\n", mdb_strerror(retval));
    }
    return result;
}


bool ln_db_routeskip_drop(bool bTemp)
{
    int         retval;
    ln_lmdb_db_t db;

    retval = node_db_open(&db, M_DBI_ROUTE_SKIP, 0, 0);
    if (retval != 0) {
        if (retval == MDB_NOTFOUND) {
            //存在しないなら削除しなくてよい
            LOGD("no db\n");
            retval = 0;
        } else {
            LOGE("ERR: %s\n", mdb_strerror(retval));
        }
        goto LABEL_EXIT;
    }

    if (bTemp) {
        MDB_cursor  *cursor;
        MDB_val     key, data;

        LOGD("remove temporary only\n");
        retval = mdb_cursor_open(db.txn, db.dbi, &cursor);
        if (retval != 0) {
            LOGE("ERR: %s\n", mdb_strerror(retval));
        }
        while ((retval = mdb_cursor_get(cursor, &key, &data, MDB_NEXT)) == 0) {
            const uint8_t *p_data = (const uint8_t *)data.mv_data;
            if ( (data.mv_size == sizeof(uint8_t)) &&
                 (p_data[0] == LNDB_ROUTE_SKIP_TEMP) ) {
                int ret = mdb_cursor_del(cursor, 0);
                if (ret == 0) {
                    uint64_t val;
                    memcpy(&val, key.mv_data, sizeof(uint64_t));
                    LOGD("del skip: %016" PRIx64 "\n", val);
                } else {
                    LOGE("ERR: %s\n", mdb_strerror(ret));
                }
            }
        }
        retval = 0;
    } else {
        LOGD("remove all\n");

        retval = mdb_drop(db.txn, db.dbi, 1);
        if (retval != 0) {
            LOGE("ERR: %s\n", mdb_strerror(retval));
        }
    }

    MDB_TXN_COMMIT(db.txn);

LABEL_EXIT:
    LOGD("skip drop=%d\n", retval);
    return retval == 0;
}


/********************************************************************
 * [node]invoice
 ********************************************************************/

bool ln_db_invoice_save(const char *pInvoice, uint64_t AddAmountMsat, const uint8_t *pPayHash)
{
    int         retval;
    MDB_val     key, data;
    ln_lmdb_db_t db;

    retval = node_db_open(&db, M_DBI_INVOICE, 0, MDB_CREATE);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }

    LOGD("\n");
    key.mv_size = BTC_SZ_HASH256;
    key.mv_data = (CONST_CAST uint8_t *)pPayHash;
    size_t len = strlen(pInvoice);
    data.mv_size = len + 1 + sizeof(AddAmountMsat);    //invoice(\0含む) + uint64_t
    uint8_t *p_data = (uint8_t *)UTL_DBG_MALLOC(data.mv_size);
    data.mv_data = p_data;
    memcpy(p_data, pInvoice, len + 1);  //\0までコピー
    p_data += len + 1;
    memcpy(p_data, &AddAmountMsat, sizeof(AddAmountMsat));
    retval = mdb_put(db.txn, db.dbi, &key, &data, 0);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
    }
    UTL_DBG_FREE(data.mv_data);

    MDB_TXN_COMMIT(db.txn);

LABEL_EXIT:
    return retval == 0;
}


bool ln_db_invoice_load(char **ppInvoice, uint64_t *pAddAmountMsat, const uint8_t *pPayHash)
{
    int         retval;
    MDB_val     key, data;
    ln_lmdb_db_t db;

    *ppInvoice = NULL;

    retval = node_db_open(&db, M_DBI_INVOICE, MDB_RDONLY, 0);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }

    key.mv_size = BTC_SZ_HASH256;
    key.mv_data = (CONST_CAST uint8_t *)pPayHash;
    retval = mdb_get(db.txn, db.dbi, &key, &data);
    if (retval == 0) {
        *ppInvoice = UTL_DBG_STRDUP(data.mv_data);
        size_t len = strlen(*ppInvoice);
        data.mv_size -= len;
        if (data.mv_size > sizeof(uint64_t)) {
            memcpy(pAddAmountMsat, data.mv_data + len + 1, sizeof(uint64_t));
        } else {
            *pAddAmountMsat = 0;
        }
    }
    MDB_TXN_ABORT(db.txn);

LABEL_EXIT:
    return retval == 0;
}


int ln_db_invoice_get(uint8_t **ppPayHash)
{
    int         retval;
    MDB_val     key, data;
    MDB_cursor  *cursor;
    ln_lmdb_db_t db;

    *ppPayHash = NULL;
    int cnt = 0;

    retval = node_db_open(&db, M_DBI_INVOICE, MDB_RDONLY, 0);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }
    retval = mdb_cursor_open(db.txn, db.dbi, &cursor);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
    }

    while ((retval = mdb_cursor_get(cursor, &key, &data, MDB_NEXT)) == 0) {
        if (key.mv_size == BTC_SZ_HASH256) {
            cnt++;
            *ppPayHash = (uint8_t *)UTL_DBG_REALLOC(*ppPayHash, cnt * BTC_SZ_HASH256);
            memcpy(*ppPayHash + (cnt - 1) * BTC_SZ_HASH256, key.mv_data, BTC_SZ_HASH256);
        }
    }
    MDB_TXN_ABORT(db.txn);

LABEL_EXIT:
    return cnt;
}


bool ln_db_invoice_del(const uint8_t *pPayHash)
{
    int         retval;
    MDB_val     key;
    ln_lmdb_db_t db;

    LOGD("payment_hash=");
    DUMPD(pPayHash, BTC_SZ_HASH256);

    retval = node_db_open(&db, M_DBI_INVOICE, 0, MDB_CREATE);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }

    //再送があるため、同じkeyで上書きして良い
    key.mv_size = BTC_SZ_HASH256;
    key.mv_data = (CONST_CAST uint8_t*)pPayHash;
    retval = mdb_del(db.txn, db.dbi, &key, NULL);
    if ((retval != 0) && (retval != MDB_NOTFOUND)) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
    }

    MDB_TXN_COMMIT(db.txn);

LABEL_EXIT:
    return retval == 0;
}


bool ln_db_invoice_drop(void)
{
    int         retval;
    ln_lmdb_db_t db;

    retval = node_db_open(&db, M_DBI_INVOICE, 0, 0);
    if (retval != 0) {
        if (retval == MDB_NOTFOUND) {
            //存在しないなら削除もしなくて良い
            retval = 0;
        } else {
            LOGE("ERR: %s\n", mdb_strerror(retval));
        }
        goto LABEL_EXIT;
    }

    LOGD("\n");
    retval = mdb_drop(db.txn, db.dbi, 1);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
    }

    MDB_TXN_COMMIT(db.txn);

LABEL_EXIT:
    return retval == 0;
}


/********************************************************************
 * [node]payment preimage
 ********************************************************************/

bool ln_db_preimage_save(ln_db_preimage_t *pPreimage, void *pDb)
{
    bool ret;
    ln_lmdb_db_t db;
    MDB_val key, data;
    MDB_txn *txn = NULL;
    preimage_info_t info;

    if (pDb != NULL) {
        txn = ((ln_lmdb_db_t *)pDb)->txn;
        MDB_TXN_CHECK_NODE(txn);
    }
    ret = preimage_open(&db, txn);
    if (!ret) {
        LOGE("fail\n");
        return false;
    }

    key.mv_size = LN_SZ_PREIMAGE;
    key.mv_data = pPreimage->preimage;
    data.mv_size = sizeof(info);
    info.amount = pPreimage->amount_msat;
    info.creation = (uint64_t)utl_time_time();
    info.expiry = pPreimage->expiry;
    data.mv_data = &info;
    int retval = mdb_put(db.txn, db.dbi, &key, &data, 0);
    if (retval == 0) {
        pPreimage->creation_time = info.creation;
    } else {
        LOGE("ERR: %s\n", mdb_strerror(retval));
    }

    preimage_close(&db, txn);

    return retval == 0;
}


bool ln_db_preimage_del(const uint8_t *pPreimage)
{
    bool ret;
    int retval = -1;
    ln_lmdb_db_t db;

    ret = preimage_open(&db, NULL);
    if (!ret) {
        LOGE("fail: open\n");
        goto LABEL_EXIT;
    }

    if (pPreimage != NULL) {
        MDB_val key;

        LOGD("remove: ");
        DUMPD(pPreimage, LN_SZ_PREIMAGE);
        key.mv_size = LN_SZ_PREIMAGE;
        key.mv_data = (CONST_CAST uint8_t *)pPreimage;
        retval = mdb_del(db.txn, db.dbi, &key, NULL);
    } else {
        LOGD("remove all\n");
        retval = mdb_drop(db.txn, db.dbi, 1);
    }
    if (retval == 0) {
        LOGD("success\n");
    } else {
        LOGE("ERR: %s\n", mdb_strerror(retval));
    }

    preimage_close(&db, NULL);

LABEL_EXIT:
    return retval == 0;
}


bool ln_db_preimage_search(ln_db_func_preimage_t pFunc, void *p_param)
{
    void *p_cur;
    bool ret = ln_db_preimage_cur_open(&p_cur);
    while (ret) {
        ln_db_preimage_t preimage;
        bool detect;
        ret = ln_db_preimage_cur_get(p_cur, &detect, &preimage);
        if (detect) {
            ret = (*pFunc)(preimage.preimage, preimage.amount_msat, preimage.expiry, p_cur, p_param);
            if (ret) {
                break;
            }
            ret = true;
        }
    }
    ln_db_preimage_cur_close(p_cur);

    return ret;
}


bool ln_db_preimage_del_hash(const uint8_t *pPreimageHash)
{
    bool ret = ln_db_preimage_search(preimage_del_func, (CONST_CAST uint8_t *)pPreimageHash);
    return ret;
}


bool ln_db_preimage_cur_open(void **ppCur)
{
    int         retval;
    lmdb_cursor_t *p_cur = (lmdb_cursor_t *)UTL_DBG_MALLOC(sizeof(lmdb_cursor_t));

    retval = node_db_open((ln_lmdb_db_t *)p_cur, M_DBI_PREIMAGE, 0, 0);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }
    retval = mdb_cursor_open(p_cur->txn, p_cur->dbi, &p_cur->cursor);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
    }

LABEL_EXIT:
    if (retval == 0) {
        *ppCur = p_cur;
    } else {
        UTL_DBG_FREE(p_cur);
        *ppCur = NULL;
    }
    return retval == 0;
}


void ln_db_preimage_cur_close(void *pCur)
{
    if (pCur != NULL) {
        lmdb_cursor_t *p_cur = (lmdb_cursor_t *)pCur;
        mdb_cursor_close(p_cur->cursor);
        if (p_cur->txn != NULL) {
            MDB_TXN_CHECK_NODE(p_cur->txn);
            MDB_TXN_COMMIT(p_cur->txn);
        }
    }
}


bool ln_db_preimage_cur_get(void *pCur, bool *pDetect, ln_db_preimage_t *pPreimage)
{
    lmdb_cursor_t *p_cur = (lmdb_cursor_t *)pCur;
    int retval;
    MDB_val key, data;
    uint64_t now = (uint64_t)utl_time_time();

    *pDetect = false;

    if ((retval = mdb_cursor_get(p_cur->cursor, &key, &data, MDB_NEXT_NODUP)) == 0) {
        preimage_info_t *p_info = (preimage_info_t *)data.mv_data;
        LOGD("amount: %" PRIu64"\n", p_info->amount);
        LOGD("time: %lu\n", p_info->creation);
        pPreimage->expiry = p_info->expiry;
        pPreimage->creation_time = p_info->creation;
        if ((p_info->expiry == UINT32_MAX) || (now <= p_info->creation + p_info->expiry)) {
            memcpy(pPreimage->preimage, key.mv_data, key.mv_size);
            pPreimage->amount_msat = p_info->amount;
            *pDetect = true;

            uint8_t hash[BTC_SZ_HASH256];
            ln_payment_hash_calc(hash, pPreimage->preimage);
            LOGD("invoice hash: ");
            DUMPD(hash, BTC_SZ_HASH256);
        } else {
            //期限切れ
            LOGD("invoice timeout del: ");
            DUMPD(key.mv_data, key.mv_size);
            mdb_cursor_del(p_cur->cursor, 0);
        }
    }

    return retval == 0;
}


bool ln_db_preimage_set_expiry(void *pCur, uint32_t Expiry)
{
    lmdb_cursor_t *p_cur = (lmdb_cursor_t *)pCur;
    int retval;
    MDB_val key, data;

    retval = mdb_cursor_get(p_cur->cursor, &key, &data, MDB_GET_CURRENT);
    if (retval == 0) {
        preimage_info_t *p_info = (preimage_info_t *)data.mv_data;
        LOGD("amount: %" PRIu64"\n", p_info->amount);
        LOGD("time: %lu\n", p_info->creation);

        mdb_val_alloccopy(&key, &key);

        preimage_info_t info;
        memcpy(&info, p_info, data.mv_size);
        info.expiry = Expiry;
        data.mv_data = &info;
        data.mv_size = sizeof(preimage_info_t);
        retval = mdb_cursor_put(p_cur->cursor, &key, &data, MDB_CURRENT);
        UTL_DBG_FREE(key.mv_data);
    }
    if (retval == 0) {
        LOGD("  change expiry: %" PRIu32 "\n", Expiry);
    } else {
        LOGE("ERR: %s\n", mdb_strerror(retval));
    }

    return retval == 0;
}


#ifdef LN_UGLY_NORMAL
/********************************************************************
 * [node]payment_hash
 ********************************************************************/

bool ln_db_phash_save(const uint8_t *pPayHash, const uint8_t *pVout, ln_commit_tx_output_type_t Type, uint32_t Expiry)
{
    int         retval;
    MDB_val     key, data;
    ln_lmdb_db_t db;

    retval = node_db_open(&db, M_DBI_PAYHASH, 0, MDB_CREATE);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }

    key.mv_size = BTC_SZ_WITPROG_P2WSH;
    key.mv_data = (CONST_CAST uint8_t *)pVout;
    uint8_t hash[1 + sizeof(uint32_t) + BTC_SZ_HASH256];
    hash[0] = (uint8_t)Type;
    memcpy(hash + 1, &Expiry, sizeof(uint32_t));
    memcpy(hash + 1 + sizeof(uint32_t), pPayHash, BTC_SZ_HASH256);
    data.mv_size = sizeof(hash);
    data.mv_data = hash;
    retval = mdb_put(db.txn, db.dbi, &key, &data, 0);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
    }

LABEL_EXIT:
    if (db.txn != NULL) {
        if (retval == 0) {
            MDB_TXN_COMMIT(db.txn);
        } else {
            MDB_TXN_ABORT(db.txn);
        }
    }

    return retval == 0;
}


bool ln_db_phash_search(uint8_t *pPayHash, ln_commit_tx_output_type_t *pType, uint32_t *pExpiry, const uint8_t *pVout, void *pDbParam)
{
    int         retval;
    MDB_txn     *txn;
    MDB_dbi     dbi;
    MDB_cursor  *cursor;
    MDB_val     key, data;
    bool found = false;

    MDB_txn *txn_tmp = ((ln_lmdb_db_t *)pDbParam)->txn;
    if (mdb_txn_env(txn_tmp) == mpDbNode) {
        txn = txn_tmp;
    } else {
        MDB_TXN_BEGIN(mpDbNode, NULL, 0, &txn);
    }

    retval = MDB_DBI_OPEN(txn, M_DBI_PAYHASH, 0, &dbi);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }
    retval = mdb_cursor_open(txn, dbi, &cursor);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }

    while ((retval = mdb_cursor_get(cursor, &key, &data, MDB_NEXT)) == 0) {
        if ( (key.mv_size == BTC_SZ_WITPROG_P2WSH) &&
             (memcmp(key.mv_data, pVout, BTC_SZ_WITPROG_P2WSH) == 0) ) {
            uint8_t *p = (uint8_t *)data.mv_data;
            *pType = (ln_commit_tx_output_type_t)*p;
            memcpy(pExpiry, p + 1, sizeof(uint32_t));
            memcpy(pPayHash, p + 1 + sizeof(uint32_t), BTC_SZ_HASH256);
            found = true;
            break;
        }
    }
    mdb_cursor_close(cursor);

LABEL_EXIT:
    if (txn != txn_tmp) {
        MDB_TXN_ABORT(txn);
    }
    return found;
}

#endif  //LN_UGLY_NORMAL


/********************************************************************
 * [channel]revoked transaction close
 ********************************************************************/

bool ln_db_revtx_load(ln_channel_t *pChannel, void *pDbParam)
{
    MDB_val key, data;
    MDB_txn     *txn;
    MDB_dbi     dbi;
    char        dbname[M_SZ_DBNAME_LEN + 1];

    txn = ((ln_lmdb_db_t *)pDbParam)->txn;

    utl_str_bin2str(dbname + M_PREFIX_LEN, pChannel->channel_id, LN_SZ_CHANNEL_ID);
    memcpy(dbname, M_PREF_REVOKED, M_PREFIX_LEN);

    int retval = MDB_DBI_OPEN(txn, dbname, 0, &dbi);
    if (retval != 0) {
        //LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }

    ln_revoked_buf_free(pChannel);
    key.mv_size = LNDBK_RLEN;

    //number of vout scripts
    key.mv_data = LNDBK_RVN;
    retval = mdb_get(txn, dbi, &key, &data);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }
    uint16_t *p = (uint16_t *)data.mv_data;
    pChannel->revoked_cnt = p[0];
    pChannel->revoked_num = p[1];
    ln_revoked_buf_alloc(pChannel);

    //vout scripts
    key.mv_data = LNDBK_RVV;
    retval = mdb_get(txn, dbi, &key, &data);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }
    uint8_t *p_scr = (uint8_t *)data.mv_data;
    for (int lp = 0; lp < pChannel->revoked_num; lp++) {
        uint16_t len;
        memcpy(&len, p_scr, sizeof(len));
        p_scr += sizeof(len);
        utl_buf_alloccopy(&pChannel->p_revoked_vout[lp], p_scr, len);
        p_scr += len;
    }

    //witness script
    key.mv_data = LNDBK_RVW;
    retval = mdb_get(txn, dbi, &key, &data);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }
    p_scr = (uint8_t *)data.mv_data;
    for (int lp = 0; lp < pChannel->revoked_num; lp++) {
        uint16_t len;
        memcpy(&len, p_scr, sizeof(len));
        p_scr += sizeof(len);
        utl_buf_alloccopy(&pChannel->p_revoked_wit[lp], p_scr, len);
        p_scr += len;
    }

    //HTLC type
    key.mv_data = LNDBK_RVT;
    retval = mdb_get(txn, dbi, &key, &data);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }
    memcpy(pChannel->p_revoked_type, data.mv_data, data.mv_size);

    //remote per_commit_secret
    key.mv_data = LNDBK_RVS;
    retval = mdb_get(txn, dbi, &key, &data);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }
    utl_buf_free(&pChannel->revoked_sec);
    utl_buf_alloccopy(&pChannel->revoked_sec, data.mv_data, data.mv_size);

    //confirmation数
    key.mv_data = LNDBK_RVC;
    retval = mdb_get(txn, dbi, &key, &data);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }
    memcpy(&pChannel->revoked_chk, data.mv_data, sizeof(uint32_t));

LABEL_EXIT:
    return retval == 0;
}


bool ln_db_revtx_save(const ln_channel_t *pChannel, bool bUpdate, void *pDbParam)
{
    MDB_val key, data;
    ln_lmdb_db_t   db;
    char        dbname[M_SZ_DBNAME_LEN + 1];
    utl_buf_t buf = UTL_BUF_INIT;
    utl_push_t push;

    db.txn = ((ln_lmdb_db_t *)pDbParam)->txn;

    utl_str_bin2str(dbname + M_PREFIX_LEN, pChannel->channel_id, LN_SZ_CHANNEL_ID);
    memcpy(dbname, M_PREF_REVOKED, M_PREFIX_LEN);

    int retval = MDB_DBI_OPEN(db.txn, dbname, MDB_CREATE, &db.dbi);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }

    key.mv_size = LNDBK_RLEN;
    key.mv_data = LNDBK_RVV;
    utl_push_init(&push, &buf, 0);
    for (int lp = 0; lp < pChannel->revoked_num; lp++) {
        utl_push_data(&push, &pChannel->p_revoked_vout[lp].len, sizeof(uint16_t));
        utl_push_data(&push, pChannel->p_revoked_vout[lp].buf, pChannel->p_revoked_vout[lp].len);
    }
    data.mv_size = buf.len;
    data.mv_data = buf.buf;
    retval = mdb_put(db.txn, db.dbi, &key, &data, 0);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }
    utl_buf_free(&buf);

    key.mv_data = LNDBK_RVW;
    utl_push_init(&push, &buf, 0);
    for (int lp = 0; lp < pChannel->revoked_num; lp++) {
        utl_push_data(&push, &pChannel->p_revoked_wit[lp].len, sizeof(uint16_t));
        utl_push_data(&push, pChannel->p_revoked_wit[lp].buf, pChannel->p_revoked_wit[lp].len);
    }
    data.mv_size = buf.len;
    data.mv_data = buf.buf;
    retval = mdb_put(db.txn, db.dbi, &key, &data, 0);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }
    utl_buf_free(&buf);

    key.mv_data = LNDBK_RVT;
    data.mv_size = sizeof(ln_commit_tx_output_type_t) * pChannel->revoked_num;
    data.mv_data = pChannel->p_revoked_type;
    retval = mdb_put(db.txn, db.dbi, &key, &data, 0);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }

    key.mv_data = LNDBK_RVS;
    data.mv_size = pChannel->revoked_sec.len;
    data.mv_data = pChannel->revoked_sec.buf;
    retval = mdb_put(db.txn, db.dbi, &key, &data, 0);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }

    key.mv_data = LNDBK_RVN;
    data.mv_size = sizeof(uint16_t) * 2;
    uint16_t p[2];
    p[0] = pChannel->revoked_cnt;
    p[1] = pChannel->revoked_num;
    data.mv_data = p;
    retval = mdb_put(db.txn, db.dbi, &key, &data, 0);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }

    key.mv_data = LNDBK_RVC;
    data.mv_size = sizeof(pChannel->revoked_chk);
    data.mv_data = (CONST_CAST uint32_t *)&pChannel->revoked_chk;
    retval = mdb_put(db.txn, db.dbi, &key, &data, 0);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }

    if (bUpdate) {
        memcpy(dbname, M_PREF_CHANNEL, M_PREFIX_LEN);
        retval = MDB_DBI_OPEN(db.txn, dbname, 0, &db.dbi);
        if (retval != 0) {
            LOGE("ERR: %s\n", mdb_strerror(retval));
            goto LABEL_EXIT;
        }
        retval = channel_save(pChannel, &db);
        if (retval != 0) {
            LOGE("ERR: %s\n", mdb_strerror(retval));
            goto LABEL_EXIT;
        }

    }
LABEL_EXIT:
    LOGD("retval=%d\n", retval);
    return retval == 0;
}


/********************************************************************
 * [walt]wallet
 ********************************************************************/

bool ln_db_wallet_load(utl_buf_t *pBuf, const uint8_t *pTxid, uint32_t Index)
{
    int         retval;
    MDB_val     key, data;
    ln_lmdb_db_t db;

    uint8_t outpoint[BTC_SZ_TXID + sizeof(uint32_t)];
    memcpy(outpoint, pTxid, BTC_SZ_TXID);
    memcpy(outpoint + BTC_SZ_TXID, &Index, sizeof(uint32_t));

    retval = wallet_db_open(&db, M_DBI_WALLET, 0, MDB_CREATE);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }

    key.mv_size = sizeof(outpoint);
    key.mv_data = outpoint;

    retval = mdb_get(db.txn, db.dbi, &key, &data);
    if (retval == 0) {
        if (pBuf != NULL) {
            utl_buf_alloccopy(pBuf, data.mv_data, data.mv_size);
        }
    } else {
        LOGE("ERR: %s\n", mdb_strerror(retval));
    }

LABEL_EXIT:
    if (db.txn != NULL) {
        if (retval == 0) {
            MDB_TXN_COMMIT(db.txn);
        } else {
            MDB_TXN_ABORT(db.txn);
        }
    }

    return retval == 0;
}


/**
 * key: outpoint
 *      [32: txid] little endian
 *      [4: index]
 * data:
 *      [1: type]
 *      [8: amount] little endian
 *      [4: sequence]
 *      [4: locktime]
 *      [1: datanum] {
 *          1: len
 *          len: data
 *      }
 */
bool ln_db_wallet_add(const ln_db_wallet_t *pWallet)
{
    // LOGD("txid=");
    // TXIDD(pWallet->p_txid);
    // LOGD("index=%d\n", pWallet->index);
    // LOGD("amount=%" PRIu64 "\n", pWallet->amount);
    // LOGD("sequence=%" PRIx32 "\n", pWallet->sequence);
    // LOGD("locktime=%" PRIx32 "\n", pWallet->locktime);
    // LOGD("cnt=%d\n", pWallet->wit_item_cnt);
    // for (uint8_t lp = 0; lp < pWallet->wit_item_cnt; lp++) {
    //     LOGD("[%d]", lp);
    //     DUMPD(pWallet->p_wit[lp].buf, pWallet->p_wit[lp].len);
    // }

    if (pWallet->wit_item_cnt < 2) {
        LOGE("fail: wit_item_cnt < 2\n");
        return false;
    }
    if (pWallet->p_wit[0].len != BTC_SZ_PRIVKEY) {
        LOGE("fail: wit0 must be privkey\n");
        return false;
    }

    int         retval;
    MDB_val     key, data;
    ln_lmdb_db_t db;
    uint8_t outpoint[BTC_SZ_TXID + sizeof(uint32_t)];

    memcpy(outpoint, pWallet->p_txid, BTC_SZ_TXID);
    memcpy(outpoint + BTC_SZ_TXID, &pWallet->index, sizeof(uint32_t));
    LOGD(" txid: ");
    TXIDD(pWallet->p_txid);
    LOGD(" idx : %d\n", (int)pWallet->index);

    retval = wallet_db_open(&db, M_DBI_WALLET, 0, MDB_CREATE);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }

    key.mv_size = sizeof(outpoint);
    key.mv_data = outpoint;
    data.mv_size = sizeof(uint8_t) +            //type
                    sizeof(uint64_t) +          //amount
                    sizeof(uint32_t) +          //sequence
                    sizeof(uint32_t) +          //locktime
                    sizeof(uint8_t);            //datanum
    for (uint32_t lp = 0; lp < pWallet->wit_item_cnt; lp++) {
        //len + data
        LOGD("[%d]len=%d, ", lp, pWallet->p_wit[lp].len);
        DUMPD(pWallet->p_wit[lp].buf, pWallet->p_wit[lp].len);
        data.mv_size += sizeof(uint8_t) + (uint8_t)pWallet->p_wit[lp].len;
    }
    uint8_t *wit = (uint8_t *)UTL_DBG_MALLOC(data.mv_size);
    data.mv_data = wit;

    *wit = pWallet->type;
    wit++;
    memcpy(wit, &pWallet->amount, sizeof(uint64_t));
    wit += sizeof(uint64_t);
    memcpy(wit, &pWallet->sequence, sizeof(uint32_t));
    wit += sizeof(uint32_t);
    memcpy(wit, &pWallet->locktime, sizeof(uint32_t));
    wit += sizeof(uint32_t);
    *wit = (uint8_t)(pWallet->wit_item_cnt);
    wit++;
    for (uint32_t lp = 0; lp < pWallet->wit_item_cnt; lp++) {
        *wit = (uint8_t)pWallet->p_wit[lp].len;
        wit++;
        memcpy(wit, pWallet->p_wit[lp].buf, pWallet->p_wit[lp].len);
        wit += pWallet->p_wit[lp].len;
    }

    //save
    retval = mdb_put(db.txn, db.dbi, &key, &data, 0);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
    }
    UTL_DBG_FREE(data.mv_data);

LABEL_EXIT:
    if (db.txn != NULL) {
        if (retval == 0) {
            MDB_TXN_COMMIT(db.txn);
        } else {
            MDB_TXN_ABORT(db.txn);
        }
    }

    return retval == 0;
}


bool ln_db_wallet_search(ln_db_func_wallet_t pWalletFunc, void *pFuncParam)
{
    bool        ret = false;
    int         retval;
    lmdb_cursor_t cur;

    cur.cursor = NULL;
    retval = wallet_db_open((ln_lmdb_db_t *)&cur, M_DBI_WALLET, 0, 0);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }

    ret = ln_lmdb_wallet_search(&cur, pWalletFunc, pFuncParam);

LABEL_EXIT:
    if (cur.cursor != NULL) {
        mdb_cursor_close(cur.cursor);
    }
    if (cur.txn != NULL) {
        MDB_TXN_COMMIT(cur.txn);
    }
    return ret;
}


bool ln_lmdb_wallet_search(lmdb_cursor_t *pCur, ln_db_func_wallet_t pWalletFunc, void *pFuncParam)
{
    int         retval;
    MDB_val     key;
    MDB_val     data;

    retval = mdb_cursor_open(pCur->txn, pCur->dbi, &pCur->cursor);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        return false;
    }

    while ((retval = mdb_cursor_get(pCur->cursor, &key, &data, MDB_NEXT_NODUP)) == 0) {
        ln_db_wallet_t wallet = LN_DB_WALLET_INIT(0);

        uint8_t *k = (uint8_t *)key.mv_data;
        uint8_t *d = (uint8_t *)data.mv_data;

        wallet.p_txid = k;
        memcpy(&wallet.index, k + BTC_SZ_TXID, sizeof(uint32_t));

        wallet.type = *d;
        d++;
        memcpy(&wallet.amount, d, sizeof(uint64_t));
        d += sizeof(uint64_t);
        memcpy(&wallet.sequence, d, sizeof(uint32_t));
        d += sizeof(uint32_t);
        memcpy(&wallet.locktime, d, sizeof(uint32_t));
        d += sizeof(uint32_t);
        wallet.wit_item_cnt = *d;
        d++;
        if (wallet.wit_item_cnt > 0) {
            wallet.p_wit = UTL_DBG_MALLOC(sizeof(utl_buf_t) * wallet.wit_item_cnt);
            for (uint8_t lp = 0; lp < wallet.wit_item_cnt; lp++) {
                wallet.p_wit[lp].len = *d;
                d++;
                if (wallet.p_wit[lp].len > 0) {
                    wallet.p_wit[lp].buf = d;
                    d += wallet.p_wit[lp].len;
                } else {
                    wallet.p_wit[lp].buf = NULL;
                }
            }
        }
        bool stop = (*pWalletFunc)(&wallet, pFuncParam);
        UTL_DBG_FREE(wallet.p_wit);
        if (stop) {
            break;
        }
    }
    if (retval == MDB_NOTFOUND) {
        retval = 0;
    } else {
        LOGE("ERR: %s\n", mdb_strerror(retval));
    }

    return retval == 0;
}


bool ln_db_wallet_del(const uint8_t *pTxid, uint32_t Index)
{
    int         retval;
    MDB_val     key;
    ln_lmdb_db_t db;
    uint8_t outpoint[BTC_SZ_TXID + sizeof(uint32_t)];

    memcpy(outpoint, pTxid, BTC_SZ_TXID);
    memcpy(outpoint + BTC_SZ_TXID, &Index, sizeof(uint32_t));
    LOGD(" txid: ");
    TXIDD(pTxid);
    LOGD(" idx : %d\n", (int)Index);

    retval = wallet_db_open(&db, M_DBI_WALLET, 0, 0);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }

    key.mv_size = sizeof(outpoint);
    key.mv_data = outpoint;
    retval = mdb_del(db.txn, db.dbi, &key, NULL);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
    }

    MDB_TXN_COMMIT(db.txn);

LABEL_EXIT:
    return retval == 0;
}


/********************************************************************
 * [channel]version
 ********************************************************************/

bool ln_db_ver_check(uint8_t *pMyNodeId, btc_block_chain_t *pGType)
{
    int             retval;
    ln_lmdb_db_t    db;

    retval = channel_db_open(&db, M_DBI_VERSION, MDB_RDONLY, 0);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }

    int32_t ver;
    char wif[BTC_SZ_WIF_STR_MAX + 1] = "";
    char alias[LN_SZ_ALIAS_STR + 1] = "";
    uint16_t port = 0;
    uint8_t genesis[BTC_SZ_HASH256];
    retval = ver_check(&db, &ver, wif, alias, &port, genesis);
    if (retval == 0) {
        btc_keys_t key;
        btc_chain_t chain;

        btc_block_chain_t gtype = btc_block_get_chain(genesis);
        bool ret = btc_keys_wif2keys(&key, &chain, wif);
        if (
          ((chain == BTC_MAINNET) && (gtype == BTC_BLOCK_CHAIN_BTCMAIN)) ||
          ((chain == BTC_TESTNET) && (
                (gtype == BTC_BLOCK_CHAIN_BTCTEST) || (gtype == BTC_BLOCK_CHAIN_BTCREGTEST)) ) ) {
            //ok
        } else {
            ret = false;
        }
        if (ret) {
            if (pMyNodeId != NULL) {
                memcpy(pMyNodeId, key.pub, BTC_SZ_PUBKEY);
            }
            if (pGType != NULL) {
                *pGType = gtype;
            }
        } else {
            retval = -1;
        }
    }
    MDB_TXN_ABORT(db.txn);

LABEL_EXIT:
    return retval == 0;
}


int ln_db_lmdb_get_mynodeid(MDB_txn *txn, MDB_dbi dbi, int32_t *ver, char *wif, char *alias, uint16_t *p_port, uint8_t *genesis)
{
    int             retval;
    ln_lmdb_db_t    db;

    db.txn = txn;
    db.dbi = dbi;
    retval = ver_check(&db, ver, wif, alias, p_port, genesis);
    return retval;
}


/********************************************************************
 * others
 ********************************************************************/

ln_lmdb_dbtype_t ln_lmdb_get_dbtype(const char *pDbName)
{
    ln_lmdb_dbtype_t dbtype;

    if (strncmp(pDbName, M_PREF_CHANNEL, M_PREFIX_LEN) == 0) {
        //channl
        dbtype = LN_LMDB_DBTYPE_CHANNEL;
    } else if (strncmp(pDbName, M_PREF_SECRET, M_PREFIX_LEN) == 0) {
        //secret
        dbtype = LN_LMDB_DBTYPE_SECRET;
    } else if (strncmp(pDbName, M_PREF_ADDHTLC, M_PREFIX_LEN) == 0) {
        //add_htlc
        dbtype = LN_LMDB_DBTYPE_ADD_HTLC;
    } else if (strncmp(pDbName, M_PREF_REVOKED, M_PREFIX_LEN) == 0) {
        //revoked transaction
        dbtype = LN_LMDB_DBTYPE_REVOKED;
    } else if (strncmp(pDbName, M_PREF_BAKCHANNEL, M_PREFIX_LEN) == 0) {
        //removed pChannel
        dbtype = LN_LMDB_DBTYPE_BKCHANNEL;
    } else if (strcmp(pDbName, M_DBI_WALLET) == 0) {
        //wallet
        dbtype = LN_LMDB_DBTYPE_WALLET;
    } else if (strcmp(pDbName, M_DBI_ANNO_CNL) == 0) {
        //channel_announcement
        dbtype = LN_LMDB_DBTYPE_ANNO_CNL;
    } else if (strcmp(pDbName, M_DBI_ANNO_NODE) == 0) {
        //node_announcement
        dbtype = LN_LMDB_DBTYPE_ANNO_NODE;
    } else if (strcmp(pDbName, M_DBI_ANNOINFO_CNL) == 0) {
        //channel_announcement/channel_update information
        dbtype = LN_LMDB_DBTYPE_ANNOINFO_CNL;
    } else if (strcmp(pDbName, M_DBI_ANNOINFO_NODE) == 0) {
        //node_announcement information
        dbtype = LN_LMDB_DBTYPE_ANNOINFO_NODE;
    } else if (strcmp(pDbName, M_DBI_ROUTE_SKIP) == 0) {
        //route skip
        dbtype = LN_LMDB_DBTYPE_ROUTE_SKIP;
    } else if (strcmp(pDbName, M_DBI_INVOICE) == 0) {
        //payment invoice
        dbtype = LN_LMDB_DBTYPE_INVOICE;
    } else if (strcmp(pDbName, M_DBI_PREIMAGE) == 0) {
        //preimage
        dbtype = LN_LMDB_DBTYPE_PREIMAGE;
#ifdef LN_UGLY_NORMAL
    } else if (strcmp(pDbName, M_DBI_PAYHASH) == 0) {
        //preimage
        dbtype = LN_LMDB_DBTYPE_PAYHASH;
#endif //LN_UGLY_NORMAL
    } else if (strcmp(pDbName, M_DBI_VERSION) == 0) {
        //version
        dbtype = LN_LMDB_DBTYPE_VERSION;
    } else {
        dbtype = LN_LMDB_DBTYPE_UNKNOWN;
    }

    return dbtype;
}


/* btcのDB動作を借りたいために、showdb/routingから使用される。
 *
 */
void ln_lmdb_setenv(MDB_env *p_env, MDB_env *p_node, MDB_env *p_anno, MDB_env *p_walt)
{
    mpDbChannel = p_env;
    mpDbNode = p_node;
    mpDbAnno = p_anno;
    mpDbWalt = p_walt;
}


bool ln_db_reset(void)
{
    int retval;

    if (mpDbChannel != NULL) {
        LOGE("fail: already started\n");
        return false;
    }

    if (mPath[0] == '\0') {
        ln_lmdb_set_path(".");
    }
    retval = init_dbenv(M_INITPARAM_CHANNEL);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }

    bool ret = false;
    lmdb_cursor_t cur;
    LOGD("channel cursor open\n");
    retval = channel_cursor_open(&cur, true);
    if (retval != 0) {
        LOGE("fail: open\n");
        goto LABEL_EXIT;
    }
    ret = true;     //ここまで来たら成功と見なしてよい

    MDB_val     key;
    while ((ret = mdb_cursor_get(cur.cursor, &key, NULL, MDB_NEXT_NODUP)) == 0) {
        if (memcmp(key.mv_data, M_DBI_VERSION, sizeof(M_DBI_VERSION) - 1) != 0) {
            //"version"以外は削除
            MDB_dbi dbi2;
            char *name = (char *)UTL_DBG_MALLOC(key.mv_size + 1);
            memcpy(name, key.mv_data, key.mv_size);
            name[key.mv_size] = '\0';
            LOGD("dbname: %s\n", name);

            retval = MDB_DBI_OPEN(cur.txn, name, 0, &dbi2);
            UTL_DBG_FREE(name);
            if (retval == 0) {
                retval = mdb_drop(cur.txn, dbi2, 1);
                if (retval != 0) {
                    LOGE("ERR: %s\n", mdb_strerror(retval));
                }
            }
        }
    }
    channel_cursor_close(&cur, true);

    //node, annoはディレクトリごと削除
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", ln_lmdb_get_nodepath());
    system(cmd);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", ln_lmdb_get_annopath());
    system(cmd);

LABEL_EXIT:
    return ret;
}


void HIDDEN ln_db_copy_channel(ln_channel_t *pOutChannel, const ln_channel_t *pInChannel)
{
    LOGD("recover\n");
    //固定サイズ

    for (size_t lp = 0; lp < ARRAY_SIZE(DBCHANNEL_VALUES); lp++) {
        memcpy((uint8_t *)pOutChannel + DBCHANNEL_VALUES[lp].offset, (uint8_t *)pInChannel + DBCHANNEL_VALUES[lp].offset,  DBCHANNEL_VALUES[lp].datalen);
    }

    memcpy(
        pOutChannel->update_info.htlcs,  pInChannel->update_info.htlcs,
        M_SIZE(ln_update_info_t, htlcs));

    //復元データ
    utl_buf_alloccopy(&pOutChannel->funding_info.wit_script, pInChannel->funding_info.wit_script.buf, pInChannel->funding_info.wit_script.len);
    pOutChannel->funding_info.key_order = pInChannel->funding_info.key_order;


    //可変サイズ(shallow copy)

    //funding_info.tx_data
    btc_tx_free(&pOutChannel->funding_info.tx_data);
    memcpy(&pOutChannel->funding_info.tx_data, &pInChannel->funding_info.tx_data, sizeof(btc_tx_t));

    //shutdown_scriptpk_local
    utl_buf_free(&pOutChannel->shutdown_scriptpk_local);
    memcpy(&pOutChannel->shutdown_scriptpk_local, &pInChannel->shutdown_scriptpk_local, sizeof(utl_buf_t));

    //shutdown_scriptpk_remote
    utl_buf_free(&pOutChannel->shutdown_scriptpk_remote);
    memcpy(&pOutChannel->shutdown_scriptpk_remote, &pInChannel->shutdown_scriptpk_remote, sizeof(utl_buf_t));

    //keys
    memcpy(&pOutChannel->keys_local, &pInChannel->keys_local, sizeof(ln_derkey_local_keys_t));
    memcpy(&pOutChannel->keys_remote, &pInChannel->keys_remote, sizeof(ln_derkey_remote_keys_t));
}


/********************************************************************
 * private functions: channel
 ********************************************************************/



static int channel_db_open(ln_lmdb_db_t *pDb, const char *pDbName, int OptTxn, int OptDb)
{
    int             retval;

    retval = MDB_TXN_BEGIN(mpDbChannel, NULL, OptTxn, &pDb->txn);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        pDb->txn = NULL;
        goto LABEL_EXIT;
    }
    retval = MDB_DBI_OPEN(pDb->txn, pDbName, OptDb, &pDb->dbi);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        MDB_TXN_ABORT(pDb->txn);
        pDb->txn = NULL;
        goto LABEL_EXIT;
    }

LABEL_EXIT:
    return retval;
}


/** channel: add_htlc読み込み
 *
 * @param[out]      pChannel
 * @param[in]       pDb
 * @retval      true    成功
 */
static int channel_htlc_load(ln_channel_t *pChannel, ln_lmdb_db_t *pDb)
{
    int         retval;
    MDB_dbi     dbi;
    MDB_val     key, data;
    char        dbname[M_SZ_DBNAME_LEN + M_SZ_HTLC_STR + 1];

    uint8_t *OFFSET =
        ((uint8_t *)pChannel) + offsetof(ln_channel_t, update_info) + offsetof(ln_update_info_t, htlcs);

    utl_str_bin2str(dbname + M_PREFIX_LEN, pChannel->channel_id, LN_SZ_CHANNEL_ID);
    memcpy(dbname, M_PREF_ADDHTLC, M_PREFIX_LEN);

    for (int lp = 0; lp < LN_HTLC_RECEIVED_MAX; lp++) {
        channel_htlc_dbname(dbname, lp);
        //LOGD("[%d]dbname: %s\n", lp, dbname);
        retval = MDB_DBI_OPEN(pDb->txn, dbname, 0, &dbi);
        if (retval != 0) {
            LOGE("ERR: %s(%s)\n", mdb_strerror(retval), dbname);
            continue;
        }
        //固定
        for (size_t lp2 = 0; lp2 < ARRAY_SIZE(DBHTLC_VALUES); lp2++) {
            key.mv_size = strlen(DBHTLC_VALUES[lp2].name);
            key.mv_data = (CONST_CAST char*)DBHTLC_VALUES[lp2].name;
            retval = mdb_get(pDb->txn, dbi, &key, &data);
            if (retval == 0) {
                //LOGD("[%d]%s: ", lp, DBHTLC_VALUES[lp2].name);
                //DUMPD(data.mv_data, data.mv_size);
                memcpy(OFFSET + sizeof(ln_htlc_t) * lp + DBHTLC_VALUES[lp2].offset, data.mv_data, DBHTLC_VALUES[lp2].datalen);
            } else {
                LOGE("ERR: %s(%s)\n", mdb_strerror(retval), DBHTLC_VALUES[lp2].name);
            }
        }

        //可変
        key.mv_size = M_SZ_PREIMAGE;
        key.mv_data = M_KEY_PREIMAGE;
        retval = mdb_get(pDb->txn, dbi, &key, &data);
        if (retval == 0) {
            utl_buf_alloccopy(&pChannel->update_info.htlcs[lp].buf_payment_preimage, data.mv_data, data.mv_size);
        } else {
            //LOGE("ERR: %s(preimage)\n", mdb_strerror(retval));
        }

        key.mv_size = M_SZ_ONIONROUTE;
        key.mv_data = M_KEY_ONIONROUTE;
        retval = mdb_get(pDb->txn, dbi, &key, &data);
        if (retval == 0) {
            utl_buf_alloccopy(&pChannel->update_info.htlcs[lp].buf_onion_reason, data.mv_data, data.mv_size);
        } else {
            //LOGE("ERR: %s(onion_route)\n", mdb_strerror(retval));
            retval = 0;     //FALLTHROUGH
        }

        key.mv_size = M_SZ_SHAREDSECRET;
        key.mv_data = M_KEY_SHAREDSECRET;
        retval = mdb_get(pDb->txn, dbi, &key, &data);
        if (retval == 0) {
            utl_buf_alloccopy(&pChannel->update_info.htlcs[lp].buf_shared_secret, data.mv_data, data.mv_size);
        } else {
            //LOGE("ERR: %s(shared_secret)\n", mdb_strerror(retval));
            retval = 0;     //FALLTHROUGH
        }
        mdb_dbi_close(mpDbChannel, dbi);
    }

    return retval;
}


/** channel: add_htlc書込み
 *
 * @param[in]       pChannel
 * @param[in]       pDb
 * @retval      true    成功
 */
static int channel_htlc_save(const ln_channel_t *pChannel, ln_lmdb_db_t *pDb)
{
    int         retval;
    MDB_dbi     dbi;
    MDB_val     key, data;
    char        dbname[M_SZ_DBNAME_LEN + M_SZ_HTLC_STR + 1];

    uint8_t *OFFSET =
        ((uint8_t *)pChannel) + offsetof(ln_channel_t, update_info) + offsetof(ln_update_info_t, htlcs);

    utl_str_bin2str(dbname + M_PREFIX_LEN, pChannel->channel_id, LN_SZ_CHANNEL_ID);
    memcpy(dbname, M_PREF_ADDHTLC, M_PREFIX_LEN);

    for (int lp = 0; lp < LN_HTLC_RECEIVED_MAX; lp++) {
        channel_htlc_dbname(dbname, lp);
        //LOGD("[%d]dbname: %s\n", lp, dbname);
        retval = MDB_DBI_OPEN(pDb->txn, dbname, MDB_CREATE, &dbi);
        if (retval != 0) {
            LOGE("ERR: %s(%s)\n", mdb_strerror(retval), dbname);
            goto LABEL_EXIT;
        }

        //固定
        ln_lmdb_db_t db;
        db.txn = pDb->txn;
        db.dbi = dbi;
        retval = backup_param_save(OFFSET + sizeof(ln_htlc_t) * lp,
                        &db, DBHTLC_VALUES, ARRAY_SIZE(DBHTLC_VALUES));
        if (retval != 0) {
            LOGE("ERR: %s\n", mdb_strerror(retval));
            goto LABEL_EXIT;
        }

        //可変
        if (pChannel->update_info.htlcs[lp].buf_payment_preimage.len > 0) {
            key.mv_size = M_SZ_PREIMAGE;
            key.mv_data = M_KEY_PREIMAGE;
            data.mv_size = pChannel->update_info.htlcs[lp].buf_payment_preimage.len;
            data.mv_data = pChannel->update_info.htlcs[lp].buf_payment_preimage.buf;
            retval = mdb_put(pDb->txn, dbi, &key, &data, 0);
            if (retval != 0) {
                LOGE("ERR: %s(preimage)\n", mdb_strerror(retval));
                goto LABEL_EXIT;
            }
        }

        key.mv_size = M_SZ_ONIONROUTE;
        key.mv_data = M_KEY_ONIONROUTE;
        data.mv_size = pChannel->update_info.htlcs[lp].buf_onion_reason.len;
        data.mv_data = pChannel->update_info.htlcs[lp].buf_onion_reason.buf;
        retval = mdb_put(pDb->txn, dbi, &key, &data, 0);
        if (retval != 0) {
            LOGE("ERR: %s(onion_route)\n", mdb_strerror(retval));
            goto LABEL_EXIT;
        }

        key.mv_size = M_SZ_SHAREDSECRET;
        key.mv_data = M_KEY_SHAREDSECRET;
        data.mv_size = pChannel->update_info.htlcs[lp].buf_shared_secret.len;
        data.mv_data = pChannel->update_info.htlcs[lp].buf_shared_secret.buf;
        retval = mdb_put(pDb->txn, dbi, &key, &data, 0);
        if (retval != 0) {
            LOGE("ERR: %s(shared_secret)\n", mdb_strerror(retval));
            goto LABEL_EXIT;
        }
    }

LABEL_EXIT:
    return retval;
}


/** channel情報書き込み
 *
 * @param[in]       pChannel
 * @param[in,out]   pDb
 * @retval      true    成功
 */
static int channel_save(const ln_channel_t *pChannel, ln_lmdb_db_t *pDb)
{
    MDB_val key, data;
    int retval;

    //固定サイズ
    retval = backup_param_save(pChannel, pDb, DBCHANNEL_VALUES, ARRAY_SIZE(DBCHANNEL_VALUES));
    if (retval != 0) {
        goto LABEL_EXIT;
    }

    //可変サイズ
    utl_buf_t buf_funding = UTL_BUF_INIT;
    btc_tx_write(&pChannel->funding_info.tx_data, &buf_funding);
    //
    backup_buf_t *p_dbscript_keys = (backup_buf_t *)UTL_DBG_MALLOC(sizeof(backup_buf_t) * M_CHANNEL_BUFS);
    int index = 0;
    p_dbscript_keys[index].name = "buf_funding";
    p_dbscript_keys[index].p_buf = &buf_funding;
    index++;
    M_BUF_ITEM(index, shutdown_scriptpk_local);
    index++;
    M_BUF_ITEM(index, shutdown_scriptpk_remote);
    //index++;

    for (size_t lp = 0; lp < M_CHANNEL_BUFS; lp++) {
        key.mv_size = strlen(p_dbscript_keys[lp].name);
        key.mv_data = (CONST_CAST char*)p_dbscript_keys[lp].name;
        data.mv_size = p_dbscript_keys[lp].p_buf->len;
        data.mv_data = p_dbscript_keys[lp].p_buf->buf;
        retval = mdb_put(pDb->txn, pDb->dbi, &key, &data, 0);
        if (retval != 0) {
            LOGE("fail: %s\n", p_dbscript_keys[lp].name);
            break;
        }
    }

    utl_buf_free(&buf_funding);
    UTL_DBG_FREE(p_dbscript_keys);

LABEL_EXIT:
    return retval;
}


static int channel_item_load(ln_channel_t *pChannel, const backup_param_t *pBackupParam, ln_lmdb_db_t *pDb)
{
    int             retval;
    MDB_val         key, data;

    key.mv_size = strlen(pBackupParam->name);
    key.mv_data = (CONST_CAST char*)pBackupParam->name;
    retval = mdb_get(pDb->txn, pDb->dbi, &key, &data);
    if ((retval == 0) && (data.mv_size == pBackupParam->datalen)) {
        memcpy((uint8_t *)pChannel + pBackupParam->offset, data.mv_data, data.mv_size);
    } else {
        LOGE("fail: %s(%s)\n", mdb_strerror(retval), pBackupParam->name);
    }

    return retval;
}


static int channel_item_save(const ln_channel_t *pChannel, const backup_param_t *pBackupParam, ln_lmdb_db_t *pDb)
{
    int             retval;
    MDB_val         key, data;

    ln_lmdb_db_t *p_bak_dbparam = pDb;
    ln_lmdb_db_t db;
    if (p_bak_dbparam == NULL) {
        char            dbname[M_SZ_DBNAME_LEN + 1];
        utl_str_bin2str(dbname + M_PREFIX_LEN, pChannel->channel_id, LN_SZ_CHANNEL_ID);
        memcpy(dbname, M_PREF_CHANNEL, M_PREFIX_LEN);
        int retval = channel_db_open(&db, dbname, 0, 0);
        if (retval != 0) {
            LOGE("ERR: %s\n", mdb_strerror(retval));
            goto LABEL_EXIT;
        }
        pDb = &db;
    }

    key.mv_size = strlen(pBackupParam->name);
    key.mv_data = (CONST_CAST char*)pBackupParam->name;
    data.mv_size = pBackupParam->datalen;
    data.mv_data = (uint8_t *)pChannel + pBackupParam->offset;
    retval = mdb_put(pDb->txn, pDb->dbi, &key, &data, 0);
    if (retval != 0) {
        LOGE("fail: %s(%s)\n", mdb_strerror(retval), pBackupParam->name);
    }

LABEL_EXIT:
    if (p_bak_dbparam == NULL) {
        if (retval == 0) {
            MDB_TXN_COMMIT(db.txn);
        } else {
            MDB_TXN_ABORT(db.txn);
        }
    }
    return retval;
}


static int channel_secret_load(ln_channel_t *pChannel, ln_lmdb_db_t *pDb)
{
    int retval;
    char        dbname[M_SZ_DBNAME_LEN + M_SZ_HTLC_STR + 1];

    utl_str_bin2str(dbname + M_PREFIX_LEN, pChannel->channel_id, LN_SZ_CHANNEL_ID);
    memcpy(dbname, M_PREF_SECRET, M_PREFIX_LEN);
    retval = MDB_DBI_OPEN(pDb->txn, dbname, 0, &pDb->dbi);
    if (retval == 0) {
        retval = backup_param_load(pChannel, pDb, DBCHANNEL_SECRET, ARRAY_SIZE(DBCHANNEL_SECRET));
    }
    if (retval != 0) {
        LOGE("ERR: %s(backup_param_load)\n", mdb_strerror(retval));
    }
    // LOGD("[priv]storage_index: %016" PRIx64 "\n", ln_derkey_local_privkeys_get_current_storage_index(&pChannel->privkeys);
    // LOGD("[priv]storage_seed: ");
    // DUMPD(pChannel->privkeys.storage_seed, BTC_SZ_PRIVKEY);
    // size_t lp;
    // for (lp = 0; lp < LN_BASEPOINT_IDX_NUM; lp++) {
    //     LOGD("[priv][%lu] ", lp);
    //     DUMPD(pChannel->privkeys.key[lp], BTC_SZ_PRIVKEY);
    // }
    // LOGD("[priv][%lu] ", lp);
    // DUMPD(pChannel->privkeys.per_commitment_secret, BTC_SZ_PRIVKEY);

    return retval;
}


static int channel_secret_restore(ln_channel_t *pChannel)
{
    int retval = 0;
    if (!ln_derkey_restore(&pChannel->keys_local, &pChannel->keys_remote)) {
        retval = -1;
        LOGE("ERR\n");
        goto LABEL_EXIT;
    }
    if (!btc_script_2of2_create_redeem_sorted(
                &pChannel->funding_info.wit_script,
                &pChannel->funding_info.key_order,
                pChannel->keys_local.basepoints[LN_BASEPOINT_IDX_FUNDING],
                pChannel->keys_remote.basepoints[LN_BASEPOINT_IDX_FUNDING])) {
        retval = -1;
        LOGE("ERR\n");
    }
    LOGD("key restored.\n");

LABEL_EXIT:
    return retval;
}


/**
 *
 * @param[out]      pCur
 * @retval      0   成功
 */
static int channel_cursor_open(lmdb_cursor_t *pCur, bool bWritable)
{
    int             retval;
    int             opt;

    opt = (bWritable) ? 0 : MDB_RDONLY;
    retval = channel_db_open((ln_lmdb_db_t *)pCur, NULL, opt, 0);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }
    retval = mdb_cursor_open(pCur->txn, pCur->dbi, &pCur->cursor);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        MDB_TXN_ABORT(pCur->txn);
        goto LABEL_EXIT;
    }

LABEL_EXIT:
    return retval;
}


/**
 *
 * @param[out]      pCur
 */
static void channel_cursor_close(lmdb_cursor_t *pCur, bool bWritable)
{
    mdb_cursor_close(pCur->cursor);
    if (bWritable) {
        MDB_TXN_COMMIT(pCur->txn);
    } else {
        MDB_TXN_ABORT(pCur->txn);
    }
}


/** htlc用db名の作成
 *
 * @note
 *      - "HT" + xxxxxxxx...xx[32*2] + "ddd"
 *        |<-- M_SZ_DBNAME_LEN  -->|
 *
 * @attention
 *      - 予め pDbName に M_PREF_ADDHTLC と channel_idはコピーしておくこと
 */
static void channel_htlc_dbname(char *pDbName, int num)
{
    char htlc_str[M_SZ_HTLC_STR + 1];

    snprintf(htlc_str, sizeof(htlc_str), "%03d", num);
    memcpy(pDbName + M_SZ_DBNAME_LEN, htlc_str, M_SZ_HTLC_STR);
    pDbName[M_SZ_DBNAME_LEN + M_SZ_HTLC_STR] = '\0';

}


/** #ln_node_search_channel()処理関数
 *
 * @param[in,out]   pChannel        channel from DB
 * @param[in,out]   p_db_param      DB情報(ln_dbで使用する)
 * @param[in,out]   p_param         comp_param_cnl_t構造体
 */
static bool channel_comp_func_cnldel(ln_channel_t *pChannel, void *p_db_param, void *p_param)
{
    (void)p_db_param;
    const uint8_t *p_channel_id = (const uint8_t *)p_param;

    bool ret = (memcmp(pChannel->channel_id, p_channel_id, LN_SZ_CHANNEL_ID) == 0);
    if (ret) {
        ln_db_channel_del_param(pChannel, p_db_param);

        //true時は呼び元では解放しないので、ここで解放する
        ln_term(pChannel);
    }
    return ret;
}


static bool channel_search(ln_db_func_cmp_t pFunc, void *pFuncParam, bool bWritable, bool bRestore)
{
    bool            result = false;
    int             retval;
    lmdb_cursor_t   cur;

    LOGD("channl cursor open(writable=%d)\n", bWritable);
    retval = channel_cursor_open(&cur, bWritable);
    if (retval != 0) {
        LOGE("fail: open\n");
        goto LABEL_EXIT;
    }

    ln_channel_t *p_channel = (ln_channel_t *)UTL_DBG_MALLOC(sizeof(ln_channel_t));
    bool ret;
    MDB_val     key;
    char name[M_SZ_DBNAME_LEN + 1];
    name[sizeof(name) - 1] = '\0';
    while ((ret = mdb_cursor_get(cur.cursor, &key, NULL, MDB_NEXT_NODUP)) == 0) {
        if ((key.mv_size == M_SZ_DBNAME_LEN) && (memcmp(key.mv_data, M_PREF_CHANNEL, M_PREFIX_LEN) == 0)) {
            memcpy(name, key.mv_data, M_SZ_DBNAME_LEN);
            ret = MDB_DBI_OPEN(cur.txn, name, 0, &cur.dbi);
            if (ret == 0) {
                memset(p_channel, 0, sizeof(ln_channel_t));
                retval = ln_lmdb_channel_load(p_channel, cur.txn, cur.dbi, bRestore);
                if (retval == 0) {
                    result = (*pFunc)(p_channel, (void *)&cur, pFuncParam);
                    if (result) {
                        LOGD("match !\n");
                        break;
                    }
                    ln_term(p_channel);     //falseのみ解放
                } else {
                    LOGE("ERR: %s\n", mdb_strerror(retval));
                }
            } else {
                LOGE("ERR: %s\n", mdb_strerror(retval));
            }
        }
    }
    channel_cursor_close(&cur, bWritable);
    UTL_DBG_FREE(p_channel);

LABEL_EXIT:
    LOGD("result=%d(writable=%d)\n", result, bWritable);
    return result;
}


/********************************************************************
 * private functions: node
 ********************************************************************/

static int node_db_open(ln_lmdb_db_t *pDb, const char *pDbName, int OptTxn, int OptDb)
{
    int             retval;

    retval = MDB_TXN_BEGIN(mpDbNode, NULL, OptTxn, &pDb->txn);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        pDb->txn = NULL;
        goto LABEL_EXIT;
    }
    retval = MDB_DBI_OPEN(pDb->txn, pDbName, OptDb, &pDb->dbi);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        MDB_TXN_ABORT(pDb->txn);
        pDb->txn = NULL;
        goto LABEL_EXIT;
    }

LABEL_EXIT:
    return retval;
}


/********************************************************************
 * private functions: announce
 ********************************************************************/

/** channel_announcement読込み
 *
 * @param[in]       pDb
 * @param[out]      pCnlAnno
 * @param[in]       ShortChannelId
 * @retval      true    成功
 */
static int annocnl_load(ln_lmdb_db_t *pDb, utl_buf_t *pCnlAnno, uint64_t ShortChannelId)
{
    LOGV("short_channel_id=%016" PRIx64 "\n", ShortChannelId);

    MDB_val key, data;
    uint8_t keydata[M_SZ_ANNOINFO_CNL + 1];

    M_ANNOINFO_CNL_SET(keydata, key, ShortChannelId, LN_DB_CNLANNO_ANNO);
    int retval = mdb_get(mTxnAnno, pDb->dbi, &key, &data);
    if (retval == 0) {
        utl_buf_alloccopy(pCnlAnno, data.mv_data, data.mv_size);
    } else {
        if (retval != MDB_NOTFOUND) {
            LOGE("ERR: %s\n", mdb_strerror(retval));
        }
    }

    return retval;
}


/** channel_announcement書込み
 *
 * @param[in,out]   pDb
 * @param[in]       pCnlAnno
 * @param[in]       ShortChannelId
 * @retval      true    成功
 */
static int annocnl_save(ln_lmdb_db_t *pDb, const utl_buf_t *pCnlAnno, uint64_t ShortChannelId)
{
    LOGV("short_channel_id=%016" PRIx64 "\n", ShortChannelId);

    MDB_val key, data;
    uint8_t keydata[M_SZ_ANNOINFO_CNL + 1];

    M_ANNOINFO_CNL_SET(keydata, key, ShortChannelId, LN_DB_CNLANNO_ANNO);
    data.mv_size = pCnlAnno->len;
    data.mv_data = pCnlAnno->buf;
    int retval = mdb_put(mTxnAnno, pDb->dbi, &key, &data, 0);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
    }

    return retval;
}


/** channel_update読込み
 *
 * @param[in]       pDb
 * @param[out]      pCnlAnno
 * @param[out]      pTimeStamp          (非NULL)保存しているchannel_updateのTimeStamp
 * @param[in]       ShortChannelId
 * @param[in]       Dir                 0:node_1, 1:node_2
 * @retval      true    成功
 */
static int annocnlupd_load(ln_lmdb_db_t *pDb, utl_buf_t *pCnlUpd, uint32_t *pTimeStamp, uint64_t ShortChannelId, uint8_t Dir)
{
    LOGV("short_channel_id=%016" PRIx64 ", dir=%d\n", ShortChannelId, Dir);

    MDB_val key, data;
    uint8_t keydata[M_SZ_ANNOINFO_CNL + 1];

    M_ANNOINFO_CNL_SET(keydata, key, ShortChannelId, ((Dir) ?  LN_DB_CNLANNO_UPD1 : LN_DB_CNLANNO_UPD0));
    int retval = mdb_get(mTxnAnno, pDb->dbi, &key, &data);
    if (retval == 0) {
        if (pTimeStamp != NULL) {
            memcpy(pTimeStamp, data.mv_data, sizeof(uint32_t));
        }
        utl_buf_alloccopy(pCnlUpd, (uint8_t *)data.mv_data + sizeof(uint32_t), data.mv_size - sizeof(uint32_t));
    } else {
        if (retval != MDB_NOTFOUND) {
            LOGE("ERR: %s\n", mdb_strerror(retval));
        }
    }

    return retval;
}


/** channel_update書込み
 *
 * @param[in,out]   pDb
 * @param[in]       pCnlAnno
 * @param[in]       pUpd
 * @retval      true    成功
 */
static int annocnlupd_save(ln_lmdb_db_t *pDb, const utl_buf_t *pCnlUpd, const ln_msg_channel_update_t *pUpd)
{
    LOGV("short_channel_id=%016" PRIx64 ", dir=%d\n", pUpd->short_channel_id, ln_cnlupd_direction(pUpd));

    MDB_val key, data;
    uint8_t keydata[M_SZ_ANNOINFO_CNL + 1];

    M_ANNOINFO_CNL_SET(keydata, key, pUpd->short_channel_id, (ln_cnlupd_direction(pUpd) ?  LN_DB_CNLANNO_UPD1 : LN_DB_CNLANNO_UPD0));
    utl_buf_t buf = UTL_BUF_INIT;
    utl_buf_alloc(&buf, sizeof(uint32_t) + pCnlUpd->len);

    //timestamp + channel_update
    memcpy(buf.buf, &pUpd->timestamp, sizeof(uint32_t));
    memcpy(buf.buf + sizeof(uint32_t), pCnlUpd->buf, pCnlUpd->len);
    data.mv_size = buf.len;
    data.mv_data = buf.buf;
    int retval = mdb_put(mTxnAnno, pDb->dbi, &key, &data, 0);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
    }
    utl_buf_free(&buf);

    return retval;
}


/* [channel_announcement / channel_update]
 *
 *  dbi: "channel_anno"
 */
static int annocnl_cur_load(MDB_cursor *cur, uint64_t *pShortChannelId, char *pType, uint32_t *pTimeStamp, utl_buf_t *pBuf, MDB_cursor_op op)
{
    MDB_val key, data;

    int retval = mdb_cursor_get(cur, &key, &data, op);
    if (retval == 0) {
        if (key.mv_size == LN_SZ_SHORT_CHANNEL_ID + 1) {
            //key = short_channel_id + type
            memcpy(pShortChannelId, key.mv_data, LN_SZ_SHORT_CHANNEL_ID);
            char type = *(char *)((uint8_t *)key.mv_data + LN_SZ_SHORT_CHANNEL_ID);
            if (pType != NULL) {
                *pType = type;
            }
            //data
            uint8_t *p_data = (uint8_t *)data.mv_data;
            if ((type == LN_DB_CNLANNO_UPD0) || (type == LN_DB_CNLANNO_UPD1)) {
                if (pTimeStamp != NULL) {
                    memcpy(pTimeStamp, p_data, sizeof(uint32_t));
                }
                p_data += sizeof(uint32_t);
                data.mv_size -= sizeof(uint32_t);
            } else {
                //channel_announcementにtimestampは無い
                if (pTimeStamp != NULL) {
                    *pTimeStamp = 0;
                }
            }
            if (pBuf != NULL) {
                utl_buf_alloccopy(pBuf, p_data, data.mv_size);
            }
        } else {
            LOGE("fail: invalid key length: %d\n", (int)key.mv_size);
            DUMPD(key.mv_data, key.mv_size);
            retval = -1;
        }
    } else {
        if (retval != MDB_NOTFOUND) {
            LOGE("fail: mdb_cursor_get(): %s\n", mdb_strerror(retval));
        }
    }

    return retval;
}


/* node_announcement取得
 *
 * @param[in,out]   pDb
 * @param[out]      pNodeAnno       (非NULL時)取得したnode_announcement
 * @param[out]      pTimeStamp      (非NULL時)タイムスタンプ
 * @paramin]        pNodeId         検索するnode_id
 * @retval      true
 */
static int annonod_load(ln_lmdb_db_t *pDb, utl_buf_t *pNodeAnno, uint32_t *pTimeStamp, const uint8_t *pNodeId)
{
    MDB_val key, data;
    uint8_t keydata[M_SZ_ANNOINFO_NODE];

    M_ANNOINFO_NODE_SET(keydata, key, pNodeId);
    int retval = mdb_get(mTxnAnno, pDb->dbi, &key, &data);
    if (retval == 0) {
        if (pTimeStamp != NULL) {
            memcpy(pTimeStamp, data.mv_data, sizeof(uint32_t));
        }
        if (pNodeAnno != NULL) {
            utl_buf_alloccopy(pNodeAnno, (uint8_t *)data.mv_data + sizeof(uint32_t), data.mv_size - sizeof(uint32_t));
        }
    } else {
        if (retval != MDB_NOTFOUND) {
            LOGE("ERR: %s\n", mdb_strerror(retval));
        }
    }

    return retval;
}


/* node_announcement書込み
 *
 * @param[in,out]   pDb
 * @param[in]       pNodeAnno       node_announcementパケット
 * @param[in]       pNodeId         node_announcementのnode_id
 * @param[in]       Timestamp       保存時間
 * @retval      true
 */
static int annonod_save(ln_lmdb_db_t *pDb, const utl_buf_t *pNodeAnno, const uint8_t *pNodeId, uint32_t Timestamp)
{
    LOGV("node_id=");
    DUMPV(pNodeId, BTC_SZ_PUBKEY);

    MDB_val key, data;
    uint8_t keydata[M_SZ_ANNOINFO_NODE];

    M_ANNOINFO_NODE_SET(keydata, key, pNodeId);
    utl_buf_t buf = UTL_BUF_INIT;
    utl_buf_alloc(&buf, sizeof(uint32_t) + pNodeAnno->len);

    //timestamp + node_announcement
    memcpy(buf.buf, &Timestamp, sizeof(uint32_t));
    memcpy(buf.buf + sizeof(uint32_t), pNodeAnno->buf, pNodeAnno->len);
    data.mv_size = buf.len;
    data.mv_data = buf.buf;
    int retval = mdb_put(mTxnAnno, pDb->dbi, &key, &data, 0);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
    }
    utl_buf_free(&buf);

    return retval;
}


/** annoinfoにnode_idを追加(channel, node共通)
 *
 * @param[in,out]   pDb         annoinfo
 * @param[in]       pMdbKey     loadしたchannel_announcement infoのkey
 * @param[in]       pMdbData    loadしたchannel_announcement infoのdata
 * @param[in]       pNodeId     追加するnode_id(NULL時はクリア)
 */
static bool annoinfo_add(ln_lmdb_db_t *pDb, MDB_val *pMdbKey, MDB_val *pMdbData, const uint8_t *pNodeId)
{
    uint8_t *p_ids;

    if (pNodeId != NULL) {
        int nums = pMdbData->mv_size / BTC_SZ_PUBKEY;
        p_ids = (uint8_t *)UTL_DBG_MALLOC((nums + 1) * BTC_SZ_PUBKEY);
        memcpy(p_ids, pMdbData->mv_data, pMdbData->mv_size);
        memcpy(p_ids + pMdbData->mv_size, pNodeId, BTC_SZ_PUBKEY);
        pMdbData->mv_size += BTC_SZ_PUBKEY;
    } else {
        pMdbData->mv_size = 0;
        p_ids = NULL;
    }

    pMdbData->mv_data = p_ids;
    int retval = mdb_put(mTxnAnno, pDb->dbi, pMdbKey, pMdbData, 0);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
    }
    UTL_DBG_FREE(p_ids);

    return retval == 0;
}


/** annoinfoからnode_idの有無を検索(channel, node共通)
 *
 * @param[in]   pMdbData
 * @param[in]   pNodeId
 * @retval  true    検出
 */
static bool annoinfo_search(MDB_val *pMdbData, const uint8_t *pNodeId)
{
    int nums = pMdbData->mv_size / BTC_SZ_PUBKEY;
    //LOGD("nums=%d\n", nums);
    //LOGD("search id: ");
    //DUMPD(pNodeId, BTC_SZ_PUBKEY);
    int lp;
    for (lp = 0; lp < nums; lp++) {
        //LOGD("  node_id[%d]= ", lp);
        //DUMPD(pMdbData->mv_data + BTC_SZ_PUBKEY * lp, BTC_SZ_PUBKEY);
        if (memcmp((uint8_t *)pMdbData->mv_data + BTC_SZ_PUBKEY * lp, pNodeId, BTC_SZ_PUBKEY) == 0) {
            break;
        }
    }

    return lp < nums;
}


/** annoinfoからnode_idを削除(channel, node共通)
 *
 * @param[in]   pCursor
 * @param[in]   pNodeId
 */
static void annoinfo_cur_trim(MDB_cursor *pCursor, const uint8_t *pNodeId)
{
    MDB_val     key, data;

    while (mdb_cursor_get(pCursor, &key, &data, MDB_NEXT) == 0) {
        int nums = data.mv_size / BTC_SZ_PUBKEY;
        for (int lp = 0; lp < nums; lp++) {
            if (memcmp((uint8_t *)data.mv_data + BTC_SZ_PUBKEY * lp, pNodeId, BTC_SZ_PUBKEY) == 0) {
                mdb_val_alloccopy(&key, &key);

                nums--;
                if (nums > 0) {
                    uint8_t *p_data = (uint8_t *)UTL_DBG_MALLOC(BTC_SZ_PUBKEY * nums);
                    memcpy(p_data,
                                (uint8_t *)data.mv_data,
                                BTC_SZ_PUBKEY * lp);
                    memcpy(p_data + BTC_SZ_PUBKEY * lp,
                                (uint8_t *)data.mv_data + BTC_SZ_PUBKEY * (lp + 1),
                                BTC_SZ_PUBKEY * (nums - lp));

                    data.mv_size = BTC_SZ_PUBKEY * nums;
                    data.mv_data = p_data;
                } else {
                    data.mv_size = 0;
                    data.mv_data = NULL;
                }
                int retval = mdb_cursor_put(pCursor, &key, &data, MDB_CURRENT);
                if (retval != 0) {
                    LOGE("ERR: %s\n", mdb_strerror(retval));
                }
                UTL_DBG_FREE(data.mv_data);
                UTL_DBG_FREE(key.mv_data);
                break;
            }
        }
    }
}


/** channel_updateの枝刈り
 *      - channel_announcementがない(自channel以外)
 *      - 期限切れ
 */
static void anno_del_prune(void)
{
    bool ret;

    ret = ln_db_anno_transaction();
    if (!ret) {
        LOGE("ERR: anno transaction\n");
        return;
    }

    //時間がかかる場合があるため、状況を出力する
    fprintf(stderr, "DB checking: announcement...");

    uint64_t now = (uint64_t)utl_time_time();
    void *p_cur;
    ret = ln_db_anno_cur_open(&p_cur, LN_DB_CUR_CNL);
    if (ret) {
        uint64_t short_channel_id;
        uint64_t last_short_chennel_id = 0;
        char type;
        utl_buf_t buf_cnl = UTL_BUF_INIT;
        uint32_t timestamp;
        while ((ret = ln_db_annocnl_cur_get(p_cur, &short_channel_id, &type, &timestamp, &buf_cnl))) {
            utl_buf_free(&buf_cnl);
            if (type == LN_DB_CNLANNO_ANNO) {
                last_short_chennel_id = short_channel_id;
            }
            bool prune = ln_db_annocnlupd_is_prune(now, timestamp);
            if ( (type != LN_DB_CNLANNO_ANNO) && ((last_short_chennel_id != short_channel_id) || prune)) {
                if (prune) {
                    MDB_cursor *cursor = ((lmdb_cursor_t *)p_cur)->cursor;
                    int retval = mdb_cursor_del(cursor, 0);
                    if (retval == 0) {
                        LOGD("prune channel_update(%c): %016" PRIx64 "\n", type, short_channel_id);
                    } else {
                        LOGE("ERR: %s\n", mdb_strerror(retval));
                    }
                }
                fprintf(stderr, ".");
            }
        }
        ln_db_anno_cur_close(p_cur);
    }

    ln_db_anno_commit(true);

    fprintf(stderr, "done!\n");
}


/********************************************************************
 * private functions: preimage
 ********************************************************************/

static bool preimage_open(ln_lmdb_db_t *p_db, MDB_txn *txn)
{
    int retval;

    if (txn == NULL) {
        retval = MDB_TXN_BEGIN(mpDbNode, NULL, 0, &p_db->txn);
        if (retval != 0) {
            LOGE("ERR: %s\n", mdb_strerror(retval));
            goto LABEL_EXIT;
        }
    } else {
        p_db->txn = txn;
    }
    retval = MDB_DBI_OPEN(p_db->txn, M_DBI_PREIMAGE, MDB_CREATE, &p_db->dbi);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        MDB_TXN_ABORT(p_db->txn);
        goto LABEL_EXIT;
    }

LABEL_EXIT:
    return retval == 0;
}


static void preimage_close(ln_lmdb_db_t *p_db, MDB_txn *txn)
{
    if (txn == NULL) {
        MDB_TXN_COMMIT(p_db->txn);
    }
}


/** #ln_db_preimage_del_hash()用処理関数
 *
 * SHA256(preimage)がpayment_hashと一致した場合にDBから削除する。
 */
static bool preimage_del_func(const uint8_t *pPreimage, uint64_t Amount, uint32_t Expiry, void *p_db_param, void *p_param)
{
    (void)Amount; (void)Expiry;

    lmdb_cursor_t *p_cur = (lmdb_cursor_t *)p_db_param;
    const uint8_t *hash = (const uint8_t *)p_param;
    uint8_t preimage_hash[BTC_SZ_HASH256];
    int retval = MDB_NOTFOUND;

    LOGD("compare preimage : ");
    DUMPD(pPreimage, LN_SZ_PREIMAGE);
    ln_payment_hash_calc(preimage_hash, pPreimage);
    if (memcmp(preimage_hash, hash, BTC_SZ_HASH256) == 0) {
        retval = mdb_cursor_del(p_cur->cursor, 0);
        LOGD("  remove from DB: %s\n", mdb_strerror(retval));
    }

    return retval == 0;
}


/** ln_db_channel_del_param用処理関数
 *
 * SHA256(preimage)がpayment_hashと一致した場合、DBから削除する。
 */
static bool preimage_close_func(const uint8_t *pPreimage, uint64_t Amount, uint32_t Expiry, void *p_db_param, void *p_param)
{
    (void)Amount; (void)Expiry;

    lmdb_cursor_t *p_cur = (lmdb_cursor_t *)p_db_param;
    preimage_close_t *param = (preimage_close_t *)p_param;
    uint8_t preimage_hash[BTC_SZ_HASH256];

    LOGD("compare preimage : ");
    DUMPD(pPreimage, LN_SZ_PREIMAGE);
    ln_payment_hash_calc(preimage_hash, pPreimage);

    for (int lp = 0; lp < LN_HTLC_RECEIVED_MAX; lp++) {
        if (memcmp(preimage_hash, param->p_htlcs[lp].payment_hash, BTC_SZ_HASH256) == 0) {
            //一致
            int retval = mdb_cursor_del(p_cur->cursor, 0);
            LOGD("  remove from DB: %s\n", mdb_strerror(retval));
        }
    }

    return false;
}


/********************************************************************
 * private functions: wallet
 ********************************************************************/

static int wallet_db_open(ln_lmdb_db_t *pDb, const char *pDbName, int OptTxn, int OptDb)
{
    int             retval;

    retval = MDB_TXN_BEGIN(mpDbWalt, NULL, OptTxn, &pDb->txn);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        pDb->txn = NULL;
        goto LABEL_EXIT;
    }
    retval = MDB_DBI_OPEN(pDb->txn, pDbName, OptDb, &pDb->dbi);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        MDB_TXN_ABORT(pDb->txn);
        pDb->txn = NULL;
        goto LABEL_EXIT;
    }

LABEL_EXIT:
    return retval;
}


/********************************************************************
 * private functions: version
 ********************************************************************/

static int ver_write(ln_lmdb_db_t *pDb, const char *pWif, const char *pNodeName, uint16_t Port)
{
    int         retval;
    MDB_val     key, data;
    int32_t     version = M_DB_VERSION_VAL;

    retval = MDB_DBI_OPEN(pDb->txn, M_DBI_VERSION, MDB_CREATE, &pDb->dbi);
    if (retval != 0) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
        goto LABEL_EXIT;
    }

    //version
    key.mv_size = LNDBK_LEN(LNDBK_VER);
    key.mv_data = LNDBK_VER;
    data.mv_size = sizeof(version);
    data.mv_data = &version;
    retval = mdb_put(pDb->txn, pDb->dbi, &key, &data, 0);

    //my node info
    if ((retval == 0) && (pWif != NULL)) {
        key.mv_size = LNDBK_LEN(LNDBK_NODEID);
        key.mv_data = LNDBK_NODEID;

        // LOGD("wif=%s\n", pWif);
        // LOGD("name=%s\n", pNodeName);
        // LOGD("port=%" PRIu16 "\n", Port);
        nodeinfo_t nodeinfo;
        memcpy(nodeinfo.genesis, ln_genesishash_get(), BTC_SZ_HASH256);
        strncpy(nodeinfo.wif, pWif, BTC_SZ_WIF_STR_MAX);
        strncpy(nodeinfo.name, pNodeName, LN_SZ_ALIAS_STR);
        nodeinfo.wif[BTC_SZ_WIF_STR_MAX] = '\0';
        nodeinfo.name[LN_SZ_ALIAS_STR] = '\0';
        nodeinfo.port = Port;
        memcpy(nodeinfo.create_bhash, ln_creationhash_get(), BTC_SZ_HASH256);
        data.mv_size = sizeof(nodeinfo);
        data.mv_data = (void *)&nodeinfo;
        retval = mdb_put(pDb->txn, pDb->dbi, &key, &data, 0);
    } else if (retval) {
        LOGE("ERR: %s\n", mdb_strerror(retval));
    }

LABEL_EXIT:
    return retval;
}


/** DBバージョンチェック
 *
 * @param[in,out]   pDb
 * @param[out]      pVer
 * @param[out]      pWif
 * @param[in,out]   pNodeName       [in]setting name(default:"") [out]set name
 * @param[in,out]   pPort           [in]setting value [out]set value
 * @retval  0   DBバージョン一致
 */
static int ver_check(ln_lmdb_db_t *pDb, int32_t *pVer, char *pWif, char *pNodeName, uint16_t *pPort, uint8_t *pGenesis)
{
    int         retval;
    MDB_val key, data;
    nodeinfo_t nodeinfo;

    //version
    key.mv_size = LNDBK_LEN(LNDBK_VER);
    key.mv_data = LNDBK_VER;
    retval = mdb_get(pDb->txn, pDb->dbi, &key, &data);
    if (retval == 0) {
        memcpy(pVer, data.mv_data, sizeof(int32_t));
        if (*pVer != M_DB_VERSION_VAL) {
            fprintf(stderr, "fail: version mismatch : %d(require %d)\n", *pVer, M_DB_VERSION_VAL);
            retval = -1;
        }
    } else {
        LOGE("ERR: %s\n", mdb_strerror(retval));
    }
    if (retval != 0) {
        goto LABEL_EXIT;
    }

    key.mv_size = LNDBK_LEN(LNDBK_NODEID);
    key.mv_data = LNDBK_NODEID;
    retval = mdb_get(pDb->txn, pDb->dbi, &key, &data);
    if (retval == 0) {
        if (data.mv_size != sizeof(nodeinfo_t)) {
            retval = MDB_BAD_VALSIZE;
            goto LABEL_EXIT;
        }
    }

    bool update = false;
    memcpy(&nodeinfo, (const nodeinfo_t*)data.mv_data, data.mv_size);
    strcpy(pWif, nodeinfo.wif);
    if ((pNodeName[0] != '\0') && (strcmp(nodeinfo.name, pNodeName) != 0)) {
        //update
        strncpy(nodeinfo.name, pNodeName, sizeof(nodeinfo.name));
        update = true;
    } else {
        strcpy(pNodeName, nodeinfo.name);
    }
    if ((*pPort != 0) && (nodeinfo.port = *pPort)) {
        //update
        nodeinfo.port = *pPort;
        update = true;
    } else {
        *pPort = nodeinfo.port;
    }
    memcpy(pGenesis, nodeinfo.genesis, BTC_SZ_HASH256);
    ln_creationhash_set(nodeinfo.create_bhash);
    // LOGD("wif=%s\n", pWif);
    // LOGD("name=%s\n", pNodeName);
    // LOGD("port=%" PRIu16 "\n", *pPort);
    // LOGD("genesis=");
    // DUMPD(p_nodeinfo->genesis, BTC_SZ_HASH256);

    if (update) {
        data.mv_data = &nodeinfo;
        data.mv_size = sizeof(nodeinfo);
        retval = mdb_put(pDb->txn, pDb->dbi, &key, &data, 0);
        if (retval != 0) {
            LOGE("fail: %s\n", mdb_strerror(retval));
        }
    }

LABEL_EXIT:
    return retval;
}


/********************************************************************
 * private functions: backup
 ********************************************************************/

/** backup_param_tデータ読込み
 *
 * @param[out]      pData
 * @param[in]       pDb
 * @param[in]       pParam
 * @param[in]       Num             pParam数
 */
static int backup_param_load(void *pData, ln_lmdb_db_t *pDb, const backup_param_t *pParam, size_t Num)
{
    int         retval;
    MDB_val     key, data;

    for (size_t lp = 0; lp < Num; lp++) {
        key.mv_size = strlen(pParam[lp].name);
        key.mv_data = (CONST_CAST char *)pParam[lp].name;
        retval = mdb_get(pDb->txn, pDb->dbi, &key, &data);
        if (retval == 0) {
            //LOGD("%s: %lu\n", pParam[lp].name, pParam[lp].offset);
            memcpy((uint8_t *)pData + pParam[lp].offset, data.mv_data,  pParam[lp].datalen);
        } else {
            LOGE("fail: %s\n", pParam[lp].name);
            if (retval != MDB_NOTFOUND) {
                break;
            } else {
                retval = 0;
            }
        }
    }

    return retval;
}


/** backup_param_tデータ保存
 *
 * @param[in]       pData
 * @param[in]       pDb
 * @param[in]       pParam
 * @param[in]       Num             pParam数
 */
static int backup_param_save(const void *pData, ln_lmdb_db_t *pDb, const backup_param_t *pParam, size_t Num)
{
    int             retval;
    MDB_val         key, data;

    for (size_t lp = 0; lp < Num; lp++) {
        key.mv_size = strlen(pParam[lp].name);
        key.mv_data = (CONST_CAST char *)pParam[lp].name;
        data.mv_size = pParam[lp].datalen;
        data.mv_data = (CONST_CAST uint8_t *)pData + pParam[lp].offset;
        retval = mdb_put(pDb->txn, pDb->dbi, &key, &data, 0);
        if (retval != 0) {
            LOGE("fail: %s\n", pParam[lp].name);
            break;
        }
    }

    return retval;
}


/********************************************************************
 * private functions: initialize
 ********************************************************************/

static int init_dbenv(int InitParamIdx)
{
    int retval;

    retval = lmdb_init(InitParamIdx);
    if (retval == 0) {
        retval = lmdb_compaction(InitParamIdx);
    }
    if (retval == 0) {
        LOGD("DB: OK(%d)\n", InitParamIdx);
    } else {
        LOGE("ERR: (%d)\n", InitParamIdx);
    }

    return retval;
}


static int lmdb_init(int InitParamIdx)
{
    int retval;
    int line = 0;
    const init_param_t *p_param = &INIT_PARAM[InitParamIdx];

    LOGD("BEGIN(%s)\n", p_param->path);

    mkdir(p_param->path, 0755);

    retval = mdb_env_create(p_param->pp_env);
    if (retval != 0) {
        line = __LINE__;
        goto LABEL_EXIT;
    }
    retval = mdb_env_set_maxdbs(*p_param->pp_env, p_param->maxdbs);
    if (retval != 0) {
        line = __LINE__;
        goto LABEL_EXIT;
    }
    retval = mdb_env_set_mapsize(*p_param->pp_env, p_param->mapsize);
    if (retval != 0) {
        line = __LINE__;
        goto LABEL_EXIT;
    }
    retval = mdb_env_open(*p_param->pp_env, p_param->path, p_param->openflag, 0644);
    if (retval != 0) {
        line = __LINE__;
        goto LABEL_EXIT;
    }

LABEL_EXIT:
    if (retval == 0) {
        LOGD("DB: OK(%s)\n", p_param->path);
    } else {
        LOGE("ERR: %s(line: %d)\n", mdb_strerror(retval), line);
    }

    return retval;
}


static int lmdb_compaction(int InitParamIdx)
{
    int retval;
    int line = 0;
    MDB_envinfo     info;
    const init_param_t *p_param = &INIT_PARAM[InitParamIdx];
    long sz = sysconf(_SC_PAGESIZE);

    //compat処理
    retval = mdb_env_info(*p_param->pp_env, &info);
    if (retval != 0) {
        line = __LINE__;
        goto LABEL_EXIT;
    }
    LOGD("-------------------------------------------\n");
    LOGD("pagesize=%d\n", sz);
    LOGD("env_info.mapsize=%lu\n", info.me_mapsize);
    LOGD("env_info.last_pgno=%lu\n", info.me_last_pgno);
    // LOGD("env_info.last_txnid=%lu\n", info.me_last_txnid);
    // LOGD("env_info.maxreaders=%lu\n", info.me_maxreaders);
    // LOGD("env_info.numreaders=%lu\n", info.me_numreaders);
    LOGD("-------------------------------------------\n");
    if (info.me_mapsize <= (info.me_last_pgno + M_MAPSIZE_REMAIN) * sz) {
        LOGE("ERR: page not remain\n");

        size_t prev_pgno = info.me_last_pgno;
        char tmppath[M_DBPATH_MAX];
        snprintf(tmppath, M_DBPATH_MAX, "%s/tmpdir", mPath);
        mkdir(tmppath, 0755);
        retval = mdb_env_copy2(*p_param->pp_env, tmppath, MDB_CP_COMPACT);
        if (retval != 0) {
            line = __LINE__;
            goto LABEL_EXIT;
        }

        retval = mdb_env_info(*p_param->pp_env, &info);
        if (retval != 0) {
            line = __LINE__;
            goto LABEL_EXIT;
        }

        int err;
        if (prev_pgno > info.me_last_pgno) {
            err = nftw(p_param->path, rm_files, 10, FTW_DEPTH | FTW_MOUNT | FTW_PHYS);
            if (err == 0) {
                rename(tmppath, p_param->path);
            } else {
                LOGE("fail\n");
            }

            //開き直す
            mdb_env_close(*p_param->pp_env);
            retval = lmdb_init(InitParamIdx);
            if (retval != 0) {
                line = __LINE__;
                goto LABEL_EXIT;
            }

            fprintf(stderr, "DB optimized. Please rerun !\n");
        } else {
            err = nftw(tmppath, rm_files, 10, FTW_DEPTH | FTW_MOUNT | FTW_PHYS);
            if (err < 0) {
                LOGE("fail\n");
            }

            fprintf(stderr, "DB mapsize is flood...\n");
        }
        LOGD("prev pgno=%lu, now pgno=%lu(rmdir=%d)\n", prev_pgno, info.me_last_pgno, err);
        retval = ENOMEM;
        line = __LINE__;
    }

LABEL_EXIT:
    if (retval == 0) {
        LOGD("DB: OK(%s)\n", p_param->path);
    } else {
        LOGE("ERR: %s(line: %d)\n", mdb_strerror(retval), line);
    }

    return retval;
}


//https://stackoverflow.com/a/42978529
static int rm_files(const char *pathname, const struct stat *sbuf, int type, struct FTW *ftwb)
{
    (void)sbuf; (void)type; (void)ftwb;

    if(remove(pathname) < 0) {
        perror("ERROR: remove");
        return -1;
    }
    return 0;
}
