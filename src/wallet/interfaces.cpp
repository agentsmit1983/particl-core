// Copyright (c) 2018-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/wallet.h>

#include <consensus/amount.h>
#include <interfaces/chain.h>
#include <interfaces/handler.h>
#include <policy/fees.h>
#include <primitives/transaction.h>
#include <rpc/server.h>
#include <script/standard.h>
#include <support/allocators/secure.h>
#include <sync.h>
#include <uint256.h>
#include <util/check.h>
#include <util/system.h>
#include <util/translation.h>
#include <util/ui_change_type.h>
#include <wallet/context.h>
#include <wallet/feebumper.h>
#include <wallet/fees.h>
#include <wallet/ismine.h>
#include <wallet/load.h>
#include <wallet/receive.h>
#include <wallet/rpcwallet.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <wallet/walletutil.h>
#include <wallet/hdwallet.h>
#include <wallet/rpchdwallet.h>
#include <key/extkey.h>
#include <smsg/smessage.h>
#if ENABLE_USBDEVICE
#include <usbdevice/rpcusbdevice.h>
#endif


void LockWallet(CWallet* pWallet)
{
    LOCK(pWallet->cs_wallet);
    pWallet->nRelockTime = 0;
    pWallet->Lock();
}

using interfaces::Chain;
using interfaces::FoundBlock;
using interfaces::Handler;
using interfaces::MakeHandler;
using interfaces::Wallet;
using interfaces::WalletAddress;
using interfaces::WalletBalances;
using interfaces::WalletClient;
using interfaces::WalletOrderForm;
using interfaces::WalletTx;
using interfaces::WalletTxOut;
using interfaces::WalletTxStatus;
using interfaces::WalletValueMap;

namespace wallet {
namespace {
//! Construct wallet tx struct.
WalletTx MakeWalletTx(CWallet& wallet, const CWalletTx& wtx)
{
    LOCK(wallet.cs_wallet);
    WalletTx result;
    result.tx = wtx.tx;
    result.txin_is_mine.reserve(wtx.tx->vin.size());
    for (const auto& txin : wtx.tx->vin) {
        result.txin_is_mine.emplace_back(InputIsMine(wallet, txin));
    }
    if (wtx.tx->IsParticlVersion()) {
        size_t nv = wtx.tx->GetNumVOuts();
        result.txout_is_mine.reserve(nv);
        result.txout_address.reserve(nv);
        result.txout_address_is_mine.reserve(nv);

        for (const auto& txout : wtx.tx->vpout) {
            // Mark data outputs as owned so txn will show as payment to self
            result.txout_is_mine.emplace_back(
                txout->IsStandardOutput() ? wallet.IsMine(txout.get()) : ISMINE_SPENDABLE);
            result.txout_address.emplace_back();

            if (txout->IsStandardOutput()) {
                result.txout_address_is_mine.emplace_back(ExtractDestination(*txout->GetPScriptPubKey(), result.txout_address.back()) ?
                                                          wallet.IsMine(result.txout_address.back()) :
                                                          ISMINE_NO);
            } else {
                result.txout_address_is_mine.emplace_back(ISMINE_NO);
            }
        }
    } else {
        result.txout_is_mine.reserve(wtx.tx->vout.size());
        result.txout_address.reserve(wtx.tx->vout.size());
        result.txout_address_is_mine.reserve(wtx.tx->vout.size());
        for (const auto& txout : wtx.tx->vout) {
            result.txout_is_mine.emplace_back(wallet.IsMine(txout));
            result.txout_address.emplace_back();
            result.txout_address_is_mine.emplace_back(ExtractDestination(txout.scriptPubKey, result.txout_address.back()) ?
                                                          wallet.IsMine(result.txout_address.back()) :
                                                          ISMINE_NO);
        }
    }
    result.credit = CachedTxGetCredit(wallet, wtx, ISMINE_ALL);
    result.debit = CachedTxGetDebit(wallet, wtx, ISMINE_ALL);
    result.change = CachedTxGetChange(wallet, wtx);
    result.time = wtx.GetTxTime();
    result.value_map = wtx.mapValue;
    result.is_coinbase = wtx.IsCoinBase();
    result.is_coinstake = wtx.IsCoinStake();
    return result;
}

//! Construct wallet tx struct.
WalletTx MakeWalletTx(CHDWallet& wallet, MapRecords_t::const_iterator irtx)
{
    WalletTx result;
    result.is_record = true;
    result.irtx = irtx;
    result.time = irtx->second.GetTxTime();
    result.partWallet = &wallet;

    result.is_coinbase = false;
    result.is_coinstake = false;

    return result;
}

//! Construct wallet tx status struct.
WalletTxStatus MakeWalletTxStatus(const CWallet& wallet, const CWalletTx& wtx)
{
    WalletTxStatus result;
    result.block_height = wtx.m_confirm.block_height > 0 ? wtx.m_confirm.block_height : std::numeric_limits<int>::max();
    result.blocks_to_maturity = wallet.GetTxBlocksToMaturity(wtx);
    result.depth_in_main_chain = wallet.GetTxDepthInMainChain(wtx);
    result.time_received = wtx.nTimeReceived;
    result.lock_time = wtx.tx->nLockTime;
    result.is_final = wallet.chain().checkFinalTx(*wtx.tx);
    result.is_trusted = CachedTxIsTrusted(wallet, wtx);
    result.is_abandoned = wtx.isAbandoned();
    result.is_coinbase = wtx.IsCoinBase();
    result.is_in_main_chain = wallet.IsTxInMainChain(wtx);
    return result;
}

WalletTxStatus MakeWalletTxStatus(CHDWallet &wallet, const uint256 &hash, const CTransactionRecord &rtx) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    WalletTxStatus result;
    result.block_height = wallet.chain().getBlockHeight(rtx.blockHash).value_or(std::numeric_limits<int>::max());
    result.blocks_to_maturity = 0;
    result.depth_in_main_chain = wallet.GetDepthInMainChain(rtx);
    result.time_received = rtx.GetTxTime();
    result.lock_time = 0; // TODO
    result.is_final = true; // TODO
    result.is_trusted = wallet.IsTrusted(hash, rtx);
    result.is_abandoned = rtx.IsAbandoned();
    result.is_coinbase = false;
    result.is_in_main_chain = result.depth_in_main_chain > 0;
    return result;
}

//! Construct wallet TxOut struct.
WalletTxOut MakeWalletTxOut(const CWallet& wallet,
    const CWalletTx& wtx,
    int n,
    int depth) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    WalletTxOut result;
    result.txout.nValue = wtx.tx->vpout[n]->GetValue();
    result.txout.scriptPubKey = *wtx.tx->vpout[n]->GetPScriptPubKey();
    result.time = wtx.GetTxTime();
    result.depth_in_main_chain = depth;
    result.is_spent = wallet.IsSpent(wtx.GetHash(), n);
    return result;
}

