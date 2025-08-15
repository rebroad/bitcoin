// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "anyone_can_spend_handler.h"
#include <validation.h>
#include <util/strencodings.h>
#include <util/translation.h>
#include <util/moneystr.h>
#include <wallet/spend.h>
#include <wallet/coincontrol.h>
#include <wallet/load.h>
#include <logging.h>
#include <txmempool.h>
#include <net_processing.h>
#include <validationinterface.h>
#include <primitives/transaction.h>
#include <wallet/wallet.h>
#include <wallet/load.h>
#include <node/context.h>
#include <script/interpreter.h>
#include <node/ui_interface.h>

AnyoneCanSpendHandler::AnyoneCanSpendHandler()
    : m_wallet_context(nullptr), m_auto_spend(false)
{
    LogPrintf("AnyoneCanSpendHandler: Constructor called\n");

    // Register with the validation interface
    RegisterValidationInterface(this);
    LogPrintf("AnyoneCanSpendHandler: Registered with validation interface\n");

    // Start heartbeat thread
    StartHeartbeat();
    LogPrintf("AnyoneCanSpendHandler: Started heartbeat thread\n");
}

AnyoneCanSpendHandler::~AnyoneCanSpendHandler()
{
    // Stop heartbeat thread
    StopHeartbeat();

    // Unregister from the validation interface
    UnregisterValidationInterface(this);
}

void AnyoneCanSpendHandler::Initialize(const std::string& destination_address,
                                      wallet::WalletContext* wallet_context,
                                      bool auto_spend)
{
    LogPrintf("AnyoneCanSpendHandler: Initialize() called with destination=%s, wallet_context=%p, auto_spend=%s\n",
              destination_address, (void*)wallet_context, auto_spend ? "true" : "false");

    m_destination_address = destination_address;
    m_wallet_context = wallet_context;
    m_auto_spend = auto_spend;

    LogPrintf("AnyoneCanSpendHandler: Initialized with destination %s, auto_spend=%s\n",
              destination_address, auto_spend ? "true" : "false");

    // Create the "Anyone" wallet immediately so it appears in the GUI
    if (auto_spend && wallet_context) {
        LogPrintf("AnyoneCanSpendHandler: Creating 'Anyone' wallet during initialization\n");
        GetAnyoneWallet();
    }
}

void AnyoneCanSpendHandler::TransactionAddedToMempool(const CTransactionRef& tx, uint64_t mempool_sequence)
{
    // Track all mempool transactions for evaluation statistics
    {
        std::lock_guard<std::mutex> lock(m_stats_mutex);
        m_stats.transactions_evaluated_mempool++;
    }
}

void AnyoneCanSpendHandler::AnyoneCanSpendTransactionAddedToMempool(const CTransactionRef& tx, uint64_t mempool_sequence)
{
    LogPrintf("AnyoneCanSpendHandler: AnyoneCanSpendTransactionAddedToMempool called for tx %s\n", tx->GetHash().ToString());

    if (!m_auto_spend || !m_wallet_context) {
        LogPrintf("AnyoneCanSpendHandler: Skipping transaction - auto_spend=%s, wallet_context=%p\n",
                  m_auto_spend ? "true" : "false", (void*)m_wallet_context);
        return;
    }

    // Update anyone-can-spend detection counter
    {
        std::lock_guard<std::mutex> lock(m_stats_mutex);
        m_stats.outputs_detected++;
    }

    LogPrintf("AnyoneCanSpendHandler: Detected transaction %s with anyone can spend outputs\n",
              tx->GetHash().ToString());

    // Process the outputs (add to wallet and optionally spend)
    auto tx_hash_opt = ProcessAnyoneCanSpendOutputs(tx);

    if (tx_hash_opt.has_value()) {
        LogPrintf("AnyoneCanSpendHandler: Created spending transaction %s for tx %s\n",
                  tx_hash_opt.value().ToString(), tx->GetHash().ToString());

        // Update statistics
        std::lock_guard<std::mutex> lock(m_stats_mutex);
        m_stats.outputs_spent++;
        m_stats.total_amount_spent += tx->GetValueOut();
    }
}

