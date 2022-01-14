// Copyright (c) 2017-2022 The Particl Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>
#include <net.h>
#include <script/signingprovider.h>
#include <script/script.h>
#include <consensus/validation.h>
#include <consensus/merkle.h>
#include <consensus/tx_verify.h>
#include <key/extkey.h>
#include <pos/kernel.h>
#include <chainparams.h>
#include <blind.h>

#include <script/sign.h>
#include <policy/policy.h>
#include <test/data/particl_taproot.json.h>
#include <core_io.h>
#include <univalue.h>

#include <boost/test/unit_test.hpp>

extern UniValue read_json(const std::string& jsondata);

bool CheckInputScripts(const CTransaction& tx, TxValidationState& state,
                       const CCoinsViewCache& inputs, unsigned int flags, bool cacheSigStore,
                       bool cacheFullScriptStore, PrecomputedTransactionData& txdata,
                       std::vector<CScriptCheck> *pvChecks, bool fAnonChecks = true) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

BOOST_FIXTURE_TEST_SUITE(particlchain_tests, ParticlBasicTestingSetup)


BOOST_AUTO_TEST_CASE(oldversion_test)
{
    CBlock blk, blkOut;
    blk.nTime = 1487406900;

    CMutableTransaction txn;
    blk.vtx.push_back(MakeTransactionRef(txn));

    CDataStream ss(SER_DISK, 0);

    ss << blk;
    ss >> blkOut;

    BOOST_CHECK(blk.vtx[0]->nVersion == blkOut.vtx[0]->nVersion);
}

BOOST_AUTO_TEST_CASE(signature_test)
{
    SeedInsecureRand();
    FillableSigningProvider keystore;

    CKey k;
    InsecureNewKey(k, true);
    keystore.AddKey(k);

    CPubKey pk = k.GetPubKey();
    CKeyID id = pk.GetID();

    CMutableTransaction txn;
    txn.nVersion = PARTICL_TXN_VERSION;
    txn.nLockTime = 0;

    int nBlockHeight = 22;
    OUTPUT_PTR<CTxOutData> out0 = MAKE_OUTPUT<CTxOutData>();
    out0->vData = SetCompressedInt64(out0->vData, nBlockHeight);
    txn.vpout.push_back(out0);

    CScript script = CScript() << OP_DUP << OP_HASH160 << ToByteVector(id) << OP_EQUALVERIFY << OP_CHECKSIG;
    OUTPUT_PTR<CTxOutStandard> out1 = MAKE_OUTPUT<CTxOutStandard>();
    out1->nValue = 10000;
    out1->scriptPubKey = script;
    txn.vpout.push_back(out1);

    CMutableTransaction txn2;
    txn2.nVersion = PARTICL_TXN_VERSION;
    txn2.vin.push_back(CTxIn(txn.GetHash(), 0));

    std::vector<uint8_t> vchAmount(8);
    part::SetAmount(vchAmount, out1->nValue);

    SignatureData sigdata;
    BOOST_CHECK(ProduceSignature(keystore, MutableTransactionSignatureCreator(&txn2, 0, vchAmount, SIGHASH_ALL), script, sigdata));

    ScriptError serror = SCRIPT_ERR_OK;
    BOOST_CHECK(VerifyScript(txn2.vin[0].scriptSig, out1->scriptPubKey, &sigdata.scriptWitness, STANDARD_SCRIPT_VERIFY_FLAGS, MutableTransactionSignatureChecker(&txn2, 0, vchAmount), &serror));
    BOOST_CHECK(serror == SCRIPT_ERR_OK);
}

