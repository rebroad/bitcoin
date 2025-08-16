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
#include <wallet/fees.h>
#include <policy/fees.h>
#include <policy/policy.h>
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
    : m_handler_enabled(false), m_wallet_context(nullptr), m_auto_spend(false)
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
    LogPrintf("AnyoneCanSpendHandler: Destructor called\n");

    // Stop heartbeat thread
    StopHeartbeat();

    // Unregister from the validation interface
    UnregisterValidationInterface(this);

    LogPrintf("AnyoneCanSpendHandler: Destructor completed\n");
}

void AnyoneCanSpendHandler::Initialize(const std::string& destination_address,
                                      wallet::WalletContext* wallet_context,
                                      bool auto_spend)
{
    // Check for shutdown before initializing
    if (ShutdownRequested()) {
        LogPrintf("AnyoneCanSpendHandler: Skipping initialization during shutdown\n");
        return;
    }

    LogPrintf("AnyoneCanSpendHandler: Initialize() called with destination=%s, wallet_context=%p, auto_spend=%s\n",
              destination_address, (void*)wallet_context, auto_spend ? "true" : "false");

    m_destination_address = destination_address;
    m_wallet_context = wallet_context;
    m_auto_spend = auto_spend;

    // Cache the decoded destination address and output script
    if (!destination_address.empty() && IsValidDestinationString(destination_address)) {
        m_cached_destination = DecodeDestination(destination_address);
        m_cached_output_script = GetScriptForDestination(m_cached_destination);
        m_handler_enabled = true;
        LogPrintf("AnyoneCanSpendHandler: Cached valid destination address: %s\n", destination_address);

        // Initialize cached transaction sizes for fee calculation
        // Create dummy transactions for each script signature type to calculate sizes
        std::vector<CScript> test_script_sigs = {
            CScript(),           // Empty script signature (0 bytes)
            CScript() << OP_1,   // Push true value (1 byte)
            CScript() << OP_0,   // Push false value (1 byte)
        };

        for (size_t i = 0; i < test_script_sigs.size(); i++) {
            CMutableTransaction mtx_dummy;

            // Add input with the test script signature
            CTxIn txin(COutPoint(uint256::ZERO, 0));
            txin.scriptSig = test_script_sigs[i];
            mtx_dummy.vin.push_back(txin);

            // Add output with cached script
            mtx_dummy.vout.push_back(CTxOut(1000000, m_cached_output_script));

            // Calculate transaction weight and size
            CTransaction tx_dummy(mtx_dummy);
            int64_t tx_weight = GetTransactionWeight(tx_dummy);
            unsigned int tx_size = (tx_weight + WITNESS_SCALE_FACTOR - 1) / WITNESS_SCALE_FACTOR;

            // Store the size based on script signature type
            switch (i) {
                case 0: // Empty script
                    m_cached_tx_sizes.empty_script_size = tx_size;
                    break;
                case 1: // OP_1 script
                    m_cached_tx_sizes.op1_script_size = tx_size;
                    break;
                case 2: // OP_0 script
                    m_cached_tx_sizes.op0_script_size = tx_size;
                    break;
            }
        }

        LogPrintf("AnyoneCanSpendHandler: Cached transaction sizes - Empty: %u bytes, OP_1: %u bytes, OP_0: %u bytes\n",
                  m_cached_tx_sizes.empty_script_size, m_cached_tx_sizes.op1_script_size, m_cached_tx_sizes.op0_script_size);
    } else {
        // Disable handler if destination address is invalid
        m_cached_destination = PKHash(uint160());
        m_cached_output_script = GetScriptForDestination(m_cached_destination);
        m_handler_enabled = false;
        LogPrintf("AnyoneCanSpendHandler: Invalid destination address '%s' - handler disabled\n", destination_address);
    }

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
    // Check for shutdown before processing
    if (ShutdownRequested()) {
        return;
    }

    // Skip processing if handler is disabled
    if (!m_handler_enabled) {
        return;
    }

    // Track all mempool transactions for evaluation statistics
    {
        std::lock_guard<std::mutex> lock(m_stats_mutex);
        m_stats.transactions_evaluated_mempool++;
    }

    // Check for anyone-can-spend outputs in this transaction
    auto anyone_can_spend_outputs = FindAnyoneCanSpendOutputs(*tx);
    if (!anyone_can_spend_outputs.empty()) {
        LogPrintf("AnyoneCanSpendHandler: Found %zu anyone-can-spend outputs in mempool transaction %s\n",
                  anyone_can_spend_outputs.size(), tx->GetHash().ToString());

        // Process the anyone-can-spend outputs
        ProcessAnyoneCanSpendOutputs(tx, anyone_can_spend_outputs, wallet::TxStateInMempool{});
    }
}



