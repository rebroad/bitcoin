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
#include <memory>
#include <string>
#include <stack>
#include <vector>

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
    void AnyoneCanSpendTransactionAddedToMempool(const CTransactionRef& tx, uint64_t mempool_sequence, const std::vector<std::pair<size_t, CScript>>& anyone_can_spend_outputs) override;
    void AnyoneCanSpendTransactionInBlock(const CTransactionRef& tx, int block_height, const std::vector<std::pair<size_t, CScript>>& anyone_can_spend_outputs) override;
    void BlockConnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex) override;
    void TransactionAddedToMempool(const CTransactionRef& tx, uint64_t mempool_sequence) override;

private:
    std::string m_destination_address;
    wallet::WalletContext* m_wallet_context;
    std::shared_ptr<wallet::CWallet> m_anyone_wallet;
    bool m_auto_spend;
    Stats m_stats;
    mutable std::mutex m_stats_mutex;
    std::thread m_heartbeat_thread;
    std::atomic<bool> m_heartbeat_running{false};

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
     * @param outputs Vector of (outpoint, (amount, script)) pairs to spend
     * @return The transaction hash if successful, std::nullopt if failed
     */
    std::optional<uint256> CreateSpendTransactionFromWallet(
        std::shared_ptr<wallet::CWallet> wallet,
        const std::vector<std::pair<COutPoint, std::pair<CAmount, CScript>>>& outputs);
};

#endif // BITCOIN_ANYONE_CAN_SPEND_HANDLER_H
