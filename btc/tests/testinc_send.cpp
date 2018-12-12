////////////////////////////////////////////////////////////////////////
//FAKE関数

//FAKE_VALUE_FUNC(int, external_function, int);

////////////////////////////////////////////////////////////////////////

class send: public testing::Test {
protected:
    virtual void SetUp() {
        //RESET_FAKE(external_function)
        utl_dbg_malloc_cnt_reset();
        btc_init(BTC_TESTNET, false);
    }

    virtual void TearDown() {
        ASSERT_EQ(0, utl_dbg_malloc_cnt());
        btc_term();
    }

public:
    static void DumpBin(const uint8_t *pData, uint16_t Len)
    {
        for (uint16_t lp = 0; lp < Len; lp++) {
            printf("%02x", pData[lp]);
        }
        printf("\n");
    }

    static void DumpTxid(const uint8_t *txid)
    {
        printf("txid= ");
        for (int lp = 0; lp < BTC_SZ_TXID; lp++) {
            printf("%02x", txid[BTC_SZ_TXID - lp - 1]);
        }
        printf("\n");
    }

};

////////////////////////////////////////////////////////////////////////

TEST_F(send, p2pkh)
{
    bool ret;
    btc_tx_t tx;
    btc_tx_init(&tx);

    //送金元 : mmsgPUnoceq7er7f9HuaZV2ktMkaVD3Za1
    //          wif: cR645M2xZJnE5mDWw5LpAghNLudXGZsCs4ZEUvRMr2NrHqU3rLWa
    //      txid: e5404e772a8b780002621babd6d677f6ada38c6d54d87755d88b12ac77c86bff
    //      inddex: 1
    //      Value: 170 mBTC

    //送金先 : mizPYQKhB2cGioZqZbP2aJJcRUUYTRN2PR
    //      Value: 1 mBTC
    //お釣り : mmsgPUnoceq7er7f9HuaZV2ktMkaVD3Za1 (送金元と同じ)
    //      Value: 168.9 mBTC
    //FEE : 0.1 mBTC
    //          1 + 168.9 + 0.1 = 170 mBTC

    const uint8_t TXID[] = {
        0xff, 0x6b, 0xc8, 0x77, 0xac, 0x12, 0x8b, 0xd8,
        0x55, 0x77, 0xd8, 0x54, 0x6d, 0x8c, 0xa3, 0xad,
        0xf6, 0x77, 0xd6, 0xd6, 0xab, 0x1b, 0x62, 0x02,
        0x00, 0x78, 0x8b, 0x2a, 0x77, 0x4e, 0x40, 0xe5,
    };
    btc_tx_add_vin(&tx, TXID, 1);

    ret = btc_tx_add_vout_p2pkh_addr(&tx, BTC_MBTC2SATOSHI(1), "mizPYQKhB2cGioZqZbP2aJJcRUUYTRN2PR");
    ASSERT_TRUE(ret);
    ret = btc_tx_add_vout_p2pkh_addr(&tx, BTC_MBTC2SATOSHI(168.9), "mmsgPUnoceq7er7f9HuaZV2ktMkaVD3Za1");
    ASSERT_TRUE(ret);

    uint8_t txhash[BTC_SZ_HASH256];
    utl_buf_t script_pk;
    ret = btc_keys_addr2spk(&script_pk, "mmsgPUnoceq7er7f9HuaZV2ktMkaVD3Za1");
    ASSERT_TRUE(ret);
    const utl_buf_t *spks[] = { &script_pk };
    ret = btc_tx_sighash(txhash, &tx, (const utl_buf_t **)spks, 1);
    ASSERT_TRUE(ret);
    uint8_t priv[BTC_SZ_PRIVKEY];
    const char WIF[] = "cR645M2xZJnE5mDWw5LpAghNLudXGZsCs4ZEUvRMr2NrHqU3rLWa";
    btc_chain_t chain;
    ret = btc_keys_wif2priv(priv, &chain, WIF);
    ASSERT_TRUE(ret);
    ASSERT_EQ(BTC_TESTNET, chain);
    ret = btc_tx_sign_p2pkh(&tx, 0, txhash, priv, NULL);
    ASSERT_TRUE(ret);

    utl_buf_t txbuf = UTL_BUF_INIT;
    btc_tx_create(&txbuf, &tx);
    printf("tx=\n");
    send::DumpBin(txbuf.buf, txbuf.len);

    //bitcoindで送信OK
    //txid : 0407afad2e6da04d61b83a9e6336f8d734c5b0c07e62b63ec824c3d6930de5ab
    const uint8_t TX_SENT[] = {
        0x02, 0x00, 0x00, 0x00, 0x01, 0xff, 0x6b, 0xc8,
        0x77, 0xac, 0x12, 0x8b, 0xd8, 0x55, 0x77, 0xd8,
        0x54, 0x6d, 0x8c, 0xa3, 0xad, 0xf6, 0x77, 0xd6,
        0xd6, 0xab, 0x1b, 0x62, 0x02, 0x00, 0x78, 0x8b,
        0x2a, 0x77, 0x4e, 0x40, 0xe5, 0x01, 0x00, 0x00,
        0x00, 0x6b, 0x48, 0x30, 0x45, 0x02, 0x21, 0x00,
        0x85, 0xf8, 0x45, 0xd2, 0xf1, 0x33, 0x35, 0xe3,
        0x98, 0x67, 0x2b, 0x01, 0xbf, 0xb4, 0x7b, 0x52,
        0x97, 0x39, 0x59, 0x61, 0xae, 0xe9, 0x66, 0x67,
        0xed, 0x1a, 0xa5, 0xfa, 0x99, 0x8c, 0x60, 0x6c,
        0x02, 0x20, 0x1f, 0x99, 0x84, 0x29, 0xd8, 0xb0,
        0xd9, 0xc1, 0x6a, 0x83, 0x1c, 0xfa, 0xde, 0xf0,
        0x24, 0x17, 0x16, 0x18, 0xf5, 0xa1, 0x2d, 0xf3,
        0xfd, 0x19, 0x77, 0xd1, 0x66, 0x3e, 0xcb, 0x53,
        0x4e, 0x00, 0x01, 0x21, 0x02, 0x44, 0xc7, 0x58,
        0x11, 0x77, 0x81, 0x96, 0x8e, 0x8e, 0x09, 0x5d,
        0xc6, 0xa5, 0xb8, 0x9f, 0x4e, 0x12, 0x40, 0xbc,
        0x7d, 0xce, 0xbc, 0xb2, 0xc1, 0x5f, 0xc9, 0x1d,
        0x33, 0x3d, 0xea, 0x79, 0xee, 0xff, 0xff, 0xff,
        0xff, 0x02, 0xa0, 0x86, 0x01, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x19, 0x76, 0xa9, 0x14, 0x26, 0x18,
        0xaf, 0x0c, 0x70, 0x51, 0xb3, 0xfc, 0xe0, 0x37,
        0x6a, 0xd5, 0x8f, 0xa6, 0x5f, 0x9d, 0xc8, 0x5e,
        0x64, 0x46, 0x88, 0xac, 0x90, 0xb8, 0x01, 0x01,
        0x00, 0x00, 0x00, 0x00, 0x19, 0x76, 0xa9, 0x14,
        0x45, 0xbc, 0x36, 0x3c, 0xc6, 0xb3, 0x91, 0x74,
        0xc2, 0xe7, 0x72, 0xe7, 0xb1, 0x32, 0x30, 0x9d,
        0x06, 0xd7, 0x49, 0x66, 0x88, 0xac, 0x00, 0x00,
        0x00, 0x00,
    };
    ASSERT_EQ(0, memcmp(TX_SENT, txbuf.buf, sizeof(TX_SENT)));
    ASSERT_EQ(sizeof(TX_SENT), txbuf.len);
    uint8_t txid[BTC_SZ_TXID];
    ret = btc_tx_txid(txid, &tx);
    send::DumpTxid(txid);

    utl_buf_free(&txbuf);
    utl_buf_free(&script_pk);
    btc_tx_free(&tx);
}


