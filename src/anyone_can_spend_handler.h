// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ANYONE_CAN_SPEND_HANDLER_H
#define BITCOIN_ANYONE_CAN_SPEND_HANDLER_H

#include <validationinterface.h>
#include <primitives/transaction.h>
#include <script/standard.h>
#include <key_io.h>
#include <wallet/wallet.h>
#include <node/context.h>
#include <util/system.h>
#include <limits>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <stack>
#include <vector>

namespace anyonecanspend {
struct ProbeLimits {
    size_t max_script_sig_templates{8};
    size_t* remaining_eval_budget{nullptr};
};

/**
 * Returns true if a scriptPubKey is spendable by anyone.
 * Witness programs (including Taproot and future SegWit versions) are excluded.
 * If provided, `spend_script_sig` receives one satisfying scriptSig candidate.
 */
bool IsAnyoneCanSpendScriptPubKey(const CScript& script_pub_key, CScript* spend_script_sig = nullptr, const ProbeLimits& limits = {});

/**
 * Find anyone-can-spend outputs in a transaction.
 * Returns vector of (vout index, satisfying scriptSig candidate).
 */
std::vector<std::pair<size_t, CScript>> FindAnyoneCanSpendOutputs(const CTransaction& tx, size_t max_outputs = std::numeric_limits<size_t>::max());
} // namespace anyonecanspend

/**
 * Handler for "anyone can spend" outputs.
 *
 * This class monitors for transactions with "anyone can spend" outputs and can
 * optionally create transactions to spend them to a specified address.
 *
 * WARNING: This is for educational/demonstration purposes. In a real-world scenario,
 * you would want additional security measures and proper wallet management.
 */
class AnyoneCanSpendHandler : public CValidationInterface
{
public:
    AnyoneCanSpendHandler();
    ~AnyoneCanSpendHandler();

    /**
     * Initialize the handler with a destination address and wallet context.
     *
     * @param destination_address The address to send "anyone can spend" outputs to
     * @param wallet_context The wallet context for creating and managing the "Anyone" wallet
     * @param auto_spend Whether to automatically spend outputs when detected
     */
    void Initialize(const std::string& destination_address,
                   wallet::WalletContext* wallet_context,
                   bool auto_spend = false);

    /**
     * Set whether to automatically spend outputs when detected.
     */
    void SetAutoSpend(bool auto_spend) { m_auto_spend = auto_spend; }

    /**
     * Get the current auto-spend setting.
     */
    bool GetAutoSpend() const { return m_auto_spend; }

    /**
     * Check if the handler is enabled (has valid destination address).
     */
    bool IsEnabled() const { return m_handler_enabled; }

    /**
     * Get the destination address.
     */
    std::string GetDestinationAddress() const { return m_destination_address; }

    /**
     * Get statistics about handled outputs.
     */
    struct Stats {
        uint64_t outputs_detected = 0;
        uint64_t outputs_spent = 0;
        CAmount total_amount_detected = 0;
        CAmount total_amount_spent = 0;
        uint64_t transactions_evaluated_blocks = 0;
        uint64_t transactions_evaluated_mempool = 0;
    };

    Stats GetStats() const { return m_stats; }

    /**
     * Get the destination address from configuration.
     * Returns the address specified in bitcoin.conf or empty string if not set.
     */
    std::string GetDestinationAddressFromConfig() const;

    /**
     * Start the heartbeat thread for periodic logging.
     */
    void StartHeartbeat();

    /**
     * Stop the heartbeat thread.
     */
    void StopHeartbeat();

    /**
     * Get or create the "Anyone" wallet.
     * Returns the wallet instance or nullptr if failed.
     */
    std::shared_ptr<wallet::CWallet> GetAnyoneWallet();

protected:
    // CValidationInterface overrides
    void BlockConnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex) override;
    void TransactionAddedToMempool(const CTransactionRef& tx, uint64_t mempool_sequence) override;