void AnyoneCanSpendHandler::BlockConnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex)
{
    // Check for shutdown before processing
    if (ShutdownRequested()) {
        return;
    }

    // Skip processing if handler is disabled
    if (!m_handler_enabled) {
        return;
    }

    LogPrintf("AnyoneCanSpendHandler: BlockConnected called for block %d\n", pindex->nHeight);

    // Check for anyone-can-spend outputs in each transaction in the block
    for (const auto& tx : block->vtx) {
        auto anyone_can_spend_outputs = FindAnyoneCanSpendOutputs(*tx);
        if (!anyone_can_spend_outputs.empty()) {
            LogPrintf("AnyoneCanSpendHandler: Found %zu anyone-can-spend outputs in block transaction %s\n",
                      anyone_can_spend_outputs.size(), tx->GetHash().ToString());

            // Update block transaction counter
            {
                std::lock_guard<std::mutex> lock(m_stats_mutex);
                m_stats.transactions_evaluated_blocks++;
            }

            // Process the anyone-can-spend outputs
            ProcessAnyoneCanSpendOutputs(tx, anyone_can_spend_outputs, wallet::TxStateInMempool{});
        }
    }
}

std::shared_ptr<wallet::CWallet> AnyoneCanSpendHandler::GetAnyoneWallet()
{
    // Check for shutdown before processing
    if (ShutdownRequested()) {
        return nullptr;
    }

    if (m_anyone_wallet) {
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
    // Check for shutdown before processing
    if (ShutdownRequested()) {
        return std::nullopt;
    }

    auto wallet = GetAnyoneWallet();
    if (!wallet) {
        LogPrintf("AnyoneCanSpendHandler: Could not get 'Anyone' wallet\n");
        return std::nullopt;
    }

    // Convert the signal data to the format expected by CreateSpendTransactionFromWallet
    // Filter out dust outputs and outputs that would cost more to spend than they're worth
    std::vector<std::pair<COutPoint, std::pair<CAmount, CScript>>> outputs_for_spending;
    CAmount total_amount = 0;

    for (const auto& [output_index, script_sig] : anyone_can_spend_outputs) {
        const CTxOut& txout = tx->vout[output_index];
        COutPoint outpoint(tx->GetHash(), output_index);

        // Check if this is dust
        if (IsDust(txout, wallet->chain().relayDustFee())) {
            LogPrintf("AnyoneCanSpendHandler: Skipping dust output %s:%d, amount %s (below dust threshold)\n",
                      outpoint.hash.ToString(), outpoint.n, FormatMoney(txout.nValue));
            continue;
        }

        // Calculate accurate fee estimation for spending this output
        // Use cached transaction size based on script signature type
        wallet::CCoinControl coin_control;
        coin_control.m_confirm_target = 1; // Target next block

        // Determine transaction size based on script signature type
        unsigned int estimated_tx_size;
        if (script_sig.empty()) {
            estimated_tx_size = m_cached_tx_sizes.empty_script_size;
        } else if (script_sig.size() == 1 && script_sig[0] == OP_1) {
            estimated_tx_size = m_cached_tx_sizes.op1_script_size;
        } else if (script_sig.size() == 1 && script_sig[0] == OP_0) {
            estimated_tx_size = m_cached_tx_sizes.op0_script_size;
        } else {
            // Fallback: calculate size dynamically for unexpected script signatures
            CMutableTransaction mtx_dummy;
            CTxIn txin(outpoint);
            txin.scriptSig = script_sig;
            mtx_dummy.vin.push_back(txin);
            mtx_dummy.vout.push_back(CTxOut(txout.nValue, m_cached_output_script));
            CTransaction tx_dummy(mtx_dummy);
            int64_t tx_weight = GetTransactionWeight(tx_dummy);
            estimated_tx_size = (tx_weight + WITNESS_SCALE_FACTOR - 1) / WITNESS_SCALE_FACTOR;
        }

        // Get accurate fee estimation using the wallet's fee estimation
        FeeCalculation fee_calc;
        CAmount estimated_fee = GetMinimumFee(*wallet, estimated_tx_size, coin_control, &fee_calc);
        if (estimated_fee == 0) {
            // Fallback to a reasonable fee if estimation fails
            estimated_fee = 1000; // 1000 sats as a reasonable fallback fee
        }

        // Skip if the output value is less than the estimated fee
        if (txout.nValue <= estimated_fee) {
            LogPrintf("AnyoneCanSpendHandler: Skipping unprofitable output %s:%d, amount %s (less than estimated fee %s, tx size: %u bytes)\n",
                      outpoint.hash.ToString(), outpoint.n, FormatMoney(txout.nValue), FormatMoney(estimated_fee), estimated_tx_size);
            continue;
        }

        LogPrintf("AnyoneCanSpendHandler: Processing anyone can spend output %s:%d, amount %s\n",
                  outpoint.hash.ToString(), outpoint.n, FormatMoney(txout.nValue));

        outputs_for_spending.push_back({outpoint, {txout.nValue, txout.scriptPubKey}});
        total_amount += txout.nValue;
    }

    // If no "anyone can spend" outputs found, return early
    if (outputs_for_spending.empty()) {
        LogPrintf("AnyoneCanSpendHandler: No profitable anyone-can-spend outputs found in transaction %s, skipping\n",
                  tx->GetHash().ToString());
        return std::nullopt;
    }

    // Add the specific anyone-can-spend outputs to the wallet
    // This makes the wallet recognize these outputs as "mine" so they show in the balance
    // They will be marked as ISMINE_ANYONE and can be spent by our custom logic
    // Only add outputs that are profitable to spend
    {
        auto spk_man = wallet->GetLegacyScriptPubKeyMan();
        LOCK(spk_man->cs_KeyStore);
        for (const auto& [output_index, script_sig] : anyone_can_spend_outputs) {
            const CTxOut& txout = tx->vout[output_index];

            // Only add to wallet if it's not dust and profitable to spend
            if (!IsDust(txout, wallet->chain().relayDustFee())) {
                // Estimate the fee to spend this output
                unsigned int estimated_tx_size = 250; // Conservative estimate
                CAmount estimated_fee = wallet->chain().relayDustFee().GetFee(estimated_tx_size);

                // Only add if the output value is greater than the estimated fee
                if (txout.nValue > estimated_fee) {
                    // Add the scriptPubKey as anyone-can-spend
                    spk_man->AddAnyoneCanSpend(txout.scriptPubKey);
                }
            }
        }
    }

    // Add the transaction to the wallet with the appropriate state
    // The wallet will only show notifications for outputs that are marked as "mine"
    // Since we only added profitable outputs to setAnyoneCanSpend, only those will be tracked
    wallet->AddToWallet(tx, state);

    LogPrintf("AnyoneCanSpendHandler: Added transaction %s to 'Anyone' wallet with %zu profitable anyone-can-spend outputs (total value: %s)\n",
              tx->GetHash().ToString(), outputs_for_spending.size(), FormatMoney(total_amount));

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
    // Check for shutdown before processing
    if (ShutdownRequested()) {
        return std::nullopt;
    }

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

        // First, create a draft transaction to calculate its size
        CMutableTransaction mtx_draft;

        // Add inputs (anyone-can-spend outputs)
        for (const auto& output : outputs) {
            CTxIn txin(output.first);
            // For anyone-can-spend outputs, we can use an empty scriptSig
            txin.scriptSig = CScript();
            mtx_draft.vin.push_back(txin);
        }

        // Add a placeholder output (we'll update the amount after fee calculation)
        mtx_draft.vout.push_back(CTxOut(total_amount, GetScriptForDestination(dest)));

        // Calculate the transaction weight for accurate fee estimation
        CTransaction tx_draft(mtx_draft);
        int64_t tx_weight = GetTransactionWeight(tx_draft);
        unsigned int nTxBytes = (tx_weight + WITNESS_SCALE_FACTOR - 1) / WITNESS_SCALE_FACTOR; // Convert weight to vsize

        // Get fee estimation using the coin control
        FeeCalculation fee_calc;
        CAmount estimated_fee = GetMinimumFee(*wallet, nTxBytes, coin_control, &fee_calc);
        if (estimated_fee == 0) {
            // Fallback to a reasonable fee if estimation fails
            estimated_fee = 1000; // 1000 sats as a reasonable fallback fee
        }

        CAmount amount_to_send = total_amount - estimated_fee;

        if (amount_to_send <= 0) {
            LogPrintf("AnyoneCanSpendHandler: Amount too small to spend after fee deduction\n");
            return std::nullopt;
        }

        // Now create the final transaction with the correct amount
        CMutableTransaction mtx;

        // Add inputs (anyone-can-spend outputs)
        for (const auto& output : outputs) {
            CTxIn txin(output.first);
            // For anyone-can-spend outputs, we can use an empty scriptSig
            txin.scriptSig = CScript();
            mtx.vin.push_back(txin);
        }

        // Add output with the correct amount
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
    // Check for shutdown before starting heartbeat
    if (ShutdownRequested()) {
        LogPrintf("AnyoneCanSpendHandler: Skipping heartbeat start during shutdown\n");
        return;
    }

    if (m_heartbeat_running.exchange(true)) {
        return; // Already running
    }

        m_heartbeat_thread = std::thread([this]() {
        while (m_heartbeat_running.load() && !ShutdownRequested()) {
            // Sleep in shorter intervals to be more responsive to shutdown
            for (int i = 0; i < 60 && !ShutdownRequested(); i++) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            if (!ShutdownRequested()) {
                std::lock_guard<std::mutex> lock(m_stats_mutex);
                if (m_handler_enabled) {
                    LogPrintf("AnyoneCanSpendHandler: Heartbeat - Transactions evaluated: %u blocks, %u mempool, Anyone-can-spend outputs detected: %u, Outputs spent: %u\n",
                             m_stats.transactions_evaluated_blocks, m_stats.transactions_evaluated_mempool,
                             m_stats.outputs_detected, m_stats.outputs_spent);
                } else {
                    LogPrintf("AnyoneCanSpendHandler: Heartbeat - Handler disabled (invalid destination address)\n");
                }
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

std::vector<std::pair<size_t, CScript>> AnyoneCanSpendHandler::FindAnyoneCanSpendOutputs(const CTransaction& tx)
{
    std::vector<std::pair<size_t, CScript>> results;

    // Safety mechanism: limit the number of anyone-can-spend outputs we process
    static int total_anyone_can_spend_outputs_found = 0;
    const int MAX_ANYONE_CAN_SPEND_OUTPUTS = 10;

    // Get the "Anyone" wallet to access fee estimation
    auto wallet = GetAnyoneWallet();
    if (!wallet) {
        LogPrintf("AnyoneCanSpendHandler: Cannot update fee rate - no wallet available\n");
        return;
    }

    try {
        // Create a dummy transaction to estimate fees for next-block inclusion
        // The fee rate (sat/vB) is independent of transaction size, so we can use a reasonable estimate
        wallet::CCoinControl coin_control;
        coin_control.m_confirm_target = 1; // Target next block

        // Use a reasonable transaction size estimate (250 bytes is typical for 1-input, 1-output)
        unsigned int nTxBytes = 250;
        FeeCalculation fee_calc;
        CAmount estimated_fee = GetMinimumFee(*wallet, nTxBytes, coin_control, &fee_calc);

        if (estimated_fee > 0) {
            CFeeRate fee_rate = CFeeRate(estimated_fee, nTxBytes);
            UpdateNextBlockFeeRateWithTimestamp(fee_rate);
            LogPrintf("AnyoneCanSpendHandler: Updated fee rate to %s sat/vB for next-block inclusion\n",
                     fee_rate.GetFeePerK() / 1000);
        } else {
            LogPrintf("AnyoneCanSpendHandler: Fee estimation failed, using fallback\n");
            // Use a conservative fallback fee rate
            UpdateNextBlockFeeRateWithTimestamp(CFeeRate(5000)); // 5 sat/vB
        }
    }

    return results;
}

void AnyoneCanSpendHandler::UpdateNextBlockFeeRateWithTimestamp(const CFeeRate& fee_rate)
{
    UpdateNextBlockFeeRate(fee_rate);
    m_last_fee_rate_update = std::chrono::steady_clock::now();
}

std::vector<std::pair<size_t, CScript>> AnyoneCanSpendHandler::FindAnyoneCanSpendOutputs(const CTransaction& tx)
{
    std::vector<std::pair<size_t, CScript>> results;

    for (size_t i = 0; i < tx.vout.size(); i++) {
        const CTxOut& txout = tx.vout[i];

        // Test every output with script execution to determine if it's anyone-can-spend
        // Use a minimal but effective test set covering the most common anyone-can-spend patterns
        std::vector<CScript> test_script_sigs = {
            CScript(),           // Empty script signature (most common anyone-can-spend case)
            CScript() << OP_1,   // Push true value
            CScript() << OP_0,   // Push false value
        };

        // Use Bitcoin Core's actual script execution engine
        for (const auto& script_sig : test_script_sigs) {
            ScriptError serror;

            // Create a dummy signature checker that always returns true for signature checks
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

            // Use EvalScript directly to avoid debug log noise from VerifyScript
            // We're intentionally testing scripts, so failures are expected
            std::vector<std::vector<unsigned char>> stack;

            // Execute the script signature first
            if (!EvalScript(stack, script_sig, STANDARD_SCRIPT_VERIFY_FLAGS, checker, SigVersion::BASE, &serror)) {
                continue; // Script signature failed, try next one - REBTODO is this right?!
            }

            // Then execute the scriptPubKey
            if (EvalScript(stack, txout.scriptPubKey, STANDARD_SCRIPT_VERIFY_FLAGS, checker, SigVersion::BASE, &serror)) {
                // Check if the final result is true (non-empty stack with truthy top element)
                if (!stack.empty() && !stack.back().empty() && stack.back()[0] != 0) {
                    // Found a working script signature for this anyone-can-spend output
                    // Add it to results - profitability checking will be done in the handler
                    results.emplace_back(i, script_sig);
                    LogPrintf("AnyoneCanSpend: Found anyone-can-spend output %s:%d, amount: %s\n",
                              tx.GetHash().ToString(), i, FormatMoney(txout.nValue));
                    break; // Found working script signature, no need to test more
                }
            }
            // Script failed - this is expected when testing, so continue silently
        }
    }

    return results;
}
