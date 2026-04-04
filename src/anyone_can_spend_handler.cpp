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
#include <hash.h>

#include <algorithm>
#include <chrono>
#include <set>
#include <string_view>

namespace {
const CFeeRate MIN_ANYONE_CAN_SPEND_FEERATE{20'000}; // 20 sat/vB floor.
constexpr size_t MIN_OUTPUTS_SCANNED_PER_TX{8};
constexpr size_t MAX_OUTPUTS_SCANNED_PER_TX{64};
constexpr size_t MIN_OUTPUTS_RETURNED_PER_TX{2};
constexpr size_t MAX_OUTPUTS_RETURNED_PER_TX{16};
constexpr size_t MIN_PROBE_EVALS_PER_TX{48};
constexpr size_t MAX_PROBE_EVALS_PER_TX{512};
constexpr size_t MIN_PROBE_SCRIPT_SIG_TEMPLATES{2};
constexpr size_t MAX_PROBE_SCRIPT_SIG_TEMPLATES{16};

class DummySignatureCheckerAllowAll final : public BaseSignatureChecker {
public:
    bool CheckECDSASignature(const std::vector<unsigned char>&, const std::vector<unsigned char>&, const CScript&, SigVersion) const override { return true; }
    bool CheckSchnorrSignature(Span<const unsigned char>, Span<const unsigned char>, SigVersion, ScriptExecutionData&, ScriptError*) const override { return true; }
    bool CheckLockTime(const CScriptNum&) const override { return true; }
    bool CheckSequence(const CScriptNum&) const override { return true; }
};

class DummySignatureCheckerDenyAll final : public BaseSignatureChecker {
public:
    bool CheckECDSASignature(const std::vector<unsigned char>&, const std::vector<unsigned char>&, const CScript&, SigVersion) const override { return false; }
    bool CheckSchnorrSignature(Span<const unsigned char>, Span<const unsigned char>, SigVersion, ScriptExecutionData&, ScriptError*) const override { return false; }
    bool CheckLockTime(const CScriptNum&) const override { return false; }
    bool CheckSequence(const CScriptNum&) const override { return false; }
};

static bool ConsumeProbeBudget(const anyonecanspend::ProbeLimits& limits, size_t units = 1)
{
    if (!limits.remaining_eval_budget) return true;
    if (*limits.remaining_eval_budget < units) {
        *limits.remaining_eval_budget = 0;
        return false;
    }
    *limits.remaining_eval_budget -= units;
    return true;
}

static bool IsTrueStackValue(const std::vector<unsigned char>& value)
{
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] != 0) {
            // Negative zero is also false in script semantics.
            return !(i == value.size() - 1 && value[i] == 0x80);
        }
    }
    return false;
}

static std::vector<CScript> BuildProbeScriptSigTemplates(size_t max_templates)
{
    std::vector<CScript> ordered_templates = {
        CScript(),                    // Empty scriptSig
        CScript() << OP_1,            // Script true
        CScript() << OP_0,            // Script false
        CScript() << OP_1 << OP_1,    // Two truthy stack items
        CScript() << OP_2,            // Small integer
        CScript() << OP_1NEGATE,      // -1
        CScript() << std::vector<unsigned char>{1},
        CScript() << std::vector<unsigned char>{0},
    };

    std::vector<CScript> result;
    result.reserve(std::min(max_templates, ordered_templates.size()));
    std::set<std::vector<unsigned char>> seen;
    for (const auto& candidate : ordered_templates) {
        if (result.size() >= max_templates) break;
        const auto inserted = seen.insert(std::vector<unsigned char>(candidate.begin(), candidate.end()));
        if (!inserted.second) continue;
        result.push_back(candidate);
    }
    return result;
}

static bool EvalProbeCandidate(const CScript& script_sig, const CScript& script_pub_key, const BaseSignatureChecker& checker, const anyonecanspend::ProbeLimits& limits)
{
    ScriptError serror;
    std::vector<std::vector<unsigned char>> stack;
    ScriptExecutionData execdata;
    execdata.m_anyone_can_spend_probe = true;

    if (!ConsumeProbeBudget(limits) || !EvalScript(stack, script_sig, STANDARD_SCRIPT_VERIFY_FLAGS, checker, SigVersion::BASE, execdata, &serror)) {
        return false;
    }
    if (!ConsumeProbeBudget(limits) || !EvalScript(stack, script_pub_key, STANDARD_SCRIPT_VERIFY_FLAGS, checker, SigVersion::BASE, execdata, &serror)) {
        return false;
    }
    return !stack.empty() && IsTrueStackValue(stack.back());
}

static uint256 ScriptCacheKey(const CScript& script_pub_key)
{
    return Hash(script_pub_key);
}

static std::string DescribeAcsSelection(const CTxOut& txout, const CScript& script_sig)
{
    if (txout.scriptPubKey.size() == 1 && (txout.scriptPubKey[0] == OP_TRUE || txout.scriptPubKey[0] == OP_1)) {
        return "scriptPubKey=OP_TRUE";
    }
    if (txout.scriptPubKey.size() == 2 && txout.scriptPubKey[0] == OP_DROP &&
        (txout.scriptPubKey[1] == OP_TRUE || txout.scriptPubKey[1] == OP_1)) {
        return "scriptPubKey=OP_DROP OP_TRUE";
    }
    return strprintf("probe(scriptSig=%s)", HexStr(script_sig));
}

