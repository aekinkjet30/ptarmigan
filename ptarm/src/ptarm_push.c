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
/** @file   ptarm_push.c
 *  @brief  bitcoinスクリプト作成
 *  @author ueno@nayuta.co
 */
#include "ptarm_local.h"


/**************************************************************************
 * public functions
 **************************************************************************/

void ptarm_push_init(ptarm_push_t *pPush, ptarm_buf_t *pBuf, uint32_t Size)
{
    pPush->pos = 0;
    pPush->data = pBuf;
    if (Size) {
        ptarm_buf_alloc(pPush->data, Size);
    } else {
        ptarm_buf_init(pPush->data);
    }
}


void ptarm_push_data(ptarm_push_t *pPush, const void *pData, uint32_t Len)
{
    int rest = pPush->data->len - pPush->pos - Len;
    if (rest < 0) {
        //足りない分を拡張
        pPush->data->buf = (uint8_t *)M_REALLOC(pPush->data->buf, pPush->data->len - rest);
        pPush->data->len = pPush->data->len - rest;
    }
    memcpy(&pPush->data->buf[pPush->pos], pData, Len);
    pPush->pos += Len;
}


void ptarm_push_value(ptarm_push_t *pPush, uint64_t Value)
{
    int len;
    uint8_t buf[7];

    if (Value == 0x00) {
        buf[0] = 0x00;
        len = 1;
    } else if ((1 <= Value) && (Value <= 16)) {
        //データ長が1で値が1～16の場合はOP_1～OP_16を使う
        buf[0] = 0x50 + Value;
        len = 1;
    } else {
        for (len = 1; len <= 6; len++) {
            if (Value < ((uint64_t)0x80 << 8 * (len - 1))) {
                buf[0] = len;
                for (int lp = 0; lp < len; lp++) {
                    buf[1 + lp] = (uint8_t)(Value >> 8 * lp);
                }
                len++;
                break;
            }
        }
    }
    ptarm_push_data(pPush, buf, len);
}


void ptarm_push_trim(ptarm_push_t *pPush)
{
    if (pPush->data->len != pPush->pos) {
        if (pPush->pos == 0) {
            ptarm_buf_free(pPush->data);
        } else {
            pPush->data->len = pPush->pos;
            pPush->data->buf = (uint8_t *)M_REALLOC(pPush->data->buf, pPush->pos);
        }
    }
}