BOOST_AUTO_TEST_CASE(particlchain_test)
{
    SeedInsecureRand();
    FillableSigningProvider keystore;

    CKey k;
    InsecureNewKey(k, true);
    keystore.AddKey(k);

    CPubKey pk = k.GetPubKey();
    CKeyID id = pk.GetID();

    CScript script = CScript() << OP_DUP << OP_HASH160 << ToByteVector(id) << OP_EQUALVERIFY << OP_CHECKSIG;

    CBlock blk;
    blk.nVersion = PARTICL_BLOCK_VERSION;
    blk.nTime = 1487406900;

    CMutableTransaction txn;
    txn.nVersion = PARTICL_TXN_VERSION;
    txn.SetType(TXN_COINBASE);
    txn.nLockTime = 0;
    OUTPUT_PTR<CTxOutStandard> out0 = MAKE_OUTPUT<CTxOutStandard>();
    out0->nValue = 10000;
    out0->scriptPubKey = script;
    txn.vpout.push_back(out0);


    blk.vtx.push_back(MakeTransactionRef(txn));

    bool mutated;
    blk.hashMerkleRoot = BlockMerkleRoot(blk, &mutated);
    blk.hashWitnessMerkleRoot = BlockWitnessMerkleRoot(blk, &mutated);


    CDataStream ss(SER_DISK, 0);
    ss << blk;

    CBlock blkOut;
    ss >> blkOut;

    BOOST_CHECK(blk.hashMerkleRoot == blkOut.hashMerkleRoot);
    BOOST_CHECK(blk.hashWitnessMerkleRoot == blkOut.hashWitnessMerkleRoot);
    BOOST_CHECK(blk.nTime == blkOut.nTime && blkOut.nTime == 1487406900);

    BOOST_CHECK(TXN_COINBASE == blkOut.vtx[0]->GetType());
}

BOOST_AUTO_TEST_CASE(varints)
{
    SeedInsecureRand();

    int start = InsecureRandRange(100);
    size_t size = 0;
    uint8_t c[128];
    std::vector<uint8_t> v;

    // Encode
    for (int i = start; i < 10000; i+=100) {
        size_t sz = GetSizeOfVarInt<VarIntMode::NONNEGATIVE_SIGNED>(i);
        BOOST_CHECK(sz = part::PutVarInt(c, i));
        BOOST_CHECK(0 == part::PutVarInt(v, i));
        BOOST_CHECK(0 == memcmp(c, &v[size], sz));
        size += sz;
        BOOST_CHECK(size == v.size());
    }
    for (uint64_t i = 0;  i < 100000000000ULL; i += 999999937) {
        BOOST_CHECK(0 == part::PutVarInt(v, i));
        size += GetSizeOfVarInt<VarIntMode::DEFAULT>(i);
        BOOST_CHECK(size == v.size());
    }

    // Decode
    size_t nB = 0, o = 0;
    for (int i = start; i < 10000; i+=100) {
        uint64_t j = (uint64_t)-1;
        BOOST_CHECK(0 == part::GetVarInt(v, o, j, nB));
        BOOST_CHECK_MESSAGE(i == (int)j, "decoded:" << j << " expected:" << i);
        o += nB;
    }
    for (uint64_t i = 0;  i < 100000000000ULL; i += 999999937) {
        uint64_t j = (uint64_t)-1;
        BOOST_CHECK(0 == part::GetVarInt(v, o, j, nB));
        BOOST_CHECK_MESSAGE(i == j, "decoded:" << j << " expected:" << i);
        o += nB;
    }
}

BOOST_AUTO_TEST_CASE(mixed_input_types)
{
    CMutableTransaction txn;
    txn.nVersion = PARTICL_TXN_VERSION;
    BOOST_CHECK(txn.IsParticlVersion());

    CAmount txfee;
    int nSpendHeight = 1;
    CCoinsView viewDummy;
    CCoinsViewCache inputs(&viewDummy);

    CMutableTransaction txnPrev;
    txnPrev.nVersion = PARTICL_TXN_VERSION;
    BOOST_CHECK(txnPrev.IsParticlVersion());

    CScript scriptPubKey;
    txnPrev.vpout.push_back(MAKE_OUTPUT<CTxOutStandard>(1 * COIN, scriptPubKey));
    txnPrev.vpout.push_back(MAKE_OUTPUT<CTxOutStandard>(2 * COIN, scriptPubKey));
    txnPrev.vpout.push_back(MAKE_OUTPUT<CTxOutCT>());
    txnPrev.vpout.push_back(MAKE_OUTPUT<CTxOutCT>());

    CTransaction txnPrev_c(txnPrev);
    AddCoins(inputs, txnPrev_c, 1);

    uint256 prevHash = txnPrev_c.GetHash();

    std::vector<std::pair<std::vector<int>, bool> > tests = {
        std::make_pair( (std::vector<int>) {0 }, true),
        std::make_pair( (std::vector<int>) {0, 1}, true),
        std::make_pair( (std::vector<int>) {0, 2}, false),
        std::make_pair( (std::vector<int>) {0, 1, 2}, false),
        std::make_pair( (std::vector<int>) {2}, true),
        std::make_pair( (std::vector<int>) {2, 3}, true),
        std::make_pair( (std::vector<int>) {2, 3, 1}, false),
        std::make_pair( (std::vector<int>) {-1}, true),
        std::make_pair( (std::vector<int>) {-1, -1}, true),
        std::make_pair( (std::vector<int>) {2, -1}, false),
        std::make_pair( (std::vector<int>) {0, -1}, false),
        std::make_pair( (std::vector<int>) {0, 0, -1}, false),
        std::make_pair( (std::vector<int>) {0, 2, -1}, false)
    };

    for (auto t : tests) {
        txn.vin.clear();

        for (auto ti : t.first) {
            if (ti < 0)  {
                CTxIn ai;
                ai.prevout.n = COutPoint::ANON_MARKER;
                ai.SetAnonInfo(1, 1);

                std::vector<uint8_t> vpkm, vki(33, 0);
                part::PutVarInt(vpkm, 1);
                ai.scriptWitness.stack.emplace_back(vpkm);
                ai.scriptData.stack.emplace_back(vki);
                txn.vin.push_back(ai);
                continue;
            }
            txn.vin.push_back(CTxIn(prevHash, ti));
        }

        CTransaction tx_c(txn);
        TxValidationState state;
        Consensus::CheckTxInputs(tx_c, state, inputs, nSpendHeight, txfee);

        if (t.second) {
            BOOST_CHECK(state.GetRejectReason() != "mixed-input-types");
        } else {
            BOOST_CHECK(state.GetRejectReason() == "mixed-input-types");
        }
    }
}

