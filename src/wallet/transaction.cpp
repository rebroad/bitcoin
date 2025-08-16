// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/transaction.h>

namespace wallet {
bool CWalletTx::IsEquivalentTo(const CWalletTx& _tx) const
{
        CMutableTransaction tx1 {*this->tx};
        CMutableTransaction tx2 {*_tx.tx};
        for (auto& txin : tx1.vin) txin.scriptSig = CScript();
        for (auto& txin : tx2.vin) txin.scriptSig = CScript();
        return CTransaction(tx1) == CTransaction(tx2);
}

bool CWalletTx::InMempool() const
{
    return state<TxStateInMempool>();
}

int64_t CWalletTx::GetTxTime() const
{
    int64_t n = nTimeSmart;
    if (n) return n;

    // Convert milliseconds to seconds if needed
    // New transactions use milliseconds, old ones use seconds
    if (nTimeReceived >= 1000000000000) {
        return nTimeReceived / 1000;
    }
    return nTimeReceived;
}
} // namespace wallet