WalletTxOut MakeWalletTxOut(CHDWallet &wallet,
    const uint256 &hash, const CTransactionRecord &rtx, int n, int depth) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    WalletTxOut result;
    const COutputRecord *oR = rtx.GetOutput(n);
    if (!oR) {
        return result;
    }
    result.txout.nValue = oR->nValue;
    result.txout.scriptPubKey = oR->scriptPubKey;
    result.time = rtx.GetTxTime();
    result.depth_in_main_chain = depth;
    result.is_spent = wallet.IsSpent(hash, n);
    return result;
}

class WalletImpl : public Wallet
{
public:
    explicit WalletImpl(WalletContext& context, const std::shared_ptr<CWallet>& wallet) : m_context(context), m_wallet(wallet)
    {
        if (::IsParticlWallet(wallet.get())) {
            m_wallet_part = GetParticlWallet(wallet.get());
        }
    }

    bool encryptWallet(const SecureString& wallet_passphrase) override
    {
        return m_wallet->EncryptWallet(wallet_passphrase);
    }
    bool isCrypted() override { return m_wallet->IsCrypted(); }
    bool lock() override { return m_wallet->Lock(); }
    bool unlock(const SecureString& wallet_passphrase, bool for_staking_only) override
    {
        if (!m_wallet->Unlock(wallet_passphrase)) {
            return false;
        }
        if (m_wallet_part) {
            LOCK(m_wallet_part->cs_wallet);
            m_wallet_part->fUnlockForStakingOnly = for_staking_only;
        }
        return true;
    }
    bool isLocked() override { return m_wallet->IsLocked(); }
    bool changeWalletPassphrase(const SecureString& old_wallet_passphrase,
        const SecureString& new_wallet_passphrase) override
    {
        return m_wallet->ChangeWalletPassphrase(old_wallet_passphrase, new_wallet_passphrase);
    }
    void abortRescan() override { m_wallet->AbortRescan(); }
    bool backupWallet(const std::string& filename) override { return m_wallet->BackupWallet(filename); }
    std::string getWalletName() override { return m_wallet->GetName(); }
    bool getNewDestination(const OutputType type, const std::string label, CTxDestination& dest) override
    {
        LOCK(m_wallet->cs_wallet);
        bilingual_str error;
        return m_wallet->GetNewDestination(type, label, dest, error);
    }
    bool getPubKey(const CScript& script, const CKeyID& address, CPubKey& pub_key) override
    {
        std::unique_ptr<SigningProvider> provider = m_wallet->GetSolvingProvider(script);
        if (provider) {
            return provider->GetPubKey(address, pub_key);
        }
        return false;
    }
    SigningResult signMessage(const std::string& message, const PKHash& pkhash, std::string& str_sig) override
    {
        return m_wallet->SignMessage(message, pkhash, str_sig);
    }
    SigningResult signMessage(const std::string& message, const CKeyID256& pkhash, std::string& str_sig) override
    {
        return m_wallet->SignMessage(message, pkhash, str_sig);
    }
    bool isSpendable(const CTxDestination& dest) override
    {
        LOCK(m_wallet->cs_wallet);
        return m_wallet->IsMine(dest) & ISMINE_SPENDABLE;
    }
    bool haveWatchOnly() override
    {
        auto spk_man = m_wallet->GetLegacyScriptPubKeyMan();
        if (spk_man) {
            return spk_man->HaveWatchOnly();
        }
        return false;
    };
    bool setAddressBook(const CTxDestination& dest, const std::string& name, const std::string& purpose) override
    {
        return m_wallet->SetAddressBook(dest, name, purpose);
    }
    bool delAddressBook(const CTxDestination& dest) override
    {
        return m_wallet->DelAddressBook(dest);
    }
    bool getAddress(const CTxDestination& dest,
        std::string* name,
        isminetype* is_mine,
        std::string* purpose) override
    {
        LOCK(m_wallet->cs_wallet);
        auto it = m_wallet->m_address_book.find(dest);
        if (it == m_wallet->m_address_book.end() || it->second.IsChange()) {
            return false;
        }
        if (name) {
            *name = it->second.GetLabel();
        }
        if (is_mine) {
            *is_mine = m_wallet->IsMine(dest);
        }
        if (purpose) {
            *purpose = it->second.purpose;
        }
        return true;
    }
    std::vector<WalletAddress> getAddresses() override
    {
        LOCK(m_wallet->cs_wallet);
        std::vector<WalletAddress> result;

        for (const auto& item : m_wallet->m_address_book) {
            if (item.second.IsChange()) continue;
            std::string str_path;
            if (item.second.vPath.size() > 1 &&
                PathToString(item.second.vPath, str_path, '\'', 1)) {
                str_path = "";
            }
            result.emplace_back(item.first, m_wallet->IsMine(item.first), item.second.GetLabel(), item.second.purpose, item.second.fBech32, str_path);
        }
        return result;
    }
    std::vector<std::string> getAddressReceiveRequests() override {
        LOCK(m_wallet->cs_wallet);
        return m_wallet->GetAddressReceiveRequests();
    }
    bool setAddressReceiveRequest(const CTxDestination& dest, const std::string& id, const std::string& value) override {
        LOCK(m_wallet->cs_wallet);
        WalletBatch batch{m_wallet->GetDatabase()};
        return m_wallet->SetAddressReceiveRequest(batch, dest, id, value);
    }
    bool displayAddress(const CTxDestination& dest) override
    {
        LOCK(m_wallet->cs_wallet);
        return m_wallet->DisplayAddress(dest);
    }
    bool lockCoin(const COutPoint& output, const bool write_to_db) override
    {
        LOCK(m_wallet->cs_wallet);
        std::unique_ptr<WalletBatch> batch = write_to_db ? std::make_unique<WalletBatch>(m_wallet->GetDatabase()) : nullptr;
        return m_wallet->LockCoin(output, batch.get());
    }
    bool unlockCoin(const COutPoint& output) override
    {
        LOCK(m_wallet->cs_wallet);
        std::unique_ptr<WalletBatch> batch = std::make_unique<WalletBatch>(m_wallet->GetDatabase());
        return m_wallet->UnlockCoin(output, batch.get());
    }
    bool isLockedCoin(const COutPoint& output) override
    {
        LOCK(m_wallet->cs_wallet);
        return m_wallet->IsLockedCoin(output.hash, output.n);
    }
    void listLockedCoins(std::vector<COutPoint>& outputs) override
    {
        LOCK(m_wallet->cs_wallet);
        return m_wallet->ListLockedCoins(outputs);
    }
    CTransactionRef createTransaction(const std::vector<CRecipient>& recipients,
        const CCoinControl& coin_control,
        bool sign,
        int& change_pos,
        CAmount& fee,
        bilingual_str& fail_reason) override
    {
        LOCK(m_wallet->cs_wallet);
        CTransactionRef tx;
        FeeCalculation fee_calc_out;
        if (!CreateTransaction(*m_wallet, recipients, tx, fee, change_pos,
                fail_reason, coin_control, fee_calc_out, sign)) {
            return {};
        }
        return tx;
    }
    void commitTransaction(CTransactionRef tx,
        WalletValueMap value_map,
        WalletOrderForm order_form) override
    {
        LOCK(m_wallet->cs_wallet);
        m_wallet->CommitTransaction(std::move(tx), std::move(value_map), std::move(order_form));
    }
    bool transactionCanBeAbandoned(const uint256& txid) override { return m_wallet->TransactionCanBeAbandoned(txid); }
    bool abandonTransaction(const uint256& txid) override
    {
        LOCK(m_wallet->cs_wallet);
        return m_wallet->AbandonTransaction(txid);
    }
    bool transactionCanBeBumped(const uint256& txid) override
    {
        return feebumper::TransactionCanBeBumped(*m_wallet.get(), txid);
    }
    bool createBumpTransaction(const uint256& txid,
        const CCoinControl& coin_control,
        std::vector<bilingual_str>& errors,
        CAmount& old_fee,
        CAmount& new_fee,
        CMutableTransaction& mtx) override
    {
        if (::IsParticlWallet(m_wallet.get())) {
            return feebumper::CreateTotalBumpTransaction(m_wallet.get(), txid, coin_control, errors, old_fee, new_fee, mtx) ==
                feebumper::Result::OK;
        } else {
            return feebumper::CreateRateBumpTransaction(*m_wallet.get(), txid, coin_control, errors, old_fee, new_fee, mtx) ==
                feebumper::Result::OK;
        }
    }
    bool signBumpTransaction(CMutableTransaction& mtx) override { return feebumper::SignTransaction(*m_wallet.get(), mtx); }
    bool commitBumpTransaction(const uint256& txid,
        CMutableTransaction&& mtx,
        std::vector<bilingual_str>& errors,
        uint256& bumped_txid) override
    {
        return feebumper::CommitTransaction(*m_wallet.get(), txid, std::move(mtx), errors, bumped_txid) ==
               feebumper::Result::OK;
    }
    CTransactionRef getTx(const uint256& txid) override
    {
        LOCK(m_wallet->cs_wallet);
        auto mi = m_wallet->mapWallet.find(txid);
        if (mi != m_wallet->mapWallet.end()) {
            return mi->second.tx;
        }
        return {};
    }
    WalletTx getWalletTx(const uint256& txid) override
    {
        LOCK(m_wallet->cs_wallet);
        auto mi = m_wallet->mapWallet.find(txid);
        if (mi != m_wallet->mapWallet.end()) {
            return MakeWalletTx(*m_wallet, mi->second);
        }

        if (m_wallet_part) {
            const auto mi = m_wallet_part->mapRecords.find(txid);
            if (mi != m_wallet_part->mapRecords.end()) {
                return MakeWalletTx(*m_wallet_part, mi);
            }
        }

        return {};
    }
    std::vector<WalletTx> getWalletTxs() override
    {
        LOCK(m_wallet->cs_wallet);
        std::vector<WalletTx> result;
        result.reserve(m_wallet->mapWallet.size());
        for (const auto& entry : m_wallet->mapWallet) {
            result.emplace_back(MakeWalletTx(*m_wallet, entry.second));
        }
        if (m_wallet_part) {
            for (auto mi = m_wallet_part->mapRecords.begin(); mi != m_wallet_part->mapRecords.end(); mi++) {
                result.emplace_back(MakeWalletTx(*m_wallet_part, mi));
            }
        }

        return result;
    }
    bool tryGetTxStatus(const uint256& txid,
        interfaces::WalletTxStatus& tx_status,
        int& num_blocks,
        int64_t& block_time) override
    {
        TRY_LOCK(m_wallet->cs_wallet, locked_wallet);
        if (!locked_wallet) {
            return false;
        }
        auto mi = m_wallet->mapWallet.find(txid);
        if (mi == m_wallet->mapWallet.end()) {
            if (m_wallet_part) {
                LOCK_ASSERTION(m_wallet_part->cs_wallet);
                auto mi = m_wallet_part->mapRecords.find(txid);
                if (mi != m_wallet_part->mapRecords.end()) {
                    num_blocks = m_wallet_part->chain().getHeight().value_or(-1);
                    tx_status = MakeWalletTxStatus(*m_wallet_part, mi->first, mi->second);
                    return true;
                }
            }
            return false;
        }
        num_blocks = m_wallet->GetLastBlockHeight();
        block_time = -1;
        CHECK_NONFATAL(m_wallet->chain().findBlock(m_wallet->GetLastBlockHash(), FoundBlock().time(block_time)));
        tx_status = MakeWalletTxStatus(*m_wallet, mi->second);
        return true;
    }
    WalletTx getWalletTxDetails(const uint256& txid,
        WalletTxStatus& tx_status,
        WalletOrderForm& order_form,
        bool& in_mempool,
        int& num_blocks) override
    {
        LOCK(m_wallet->cs_wallet);
        auto mi = m_wallet->mapWallet.find(txid);
        if (mi != m_wallet->mapWallet.end()) {
            num_blocks = m_wallet->GetLastBlockHeight();
            in_mempool = mi->second.InMempool();
            order_form = mi->second.vOrderForm;
            tx_status = MakeWalletTxStatus(*m_wallet, mi->second);
            return MakeWalletTx(*m_wallet, mi->second);
        }
        if (m_wallet_part) {
            LOCK_ASSERTION(m_wallet_part->cs_wallet);
            auto mi = m_wallet_part->mapRecords.find(txid);
            if (mi != m_wallet_part->mapRecords.end()) {
                num_blocks = m_wallet_part->chain().getHeight().value_or(-1);
                in_mempool = m_wallet_part->InMempool(mi->first);
                order_form = {};
                tx_status = MakeWalletTxStatus(*m_wallet_part, mi->first, mi->second);
                return MakeWalletTx(*m_wallet_part, mi);
            }
        }
        return {};
    }
    TransactionError fillPSBT(int sighash_type,
        bool sign,
        bool bip32derivs,
        size_t* n_signed,
        PartiallySignedTransaction& psbtx,
        bool& complete) override
    {
        return m_wallet->FillPSBT(psbtx, complete, sighash_type, sign, bip32derivs, n_signed);
    }
    WalletBalances getBalances() override
    {
        WalletBalances result;

        if (m_wallet_part) {
            CHDWalletBalances bal;
            if (!m_wallet_part->GetBalances(bal)) {
                return result;
            }

            result.balance = bal.nPart;
            result.balanceStaked = bal.nPartStaked;
            result.balanceBlind = bal.nBlind;
            result.balanceAnon = bal.nAnon;
            result.unconfirmed_balance = bal.nPartUnconf + bal.nBlindUnconf + bal.nAnonUnconf;
            result.immature_balance = bal.nPartImmature;
            result.immature_anon_balance = bal.nAnonImmature;
            result.have_watch_only = bal.nPartWatchOnly || bal.nPartWatchOnlyUnconf || bal.nPartWatchOnlyStaked;
            if (result.have_watch_only) {
                result.watch_only_balance = bal.nPartWatchOnly;
                result.unconfirmed_watch_only_balance = bal.nPartWatchOnlyUnconf;
                //result.immature_watch_only_balance = m_wallet.GetImmatureWatchOnlyBalance();
                result.balanceWatchStaked = bal.nPartWatchOnlyStaked;
            }

            return result;
        }

        const auto bal = GetBalance(*m_wallet);

        result.balance = bal.m_mine_trusted;
        result.unconfirmed_balance = bal.m_mine_untrusted_pending;
        result.immature_balance = bal.m_mine_immature;
        result.have_watch_only = haveWatchOnly();
        if (result.have_watch_only) {
            result.watch_only_balance = bal.m_watchonly_trusted;
            result.unconfirmed_watch_only_balance = bal.m_watchonly_untrusted_pending;
            result.immature_watch_only_balance = bal.m_watchonly_immature;
        }
        return result;
    }
    bool tryGetBalances(WalletBalances& balances, uint256& block_hash) override
    {
        TRY_LOCK(m_wallet->cs_wallet, locked_wallet);
        if (!locked_wallet) {
            return false;
        }
        block_hash = m_wallet->GetLastBlockHash();
        balances = getBalances();
        return true;
    }
    CAmount getBalance() override { return GetBalance(*m_wallet).m_mine_trusted; }
    CAmount getAvailableBalance(const CCoinControl& coin_control) override
    {
        return GetAvailableBalance(*m_wallet, &coin_control);
    }
    isminetype txinIsMine(const CTxIn& txin) override
    {
        LOCK(m_wallet->cs_wallet);
        return InputIsMine(*m_wallet, txin);
    }
    isminetype txoutIsMine(const CTxOut& txout) override
    {
        LOCK(m_wallet->cs_wallet);
        return m_wallet->IsMine(txout);
    }
    CAmount getDebit(const CTxIn& txin, isminefilter filter) override
    {
        LOCK(m_wallet->cs_wallet);
        return m_wallet->GetDebit(txin, filter);
    }
    CAmount getCredit(const CTxOut& txout, isminefilter filter) override
    {
        LOCK(m_wallet->cs_wallet);
        return OutputGetCredit(*m_wallet, txout, filter);
    }
    CoinsList listCoins(OutputTypes nType) override
    {
        CoinsList result;
        if (m_wallet_part
            && nType != OUTPUT_STANDARD) {
            LOCK(m_wallet_part->cs_wallet);
            for (const auto& entry : m_wallet_part->ListCoins(nType)) {
                auto& group = result[entry.first];
                for (const auto& coin : entry.second) {
                    group.emplace_back(
                        COutPoint(coin.rtx->first, coin.i), MakeWalletTxOut(*m_wallet_part, coin.txhash, coin.rtx->second, coin.i, coin.nDepth));
                }
            }
            return result;
        }

        LOCK(m_wallet->cs_wallet);
        for (const auto& entry : ListCoins(*m_wallet)) {
            auto& group = result[entry.first];
            for (const auto& coin : entry.second) {
                group.emplace_back(COutPoint(coin.tx->GetHash(), coin.i),
                    MakeWalletTxOut(*m_wallet, *coin.tx, coin.i, coin.nDepth));
            }
        }
        return result;
    }
    std::vector<WalletTxOut> getCoins(const std::vector<COutPoint>& outputs) override
    {
        LOCK(m_wallet->cs_wallet);
        std::vector<WalletTxOut> result;
        result.reserve(outputs.size());
        for (const auto& output : outputs) {
            result.emplace_back();
            auto it = m_wallet->mapWallet.find(output.hash);
            if (it != m_wallet->mapWallet.end()) {
                int depth = m_wallet->GetTxDepthInMainChain(it->second);
                if (depth >= 0) {
                    result.back() = MakeWalletTxOut(*m_wallet, it->second, output.n, depth);
                }
            } else
            if (m_wallet_part) {
                LOCK_ASSERTION(m_wallet_part->cs_wallet);
                const auto mi = m_wallet_part->mapRecords.find(output.hash);
                if (mi != m_wallet_part->mapRecords.end()) {
                    const auto &rtx = mi->second;
                    int depth = m_wallet_part->GetDepthInMainChain(rtx);
                    if (depth >= 0) {
                        result.back() = MakeWalletTxOut(*m_wallet_part, output.hash, rtx, output.n, depth);
                    }
                }
            }
        }
        return result;
    }
    CAmount getRequiredFee(unsigned int tx_bytes) override { return GetRequiredFee(*m_wallet, tx_bytes); }
    CAmount getMinimumFee(unsigned int tx_bytes,
        const CCoinControl& coin_control,
        int* returned_target,
        FeeReason* reason) override
    {
        FeeCalculation fee_calc;
        CAmount result;
        result = GetMinimumFee(*m_wallet, tx_bytes, coin_control, &fee_calc);
        if (returned_target) *returned_target = fee_calc.returnedTarget;
        if (reason) *reason = fee_calc.reason;
        return result;
    }
    unsigned int getConfirmTarget() override { return m_wallet->m_confirm_target; }
    bool hdEnabled() override { return m_wallet->IsHDEnabled(); }
    bool canGetAddresses() override { return m_wallet->CanGetAddresses(); }
    bool hasExternalSigner() override { return m_wallet->IsWalletFlagSet(WALLET_FLAG_EXTERNAL_SIGNER); }
    bool privateKeysDisabled() override { return m_wallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS); }
    OutputType getDefaultAddressType() override { return m_wallet->m_default_address_type; }
    CAmount getDefaultMaxTxFee() override { return m_wallet->m_default_max_tx_fee; }
    void remove() override
    {
        RemoveWallet(m_context, m_wallet, false /* load_on_start */);
        if (m_wallet_part) {
            ChainstateManager *chainman = m_wallet_part->chain().getChainman();
            smsgModule.WalletUnloaded(m_wallet_part);
            m_wallet_part = nullptr;
            if (chainman) {
                RestartStakingThreads(m_context, *chainman);
            }
        }
    }
    bool isLegacy() override { return m_wallet->IsLegacy(); }
    std::unique_ptr<Handler> handleUnload(UnloadFn fn) override
    {
        return MakeHandler(m_wallet->NotifyUnload.connect(fn));
    }
    std::unique_ptr<Handler> handleShowProgress(ShowProgressFn fn) override
    {
        return MakeHandler(m_wallet->ShowProgress.connect(fn));
    }
    std::unique_ptr<Handler> handleStatusChanged(StatusChangedFn fn) override
    {
        return MakeHandler(m_wallet->NotifyStatusChanged.connect([fn](CWallet*) { fn(); }));
    }
    std::unique_ptr<Handler> handleAddressBookChanged(AddressBookChangedFn fn) override
    {
        return MakeHandler(m_wallet->NotifyAddressBookChanged.connect(
            [fn](const CTxDestination& address, const std::string& label, bool is_mine,
                 const std::string& purpose, const std::string& path, ChangeType status) { fn(address, label, is_mine, purpose, path, status); }));
    }
    std::unique_ptr<Handler> handleTransactionChanged(TransactionChangedFn fn) override
    {
        return MakeHandler(m_wallet->NotifyTransactionChanged.connect(
            [fn](const uint256& txid, ChangeType status) { fn(txid, status); }));
    }
    std::unique_ptr<Handler> handleWatchOnlyChanged(WatchOnlyChangedFn fn) override
    {
        return MakeHandler(m_wallet->NotifyWatchonlyChanged.connect(fn));
    }
    std::unique_ptr<Handler> handleCanGetAddressesChanged(CanGetAddressesChangedFn fn) override
    {
        return MakeHandler(m_wallet->NotifyCanGetAddressesChanged.connect(fn));
    }
    CWallet* wallet() override { return m_wallet.get(); }

    WalletContext& m_context;
    std::shared_ptr<CWallet> m_wallet;

    std::unique_ptr<Handler> handleReservedBalanceChanged(ReservedBalanceChangedFn fn) override
    {
        return MakeHandler(m_wallet_part->NotifyReservedBalanceChanged.connect(fn));
    }

    bool IsParticlWallet() override
    {
        return m_wallet_part;
    }

    CAmount getReserveBalance() override
    {
        if (!m_wallet_part)
            return 0;
        return m_wallet_part->nReserveBalance;
    }

    bool ownDestination(const CTxDestination &dest) override
    {
        if (!m_wallet_part)
            return false;
        return m_wallet_part->HaveAddress(dest);
    }

    bool isUnlockForStakingOnlySet() override
    {
        if (!m_wallet_part)
            return false;
        return m_wallet_part->fUnlockForStakingOnly;
    }

    CAmount getAvailableAnonBalance(const CCoinControl& coin_control) override
    {
        if (!m_wallet_part)
            return 0;
        return m_wallet_part->GetAvailableAnonBalance(&coin_control);
    }

    CAmount getAvailableBlindBalance(const CCoinControl& coin_control) override
    {
        if (!m_wallet_part)
            return 0;
        return m_wallet_part->GetAvailableBlindBalance(&coin_control);
    }

    CHDWallet *getParticlWallet() override
    {
        return m_wallet_part;
    }

    bool setReserveBalance(CAmount nValue) override
    {
        if (!m_wallet_part)
            return false;
        return m_wallet_part->SetReserveBalance(nValue);
    }

    void lockWallet() override
    {
        if (!m_wallet_part)
            return;
        ::LockWallet(m_wallet_part);
    }

    bool setUnlockedForStaking() override
    {
        if (!m_wallet_part || m_wallet_part->IsLocked()) {
            return false;
        }
        m_wallet_part->fUnlockForStakingOnly = true;
        return true;
    }

    bool isDefaultAccountSet() override
    {
        return (m_wallet_part && !m_wallet_part->idDefaultAccount.IsNull());
    }

    bool isHardwareLinkedWallet() override
    {
        return (m_wallet_part && m_wallet_part->IsHardwareLinkedWallet());
    }

    CAmount getCredit(const CTxOutBase *txout, isminefilter filter) override
    {
        if (!m_wallet_part)
            return 0;
        LOCK(m_wallet_part->cs_wallet);
        return m_wallet_part->GetCredit(txout, filter);
    }

    isminetype txoutIsMine(const CTxOutBase *txout) override
    {
        if (!m_wallet_part)
            return ISMINE_NO;
        LOCK(m_wallet_part->cs_wallet);
        return m_wallet_part->IsMine(txout);
    }

    CHDWallet *m_wallet_part = nullptr;
};