BOOST_AUTO_TEST_CASE(mixed_output_types)
{
    ECC_Start_Blinding();
    // When sending from plain only CT or RCT outputs are valid
    CAmount txfee = 2000;
    int nSpendHeight = 1;
    CCoinsView viewDummy;
    CCoinsViewCache inputs(&viewDummy);

    CMutableTransaction txnPrev;
    txnPrev.nVersion = PARTICL_TXN_VERSION;
    BOOST_CHECK(txnPrev.IsParticlVersion());

    CScript scriptPubKey;
    txnPrev.vpout.push_back(MAKE_OUTPUT<CTxOutStandard>(1 * COIN, scriptPubKey));

    CTransaction txnPrev_c(txnPrev);
    AddCoins(inputs, txnPrev_c, 1);
    uint256 prevHash = txnPrev_c.GetHash();

    CMutableTransaction txn;
    txn.nVersion = PARTICL_TXN_VERSION;
    BOOST_CHECK(txn.IsParticlVersion());
    txn.vin.push_back(CTxIn(prevHash, 0));

    OUTPUT_PTR<CTxOutData> out_fee = MAKE_OUTPUT<CTxOutData>();
    out_fee->vData.push_back(DO_FEE);
    BOOST_REQUIRE(0 == part::PutVarInt(out_fee->vData, txfee));
    txn.vpout.push_back(out_fee);

    txn.vpout.push_back(MAKE_OUTPUT<CTxOutStandard>(1 * COIN - txfee, scriptPubKey));
    txn.vpout.push_back(MAKE_OUTPUT<CTxOutCT>());
    txn.vpout.push_back(MAKE_OUTPUT<CTxOutRingCT>());

    CTransaction tx_c(txn);
    TxValidationState state;
    state.SetStateInfo(GetTime(), nSpendHeight, Params().GetConsensus(), true /* particl_mode */, false /* skip_rangeproof */);
    state.m_clamp_tx_version = true; // Using mainnet chainparams
    gArgs.ForceSetArg("-acceptanontxn", "1"); // TODO: remove
    gArgs.ForceSetArg("-acceptblindtxn", "1"); // TODO: remove
    BOOST_CHECK(!Consensus::CheckTxInputs(tx_c, state, inputs, nSpendHeight, txfee));
    BOOST_CHECK(state.GetRejectReason() == "bad-txns-plain-in-mixed-out");

    txn.vpout.pop_back();
    CTransaction tx_c2(txn);
    BOOST_CHECK(!Consensus::CheckTxInputs(tx_c2, state, inputs, nSpendHeight, txfee));
    BOOST_CHECK(state.GetRejectReason() != "bad-txns-plain-in-mixed-out");

    ECC_Stop_Blinding();
}