std::shared_ptr<wallet::CWallet> AnyoneCanSpendHandler::GetAnyoneWallet()
{
    LogPrintf("AnyoneCanSpendHandler: GetAnyoneWallet() called\n");

    if (m_anyone_wallet) {
        LogPrintf("AnyoneCanSpendHandler: Returning existing wallet instance\n");
        return m_anyone_wallet;
    }

    if (!m_wallet_context) {
        LogPrintf("AnyoneCanSpendHandler: No wallet context available\n");
        return nullptr;
    }

    LogPrintf("AnyoneCanSpendHandler: Wallet context available, attempting to get/create wallet\n");

    try {
        LogPrintf("AnyoneCanSpendHandler: Attempting to load existing 'Anyone' wallet\n");
        // Try to load existing "Anyone" wallet
        m_anyone_wallet = GetWallet(*m_wallet_context, "Anyone");
        if (m_anyone_wallet) {
            LogPrintf("AnyoneCanSpendHandler: Successfully loaded existing 'Anyone' wallet\n");
            return m_anyone_wallet;
        }

        LogPrintf("AnyoneCanSpendHandler: No existing 'Anyone' wallet found, creating new one\n");
        // Create new "Anyone" wallet
        bilingual_str error;
        std::vector<bilingual_str> warnings;
        wallet::DatabaseOptions options;
        wallet::DatabaseStatus status;
        options.require_create = true;
        options.create_flags = 0;

        LogPrintf("AnyoneCanSpendHandler: Calling CreateWallet for 'Anyone' wallet\n");
        m_anyone_wallet = CreateWallet(*m_wallet_context, "Anyone", true, options, status, error, warnings);
        if (!m_anyone_wallet) {
            LogPrintf("AnyoneCanSpendHandler: Failed to create 'Anyone' wallet: %s\n", error.original);
            return nullptr;
        }

        LogPrintf("AnyoneCanSpendHandler: Successfully created new 'Anyone' wallet\n");

        // Add the wallet to the wallet context so it appears in the GUI
        AddWallet(*m_wallet_context, m_anyone_wallet);

        return m_anyone_wallet;

    } catch (const std::exception& e) {
        LogPrintf("AnyoneCanSpendHandler: Error getting 'Anyone' wallet: %s\n", e.what());
        return nullptr;
    }
}

std::optional<uint256> AnyoneCanSpendHandler::ProcessAnyoneCanSpendOutputs(const CTransactionRef& tx)
{
    auto wallet = GetAnyoneWallet();
    if (!wallet) {
        LogPrintf("AnyoneCanSpendHandler: Could not get 'Anyone' wallet\n");
        return std::nullopt;
    }

    // Collect all "anyone can spend" outputs from the transaction
    std::vector<std::pair<COutPoint, std::pair<CAmount, CScript>>> anyone_can_spend_outputs;
    CAmount total_amount = 0;

    for (size_t i = 0; i < tx->vout.size(); i++) {
        const CTxOut& txout = tx->vout[i];

        auto script_sig = FindWorkingScriptSig(txout.scriptPubKey);
        if (script_sig.has_value()) {
            COutPoint outpoint(tx->GetHash(), i);

            LogPrintf("AnyoneCanSpendHandler: Found anyone can spend output %s:%d, amount %s\n",
                      outpoint.hash.ToString(), outpoint.n, FormatMoney(txout.nValue));

            anyone_can_spend_outputs.push_back({outpoint, {txout.nValue, txout.scriptPubKey}});
            total_amount += txout.nValue;
        }
    }

    // If no "anyone can spend" outputs found, return early
    if (anyone_can_spend_outputs.empty()) {
        return std::nullopt;
    }

    // Update detection statistics
    std::lock_guard<std::mutex> lock(m_stats_mutex);
    m_stats.outputs_detected++;
    m_stats.total_amount_detected += total_amount;

    // Add the transaction to the wallet as if it was received
    // This will make the outputs available for spending
    wallet->AddToWallet(tx, wallet::TxStateInactive{});

    LogPrintf("AnyoneCanSpendHandler: Added transaction %s to 'Anyone' wallet with %zu anyone can spend outputs\n",
              tx->GetHash().ToString(), anyone_can_spend_outputs.size());

    // If auto-spend is enabled, create a spending transaction
    if (m_auto_spend) {
        return CreateSpendTransactionFromWallet(wallet, anyone_can_spend_outputs);
    }

    return std::nullopt;
}