class WalletClientImpl : public WalletClient
{
public:
    WalletClientImpl(Chain& chain, ArgsManager& args)
    {
        m_context.chain = &chain;
        m_context.args = &args;
    }
    ~WalletClientImpl() override { UnloadWallets(m_context); }

    //! ChainClient methods
    void registerRpcs() override
    {
        auto add_command = [&] (const CRPCCommand& command) {
            m_rpc_commands.emplace_back(command.category, command.name, [this, &command](const JSONRPCRequest& request, UniValue& result, bool last_handler) {
                JSONRPCRequest wallet_request = request;
                wallet_request.context = &m_context;
                return command.actor(wallet_request, result, last_handler);
            }, command.argNames, command.unique_id);
            m_rpc_handlers.emplace_back(m_context.chain->handleRpc(m_rpc_commands.back()));
        };

        for (const CRPCCommand& command : GetWalletRPCCommands()) {
            add_command(command);
        }
        for (const CRPCCommand& command : GetHDWalletRPCCommands()) {
            add_command(command);
        }
#if ENABLE_USBDEVICE
        for (const CRPCCommand& command : GetDeviceWalletRPCCommands()) {
            add_command(command);
        }
#endif
    }
    bool verify() override { return VerifyWallets(m_context); }
    bool load() override { return LoadWallets(m_context); }
    void start(CScheduler& scheduler) override { return StartWallets(m_context, scheduler); }
    void flush() override { return FlushWallets(m_context); }
    void stop() override { return StopWallets(m_context); }
    void setMockTime(int64_t time) override { return SetMockTime(time); }