BOOST_AUTO_TEST_CASE(op_iscoinstake_tests)
{
    CKey k1, k2;
    InsecureNewKey(k1, true);
    InsecureNewKey(k2, true);
    CPubKey pk1 = k1.GetPubKey(), pk2 = k2.GetPubKey();
    CKeyID id1 = pk1.GetID(), id2 = pk2.GetID();

    CScript scriptOutA, scriptOutB;
    CScript scriptStake = CScript() << OP_DUP << OP_HASH160 << ToByteVector(id1) << OP_EQUALVERIFY << OP_CHECKSIG;
    CScript scriptSpend = CScript() << OP_DUP << OP_HASH160 << ToByteVector(id2) << OP_EQUALVERIFY << OP_CHECKSIG;

    CScript script = CScript() << OP_ISCOINSTAKE << OP_IF;
    script.append(scriptStake);
    script << OP_ELSE;
    script.append(scriptSpend);
    script << OP_ENDIF;

    BOOST_CHECK(true == SplitConditionalCoinstakeScript(script, scriptOutA, scriptOutB));
    BOOST_CHECK(true == SplitConditionalCoinstakeScript(script, scriptOutA, scriptOutB, true));

    script << OP_DROP;
    script << CScriptNum(123);

    BOOST_CHECK(true == SplitConditionalCoinstakeScript(script, scriptOutA, scriptOutB));
    BOOST_CHECK(false == SplitConditionalCoinstakeScript(script, scriptOutA, scriptOutB, true));
}

inline static void memput_uint32_le(uint8_t *p, uint32_t v) {
    v = htole32((uint32_t) v);
    memcpy(p, &v, 4);
}

BOOST_AUTO_TEST_CASE(smsg_fees)
{
    int nSpendHeight = 1;
    CCoinsView viewDummy;
    CCoinsViewCache inputs(&viewDummy);

    CMutableTransaction txnPrev;
    txnPrev.nVersion = PARTICL_TXN_VERSION;
    BOOST_CHECK(txnPrev.IsParticlVersion());

    CScript scriptPubKey;
    txnPrev.vpout.push_back(MAKE_OUTPUT<CTxOutStandard>(1 * COIN, scriptPubKey));

    CTransaction txnPrev_c(txnPrev);
    AddCoins(inputs, txnPrev_c, 1);
    uint256 prevHash = txnPrev_c.GetHash();

    CMutableTransaction txn;
    txn.nVersion = PARTICL_TXN_VERSION;
    BOOST_CHECK(txn.IsParticlVersion());
    txn.vin.push_back(CTxIn(prevHash, 0));

    CAmount smsg_fee = 20000;
    std::vector<uint8_t> vData(1 + 24);
    vData[0] = DO_FUND_MSG;
    memset(&vData[1], 0, 20);
    memput_uint32_le(&vData[21], smsg_fee);


    OUTPUT_PTR<CTxOutData> out_smsg_fees = MAKE_OUTPUT<CTxOutData>();
    out_smsg_fees->vData = vData;
    txn.vpout.push_back(out_smsg_fees);
    txn.vpout.push_back(MAKE_OUTPUT<CTxOutStandard>(1 * COIN - smsg_fee, scriptPubKey));

    CFeeRate funding_tx_fee = CFeeRate(Params().GetConsensus().smsg_fee_funding_tx_per_k);
    size_t nBytes = GetVirtualTransactionSize(CTransaction(txn));
    CAmount txfee = funding_tx_fee.GetFee(nBytes);

    {
        CTransaction tx_c(txn);
        TxValidationState state;
        state.SetStateInfo(GetTime(), nSpendHeight, Params().GetConsensus(), true /* particl_mode */, false /* skip_rangeproof */);
        BOOST_CHECK(CheckTransaction(tx_c, state));
        BOOST_CHECK(!Consensus::CheckTxInputs(tx_c, state, inputs, nSpendHeight, txfee));
        BOOST_CHECK(state.GetRejectReason() == "bad-txns-fee-smsg");
    }

    ((CTxOutStandard*)txn.vpout.back().get())->nValue -= txfee;
    {
        CTransaction tx_c(txn);
        TxValidationState state;
        state.SetStateInfo(GetTime(), nSpendHeight, Params().GetConsensus(), true /* particl_mode */, false /* skip_rangeproof */);
        BOOST_CHECK(CheckTransaction(tx_c, state));
        BOOST_CHECK(Consensus::CheckTxInputs(tx_c, state, inputs, nSpendHeight, txfee));
    }

    txn.vpout.push_back(out_smsg_fees);
    txn.vpout.push_back(out_smsg_fees);
    {
        CTransaction tx_c(txn);
        TxValidationState state;
        state.SetStateInfo(GetTime(), nSpendHeight, Params().GetConsensus(), true /* particl_mode */, false /* skip_rangeproof */);
        BOOST_CHECK(!CheckTransaction(tx_c, state));
        BOOST_CHECK(state.GetRejectReason() == "too-many-data-outputs");
    }

    // Test multiple messages
    txn.vpout.pop_back();
    txn.vpout.pop_back();

    CAmount smsg_fee_2 = 10000;
    {
        std::vector<uint8_t> &vData = *txn.vpout[0]->GetPData();
        vData.resize(49);
        memset(&vData[25], 0, 20);
        memput_uint32_le(&vData[1 + 24 + 20], smsg_fee_2);
    }
    {
        CTransaction tx_c(txn);
        TxValidationState state;
        state.SetStateInfo(GetTime(), nSpendHeight, Params().GetConsensus(), true /* particl_mode */, false /* skip_rangeproof */);
        BOOST_CHECK(CheckTransaction(tx_c, state));
        BOOST_CHECK(!Consensus::CheckTxInputs(tx_c, state, inputs, nSpendHeight, txfee));
        BOOST_CHECK(state.GetRejectReason() == "bad-txns-fee-smsg");
    }
    nBytes = GetVirtualTransactionSize(CTransaction(txn));
    txfee = funding_tx_fee.GetFee(nBytes);
    ((CTxOutStandard*)txn.vpout[1].get())->nValue = 1 * COIN - (smsg_fee + smsg_fee_2 + txfee);
    {
        CTransaction tx_c(txn);
        TxValidationState state;
        state.SetStateInfo(GetTime(), nSpendHeight, Params().GetConsensus(), true /* particl_mode */, false /* skip_rangeproof */);
        BOOST_CHECK(CheckTransaction(tx_c, state));
        BOOST_CHECK(Consensus::CheckTxInputs(tx_c, state, inputs, nSpendHeight, txfee));
    }
}