std::optional<uint256> AnyoneCanSpendHandler::CreateSpendTransactionFromWallet(
    std::shared_ptr<wallet::CWallet> wallet,
    const std::vector<std::pair<COutPoint, std::pair<CAmount, CScript>>>& outputs)
{
    if (!wallet || outputs.empty()) {
        return std::nullopt;
    }

    try {
        // Get destination address from configuration if not set
        std::string dest_address = m_destination_address;
        if (dest_address.empty()) {
            dest_address = GetDestinationAddressFromConfig();
            if (dest_address.empty()) {
                LogPrintf("AnyoneCanSpendHandler: No destination address configured\n");
                return std::nullopt;
            }
        }

        // Create destination for the output
        CTxDestination dest = DecodeDestination(dest_address);
        if (std::holds_alternative<CNoDestination>(dest)) {
            LogPrintf("AnyoneCanSpendHandler: Invalid destination address %s\n", dest_address);
            return std::nullopt;
        }

        // Calculate total amount
        CAmount total_amount = 0;
        for (const auto& output : outputs) {
            total_amount += output.second.first;
        }

        // Create coin control to specify the exact outputs to spend
        wallet::CCoinControl coin_control;
        for (const auto& output : outputs) {
            coin_control.Select(output.first);
        }

        // Create recipient
        std::vector<wallet::CRecipient> recipients;
        wallet::CRecipient recipient{GetScriptForDestination(dest), total_amount, false};
        recipients.push_back(recipient);

        // Create the transaction using wallet's CreateTransaction method
        CTransactionRef tx_new;
        CAmount fee;
        int change_pos = -1;
        bilingual_str error;
        FeeCalculation fee_calc_out;

        if (!CreateTransaction(*wallet, recipients, tx_new, fee, change_pos, error, coin_control, fee_calc_out, true)) {
            LogPrintf("AnyoneCanSpendHandler: Failed to create transaction: %s\n", error.original);
            return std::nullopt;
        }

        // Commit the transaction to the wallet
        wallet->CommitTransaction(tx_new, {}, {});

        LogPrintf("AnyoneCanSpendHandler: Created and committed spending transaction %s for %zu outputs (total: %s, fee: %s)\n",
                  tx_new->GetHash().ToString(), outputs.size(), FormatMoney(total_amount), FormatMoney(fee));

        return tx_new->GetHash();

    } catch (const std::exception& e) {
        LogPrintf("AnyoneCanSpendHandler: Error creating spending transaction: %s\n", e.what());
        return std::nullopt;
    }
}

std::optional<CScript> AnyoneCanSpendHandler::FindWorkingScriptSig(const CScript& script) const
{
    // Test with different script signatures to see if the script can be spent
    std::vector<CScript> test_script_sigs = {
        CScript(),                    // Empty script signature
        CScript() << OP_1,           // Push 1
        CScript() << OP_0,           // Push 0
        CScript() << OP_1 << OP_2,   // Push multiple values
        CScript() << OP_0 << OP_0,   // Push multiple zeros
        CScript() << OP_1 << OP_DROP, // Push 1, then drop it
        CScript() << OP_1 << OP_1,   // Push two 1s
        CScript() << OP_0 << OP_1,   // Push 0, then 1
    };

    // Use Bitcoin Core's actual script execution engine
    // Return the first working script signature
    for (const auto& script_sig : test_script_sigs) {
        if (TestScriptExecution(script_sig, script)) {
            return script_sig; // Return the working input
        }
    }

    return std::nullopt; // No working input found
}