TEST_F(send, p2wpkh)
{
    bool ret;
    btc_tx_t tx;
    btc_tx_init(&tx);

    //送金元 : 2NCFo5oZuEbXgZdMDzLMA2qQiroHrU6oXSU(<== mtLLAiafrhzcjSZqp2Ts86Gv7PupWnXKUc)
    //          wif: cW8SSTFrM42mX5YKHKbDfvXF5qEJrAgLoRTc68bNJo5GFDv6WvX1
    //      txid:  6715f11b3d78e89bb50c90ee1d8f9f0f92779cfc3438873fb1367d61f5e81c8e
    //      inddex: 1
    //      Value: 3 mBTC

    //送金先 : mizPYQKhB2cGioZqZbP2aJJcRUUYTRN2PR
    //      Value: 1 mBTC
    //お釣り : 2NCFo5oZuEbXgZdMDzLMA2qQiroHrU6oXSU (送金元と同じ)
    //      Value: 1.9 mBTC
    //FEE : 0.1 mBTC
    //          1 + 1.9 + 0.1 = 3 mBTC

    const uint8_t TXID[] = {
        0x8e, 0x1c, 0xe8, 0xf5, 0x61, 0x7d, 0x36, 0xb1,
        0x3f, 0x87, 0x38, 0x34, 0xfc, 0x9c, 0x77, 0x92,
        0x0f, 0x9f, 0x8f, 0x1d, 0xee, 0x90, 0x0c, 0xb5,
        0x9b, 0xe8, 0x78, 0x3d, 0x1b, 0xf1, 0x15, 0x67,
    };
    btc_tx_add_vin(&tx, TXID, 1);

    ret = btc_tx_add_vout_p2pkh_addr(&tx, BTC_MBTC2SATOSHI(1), "mizPYQKhB2cGioZqZbP2aJJcRUUYTRN2PR");
    ASSERT_TRUE(ret);
    ret = btc_tx_add_vout_p2sh_addr(&tx, BTC_MBTC2SATOSHI(1.9), "2NCFo5oZuEbXgZdMDzLMA2qQiroHrU6oXSU");
    ASSERT_TRUE(ret);

    btc_util_keys_t keys;
    const char WIF[] = "cW8SSTFrM42mX5YKHKbDfvXF5qEJrAgLoRTc68bNJo5GFDv6WvX1";
    btc_chain_t chain;
    ret = btc_util_wif2keys(&keys, &chain, WIF);
    ASSERT_TRUE(ret);
    ASSERT_EQ(BTC_TESTNET, chain);
    ret = btc_util_sign_p2wpkh(&tx, 0, BTC_MBTC2SATOSHI(3), &keys);
    ASSERT_TRUE(ret);

    utl_buf_t txbuf = UTL_BUF_INIT;
    btc_tx_create(&txbuf, &tx);
    printf("tx=\n");
    send::DumpBin(txbuf.buf, txbuf.len);

    //bitcoindで送信OK
    // txid : 6a6c20391e4ba1a9fa7afb9c67aef02116a3b6a47cacf2dada0434ff664109eb
    const uint8_t TX_SENT[] = {
        0x02, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x8e,
        0x1c, 0xe8, 0xf5, 0x61, 0x7d, 0x36, 0xb1, 0x3f,
        0x87, 0x38, 0x34, 0xfc, 0x9c, 0x77, 0x92, 0x0f,
        0x9f, 0x8f, 0x1d, 0xee, 0x90, 0x0c, 0xb5, 0x9b,
        0xe8, 0x78, 0x3d, 0x1b, 0xf1, 0x15, 0x67, 0x01,
        0x00, 0x00, 0x00, 0x17, 0x16, 0x00, 0x14, 0x8c,
        0x97, 0x51, 0x96, 0x2d, 0xc5, 0xe4, 0xd6, 0x62,
        0x82, 0x9b, 0x56, 0xd7, 0xf2, 0xa7, 0xf5, 0x95,
        0xea, 0xd0, 0x78, 0xff, 0xff, 0xff, 0xff, 0x02,
        0xa0, 0x86, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x19, 0x76, 0xa9, 0x14, 0x26, 0x18, 0xaf, 0x0c,
        0x70, 0x51, 0xb3, 0xfc, 0xe0, 0x37, 0x6a, 0xd5,
        0x8f, 0xa6, 0x5f, 0x9d, 0xc8, 0x5e, 0x64, 0x46,
        0x88, 0xac, 0x30, 0xe6, 0x02, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x17, 0xa9, 0x14, 0xd0, 0x86, 0x04,
        0x2f, 0x4e, 0x22, 0x9f, 0x91, 0xd6, 0x2a, 0x46,
        0xa5, 0xd2, 0x62, 0x34, 0x05, 0x6f, 0x6b, 0x2d,
        0x18, 0x87, 0x02, 0x47, 0x30, 0x44, 0x02, 0x20,
        0x5b, 0x9c, 0xb4, 0x5a, 0xac, 0x6c, 0xd8, 0x54,
        0x03, 0x4d, 0x6e, 0x5f, 0x28, 0xb8, 0x75, 0x66,
        0x32, 0x08, 0x64, 0x49, 0x3a, 0x65, 0x06, 0x0a,
        0x42, 0x3b, 0x3d, 0xa2, 0xed, 0xd5, 0xa1, 0x1f,
        0x02, 0x20, 0x4b, 0x4b, 0x19, 0x0d, 0x50, 0x6d,
        0xf3, 0x3b, 0xd5, 0xcf, 0xac, 0xb3, 0xa8, 0xb8,
        0x1c, 0x04, 0x99, 0x06, 0x29, 0x5e, 0xc3, 0xb7,
        0x86, 0xcc, 0xe7, 0xec, 0xa3, 0x77, 0x61, 0xfe,
        0x9a, 0x7b, 0x01, 0x21, 0x03, 0x38, 0x36, 0x97,
        0x77, 0x43, 0x26, 0x0c, 0x86, 0xb0, 0xca, 0xbd,
        0x86, 0x40, 0xc7, 0x9d, 0x75, 0x0c, 0x81, 0xb2,
        0xa5, 0x68, 0x37, 0xcf, 0x8e, 0x5d, 0xbc, 0x23,
        0x0f, 0xec, 0x92, 0x36, 0xe6, 0x00, 0x00, 0x00,
        0x00,
    };
    ASSERT_EQ(0, memcmp(TX_SENT, txbuf.buf, sizeof(TX_SENT)));
    ASSERT_EQ(sizeof(TX_SENT), txbuf.len);

    uint8_t txid[BTC_SZ_TXID];
    ret = btc_tx_txid(txid, &tx);
    send::DumpTxid(txid);

    utl_buf_free(&txbuf);
    btc_tx_free(&tx);
}