BOOST_AUTO_TEST_CASE(coin_year_reward)
{
    BOOST_CHECK(Params().GetCoinYearReward(1529700000) == 5 * CENT);
    BOOST_CHECK(Params().GetCoinYearReward(1531832399) == 5 * CENT);
    BOOST_CHECK(Params().GetCoinYearReward(1531832400) == 4 * CENT);    // 2018-07-17 13:00:00
    BOOST_CHECK(Params().GetCoinYearReward(1563368399) == 4 * CENT);
    BOOST_CHECK(Params().GetCoinYearReward(1563368400) == 3 * CENT);    // 2019-07-17 13:00:00
    BOOST_CHECK(Params().GetCoinYearReward(1594904399) == 3 * CENT);
    BOOST_CHECK(Params().GetCoinYearReward(1594904400) == 2 * CENT);    // 2020-07-16 13:00:00

    size_t seconds_in_year = 60 * 60 * 24 * 365;
    BOOST_CHECK(Params().GetCoinYearReward(1626109199) == 2 * CENT);
    BOOST_CHECK(Params().GetCoinYearReward(1626109200) == 8 * CENT);                                // 2021-07-12 17:00:00 UTC
    BOOST_CHECK(Params().GetCoinYearReward(1626109200 + seconds_in_year) == 8 * CENT);
    BOOST_CHECK(Params().GetCoinYearReward(1626109200 + seconds_in_year * 2 - 1) == 8 * CENT);
    BOOST_CHECK(Params().GetCoinYearReward(1626109200 + seconds_in_year * 2) == 7 * CENT);          // 2023-07-12 17:00:00 UTC
    BOOST_CHECK(Params().GetCoinYearReward(1626109200 + seconds_in_year * 3) == 7 * CENT);
    BOOST_CHECK(Params().GetCoinYearReward(1626109200 + seconds_in_year * 4 - 1) == 7 * CENT);
    BOOST_CHECK(Params().GetCoinYearReward(1626109200 + seconds_in_year * 4) == 6 * CENT);          // 2025-07-11 17:00:00 UTC
    BOOST_CHECK(Params().GetCoinYearReward(1626109200 + seconds_in_year * 6) == 6 * CENT);
}