void AnyoneCanSpendHandler::BlockConnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex)
{
    LogPrintf("AnyoneCanSpendHandler: BlockConnected called for block %d\n", pindex->nHeight);

    if (!m_auto_spend || !m_wallet_context) {
        LogPrintf("AnyoneCanSpendHandler: Skipping block - auto_spend=%s, wallet_context=%p\n",
                  m_auto_spend ? "true" : "false", (void*)m_wallet_context);
        return;
    }

    // Update block transaction counter
    {
        std::lock_guard<std::mutex> lock(m_stats_mutex);
        m_stats.transactions_evaluated_blocks += block->vtx.size();
    }

    LogPrintf("AnyoneCanSpendHandler: BlockConnected - Evaluating %zu transactions in block %d\n",
              block->vtx.size(), pindex->nHeight);

    // Process each transaction in the block
    for (const auto& tx : block->vtx) {
        // Skip coinbase transaction
        if (tx->IsCoinBase()) {
            continue;
        }

        // Check for anyone-can-spend outputs
        for (size_t i = 0; i < tx->vout.size(); i++) {
            const CTxOut& txout = tx->vout[i];

            auto script_sig = FindWorkingScriptSig(txout.scriptPubKey);
            if (script_sig.has_value()) {
                COutPoint outpoint(tx->GetHash(), i);
                LogPrintf("AnyoneCanSpendHandler: Found anyone can spend output %s:%d in block %d, amount %s\n",
                          outpoint.hash.ToString(), outpoint.n, pindex->nHeight, FormatMoney(txout.nValue));
            }
        }
    }
}

bool AnyoneCanSpendHandler::TestScriptExecution(const CScript& script_sig, const CScript& script_pub_key) const
{
    // Use Bitcoin Core's VerifyScript function with a dummy signature checker
    ScriptError serror;

    // Create a dummy signature checker that always returns true for signature checks
    // This allows us to test script logic without needing real signatures
    class DummySignatureChecker : public BaseSignatureChecker {
    public:
        bool CheckECDSASignature(const std::vector<unsigned char>& scriptSig,
                                const std::vector<unsigned char>& vchPubKey,
                                const CScript& scriptCode,
                                SigVersion sigversion) const override {
            return true; // Always return true for signature checks
        }

        bool CheckSchnorrSignature(Span<const unsigned char> sig,
                                  Span<const unsigned char> pubkey,
                                  SigVersion sigversion,
                                  ScriptExecutionData& execdata,
                                  ScriptError* serror) const override {
            return true; // Always return true for signature checks
        }

        bool CheckLockTime(const CScriptNum& nLockTime) const override {
            return true; // Always return true for lock time checks
        }

        bool CheckSequence(const CScriptNum& nSequence) const override {
            return true; // Always return true for sequence checks
        }
    };

    DummySignatureChecker checker;

    // Use Bitcoin Core's VerifyScript function
    return VerifyScript(script_sig, script_pub_key, nullptr,
                       STANDARD_SCRIPT_VERIFY_FLAGS, checker, &serror);
}

std::string AnyoneCanSpendHandler::GetDestinationAddressFromConfig() const
{
    // Get destination address from bitcoin.conf
    return gArgs.GetArg("-anyonecanspenddestination", "");
}

void AnyoneCanSpendHandler::StartHeartbeat()
{
    if (m_heartbeat_running.exchange(true)) {
        return; // Already running
    }

    m_heartbeat_thread = std::thread([this]() {
        while (m_heartbeat_running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(5));

            std::lock_guard<std::mutex> lock(m_stats_mutex);
            LogPrintf("AnyoneCanSpendHandler: Heartbeat - Transactions evaluated: %u blocks, %u mempool, Anyone-can-spend outputs detected: %u, Outputs spent: %u\n",
                     m_stats.transactions_evaluated_blocks, m_stats.transactions_evaluated_mempool,
                     m_stats.outputs_detected, m_stats.outputs_spent);
        }
    });
}

void AnyoneCanSpendHandler::StopHeartbeat()
{
    if (!m_heartbeat_running.exchange(false)) {
        return; // Not running
    }

    if (m_heartbeat_thread.joinable()) {
        m_heartbeat_thread.join();
    }
}
