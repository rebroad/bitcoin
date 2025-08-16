// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "anyone_can_spend_handler.h"
#include <validation.h>
#include <util/strencodings.h>
#include <shutdown.h>
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

void AnyoneCanSpendHandler::AnyoneCanSpendTransactionAddedToMempool(const CTransactionRef& tx, uint64_t mempool_sequence, const std::vector<std::pair<size_t, CScript>>& anyone_can_spend_outputs)
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

    LogPrintf("AnyoneCanSpendHandler: Detected transaction %s with %zu anyone can spend outputs\n",
              tx->GetHash().ToString(), anyone_can_spend_outputs.size());

    // Process the outputs (add to wallet and optionally spend)
    auto tx_hash_opt = ProcessAnyoneCanSpendOutputs(tx, anyone_can_spend_outputs, wallet::TxStateInMempool{});

    if (tx_hash_opt.has_value()) {
        LogPrintf("AnyoneCanSpendHandler: Created spending transaction %s for tx %s\n",
                  tx_hash_opt.value().ToString(), tx->GetHash().ToString());

        // Update statistics
        std::lock_guard<std::mutex> lock(m_stats_mutex);
        m_stats.outputs_spent++;
        m_stats.total_amount_spent += tx->GetValueOut();
    }
}

void AnyoneCanSpendHandler::AnyoneCanSpendTransactionInBlock(const CTransactionRef& tx, int block_height, const std::vector<std::pair<size_t, CScript>>& anyone_can_spend_outputs)
{
    LogPrintf("AnyoneCanSpendHandler: AnyoneCanSpendTransactionInBlock called for tx %s in block %d with %zu outputs\n",
              tx->GetHash().ToString(), block_height, anyone_can_spend_outputs.size());

    if (!m_auto_spend || !m_wallet_context) {
        LogPrintf("AnyoneCanSpendHandler: Skipping transaction - auto_spend=%s, wallet_context=%p\n",
                  m_auto_spend ? "true" : "false", (void*)m_wallet_context);
        return;
    }

    // Update block transaction counter
    {
        std::lock_guard<std::mutex> lock(m_stats_mutex);
        m_stats.transactions_evaluated_blocks++;
    }

    // Process each anyone-can-spend output
    for (const auto& [output_index, script_sig] : anyone_can_spend_outputs) {
        const CTxOut& txout = tx->vout[output_index];
        COutPoint outpoint(tx->GetHash(), output_index);

        LogPrintf("AnyoneCanSpendHandler: Found anyone can spend output %s:%d in block %d, amount %s\n",
                  outpoint.hash.ToString(), outpoint.n, block_height, FormatMoney(txout.nValue));

        // Update detection statistics
        {
            std::lock_guard<std::mutex> lock(m_stats_mutex);
            m_stats.outputs_detected++;
            m_stats.total_amount_detected += txout.nValue;
        }
    }

    // Process the outputs (add to wallet and optionally spend)
    auto tx_hash_opt = ProcessAnyoneCanSpendOutputs(tx, anyone_can_spend_outputs, wallet::TxStateInMempool{});

    if (tx_hash_opt.has_value()) {
        LogPrintf("AnyoneCanSpendHandler: Created spending transaction %s for tx %s\n",
                  tx_hash_opt.value().ToString(), tx->GetHash().ToString());

        // Update statistics
        std::lock_guard<std::mutex> lock(m_stats_mutex);
        m_stats.outputs_spent++;
        m_stats.total_amount_spent += tx->GetValueOut();
    }
}

