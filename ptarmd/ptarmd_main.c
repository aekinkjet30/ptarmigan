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
/** @file   ptarmd_main.c
 *  @brief  ptarmd entry point
 */
#include <stdio.h>
#include <pthread.h>
#include <getopt.h>
#include <signal.h>

#include "btcrpc.h"
#include "conf.h"
#include "ln_db.h"
#include "utl_log.h"
#include "utl_addr.h"


/**************************************************************************
 * macros
 **************************************************************************/
#define M_OPTSTRING     "p:n:a:c:d:xNh"


/********************************************************************
 * prototypes
 ********************************************************************/

static void reset_getopt();
static void sig_set_catch_sigs(sigset_t *pSigSet);
static void *sig_handler_start(void *pArg);


/********************************************************************
 * entry point
 ********************************************************************/

int main(int argc, char *argv[])
{
    bool bret;
    rpc_conf_t rpc_conf;
    ln_nodeaddr_t *p_addr;
    char *p_alias;
    int opt;
    uint16_t my_rpcport = 0;

    const struct option OPTIONS[] = {
        { "rpcport", required_argument, NULL, 'P' },
        { 0, 0, 0, 0 }
    };

    //`d` option is used to change working directory.
    // It is done at the beginning of this process.
    while ((opt = getopt_long(argc, argv, M_OPTSTRING, OPTIONS, NULL)) != -1) {
        switch (opt) {
        case 'd':
            if (chdir(optarg) != 0) {
                fprintf(stderr, "fail: change the working directory\n");
                return -1;
            }
            break;
        default:
            break;
        }
    }
    reset_getopt();

    p_addr = ln_node_addr();
    p_alias = ln_node_alias();

#ifdef ENABLE_PLOG_TO_STDOUT
    utl_log_init_stdout();
#else
    utl_log_init();
#endif

#ifndef NETKIND
#error not define NETKIND
#endif
#if NETKIND==0
    bret = btc_init(BTC_MAINNET, true);
#elif NETKIND==1
    bret = btc_init(BTC_TESTNET, true);
#endif
    if (!bret) {
        fprintf(stderr, "fail: btc_init()\n");
        return -1;
    }

    conf_btcrpc_init(&rpc_conf);
    p_addr->type = LN_NODEDESC_NONE;
    p_addr->port = 0;

    int options = 0;
    while ((opt = getopt_long(argc, argv, M_OPTSTRING, OPTIONS, NULL)) != -1) {
        switch (opt) {
        //case 'd':
        //    //`d` option is used to change working directory.
        //    // It is done at the beginning of this process.
        //    break;
        case 'p':
            //port num
            p_addr->port = (uint16_t)atoi(optarg);
            break;
        case 'n':
            //node name(alias)
            strncpy(p_alias, optarg, LN_SZ_ALIAS);
            p_alias[LN_SZ_ALIAS] = '\0';
            break;
        case 'a':
            //ip address
            {
                uint8_t ipbin[4];
                bool addrret = utl_addr_ipv4_str2bin(ipbin, optarg);
                if (addrret) {
                    p_addr->type = LN_NODEDESC_IPV4;
                    memcpy(p_addr->addrinfo.addr, ipbin, sizeof(ipbin));
                }
            }
            break;
        case 'c':
            //load btcconf file
            bret = conf_btcrpc_load(optarg, &rpc_conf);
            if (!bret) {
                goto LABEL_EXIT;
            }
            break;
        case 'P':
            //my rpcport num
            my_rpcport = (uint16_t)atoi(optarg);
            break;
        case 'x':
            //ノード情報を残してすべて削除
            options |= 0x80;
            break;
        case 'N':
            //node_announcementを全削除
            options |= 0x40;
            break;
        case 'h':
            //help
            goto LABEL_EXIT;
        default:
            break;
        }
    }

    if (options & 0x40) {
        bret = ln_db_annonod_drop_startup();
        fprintf(stderr, "db_annonod_drop: %d\n", bret);
        return 0;
    }

    if (options & 0x80) {
        //
        bret = ln_db_reset();
        fprintf(stderr, "db_reset: %d\n", bret);
        return 0;
    }

    if ((strlen(rpc_conf.rpcuser) == 0) || (strlen(rpc_conf.rpcpasswd) == 0)) {
        //bitcoin.confから読込む
        bret = conf_btcrpc_load_default(&rpc_conf);
        if (!bret) {
            goto LABEL_EXIT;
        }
    }

    //O'REILLY Japan: BINARY HACKS #52
    sigset_t ss;
    pthread_t th_sig;
    sig_set_catch_sigs(&ss);
    sigprocmask(SIG_BLOCK, &ss, NULL);
    signal(SIGPIPE , SIG_IGN);   //ignore SIGPIPE
    pthread_create(&th_sig, NULL, &sig_handler_start, NULL);

    //bitcoind起動確認
    uint8_t genesis[LN_SZ_HASH];
    bret = btcrpc_init(&rpc_conf);
    if (!bret) {
        fprintf(stderr, "fail: initialize btcrpc\n");
        return -1;
    }
    bret = btcrpc_getgenesisblock(genesis);
    if (!bret) {
        fprintf(stderr, "fail: bitcoin getblockhash\n");
        return -1;
    }

    // https://github.com/lightningnetwork/lightning-rfc/issues/237
    for (int lp = 0; lp < LN_SZ_HASH / 2; lp++) {
        uint8_t tmp = genesis[lp];
        genesis[lp] = genesis[LN_SZ_HASH - lp - 1];
        genesis[LN_SZ_HASH - lp - 1] = tmp;
    }
    ln_set_genesishash(genesis);

#if NETKIND==0
    LOGD("start bitcoin mainnet\n");
#elif NETKIND==1
    LOGD("start bitcoin testnet/regtest\n");
#endif

    ptarmd_start(my_rpcport);

    return 0;

LABEL_EXIT:
    fprintf(stderr, "[usage]\n");
    fprintf(stderr, "\t%s [-p PORT NUM] [-n ALIAS NAME] [-c BITCOIN.CONF] [-a IPv4 ADDRESS] [-i]\n", argv[0]);
    fprintf(stderr, "\n");
    fprintf(stderr, "\t\t-h : help\n");
    fprintf(stderr, "\t\t-p PORT : node port(default: 9735)\n");
    fprintf(stderr, "\t\t-n NAME : alias name(default: \"node_xxxxxxxxxxxx\")\n");
    fprintf(stderr, "\t\t-c CONF_FILE : using bitcoin.conf(default: ~/.bitcoin/bitcoin.conf)\n");
    fprintf(stderr, "\t\t-a IPADDRv4 : announce IPv4 address(default: none)\n");
    fprintf(stderr, "\t\t-d DIR_PATH : change working directory\n");
    fprintf(stderr, "\t\t--rpcport PORT : JSON-RPC port(default: node port+1)\n");
    fprintf(stderr, "\t\t-x : erase current DB(without node_id)(TEST)\n");
    fprintf(stderr, "\t\t-N : erase node_announcement DB(TEST)\n");
    return -1;
}


/********************************************************************
 * private functions
 ********************************************************************/

static void reset_getopt()
{
    //optreset = 1;
    //optind = 1;

    //ref. http://man7.org/linux/man-pages/man3/getopt.3.html#NOTES
    optind = 0;
}


//捕捉するsignal設定
static void sig_set_catch_sigs(sigset_t *pSigSet)
{
    sigemptyset(pSigSet);
    sigaddset(pSigSet, SIGHUP);
    sigaddset(pSigSet, SIGINT);
    sigaddset(pSigSet, SIGQUIT);
    sigaddset(pSigSet, SIGTERM);
    sigaddset(pSigSet, SIGABRT);
    sigaddset(pSigSet, SIGSEGV);
}


//signal捕捉スレッド
static void *sig_handler_start(void *pArg)
{
    (void)pArg;

    LOGD("signal handler\n");
    pthread_detach(pthread_self());

    sigset_t ss;
    siginfo_t info;
    sig_set_catch_sigs(&ss);
    while (1) {
        if (sigwaitinfo(&ss, &info) > 0) {
            printf("!!! SIGNAL DETECT !!!\n");
            LOGD("!!! SIGNAL DETECT: %d !!!\n", info.si_signo);
            exit(-1);
        }
    }
    return NULL;
}