private:
    struct SpendableAnyoneOutput {
        COutPoint outpoint;
        CAmount amount;
        CScript script_pub_key;
        CScript script_sig;
    };

    std::string m_destination_address;
    CTxDestination m_cached_destination; // Cached decoded destination address
    CScript m_cached_output_script; // Cached output script for the destination
    bool m_handler_enabled; // Whether the handler is enabled (valid destination required)
    wallet::WalletContext* m_wallet_context;
    
    // Pre-calculated transaction sizes for different script signature types
    struct CachedTxSizes {
        unsigned int empty_script_size;    // For CScript() (0 bytes)
        unsigned int op1_script_size;      // For CScript() << OP_1 (1 byte)
        unsigned int op0_script_size;      // For CScript() << OP_0 (1 byte)
    };
    CachedTxSizes m_cached_tx_sizes;
    std::shared_ptr<wallet::CWallet> m_anyone_wallet;
    bool m_auto_spend;
    Stats m_stats;
    mutable std::mutex m_stats_mutex;
    std::thread m_heartbeat_thread;
    std::atomic<bool> m_heartbeat_running{false};
    struct DetectionCacheEntry {
        bool is_anyone_can_spend{false};
        CScript spend_script_sig;
    };
    struct RuntimeTuning {
        size_t max_outputs_scanned_per_tx{32};
        size_t max_outputs_returned_per_tx{8};
        size_t max_probe_evals_per_tx{192};
        size_t max_probe_script_sig_templates{8};
        double ema_elapsed_us_mempool{0.0};
        double ema_elapsed_us_block{0.0};
        uint64_t mempool_events{0};
        uint64_t block_events{0};
        uint64_t mempool_budget_exhaustions{0};
        uint64_t block_budget_exhaustions{0};
        uint64_t mempool_overruns{0};
        uint64_t block_overruns{0};
        uint64_t cache_hits{0};
        uint64_t cache_misses{0};
        uint64_t mempool_events_since_adjust{0};
        uint64_t block_events_since_adjust{0};
    };
    mutable std::mutex m_tuning_mutex;
    RuntimeTuning m_tuning_state;
    mutable std::mutex m_detection_cache_mutex;
    std::map<uint256, DetectionCacheEntry> m_detection_cache;
    std::deque<uint256> m_detection_cache_order;
    static constexpr size_t MAX_DETECTION_CACHE_ENTRIES{4096};

    /**
     * Process "anyone can spend" outputs from a transaction.
     * Adds them to the "Anyone" wallet and optionally spends them.
     *
     * @param tx The transaction containing "anyone can spend" outputs
     * @param anyone_can_spend_outputs Vector of (output_index, working_script_sig) pairs
     * @param state The transaction state (mempool or confirmed)
     * @return The transaction hash if spending was successful, std::nullopt if failed or no outputs
     */
    std::optional<uint256> ProcessAnyoneCanSpendOutputs(const CTransactionRef& tx, const std::vector<std::pair<size_t, CScript>>& anyone_can_spend_outputs, const wallet::TxState& state);

    /**
     * Create a spending transaction using the wallet's transaction creation.
     *
     * @param wallet The wallet to use for transaction creation
     * @param outputs Vector of spendable anyone-can-spend outputs
     * @return The transaction hash if successful, std::nullopt if failed
     */
    std::optional<uint256> CreateSpendTransactionFromWallet(
        std::shared_ptr<wallet::CWallet> wallet,
        const std::vector<SpendableAnyoneOutput>& outputs);

    /**
     * Find anyone-can-spend outputs in a transaction.
     *
     * @param tx The transaction to check
     * @return Vector of (output_index, working_script_sig) pairs
     */
    std::vector<std::pair<size_t, CScript>> FindAnyoneCanSpendOutputs(const CTransaction& tx, bool from_mempool, bool* budget_exhausted = nullptr);

    bool LookupDetectionCache(const CScript& script_pub_key, bool& is_anyone_can_spend, CScript* spend_script_sig) const;
    void StoreDetectionCache(const CScript& script_pub_key, bool is_anyone_can_spend, const CScript& spend_script_sig);
    void UpdateRuntimeTuning(bool from_mempool, int64_t elapsed_us, bool budget_exhausted);
};

#endif // BITCOIN_ANYONE_CAN_SPEND_HANDLER_H