    //! WalletClient methods
    std::unique_ptr<Wallet> createWallet(const std::string& name, const SecureString& passphrase, uint64_t wallet_creation_flags, bilingual_str& error, std::vector<bilingual_str>& warnings) override
    {
        std::shared_ptr<CWallet> wallet;
        DatabaseOptions options;
        DatabaseStatus status;
        options.require_create = true;
        options.create_flags = wallet_creation_flags;
        options.create_passphrase = passphrase;
        return MakeWallet(m_context, CreateWallet(m_context, name, true /* load_on_start */, options, status, error, warnings));
    }
    std::unique_ptr<Wallet> loadWallet(const std::string& name, bilingual_str& error, std::vector<bilingual_str>& warnings) override
    {
        DatabaseOptions options;
        DatabaseStatus status;
        options.require_existing = true;
        return MakeWallet(m_context, LoadWallet(m_context, name, true /* load_on_start */, options, status, error, warnings));
    }
    std::string getWalletDir() override
    {
        return fs::PathToString(GetWalletDir());
    }
    std::vector<std::string> listWalletDir() override
    {
        std::vector<std::string> paths;
        for (auto& path : ListDatabases(GetWalletDir())) {
            paths.push_back(fs::PathToString(path));
        }
        return paths;
    }
    std::vector<std::unique_ptr<Wallet>> getWallets() override
    {
        std::vector<std::unique_ptr<Wallet>> wallets;
        for (const auto& wallet : GetWallets(m_context)) {
            wallets.emplace_back(MakeWallet(m_context, wallet));
        }
        return wallets;
    }
    std::unique_ptr<Handler> handleLoadWallet(LoadWalletFn fn) override
    {
        return HandleLoadWallet(m_context, std::move(fn));
    }
    WalletContext* context() override  { return &m_context; }

    WalletContext m_context;
    const std::vector<std::string> m_wallet_filenames;
    std::vector<std::unique_ptr<Handler>> m_rpc_handlers;
    std::list<CRPCCommand> m_rpc_commands;
};
} // namespace
} // namespace wallet

namespace interfaces {
std::unique_ptr<Wallet> MakeWallet(WalletContext& context, const std::shared_ptr<CWallet>& wallet) { return wallet ? std::make_unique<wallet::WalletImpl>(context, wallet) : nullptr; }

std::unique_ptr<WalletClient> MakeWalletClient(Chain& chain, ArgsManager& args)
{
    return std::make_unique<wallet::WalletClientImpl>(chain, args);
}
} // namespace interfaces