void AnyoneCanSpendHandler::BlockConnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex)
{
    // Note: AnyoneCanSpend detection is now handled by AnyoneCanSpendTransactionInBlock signal
    // which is emitted from the validation layer, eliminating duplicate processing
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
        // Try to load existing wallet first, then create if it doesn't exist
        bilingual_str error;
        std::vector<bilingual_str> warnings;
        wallet::DatabaseOptions options;
        wallet::DatabaseStatus status;
        options.require_existing = true; // Try to load existing first
        options.verify = false; // Don't verify for now

        LogPrintf("AnyoneCanSpendHandler: Attempting to load existing 'Anyone' wallet database\n");
        m_anyone_wallet = LoadWallet(*m_wallet_context, "Anyone", true, options, status, error, warnings);
        if (!m_anyone_wallet) {
            LogPrintf("AnyoneCanSpendHandler: Failed to load existing wallet, trying to create new one: %s\n", error.original);

            // Try to create new wallet
            options.require_existing = false;
            options.require_create = true;
            m_anyone_wallet = CreateWallet(*m_wallet_context, "Anyone", true, options, status, error, warnings);
            if (!m_anyone_wallet) {
                LogPrintf("AnyoneCanSpendHandler: Failed to create 'Anyone' wallet: %s\n", error.original);
                return nullptr;
            }
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

std::optional<uint256> AnyoneCanSpendHandler::ProcessAnyoneCanSpendOutputs(const CTransactionRef& tx, const std::vector<std::pair<size_t, CScript>>& anyone_can_spend_outputs, const wallet::TxState& state)
{
    auto wallet = GetAnyoneWallet();
    if (!wallet) {
        LogPrintf("AnyoneCanSpendHandler: Could not get 'Anyone' wallet\n");
        return std::nullopt;
    }

    // Convert the signal data to the format expected by CreateSpendTransactionFromWallet
    std::vector<std::pair<COutPoint, std::pair<CAmount, CScript>>> outputs_for_spending;
    CAmount total_amount = 0;

    for (const auto& [output_index, script_sig] : anyone_can_spend_outputs) {
        const CTxOut& txout = tx->vout[output_index];
        COutPoint outpoint(tx->GetHash(), output_index);

        LogPrintf("AnyoneCanSpendHandler: Processing anyone can spend output %s:%d, amount %s\n",
                  outpoint.hash.ToString(), outpoint.n, FormatMoney(txout.nValue));

        outputs_for_spending.push_back({outpoint, {txout.nValue, txout.scriptPubKey}});
        total_amount += txout.nValue;
    }

    // If no "anyone can spend" outputs found, return early
    if (outputs_for_spending.empty()) {
        return std::nullopt;
    }

    // Add the specific anyone-can-spend outputs to the wallet
    // This makes the wallet recognize these outputs as "mine" so they show in the balance
    // They will be marked as ISMINE_ANYONE and can be spent by our custom logic
    {
        auto spk_man = wallet->GetLegacyScriptPubKeyMan();
        LOCK(spk_man->cs_KeyStore);
        for (const auto& [output_index, script_sig] : anyone_can_spend_outputs) {
            const CTxOut& txout = tx->vout[output_index];
            // Add the scriptPubKey as anyone-can-spend
            spk_man->AddAnyoneCanSpend(txout.scriptPubKey);
        }
    }

    // Add the transaction to the wallet with the appropriate state
    // This will make the outputs available for spending and show in the balance
    // The wallet will automatically update the state when the transaction is included in a block
    wallet->AddToWallet(tx, state);

    LogPrintf("AnyoneCanSpendHandler: Added transaction %s to 'Anyone' wallet with %zu anyone can spend outputs\n",
              tx->GetHash().ToString(), outputs_for_spending.size());

    // If auto-spend is enabled, create a spending transaction
    if (m_auto_spend) {
        return CreateSpendTransactionFromWallet(wallet, outputs_for_spending);
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
            dest_address = gArgs.GetArg("-anyonecanspenddestination", "");
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

        // Create coin control to specify the exact outputs to spend and get fee estimation
        wallet::CCoinControl coin_control;
        for (const auto& output : outputs) {
            coin_control.Select(output.first);
        }

        // Set coin control to target next block inclusion
        coin_control.m_confirm_target = 1; // Target next block

        // Create a custom transaction for anyone-can-spend outputs
        // Since these are ISMINE_ANYONE, we can't use the wallet's CreateTransaction method
        // Instead, we'll manually construct the transaction

        // Get fee estimation using the coin control
        FeeCalculation fee_calc;
        CAmount estimated_fee = wallet->GetMinimumFee(coin_control, fee_calc);
        if (estimated_fee == 0) {
            // Fallback to a reasonable fee if estimation fails
            estimated_fee = 1000; // 1000 sats as a reasonable fallback fee
        }
        CAmount amount_to_send = total_amount - estimated_fee;

        if (amount_to_send <= 0) {
            LogPrintf("AnyoneCanSpendHandler: Amount too small to spend after fee deduction\n");
            return std::nullopt;
        }

        // Create the transaction manually
        CMutableTransaction mtx;

        // Add inputs (anyone-can-spend outputs)
        for (const auto& output : outputs) {
            CTxIn txin(output.first);
            // For anyone-can-spend outputs, we can use an empty scriptSig
            txin.scriptSig = CScript();
            mtx.vin.push_back(txin);
        }

        // Add output
        mtx.vout.push_back(CTxOut(amount_to_send, GetScriptForDestination(dest)));

        // Create the transaction
        CTransactionRef tx_new = MakeTransactionRef(mtx);

        // Commit the transaction to the wallet
        wallet->CommitTransaction(tx_new, {}, {});

        LogPrintf("AnyoneCanSpendHandler: Created and committed spending transaction %s for %zu outputs (total: %s, amount sent: %s, fee: %s)\n",
                  tx_new->GetHash().ToString(), outputs.size(), FormatMoney(total_amount), FormatMoney(amount_to_send), FormatMoney(estimated_fee));

        return tx_new->GetHash();

    } catch (const std::exception& e) {
        LogPrintf("AnyoneCanSpendHandler: Error creating spending transaction: %s\n", e.what());
        return std::nullopt;
    }
}

void AnyoneCanSpendHandler::StartHeartbeat()
{
    if (m_heartbeat_running.exchange(true)) {
        return; // Already running
    }

    m_heartbeat_thread = std::thread([this]() {
        while (m_heartbeat_running.load() && !ShutdownRequested()) {
            // Sleep in shorter intervals to be more responsive to shutdown
            for (int i = 0; i < 5 && !ShutdownRequested(); i++) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            if (!ShutdownRequested()) {
                std::lock_guard<std::mutex> lock(m_stats_mutex);
                LogPrintf("AnyoneCanSpendHandler: Heartbeat - Transactions evaluated: %u blocks, %u mempool, Anyone-can-spend outputs detected: %u, Outputs spent: %u\n",
                         m_stats.transactions_evaluated_blocks, m_stats.transactions_evaluated_mempool,
                         m_stats.outputs_detected, m_stats.outputs_spent);
            }
        }
        LogPrintf("AnyoneCanSpendHandler: Heartbeat thread shutting down\n");
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
