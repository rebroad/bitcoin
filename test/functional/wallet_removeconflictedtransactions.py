#!/usr/bin/env python3
# Copyright (c) 2026 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test removeconflictedtransactions RPC."""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)


class RemoveConflictedTransactionsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        self.generate(self.nodes[0], 101)
        self.sync_blocks()

        txid_conflict_from = self.nodes[0].sendtoaddress(self.nodes[0].getnewaddress(), Decimal("10"))
        self.generate(self.nodes[0], 1)
        self.sync_blocks()

        self.disconnect_nodes(0, 1)

        n_a = next(tx_out["vout"] for tx_out in self.nodes[0].gettransaction(txid_conflict_from)["details"] if tx_out["category"] == "receive" and tx_out["amount"] == Decimal("10"))
        inputs = [{"txid": txid_conflict_from, "vout": n_a}]
        conflicted = self.nodes[0].signrawtransactionwithwallet(self.nodes[0].createrawtransaction(inputs, {self.nodes[0].getnewaddress(): Decimal("9.99998")}))
        conflicting = self.nodes[0].signrawtransactionwithwallet(self.nodes[0].createrawtransaction(inputs, {self.nodes[0].getnewaddress(): Decimal("9.99997")}))

        conflicted_txid = self.nodes[0].sendrawtransaction(conflicted["hex"])
        self.generate(self.nodes[0], 1, sync_fun=self.no_op)
        conflicting_txid = self.nodes[1].sendrawtransaction(conflicting["hex"])
        self.generate(self.nodes[1], 2, sync_fun=self.no_op)

        self.connect_nodes(0, 1)
        self.sync_blocks([self.nodes[0], self.nodes[1]])

        conflicted_tx = self.nodes[0].gettransaction(conflicted_txid)
        assert conflicted_tx["confirmations"] < 0
        assert_equal(conflicted_tx["walletconflicts"], [conflicting_txid])

        result = self.nodes[0].removeconflictedtransactions()
        assert_equal(result["removed"], 1)
        assert_equal(result["txids"], [conflicted_txid])

        assert_raises_rpc_error(-5, "Invalid or non-wallet transaction id", self.nodes[0].gettransaction, conflicted_txid)
        assert self.nodes[0].gettransaction(conflicting_txid)["confirmations"] > 0


if __name__ == '__main__':
    RemoveConflictedTransactionsTest().main()