static std::string BuildAcsReasonSummary(const CTransactionRef& tx, const std::vector<std::pair<size_t, CScript>>& anyone_can_spend_outputs)
{
    std::string summary = "ACS-selected outputs: ";
    bool first = true;
    for (const auto& [output_index, script_sig] : anyone_can_spend_outputs) {
        if (!first) summary += "; ";
        first = false;
        summary += strprintf("vout=%u amount=%s %s",
                             (unsigned)output_index,
                             FormatMoney(tx->vout[output_index].nValue),
                             DescribeAcsSelection(tx->vout[output_index], script_sig));
    }
    return summary;
}
} // namespace

namespace anyonecanspend {
bool IsAnyoneCanSpendScriptPubKey(const CScript& script_pub_key, CScript* spend_script_sig, const ProbeLimits& limits)
{
    int witness_version;
    std::vector<unsigned char> witness_program;
    if (script_pub_key.IsWitnessProgram(witness_version, witness_program)) {
        return false;
    }

    if (script_pub_key.size() == 1 && (script_pub_key[0] == OP_TRUE || script_pub_key[0] == OP_1)) {
        if (spend_script_sig) *spend_script_sig = CScript();
        return true;
    }

    if (script_pub_key.size() == 2 && script_pub_key[0] == OP_DROP &&
        (script_pub_key[1] == OP_TRUE || script_pub_key[1] == OP_1)) {
        if (spend_script_sig) *spend_script_sig = CScript() << OP_1;
        return true;
    }

    const size_t template_limit = std::max<size_t>(1, limits.max_script_sig_templates);
    const std::vector<CScript> test_script_sigs = BuildProbeScriptSigTemplates(template_limit);
    DummySignatureCheckerAllowAll allow_checker;
    DummySignatureCheckerDenyAll deny_checker;

    for (const auto& script_sig : test_script_sigs) {
        // Option B: Only classify as anyone-can-spend if the candidate succeeds
        // both with permissive and strict checker behavior.
        const bool allow_ok = EvalProbeCandidate(script_sig, script_pub_key, allow_checker, limits);
        if (!allow_ok) {
            continue;
        }
        const bool deny_ok = EvalProbeCandidate(script_sig, script_pub_key, deny_checker, limits);
        if (deny_ok) {
            if (spend_script_sig) *spend_script_sig = script_sig;
            return true;
        }

        if (limits.remaining_eval_budget && *limits.remaining_eval_budget == 0) {
            LogPrint(BCLog::ANYONECANSPEND, "AnyoneCanSpend: Probe budget exhausted while evaluating scriptPubKey=%s\n", HexStr(script_pub_key));
            break;
        }
    }
    return false;
}

std::vector<std::pair<size_t, CScript>> FindAnyoneCanSpendOutputs(const CTransaction& tx, size_t max_outputs)
{
    std::vector<std::pair<size_t, CScript>> results;
    for (size_t i = 0; i < tx.vout.size(); ++i) {
        CScript spend_script_sig;
        if (!IsAnyoneCanSpendScriptPubKey(tx.vout[i].scriptPubKey, &spend_script_sig)) {
            continue;
        }
        results.emplace_back(i, spend_script_sig);
        if (results.size() >= max_outputs) {
            break;
        }
    }
    return results;
}

std::string DescribeAnyoneCanSpendOutput(const CTransaction& tx, size_t output_index)
{
    if (output_index >= tx.vout.size()) {
        return {};
    }
    CScript spend_script_sig;
    if (!IsAnyoneCanSpendScriptPubKey(tx.vout[output_index].scriptPubKey, &spend_script_sig)) {
        return {};
    }
    return strprintf("vout=%u amount=%s %s",
                     static_cast<unsigned>(output_index),
                     FormatMoney(tx.vout[output_index].nValue),
                     DescribeAcsSelection(tx.vout[output_index], spend_script_sig));
}

bool IsAnyoneWalletName(const std::string& wallet_name)
{
    if (wallet_name == "Anyone") return true;
    constexpr std::string_view suffix{"/Anyone"};
    if (wallet_name.size() >= suffix.size() &&
        wallet_name.compare(wallet_name.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return true;
    }
    constexpr std::string_view suffix_windows{"\\Anyone"};
    return wallet_name.size() >= suffix_windows.size() &&
           wallet_name.compare(wallet_name.size() - suffix_windows.size(), suffix_windows.size(), suffix_windows) == 0;
}
} // namespace anyonecanspend

namespace {
static CFeeRate GetAnyoneCanSpendFeeRate(const wallet::CWallet& wallet, const wallet::CCoinControl& coin_control_in, FeeCalculation* fee_calc)
{
    wallet::CCoinControl coin_control{coin_control_in};
    coin_control.m_confirm_target = 1;
    coin_control.m_fee_mode = FeeEstimateMode::CONSERVATIVE;

    const CFeeRate estimated_rate = GetMinimumFeeRate(wallet, coin_control, fee_calc);
    const CFeeRate relay_floor = std::max(wallet.chain().relayMinFee(), wallet.chain().mempoolMinFee());
    return std::max({estimated_rate, relay_floor, MIN_ANYONE_CAN_SPEND_FEERATE});
}
} // namespace

AnyoneCanSpendHandler::AnyoneCanSpendHandler()
    : m_handler_enabled(false), m_wallet_context(nullptr), m_auto_spend(false)
{
    LogPrintf("AnyoneCanSpendHandler: Constructor called\n");
    LoadAutoTuneConfigFromArgs();

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
        CleanupStaleAnyoneWalletTransactions(/*force=*/true);
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

    const auto start = std::chrono::steady_clock::now();
    bool budget_exhausted{false};
    // Check for anyone-can-spend outputs in this transaction
    auto anyone_can_spend_outputs = FindAnyoneCanSpendOutputs(*tx, /*from_mempool=*/true, &budget_exhausted);
    const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    UpdateRuntimeTuning(/*from_mempool=*/true, elapsed_us, budget_exhausted);
    if (!anyone_can_spend_outputs.empty()) {
        LogPrint(BCLog::ANYONECANSPEND, "AnyoneCanSpendHandler: Found %zu anyone-can-spend outputs in mempool transaction %s\n",
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
        const auto start = std::chrono::steady_clock::now();
        bool budget_exhausted{false};
        auto anyone_can_spend_outputs = FindAnyoneCanSpendOutputs(*tx, /*from_mempool=*/false, &budget_exhausted);
        const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
        UpdateRuntimeTuning(/*from_mempool=*/false, elapsed_us, budget_exhausted);
        if (!anyone_can_spend_outputs.empty()) {
            LogPrint(BCLog::ANYONECANSPEND, "AnyoneCanSpendHandler: Found %zu anyone-can-spend outputs in block transaction %s\n",
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
    std::vector<SpendableAnyoneOutput> outputs_for_spending;
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

        // Use a conservative next-block fee target with a hard floor.
        FeeCalculation fee_calc;
        const CFeeRate fee_rate = GetAnyoneCanSpendFeeRate(*wallet, coin_control, &fee_calc);
        CAmount estimated_fee = fee_rate.GetFee(estimated_tx_size);

        // Skip if the output value is less than the estimated fee
        if (txout.nValue <= estimated_fee) {
            LogPrintf("AnyoneCanSpendHandler: Skipping unprofitable output %s:%d, amount %s (less than estimated fee %s, tx size: %u bytes)\n",
                      outpoint.hash.ToString(), outpoint.n, FormatMoney(txout.nValue), FormatMoney(estimated_fee), estimated_tx_size);
            continue;
        }

        LogPrint(BCLog::ANYONECANSPEND, "AnyoneCanSpendHandler: Processing anyone can spend output %s:%d, amount %s\n",
                  outpoint.hash.ToString(), outpoint.n, FormatMoney(txout.nValue));

        outputs_for_spending.push_back({outpoint, txout.nValue, txout.scriptPubKey, script_sig});
        total_amount += txout.nValue;
    }

    // If no "anyone can spend" outputs found, return early
    if (outputs_for_spending.empty()) {
        LogPrintf("AnyoneCanSpendHandler: No profitable anyone-can-spend outputs found in transaction %s, skipping\n",
                  tx->GetHash().ToString());
        return std::nullopt;
    }

    {
        std::lock_guard<std::mutex> lock(m_stats_mutex);
        m_stats.outputs_detected += outputs_for_spending.size();
        m_stats.total_amount_detected += total_amount;
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
    const std::string acs_reason_summary = BuildAcsReasonSummary(tx, anyone_can_spend_outputs);
    wallet->AddToWallet(tx, state, [&](wallet::CWalletTx& wtx, bool) {
        wtx.mapValue["acs_reason"] = acs_reason_summary;
        return true;
    });

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
    const std::vector<AnyoneCanSpendHandler::SpendableAnyoneOutput>& outputs)
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
            total_amount += output.amount;
        }

        // Create coin control to specify the exact outputs to spend and get fee estimation
        wallet::CCoinControl coin_control;
        for (const auto& output : outputs) {
            coin_control.Select(output.outpoint);
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
            CTxIn txin(output.outpoint);
            txin.scriptSig = output.script_sig;
            mtx_draft.vin.push_back(txin);
        }

        // Add a placeholder output (we'll update the amount after fee calculation)
        mtx_draft.vout.push_back(CTxOut(total_amount, GetScriptForDestination(dest)));

        // Calculate the transaction weight for accurate fee estimation
        CTransaction tx_draft(mtx_draft);
        int64_t tx_weight = GetTransactionWeight(tx_draft);
        unsigned int nTxBytes = (tx_weight + WITNESS_SCALE_FACTOR - 1) / WITNESS_SCALE_FACTOR; // Convert weight to vsize

        // Use a conservative next-block fee target with a hard floor.
        FeeCalculation fee_calc;
        const CFeeRate fee_rate = GetAnyoneCanSpendFeeRate(*wallet, coin_control, &fee_calc);
        CAmount estimated_fee = fee_rate.GetFee(nTxBytes);

        CAmount amount_to_send = total_amount - estimated_fee;

        if (amount_to_send <= 0) {
            LogPrintf("AnyoneCanSpendHandler: Amount too small to spend after fee deduction\n");
            return std::nullopt;
        }

        // Now create the final transaction with the correct amount
        CMutableTransaction mtx;

        // Add inputs (anyone-can-spend outputs)
        for (const auto& output : outputs) {
            CTxIn txin(output.outpoint);
            txin.scriptSig = output.script_sig;
            mtx.vin.push_back(txin);
        }

        // Add output with the correct amount
        mtx.vout.push_back(CTxOut(amount_to_send, GetScriptForDestination(dest)));

        // Create the transaction
        CTransactionRef tx_new = MakeTransactionRef(mtx);

        // Option D: Preflight mempool policy acceptance before committing to wallet.
        std::string preflight_err;
        if (!wallet->chain().broadcastTransaction(tx_new, /*max_tx_fee=*/0, /*relay=*/false, preflight_err, NODEID_WALLET_ORIGIN)) {
            LogPrintf("AnyoneCanSpendHandler: Preflight reject for auto-spend tx %s: %s\n",
                      tx_new->GetHash().ToString(), preflight_err);
            return std::nullopt;
        }

        // Commit the transaction to the wallet with ACS metadata for UI explainability.
        wallet::mapValue_t map_value;
        map_value["acs_sweep"] = "1";
        map_value["acs_sweep_outputs"] = ToString(outputs.size());
        map_value["acs_sweep_total"] = FormatMoney(total_amount);
        map_value["acs_sweep_fee"] = FormatMoney(estimated_fee);
        wallet->CommitTransaction(tx_new, std::move(map_value), {});

        {
            std::lock_guard<std::mutex> lock(m_stats_mutex);
            m_stats.outputs_spent += outputs.size();
            m_stats.total_amount_spent += total_amount;
        }

        LogPrintf("AnyoneCanSpendHandler: Created and committed spending transaction %s for %zu outputs (total: %s, amount sent: %s, fee: %s, feerate: %s)\n",
                  tx_new->GetHash().ToString(), outputs.size(), FormatMoney(total_amount), FormatMoney(amount_to_send), FormatMoney(estimated_fee), fee_rate.ToString(FeeEstimateMode::SAT_VB));

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
                CleanupStaleAnyoneWalletTransactions();
                Stats stats_snapshot;
                RuntimeTuning tuning_snapshot;
                {
                    std::lock_guard<std::mutex> lock(m_stats_mutex);
                    stats_snapshot = m_stats;
                }
                {
                    std::lock_guard<std::mutex> lock(m_tuning_mutex);
                    tuning_snapshot = m_tuning_state;
                }
                if (m_handler_enabled) {
                    LogPrintf("AnyoneCanSpendHandler: Heartbeat - tx_eval(block=%llu,mempool=%llu) outputs(detected=%llu,spent=%llu) cleanup(runs=%llu,removed=%llu) tuning(scan=%zu,return=%zu,eval_budget=%zu,templates=%zu) perf(ema_us_mempool=%.1f,ema_us_block=%.1f,overruns_mempool=%llu,overruns_block=%llu,budget_exhaust_mempool=%llu,budget_exhaust_block=%llu,cache_hit_rate=%.1f%%)\n",
                             (unsigned long long)stats_snapshot.transactions_evaluated_blocks, (unsigned long long)stats_snapshot.transactions_evaluated_mempool,
                             (unsigned long long)stats_snapshot.outputs_detected, (unsigned long long)stats_snapshot.outputs_spent,
                             (unsigned long long)stats_snapshot.cleanup_runs, (unsigned long long)stats_snapshot.cleanup_removed,
                             tuning_snapshot.max_outputs_scanned_per_tx, tuning_snapshot.max_outputs_returned_per_tx,
                             tuning_snapshot.max_probe_evals_per_tx, tuning_snapshot.max_probe_script_sig_templates,
                             tuning_snapshot.ema_elapsed_us_mempool, tuning_snapshot.ema_elapsed_us_block,
                             (unsigned long long)tuning_snapshot.mempool_overruns, (unsigned long long)tuning_snapshot.block_overruns,
                             (unsigned long long)tuning_snapshot.mempool_budget_exhaustions, (unsigned long long)tuning_snapshot.block_budget_exhaustions,
                             (tuning_snapshot.cache_hits + tuning_snapshot.cache_misses) > 0
                                 ? (100.0 * tuning_snapshot.cache_hits) / (tuning_snapshot.cache_hits + tuning_snapshot.cache_misses)
                                 : 0.0);
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

void AnyoneCanSpendHandler::CleanupStaleAnyoneWalletTransactions(bool force)
{
    if (ShutdownRequested()) return;
    auto wallet = GetAnyoneWallet();
    if (!wallet) return;

    AutoTuneConfig cfg;
    {
        std::lock_guard<std::mutex> lock(m_tuning_mutex);
        cfg = m_tuning_config;
    }

    const int64_t now = GetTime();
    if (!force && m_last_cleanup_time > 0 && (now - m_last_cleanup_time) < cfg.cleanup_interval_secs) {
        return;
    }
    m_last_cleanup_time = now;

    struct Candidate {
        uint256 txid;
        CTransactionRef tx;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(static_cast<size_t>(cfg.cleanup_max_candidates));
    std::vector<uint256> confirmed_prune_txids;
    confirmed_prune_txids.reserve(static_cast<size_t>(cfg.cleanup_max_candidates));
    size_t retained_acs_sweeps{0};

    {
        LOCK(wallet->cs_wallet);
        for (const auto& [txid, wtx] : wallet->mapWallet) {
            if ((candidates.size() + confirmed_prune_txids.size()) >= static_cast<size_t>(cfg.cleanup_max_candidates)) break;
            const int depth = wallet->GetTxDepthInMainChain(wtx);
            // Prune mature confirmed ACS history aggressively to keep the Anyone wallet small.
            if (depth > 1) {
                confirmed_prune_txids.push_back(txid);
                continue;
            }
            if (depth != 0) continue;
            if (wallet->chain().isInMempool(txid)) continue;
            const bool is_acs_sweep = wtx.mapValue.count("acs_sweep") != 0;
            const bool has_replacement = wtx.mapValue.count("replaced_by_txid") != 0;
            // Keep stale unconfirmed ACS sweep transactions for post-mortem analysis
            // unless they were explicitly replaced or abandoned.
            if (is_acs_sweep && !has_replacement && !wtx.isAbandoned()) {
                ++retained_acs_sweeps;
                continue;
            }
            const int64_t received = wtx.nTimeReceivedMillis > 0 ? (wtx.nTimeReceivedMillis / 1000) : wtx.nTimeReceived;
            if ((now - received) < cfg.cleanup_stale_age_secs) continue;
            candidates.push_back({txid, wtx.tx});
        }
    }

    std::vector<uint256> removable_txids;
    removable_txids.reserve(candidates.size());
    for (const auto& c : candidates) {
        std::string err_string;
        const bool accepted = wallet->chain().broadcastTransaction(c.tx, /*max_tx_fee=*/0, /*relay=*/false, err_string, NODEID_WALLET_ORIGIN);
        if (!accepted) {
            removable_txids.push_back(c.txid);
        }
    }

    std::vector<uint256> removed_txids;
    if (!removable_txids.empty() || !confirmed_prune_txids.empty()) {
        LOCK(wallet->cs_wallet);
        std::vector<uint256> to_zap;
        to_zap.reserve(removable_txids.size() + confirmed_prune_txids.size());
        for (const auto& txid : confirmed_prune_txids) {
            const auto it = wallet->mapWallet.find(txid);
            if (it == wallet->mapWallet.end()) continue;
            if (wallet->GetTxDepthInMainChain(it->second) > 1) {
                to_zap.push_back(txid);
            }
        }
        std::vector<uint256> still_unconfirmed;
        still_unconfirmed.reserve(removable_txids.size());
        for (const auto& txid : removable_txids) {
            const auto it = wallet->mapWallet.find(txid);
            if (it == wallet->mapWallet.end()) continue;
            if (wallet->GetTxDepthInMainChain(it->second) != 0) continue;
            still_unconfirmed.push_back(txid);
        }
        to_zap.insert(to_zap.end(), still_unconfirmed.begin(), still_unconfirmed.end());
        if (!to_zap.empty() && wallet->ZapSelectTx(to_zap, removed_txids) != wallet::DBErrors::LOAD_OK) {
            LogPrintf("AnyoneCanSpendHandler: Cleanup failed to remove stale txs from Anyone wallet\n");
            removed_txids.clear();
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_stats_mutex);
        m_stats.cleanup_runs++;
        m_stats.cleanup_removed += removed_txids.size();
    }

    if (!candidates.empty() || !confirmed_prune_txids.empty() || !removed_txids.empty() || retained_acs_sweeps > 0) {
        LogPrintf("AnyoneCanSpendHandler: Cleanup run stale_candidates=%u confirmed_candidates=%u retained_acs_sweeps=%u removed=%u force=%s\n",
                  (unsigned)candidates.size(), (unsigned)confirmed_prune_txids.size(), (unsigned)retained_acs_sweeps, (unsigned)removed_txids.size(), force ? "true" : "false");
    }
}

std::vector<std::pair<size_t, CScript>> AnyoneCanSpendHandler::FindAnyoneCanSpendOutputs(const CTransaction& tx, bool from_mempool, bool* budget_exhausted)
{
    if (budget_exhausted) *budget_exhausted = false;

    size_t max_outputs_scanned_per_tx;
    size_t max_outputs_returned_per_tx;
    size_t max_probe_evals_per_tx;
    size_t max_probe_script_sig_templates;
    {
        std::lock_guard<std::mutex> lock(m_tuning_mutex);
        max_outputs_scanned_per_tx = m_tuning_state.max_outputs_scanned_per_tx;
        max_outputs_returned_per_tx = m_tuning_state.max_outputs_returned_per_tx;
        max_probe_evals_per_tx = m_tuning_state.max_probe_evals_per_tx;
        max_probe_script_sig_templates = m_tuning_state.max_probe_script_sig_templates;
    }

    std::vector<std::pair<size_t, CScript>> results;
    results.reserve(std::min(tx.vout.size(), max_outputs_returned_per_tx));

    size_t remaining_eval_budget{max_probe_evals_per_tx};
    const size_t outputs_to_scan = std::min(tx.vout.size(), max_outputs_scanned_per_tx);
    for (size_t index = 0; index < outputs_to_scan && results.size() < max_outputs_returned_per_tx; ++index) {
        const CTxOut& txout = tx.vout[index];
        CScript script_sig;
        bool is_anyone_can_spend_cached{false};
        if (LookupDetectionCache(txout.scriptPubKey, is_anyone_can_spend_cached, &script_sig)) {
            std::lock_guard<std::mutex> lock(m_tuning_mutex);
            m_tuning_state.cache_hits++;
            if (is_anyone_can_spend_cached) {
                results.emplace_back(index, script_sig);
            }
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(m_tuning_mutex);
            m_tuning_state.cache_misses++;
        }

        anyonecanspend::ProbeLimits limits;
        limits.max_script_sig_templates = max_probe_script_sig_templates;
        limits.remaining_eval_budget = &remaining_eval_budget;
        const bool is_anyone_can_spend = anyonecanspend::IsAnyoneCanSpendScriptPubKey(txout.scriptPubKey, &script_sig, limits);
        StoreDetectionCache(txout.scriptPubKey, is_anyone_can_spend, script_sig);
        if (is_anyone_can_spend) {
            results.emplace_back(index, script_sig);
        }
        if (remaining_eval_budget == 0) {
            LogPrint(BCLog::ANYONECANSPEND, "AnyoneCanSpend: Probe budget exhausted while scanning tx=%s mode=%s\n",
                     tx.GetHash().ToString(), from_mempool ? "mempool" : "block");
            if (budget_exhausted) *budget_exhausted = true;
            break;
        }
    }

    for (const auto& [index, script_sig] : results) {
        const CTxOut& txout = tx.vout[index];
        LogPrint(BCLog::ANYONECANSPEND, "AnyoneCanSpend: Found anyone-can-spend output %s:%d, amount: %s, scriptPubKey: %s, scriptSig: %s\n",
                  tx.GetHash().ToString(), index, FormatMoney(txout.nValue),
                  HexStr(txout.scriptPubKey), HexStr(script_sig));
    }

    return results;
}

void AnyoneCanSpendHandler::UpdateRuntimeTuning(bool from_mempool, int64_t elapsed_us, bool budget_exhausted)
{
    std::lock_guard<std::mutex> lock(m_tuning_mutex);
    auto& t = m_tuning_state;
    const auto cfg = m_tuning_config;
    const size_t prev_scan = t.max_outputs_scanned_per_tx;
    const size_t prev_return = t.max_outputs_returned_per_tx;
    const size_t prev_eval_budget = t.max_probe_evals_per_tx;
    const size_t prev_templates = t.max_probe_script_sig_templates;

    const int64_t target_us = from_mempool ? cfg.target_elapsed_us_mempool : cfg.target_elapsed_us_block;
    double& ema = from_mempool ? t.ema_elapsed_us_mempool : t.ema_elapsed_us_block;
    uint64_t& events = from_mempool ? t.mempool_events : t.block_events;
    uint64_t& budget_exhaustions = from_mempool ? t.mempool_budget_exhaustions : t.block_budget_exhaustions;
    uint64_t& overruns = from_mempool ? t.mempool_overruns : t.block_overruns;
    uint64_t& since_adjust = from_mempool ? t.mempool_events_since_adjust : t.block_events_since_adjust;

    events++;
    since_adjust++;
    if (budget_exhausted) budget_exhaustions++;
    if (elapsed_us > target_us) overruns++;

    ema = (events == 1) ? static_cast<double>(elapsed_us) : (cfg.ema_alpha * static_cast<double>(elapsed_us) + (1.0 - cfg.ema_alpha) * ema);

    if (since_adjust < cfg.adjust_interval_events) {
        return;
    }
    since_adjust = 0;

    const double exhaust_rate = events > 0 ? static_cast<double>(budget_exhaustions) / static_cast<double>(events) : 0.0;
    const bool overloaded = ema > static_cast<double>(target_us) || exhaust_rate > cfg.overload_exhaust_threshold;
    const bool underloaded = ema < static_cast<double>(target_us) * 0.5 && exhaust_rate < cfg.underload_exhaust_threshold;
    if (overloaded) {
        if (t.max_probe_evals_per_tx > MIN_PROBE_EVALS_PER_TX) {
            t.max_probe_evals_per_tx = std::max(MIN_PROBE_EVALS_PER_TX, (t.max_probe_evals_per_tx * static_cast<size_t>(cfg.overload_scale_percent)) / 100);
        } else if (t.max_probe_script_sig_templates > MIN_PROBE_SCRIPT_SIG_TEMPLATES) {
            t.max_probe_script_sig_templates = std::max(MIN_PROBE_SCRIPT_SIG_TEMPLATES, (t.max_probe_script_sig_templates * static_cast<size_t>(cfg.overload_scale_percent)) / 100);
        } else if (t.max_outputs_scanned_per_tx > MIN_OUTPUTS_SCANNED_PER_TX) {
            t.max_outputs_scanned_per_tx = std::max(MIN_OUTPUTS_SCANNED_PER_TX, (t.max_outputs_scanned_per_tx * static_cast<size_t>(cfg.overload_scale_percent)) / 100);
        }
    } else if (underloaded) {
        if (t.max_probe_evals_per_tx < MAX_PROBE_EVALS_PER_TX) {
            t.max_probe_evals_per_tx = std::min(MAX_PROBE_EVALS_PER_TX, (t.max_probe_evals_per_tx * static_cast<size_t>(cfg.underload_scale_percent)) / 100 + 1);
        } else if (t.max_probe_script_sig_templates < MAX_PROBE_SCRIPT_SIG_TEMPLATES) {
            t.max_probe_script_sig_templates = std::min(MAX_PROBE_SCRIPT_SIG_TEMPLATES, (t.max_probe_script_sig_templates * static_cast<size_t>(cfg.underload_scale_percent)) / 100 + 1);
        } else if (t.max_outputs_scanned_per_tx < MAX_OUTPUTS_SCANNED_PER_TX) {
            t.max_outputs_scanned_per_tx = std::min(MAX_OUTPUTS_SCANNED_PER_TX, (t.max_outputs_scanned_per_tx * static_cast<size_t>(cfg.underload_scale_percent)) / 100 + 1);
        }
    }

    t.max_outputs_returned_per_tx = std::min(t.max_outputs_scanned_per_tx, std::max(MIN_OUTPUTS_RETURNED_PER_TX, t.max_outputs_returned_per_tx));
    const bool limits_changed =
        prev_scan != t.max_outputs_scanned_per_tx ||
        prev_return != t.max_outputs_returned_per_tx ||
        prev_eval_budget != t.max_probe_evals_per_tx ||
        prev_templates != t.max_probe_script_sig_templates;
    const int64_t now = GetTime();
    if (limits_changed || now - m_last_tuning_log_time >= 60) {
        LogPrint(BCLog::ANYONECANSPEND, "AnyoneCanSpend: tuning update mode=%s ema_us=%.1f target_us=%d exhaust_rate=%.2f limits(scan=%zu,return=%zu,eval_budget=%zu,templates=%zu)\n",
                 from_mempool ? "mempool" : "block", ema, target_us, exhaust_rate,
                 t.max_outputs_scanned_per_tx, t.max_outputs_returned_per_tx,
                 t.max_probe_evals_per_tx, t.max_probe_script_sig_templates);
        m_last_tuning_log_time = now;
    }
}

bool AnyoneCanSpendHandler::LookupDetectionCache(const CScript& script_pub_key, bool& is_anyone_can_spend, CScript* spend_script_sig) const
{
    const uint256 key = ScriptCacheKey(script_pub_key);
    std::lock_guard<std::mutex> lock(m_detection_cache_mutex);
    const auto it = m_detection_cache.find(key);
    if (it == m_detection_cache.end()) {
        return false;
    }
    is_anyone_can_spend = it->second.is_anyone_can_spend;
    if (is_anyone_can_spend && spend_script_sig) {
        *spend_script_sig = it->second.spend_script_sig;
    }
    return true;
}

void AnyoneCanSpendHandler::StoreDetectionCache(const CScript& script_pub_key, bool is_anyone_can_spend, const CScript& spend_script_sig)
{
    const uint256 key = ScriptCacheKey(script_pub_key);
    std::lock_guard<std::mutex> lock(m_detection_cache_mutex);
    auto [it, inserted] = m_detection_cache.emplace(key, DetectionCacheEntry{is_anyone_can_spend, spend_script_sig});
    if (!inserted) {
        it->second = DetectionCacheEntry{is_anyone_can_spend, spend_script_sig};
        return;
    }

    m_detection_cache_order.push_back(key);
    while (m_detection_cache.size() > MAX_DETECTION_CACHE_ENTRIES && !m_detection_cache_order.empty()) {
        const uint256 oldest = m_detection_cache_order.front();
        m_detection_cache_order.pop_front();
        m_detection_cache.erase(oldest);
    }
}

AnyoneCanSpendHandler::RuntimeMetrics AnyoneCanSpendHandler::GetRuntimeMetrics() const
{
    std::lock_guard<std::mutex> lock(m_tuning_mutex);
    RuntimeMetrics out;
    out.max_outputs_scanned_per_tx = m_tuning_state.max_outputs_scanned_per_tx;
    out.max_outputs_returned_per_tx = m_tuning_state.max_outputs_returned_per_tx;
    out.max_probe_evals_per_tx = m_tuning_state.max_probe_evals_per_tx;
    out.max_probe_script_sig_templates = m_tuning_state.max_probe_script_sig_templates;
    out.ema_elapsed_us_mempool = m_tuning_state.ema_elapsed_us_mempool;
    out.ema_elapsed_us_block = m_tuning_state.ema_elapsed_us_block;
    out.mempool_events = m_tuning_state.mempool_events;
    out.block_events = m_tuning_state.block_events;
    out.mempool_budget_exhaustions = m_tuning_state.mempool_budget_exhaustions;
    out.block_budget_exhaustions = m_tuning_state.block_budget_exhaustions;
    out.mempool_overruns = m_tuning_state.mempool_overruns;
    out.block_overruns = m_tuning_state.block_overruns;
    out.cache_hits = m_tuning_state.cache_hits;
    out.cache_misses = m_tuning_state.cache_misses;
    out.target_elapsed_us_mempool = m_tuning_config.target_elapsed_us_mempool;
    out.target_elapsed_us_block = m_tuning_config.target_elapsed_us_block;
    out.adjust_interval_events = m_tuning_config.adjust_interval_events;
    out.ema_alpha = m_tuning_config.ema_alpha;
    out.overload_exhaust_threshold = m_tuning_config.overload_exhaust_threshold;
    out.underload_exhaust_threshold = m_tuning_config.underload_exhaust_threshold;
    out.overload_scale_percent = m_tuning_config.overload_scale_percent;
    out.underload_scale_percent = m_tuning_config.underload_scale_percent;
    out.cleanup_interval_secs = m_tuning_config.cleanup_interval_secs;
    out.cleanup_stale_age_secs = m_tuning_config.cleanup_stale_age_secs;
    out.cleanup_max_candidates = m_tuning_config.cleanup_max_candidates;
    return out;
}

AnyoneCanSpendHandler::Stats AnyoneCanSpendHandler::GetStats() const
{
    std::lock_guard<std::mutex> lock(m_stats_mutex);
    return m_stats;
}

void AnyoneCanSpendHandler::LoadAutoTuneConfigFromArgs()
{
    auto clamp_double = [](double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); };
    auto clamp_i64 = [](int64_t v, int64_t lo, int64_t hi) { return std::max(lo, std::min(hi, v)); };
    auto clamp_u64 = [](uint64_t v, uint64_t lo, uint64_t hi) { return std::max(lo, std::min(hi, v)); };
    auto clamp_int = [](int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); };

    AutoTuneConfig cfg;
    cfg.target_elapsed_us_mempool = clamp_i64(gArgs.GetIntArg("-anyonecanspendtargetusmempool", cfg.target_elapsed_us_mempool), 100, 1'000'000);
    cfg.target_elapsed_us_block = clamp_i64(gArgs.GetIntArg("-anyonecanspendtargetusblock", cfg.target_elapsed_us_block), 100, 1'000'000);
    cfg.adjust_interval_events = clamp_u64(gArgs.GetIntArg("-anyonecanspendadjustintervalevents", cfg.adjust_interval_events), 1, 10'000);
    cfg.ema_alpha = clamp_double(gArgs.GetIntArg("-anyonecanspendemalphapct", static_cast<int64_t>(cfg.ema_alpha * 100.0)) / 100.0, 0.01, 1.0);
    cfg.overload_exhaust_threshold = clamp_double(gArgs.GetIntArg("-anyonecanspendoverloadexhaustpct", static_cast<int64_t>(cfg.overload_exhaust_threshold * 100.0)) / 100.0, 0.0, 1.0);
    cfg.underload_exhaust_threshold = clamp_double(gArgs.GetIntArg("-anyonecanspendunderloadexhaustpct", static_cast<int64_t>(cfg.underload_exhaust_threshold * 100.0)) / 100.0, 0.0, 1.0);
    cfg.overload_scale_percent = clamp_int(gArgs.GetIntArg("-anyonecanspendoverloadscalepct", cfg.overload_scale_percent), 10, 100);
    cfg.underload_scale_percent = clamp_int(gArgs.GetIntArg("-anyonecanspendunderloadscalepct", cfg.underload_scale_percent), 100, 200);
    cfg.cleanup_interval_secs = clamp_i64(gArgs.GetIntArg("-anyonecanspendcleanupintervalsecs", cfg.cleanup_interval_secs), 30, 86'400);
    cfg.cleanup_stale_age_secs = clamp_i64(gArgs.GetIntArg("-anyonecanspendcleanupstaleagesecs", cfg.cleanup_stale_age_secs), 60, 7 * 86'400);
    cfg.cleanup_max_candidates = clamp_i64(gArgs.GetIntArg("-anyonecanspendcleanupmaxcandidates", cfg.cleanup_max_candidates), 10, 50'000);

    {
        std::lock_guard<std::mutex> lock(m_tuning_mutex);
        m_tuning_config = cfg;
    }

    LogPrintf("AnyoneCanSpendHandler: Auto-tune config target_us(mempool=%lld,block=%lld) adjust_interval=%llu ema_alpha=%.2f exhaust(overload=%.2f,underload=%.2f) scale(overload=%d%%,underload=%d%%) cleanup(interval=%lld,stale_age=%lld,max=%lld)\n",
              (long long)cfg.target_elapsed_us_mempool, (long long)cfg.target_elapsed_us_block, (unsigned long long)cfg.adjust_interval_events, cfg.ema_alpha,
              cfg.overload_exhaust_threshold, cfg.underload_exhaust_threshold, cfg.overload_scale_percent, cfg.underload_scale_percent,
              (long long)cfg.cleanup_interval_secs, (long long)cfg.cleanup_stale_age_secs, (long long)cfg.cleanup_max_candidates);
}