TEST_F(send, p2wsh)
{
    bool ret;
    btc_tx_t tx;
    btc_tx_init(&tx);

    //送金元#1 : 2NCFo5oZuEbXgZdMDzLMA2qQiroHrU6oXSU(<== mtLLAiafrhzcjSZqp2Ts86Gv7PupWnXKUc)
    //          wif: cW8SSTFrM42mX5YKHKbDfvXF5qEJrAgLoRTc68bNJo5GFDv6WvX1
    //      txid:  6a6c20391e4ba1a9fa7afb9c67aef02116a3b6a47cacf2dada0434ff664109eb
    //      inddex: 1
    //      Value: 1.9 mBTC

    //送金元#2 : 2NDxM8795n9HsLiniWowcn6gwSemNKzsN7a(<== mmsgPUnoceq7er7f9HuaZV2ktMkaVD3Za1)
    //          wif: cR645M2xZJnE5mDWw5LpAghNLudXGZsCs4ZEUvRMr2NrHqU3rLWa
    //      txid:  1167092968bd8e2c37f0977036e35b375529fc468cc95719bc8c294e00a2bf0b
    //      inddex: 0
    //      Value: 4 mBTC

    //送金先 : 2-of-2
    //      Value: 5.8 mBTC
    //FEE : 0.1 mBTC
    //          5.8 + 0.1 = 1.9 + 4 mBTC

    const uint8_t TXID1[] = {
        0xeb, 0x09, 0x41, 0x66, 0xff, 0x34, 0x04, 0xda,
        0xda, 0xf2, 0xac, 0x7c, 0xa4, 0xb6, 0xa3, 0x16,
        0x21, 0xf0, 0xae, 0x67, 0x9c, 0xfb, 0x7a, 0xfa,
        0xa9, 0xa1, 0x4b, 0x1e, 0x39, 0x20, 0x6c, 0x6a,
    };
    const uint8_t TXID2[] = {
        0x0b, 0xbf, 0xa2, 0x00, 0x4e, 0x29, 0x8c, 0xbc,
        0x19, 0x57, 0xc9, 0x8c, 0x46, 0xfc, 0x29, 0x55,
        0x37, 0x5b, 0xe3, 0x36, 0x70, 0x97, 0xf0, 0x37,
        0x2c, 0x8e, 0xbd, 0x68, 0x29, 0x09, 0x67, 0x11,
    };
    btc_tx_add_vin(&tx, TXID1, 1);
    btc_tx_add_vin(&tx, TXID2, 0);

    btc_util_keys_t keys1;
    btc_chain_t chain;
    const char WIF1[] = "cW8SSTFrM42mX5YKHKbDfvXF5qEJrAgLoRTc68bNJo5GFDv6WvX1";
    ret = btc_util_wif2keys(&keys1, &chain, WIF1);
    ASSERT_TRUE(ret);
    ASSERT_EQ(BTC_TESTNET, chain);

    btc_util_keys_t keys2;
    const char WIF2[] = "cR645M2xZJnE5mDWw5LpAghNLudXGZsCs4ZEUvRMr2NrHqU3rLWa";
    ret = btc_util_wif2keys(&keys2, &chain, WIF2);
    ASSERT_TRUE(ret);
    ASSERT_EQ(BTC_TESTNET, chain);

    //2-of-2
    utl_buf_t wit = UTL_BUF_INIT;
    ret = btc_keys_create2of2(&wit, keys2.pub, keys1.pub);      //ソートしないようにしたので順番をあわせる
    ASSERT_TRUE(ret);
    printf("wit= \n");
    send::DumpBin(wit.buf, wit.len);
    btc_sw_add_vout_p2wsh(&tx, BTC_MBTC2SATOSHI(5.8), &wit);

    const char ADDR_2OF2[] = "2MuuDWRBQ5KTxJzAk1qPFZfzeheLcoSu3vy";
    char addr_2of2[BTC_SZ_ADDR_MAX + 1];
    btc_keys_wit2waddr(addr_2of2, &wit);
    ASSERT_STREQ(ADDR_2OF2, addr_2of2);
    printf("addr 2of2= %s\n", addr_2of2);
    utl_buf_free(&wit);

    //vinの順番は、2-of-2の順番と関係が無い
    ret = btc_util_sign_p2wpkh(&tx, 0, BTC_MBTC2SATOSHI(1.9), &keys1);
    ASSERT_TRUE(ret);
    ret = btc_util_sign_p2wpkh(&tx, 1, BTC_MBTC2SATOSHI(4), &keys2);
    ASSERT_TRUE(ret);


    utl_buf_t txbuf = UTL_BUF_INIT;
    btc_tx_create(&txbuf, &tx);
    printf("tx=\n");
    send::DumpBin(txbuf.buf, txbuf.len);
    //btc_print_tx(&tx);
    // txid : 623948367973c3813a4e3ed6aa3b714e7e5303a0852057f3e4ecd70c121827d8
    const uint8_t TX_SENT[] = {
        0x02, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0xeb,
        0x09, 0x41, 0x66, 0xff, 0x34, 0x04, 0xda, 0xda,
        0xf2, 0xac, 0x7c, 0xa4, 0xb6, 0xa3, 0x16, 0x21,
        0xf0, 0xae, 0x67, 0x9c, 0xfb, 0x7a, 0xfa, 0xa9,
        0xa1, 0x4b, 0x1e, 0x39, 0x20, 0x6c, 0x6a, 0x01,
        0x00, 0x00, 0x00, 0x17, 0x16, 0x00, 0x14, 0x8c,
        0x97, 0x51, 0x96, 0x2d, 0xc5, 0xe4, 0xd6, 0x62,
        0x82, 0x9b, 0x56, 0xd7, 0xf2, 0xa7, 0xf5, 0x95,
        0xea, 0xd0, 0x78, 0xff, 0xff, 0xff, 0xff, 0x0b,
        0xbf, 0xa2, 0x00, 0x4e, 0x29, 0x8c, 0xbc, 0x19,
        0x57, 0xc9, 0x8c, 0x46, 0xfc, 0x29, 0x55, 0x37,
        0x5b, 0xe3, 0x36, 0x70, 0x97, 0xf0, 0x37, 0x2c,
        0x8e, 0xbd, 0x68, 0x29, 0x09, 0x67, 0x11, 0x00,
        0x00, 0x00, 0x00, 0x17, 0x16, 0x00, 0x14, 0x45,
        0xbc, 0x36, 0x3c, 0xc6, 0xb3, 0x91, 0x74, 0xc2,
        0xe7, 0x72, 0xe7, 0xb1, 0x32, 0x30, 0x9d, 0x06,
        0xd7, 0x49, 0x66, 0xff, 0xff, 0xff, 0xff, 0x01,
        0xa0, 0xd9, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x17, 0xa9, 0x14, 0x1d, 0x1f, 0xa8, 0x34, 0xaf,
        0x3e, 0xbb, 0xd3, 0x9f, 0x94, 0x15, 0x8e, 0x37,
        0x7d, 0xbc, 0x74, 0x37, 0x22, 0x90, 0xf5, 0x87,
        0x02, 0x47, 0x30, 0x44, 0x02, 0x20, 0x58, 0x95,
        0x41, 0xde, 0x5d, 0x04, 0xdf, 0xc0, 0x53, 0x23,
        0xe5, 0x4d, 0x2d, 0x98, 0x63, 0x27, 0x01, 0x56,
        0x8e, 0x46, 0x60, 0x5d, 0x1d, 0xe0, 0x52, 0xc8,
        0x7f, 0xeb, 0xf4, 0x3b, 0x4c, 0x62, 0x02, 0x20,
        0x6d, 0x7c, 0x3d, 0xbe, 0x35, 0x39, 0xae, 0x74,
        0xa1, 0xaa, 0x97, 0x26, 0x5a, 0x60, 0x96, 0x84,
        0x4c, 0x69, 0x7d, 0x76, 0x6e, 0x8c, 0xba, 0x74,
        0x66, 0x11, 0x34, 0x5c, 0xd1, 0x3f, 0x97, 0x4a,
        0x01, 0x21, 0x03, 0x38, 0x36, 0x97, 0x77, 0x43,
        0x26, 0x0c, 0x86, 0xb0, 0xca, 0xbd, 0x86, 0x40,
        0xc7, 0x9d, 0x75, 0x0c, 0x81, 0xb2, 0xa5, 0x68,
        0x37, 0xcf, 0x8e, 0x5d, 0xbc, 0x23, 0x0f, 0xec,
        0x92, 0x36, 0xe6, 0x02, 0x47, 0x30, 0x44, 0x02,
        0x20, 0x63, 0xf7, 0xf1, 0x1a, 0xe2, 0xe0, 0xae,
        0x58, 0xc2, 0x5e, 0x28, 0x17, 0x1d, 0x4f, 0x6b,
        0xdf, 0x5c, 0xab, 0x05, 0x72, 0x8a, 0x3e, 0xdf,
        0x63, 0x90, 0x3c, 0x1d, 0xf9, 0x74, 0xe2, 0xd1,
        0xdd, 0x02, 0x20, 0x0f, 0xef, 0xe4, 0x39, 0xa3,
        0x87, 0xf9, 0xba, 0x95, 0x9b, 0xd5, 0xe2, 0xc8,
        0x9d, 0x7a, 0x9f, 0x14, 0x34, 0x91, 0xe1, 0xc3,
        0xcc, 0x4f, 0xc8, 0xb1, 0xdd, 0x1b, 0x7b, 0xb4,
        0x7f, 0xb7, 0xf6, 0x01, 0x21, 0x02, 0x44, 0xc7,
        0x58, 0x11, 0x77, 0x81, 0x96, 0x8e, 0x8e, 0x09,
        0x5d, 0xc6, 0xa5, 0xb8, 0x9f, 0x4e, 0x12, 0x40,
        0xbc, 0x7d, 0xce, 0xbc, 0xb2, 0xc1, 0x5f, 0xc9,
        0x1d, 0x33, 0x3d, 0xea, 0x79, 0xee, 0x00, 0x00,
        0x00, 0x00,
    };
    ASSERT_EQ(0, memcmp(TX_SENT, txbuf.buf, sizeof(TX_SENT)));
    ASSERT_EQ(sizeof(TX_SENT), txbuf.len);

    uint8_t txid[BTC_SZ_TXID];
    ret = btc_tx_txid(txid, &tx);
    send::DumpTxid(txid);

    utl_buf_free(&txbuf);
    btc_tx_free(&tx);
}