BOOST_AUTO_TEST_CASE(taproot)
{
    // Import txns from version 22.x
    UniValue test_txns = read_json(
        std::string(json_tests::particl_taproot,
        json_tests::particl_taproot + sizeof(json_tests::particl_taproot)));

    unsigned int flags = SCRIPT_VERIFY_P2SH;
    flags |= SCRIPT_VERIFY_DERSIG;
    flags |= SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;
    flags |= SCRIPT_VERIFY_CHECKSEQUENCEVERIFY;
    flags |= SCRIPT_VERIFY_WITNESS;
    flags |= SCRIPT_VERIFY_NULLDUMMY;

    unsigned int flags_with_taproot = flags | SCRIPT_VERIFY_TAPROOT;

    CAmount txfee_out;
    int nSpendHeight = 1;

    // Test signing with the internal pubkey
    {
    CCoinsView viewDummy;
    CCoinsViewCache inputs(&viewDummy);

    CMutableTransaction mtx1;
    BOOST_CHECK(DecodeHexTx(mtx1, test_txns[0].get_str()));
    CTransaction tx1(mtx1);

    AddCoins(inputs, tx1, 1);

    CMutableTransaction mtx2;
    BOOST_CHECK(DecodeHexTx(mtx2, test_txns[1].get_str()));
    CTransaction tx2(mtx2);

    TxValidationState state;
    bool rv = Consensus::CheckTxInputs(tx2, state, inputs, nSpendHeight, txfee_out);
    BOOST_CHECK(rv);

    {
    // Without SCRIPT_VERIFY_TAPROOT prevout defaults to spendable
    LOCK(cs_main);
    PrecomputedTransactionData txdata;
    bool ret = CheckInputScripts(tx2, state, inputs, flags, /* cacheSigStore */ false, /* cacheFullScriptStore */ false, txdata, nullptr);
    BOOST_CHECK(ret);
    }

    {
    // With SCRIPT_VERIFY_TAPROOT prevout must pass verification
    LOCK(cs_main);
    PrecomputedTransactionData txdata;
    bool ret = CheckInputScripts(tx2, state, inputs, flags_with_taproot, /* cacheSigStore */ false, /* cacheFullScriptStore */ false, txdata, nullptr);
    BOOST_CHECK(!ret);
    }

    {
    TxValidationState state;
    CMutableTransaction mtx3;
    BOOST_CHECK(DecodeHexTx(mtx3, test_txns[2].get_str()));
    CTransaction tx3(mtx3);

    LOCK(cs_main);
    PrecomputedTransactionData txdata;
    bool ret = CheckInputScripts(tx3, state, inputs, flags_with_taproot, /* cacheSigStore */ false, /* cacheFullScriptStore */ false, txdata, nullptr);
    BOOST_CHECK(ret);
    }
    }

    // Test signing a script path
    {
    CCoinsView viewDummy;
    CCoinsViewCache inputs(&viewDummy);

    CMutableTransaction mtx4;
    BOOST_CHECK(DecodeHexTx(mtx4, test_txns[3].get_str()));
    CTransaction tx4(mtx4);

    AddCoins(inputs, tx4, 1);

    {
    // Should fail as !IsCoinStake()
    TxValidationState state;
    CMutableTransaction mtx5;
    BOOST_CHECK(DecodeHexTx(mtx5, test_txns[4].get_str()));
    CTransaction tx5(mtx5);

    LOCK(cs_main);
    PrecomputedTransactionData txdata;
    bool ret = CheckInputScripts(tx5, state, inputs, flags_with_taproot, /* cacheSigStore */ false, /* cacheFullScriptStore */ false, txdata, nullptr);
    BOOST_CHECK(!ret);
    BOOST_CHECK(state.GetRejectReason() == "non-mandatory-script-verify-flag (Script failed an OP_VERIFY operation)");

    // Should pass without SCRIPT_VERIFY_TAPROOT
    bool ret_without_taproot = CheckInputScripts(tx5, state, inputs, flags, /* cacheSigStore */ false, /* cacheFullScriptStore */ false, txdata, nullptr);
    BOOST_CHECK(ret_without_taproot);
    }

    {
    // Should pass
    TxValidationState state;
    CMutableTransaction mtx6;
    BOOST_CHECK(DecodeHexTx(mtx6, test_txns[5].get_str()));
    CTransaction tx6(mtx6);

    LOCK(cs_main);
    PrecomputedTransactionData txdata;
    bool ret = CheckInputScripts(tx6, state, inputs, flags_with_taproot, /* cacheSigStore */ false, /* cacheFullScriptStore */ false, txdata, nullptr);
    BOOST_CHECK(ret);
    }
    }
}

BOOST_AUTO_TEST_SUITE_END()
