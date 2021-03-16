#!/usr/bin/env python3
# Copyhigh (c) 2016-2017 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test reconciliation-based transaction relay protocol."""

from io import BytesIO
from test_framework.siphash import siphash256
import random
import struct
import time

from test_framework.key import TaggedHash
from test_framework.messages import (
    msg_inv, msg_getdata, msg_wtxidrelay,
    msg_verack, msg_sendrecon, msg_reqrecon,
    msg_sketch, msg_reqsketchext, msg_reconcildiff,
    MSG_WTX, MSG_BLOCK, CTransaction, CInv,
)
from test_framework.p2p import P2PDataStore
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, hex_str_to_bytes

# These parameters are specified in the BIP-0330.
Q_PRECISION = (2 << 14) - 1
FIELD_BITS = 32
FIELD_MODULUS = (1 << FIELD_BITS) + 0b10001101
BYTES_PER_SKETCH_CAPACITY = FIELD_BITS / 8

# These parameters are suggested by the Erlay paper based on analysis and
# simulations.
RECON_Q = 0.01
RECON_REQUEST_INTERVAL = 2
EXTRA_FANOUT_DESTINATIONS = 4
INBOUND_PENALIZED_INVENTORY_BROADCAST_INTERVAL = 5


def mul2(x):
    """Compute 2*x in GF(2^FIELD_BITS)"""
    return (x << 1) ^ (FIELD_MODULUS if x.bit_length() >= FIELD_BITS else 0)


def mul(x, y):
    """Compute x*y in GF(2^FIELD_BITS)"""
    ret = 0
    for bit in [(x >> i) & 1 for i in range(x.bit_length())]:
        ret, y = ret ^ bit * y, mul2(y)
    return ret


def create_sketch(shortids, capacity):
    """Compute the bytes of a sketch for given shortids and given capacity."""
    odd_sums = [0 for _ in range(capacity)]
    for shortid in shortids:
        squared = mul(shortid, shortid)
        for i in range(capacity):
            odd_sums[i] ^= shortid
            shortid = mul(shortid, squared)
    sketch_bytes = []
    for odd_sum in odd_sums:
        for i in range(4):
            sketch_bytes.append((odd_sum >> (i * 8)) & 0xff)
    return sketch_bytes


def get_short_id(tx, salt):
    (k0, k1) = salt
    wtxid = tx.calc_sha256(with_witness=True)
    s = siphash256(k0, k1, wtxid)
    return 1 + (s & 0xFFFFFFFF)


def estimate_capacity(theirs, ours=1):
    capacity = int(abs(theirs - ours) + RECON_Q * min(theirs, ours)) + 1
    if capacity < 9:
        # Poor man's minisketch_compute_capacity.
        capacity += 1
    return capacity


def generate_transaction(node, from_txid):
    to_address = node.getnewaddress()
    inputs = [{"txid": from_txid, "vout": 0}]
    outputs = {to_address: 0.0001}
    rawtx = node.createrawtransaction(inputs, outputs)
    signresult = node.signrawtransactionwithwallet(rawtx)
    tx = CTransaction()
    tx.deserialize(BytesIO(hex_str_to_bytes(signresult['hex'])))
    tx.rehash()
    return tx


class TestP2PConn(P2PDataStore):
    def __init__(self, recon_version, mininode_salt, be_initiator=False,
                 be_responder=False):
        super().__init__()
        self.recon_version = recon_version
        self.mininode_salt = mininode_salt
        self.be_initiator = be_initiator
        self.be_responder = be_responder
        self.node_salt = 0
        self.last_sendrecon = []
        self.last_sketch = []
        self.last_inv = []
        self.last_tx = []
        self.last_reqreconcil = []
        self.last_reconcildiff = []
        self.last_reqsketchext = []
        self.last_getdata = []
        self.last_wtxidrelay = []

    def on_version(self, message):
        if self.recon_version == 1:
            assert message.nVersion >= 70017, "We expect the node to support reconciliations"
            self.send_message(msg_wtxidrelay())
            self.send_sendrecon(self.be_initiator, self.be_responder)
            self.send_message(msg_verack())
            self.nServices = message.nServices
        else:
            super().on_version(message)

    def on_sendrecon(self, message):
        self.last_sendrecon.append(message)
        self.node_salt = message.salt

    def on_wtxidrelay(self, message):
        self.last_wtxidrelay.append(message)

    def on_sketch(self, message):
        self.last_sketch.append(message)

    def on_inv(self, message):
        for inv in message.inv:
            if inv.type != MSG_BLOCK:  # ignore block invs
                self.last_inv.append(inv.hash)

    def on_tx(self, message):
        self.last_tx.append(message.tx.calc_sha256(True))

    def on_reqrecon(self, message):
        self.last_reqreconcil.append(message)

    def on_reqsketchext(self, message):
        self.last_reqsketchext.append(message)

    def on_reconcildiff(self, message):
        self.last_reconcildiff.append(message)

    def send_sendrecon(self, sender, responder):
        msg = msg_sendrecon()
        msg.salt = self.mininode_salt
        msg.version = self.recon_version
        msg.sender = sender
        msg.responder = responder
        self.send_message(msg)

    def send_reqrecon(self, set_size, q):
        msg = msg_reqrecon()
        msg.set_size = set_size
        msg.q = q
        self.send_message(msg)

    def send_sketch(self, skdata):
        msg = msg_sketch()
        msg.skdata = skdata
        self.send_message(msg)

    def send_reqsketchext(self):
        msg = msg_reqsketchext()
        self.send_message(msg)

    def send_reconcildiff(self, success, ask_shortids):
        msg = msg_reconcildiff()
        msg.success = success
        msg.ask_shortids = ask_shortids
        self.send_message(msg)

    def send_inv(self, inv_wtxids):
        msg = msg_inv(inv=[CInv(MSG_WTX, h=wtxid) for wtxid in inv_wtxids])
        self.send_message(msg)

    def send_getdata(self, ask_wtxids):
        msg = msg_getdata(inv=[CInv(MSG_WTX, h=wtxid) for wtxid in ask_wtxids])
        self.send_message(msg)


class ReconciliationTest(BitcoinTestFramework):
    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def proceed_in_time(self, jump_in_seconds):
        # We usually need the node to process some messages first.
        time.sleep(0.01)

        self.mocktime += jump_in_seconds
        self.nodes[0].setmocktime(self.mocktime)

    def generate_txs(self, n_mininode_unique, n_node_unique, n_shared):
        mininode_unique = []
        node_unique = []
        shared = []

        utxos = [u for u in self.nodes[0].listunspent(1) if u['spendable']]

        for i in range(n_mininode_unique):
            tx = generate_transaction(self.nodes[0], utxos[i]['txid'])
            mininode_unique.append(tx)

        for i in range(n_mininode_unique, n_mininode_unique + n_node_unique):
            tx = generate_transaction(self.nodes[0], utxos[i]['txid'])
            node_unique.append(tx)

        for i in range(n_mininode_unique + n_node_unique,
                       n_mininode_unique + n_node_unique + n_shared):
            tx = generate_transaction(self.nodes[0], utxos[i]['txid'])
            shared.append(tx)

        tx_submitter = self.nodes[0].add_p2p_connection(P2PDataStore())
        tx_submitter.wait_for_verack()
        tx_submitter.send_txs_and_test(
            node_unique + shared, self.nodes[0], success=True)
        tx_submitter.peer_disconnect()

        time.sleep(0.01)  # give time to register txs

        return mininode_unique, node_unique, shared

    def compute_salt(self):
        RECON_STATIC_SALT = "Tx Relay Salting"
        salt1, salt2 = self.test_node.node_salt, self.test_node.mininode_salt
        if salt1 > salt2:
            salt1, salt2 = salt2, salt1
        salt = struct.pack("<Q", salt1) + struct.pack("<Q", salt2)
        h = TaggedHash(RECON_STATIC_SALT, salt)
        k0 = int.from_bytes(h[0:8], "little")
        k1 = int.from_bytes(h[8:16], "little")
        return k0, k1

    def expect_sketch(self, n_mininode, expected_transactions, extension):
        self.proceed_in_time(10)

        def received_sketch():
            return (len(self.test_node.last_sketch) == 1)
        self.wait_until(received_sketch, timeout=30)

        expected_short_ids = [get_short_id(
            tx, self.compute_salt()) for tx in expected_transactions]

        if extension:
            expected_capacity = estimate_capacity(
                n_mininode, len(expected_transactions))

            expected_sketch = create_sketch(
                expected_short_ids, expected_capacity * 2)[
                int(expected_capacity * BYTES_PER_SKETCH_CAPACITY):]
        else:
            if n_mininode == 0 or len(expected_transactions) == 0:
                # A peer should send us an empty sketch to trigger reconciliation termination.
                expected_capacity = 0
            else:
                expected_capacity = estimate_capacity(
                    n_mininode, len(expected_transactions))

            expected_sketch = create_sketch(
                expected_short_ids, expected_capacity)

        assert_equal(
            self.test_node.last_sketch[-1].skdata, expected_sketch)
        self.test_node.last_sketch = []

    def transmit_sketch(self, txs_to_sketch, extension, capacity):
        short_txids = [get_short_id(tx, self.compute_salt())
                       for tx in txs_to_sketch]
        if extension:
            sketch = create_sketch(
                short_txids, capacity * 2)[int(capacity * BYTES_PER_SKETCH_CAPACITY):]
        else:
            self.test_node.last_full_mininode_size = len(txs_to_sketch)
            sketch = create_sketch(short_txids, capacity)
        self.test_node.send_sketch(sketch)

    def handle_reconciliation_finalization(self, expected_success,
                                           expected_requested_txs,
                                           might_be_requested_txs):
        expected_requested_shortids = [get_short_id(
            tx, self.compute_salt()) for tx in expected_requested_txs]

        def received_reconcildiff():
            return (len(self.test_node.last_reconcildiff) == 1)
        self.wait_until(received_reconcildiff, timeout=30)
        success = self.test_node.last_reconcildiff[0].success
        assert_equal(success, int(expected_success))
        # They could ask for more, if they didn't add some transactions to the reconciliation set,
        # but set it to low-fanout to us instead.
        assert(set(expected_requested_shortids).
               issubset(set(self.test_node.last_reconcildiff[0].ask_shortids)))
        extra_requested_shortids = set(
            self.test_node.last_reconcildiff[0].ask_shortids) - set(expected_requested_shortids)
        might_be_requested_shortids = [get_short_id(
            tx, self.compute_salt()) for tx in might_be_requested_txs]
        assert(extra_requested_shortids.issubset(might_be_requested_shortids))
        self.test_node.last_reconcildiff = []

    def handle_extension_request(self):
        def received_reqsketchext():
            return (len(self.test_node.last_reqsketchext) == 1)
        self.wait_until(received_reqsketchext, timeout=30)
        self.test_node.last_reqsketchext = []

    def finalize_reconciliation(self, success, txs_to_request):
        ask_shortids = [get_short_id(tx, self.compute_salt())
                        for tx in txs_to_request]
        self.test_node.send_reconcildiff(success, ask_shortids)

    def request_transactions(self, txs_to_request):
        # Make sure there were no unexpected transactions received before
        assert_equal(self.test_node.last_tx, [])

        wtxids_to_request = [tx.calc_sha256(
            with_witness=True) for tx in txs_to_request]
        self.test_node.send_getdata(wtxids_to_request)

        def received_tx():
            return (len(self.test_node.last_tx) == len(txs_to_request))
        self.wait_until(received_tx, timeout=30)
        assert_equal(set([tx.calc_sha256(True)
                     for tx in txs_to_request]), set(self.test_node.last_tx))
        self.test_node.last_tx = []

    def receive_reqreconcil(self, expected_set_size):
        for _ in range(EXTRA_FANOUT_DESTINATIONS + 1):
            time.sleep(0.1)  # give time to issue other recon requests
            self.proceed_in_time(RECON_REQUEST_INTERVAL + 1)

        def received_reqreconcil():
            return (len(self.test_node.last_reqreconcil) >= 1)
        self.wait_until(received_reqreconcil, timeout=30)

        if self.test_node.last_reqreconcil[-1].set_size == 0 and expected_set_size != 0:
            # Sometimes transactions are added to the reconciliation sets
            # after the reqrecon message is sent out by the peer. In that case
            # we need to immediately terminate this reconciliation by sending
            # an empty sketch, and repeat the experiment.
            self.test_node.last_reqreconcil = []
            self.transmit_sketch(txs_to_sketch=[],
                                 extension=False, capacity=0)
            self.handle_reconciliation_finalization(expected_success=False,
                                                    expected_requested_txs=[],
                                                    might_be_requested_txs=[])

            return False
        else:
            # Some of the transactions are set for low-fanout to the mininode, so they won't be
            # in the set. The caller should check that they are indeed announced later.
            assert(
                self.test_node.last_reqreconcil[-1].set_size <= expected_set_size)
            self.test_node.last_reqreconcil = []
            return True

    #
    # Actual test cases
    #

    def handle_announcements(self):
        time.sleep(0.3)
        self.proceed_in_time(30)
        # We are not sure that we will receive any announcements here, that's why wait_until
        # does not fit here.
        time.sleep(0.3)
        announced_invs = []
        if len(self.test_node.last_inv) > 0:
            announced_invs = self.test_node.last_inv
            self.test_node.last_inv = []
        return announced_invs

    def reconciliation_responder_flow(self, n_mininode, n_node, initial_result,
                                      result):
        # Generate transactions
        _, node_txs, _ = self.generate_txs(n_mininode, n_node, 0)
        more_node_txs = []
        announced_invs = []

        # Some transactions would be announced via fanout, so they won't be included in the
        # sketch.
        announced_invs += self.handle_announcements()
        node_txs = [tx for tx in node_txs if tx.calc_sha256(
            with_witness=True) not in announced_invs]

        self.test_node.send_reqrecon(n_mininode, int(RECON_Q * Q_PRECISION))
        self.expect_sketch(n_mininode, node_txs, False)

        more_node_txs.extend(self.generate_txs(0, 8, 0)[1])

        announced_invs += self.handle_announcements()
        node_txs = [tx for tx in node_txs if tx.calc_sha256(
            with_witness=True) not in announced_invs]

        if not initial_result:
            self.test_node.send_reqsketchext()
            self.expect_sketch(n_mininode, node_txs, True)

        more_node_txs.extend(self.generate_txs(0, 8, 0)[1])

        if result:
            txs_to_request = random.sample(node_txs, 3)
            expected_wtxids = [tx.calc_sha256(
                with_witness=True) for tx in txs_to_request]

            self.finalize_reconciliation(True, txs_to_request)

            def received_inv():
                # These may include extra transactions: in addition to the expected diff,
                # a peer might INV us the shared transactions they selected us for low-fanout
                # instead of reconciling.
                return (set(expected_wtxids).issubset(set(self.test_node.last_inv)))

            self.proceed_in_time(
                INBOUND_PENALIZED_INVENTORY_BROADCAST_INTERVAL + 1)
            self.wait_until(received_inv, timeout=30)
            # check that those extra INVs here are actually from former transactions
            # being low-fanouted to the node.
            announced_invs += self.test_node.last_inv
            self.test_node.last_inv = []
        else:
            self.finalize_reconciliation(True, txs_to_request=[])

        # Check those additional transactions are not lost.
        self.test_node.send_reqrecon(1, int(RECON_Q * Q_PRECISION))
        announced_invs += self.handle_announcements()
        more_node_txs = [tx for tx in more_node_txs if tx.calc_sha256(
            with_witness=True) not in announced_invs]
        self.expect_sketch(1, more_node_txs, False)
        self.finalize_reconciliation(True, txs_to_request=[])

    def make_or_reset_reconciliation_conn(self, node_initiator):
        be_initiator = not node_initiator
        be_responder = node_initiator
        outbound = node_initiator
        self.test_node = self.nodes[0].add_p2p_connection(TestP2PConn(1,
                                                                      random.randrange(
                                                                          0xffffff),
                                                                      be_initiator,
                                                                      be_responder),
                                                          node_outgoing=outbound)
        self.test_node.wait_for_verack()

    def test_recon_responder(self):
        self.make_or_reset_reconciliation_conn(False)

        # These node will consume some of the low-fanout announcements.
        for _ in range(EXTRA_FANOUT_DESTINATIONS):
            fanout_consumer = self.nodes[0].add_p2p_connection(TestP2PConn(1,
                                                                           random.randrange(
                                                                               0xffffff),
                                                                           True,
                                                                           False),
                                                               node_outgoing=False)
            fanout_consumer.wait_for_verack()

        # 0 at mininode, 20 at node, 0 shared, early exit, expect empty sketch.
        self.reconciliation_responder_flow(0, 20, True, False)
        # 20 at mininode, 0 at node, 0 shared, early exit, expect empty sketch.
        self.reconciliation_responder_flow(20, 0, True, False)
        # Initial reconciliation succeeds
        self.reconciliation_responder_flow(10, 20, True, False)
        # Initial reconciliation fails, extension succeeds
        self.reconciliation_responder_flow(10, 20, False, True)
        # Initial reconciliation fails, extension fails
        self.reconciliation_responder_flow(10, 20, False, False)
        # Test disconnect on RECONCILDIFF violation
        self.make_or_reset_reconciliation_conn(False)
        self.finalize_reconciliation(True, [])
        self.test_node.wait_for_disconnect()

    def expect_announcements(self, expected_txs):
        expected_wtxids = [tx.calc_sha256(True)
                           for tx in expected_txs]
        self.wait_until(lambda: (set(expected_wtxids).issubset(
            set(self.test_node.last_inv))), timeout=30)
        # TODO check that rest of invs are shared transactions
        self.test_node.last_inv = []

    def reconciliation_initiator_flow(self, n_node, n_mininode, n_shared,
                                      capacity, terminate_after_initial,
                                      expected_success):

        # Generate and submit transactions.
        mininode_unique_txs, node_unique_txs, shared_txs = self.generate_txs(
            n_mininode, n_node, n_shared)
        mininode_txs = mininode_unique_txs + shared_txs
        node_txs = node_unique_txs + shared_txs

        # If we received an empty-set reconciliation request from the first time, we should
        # do this one more time. It's guaranteed to work from the second attempt.
        if not self.receive_reqreconcil(expected_set_size=len(node_txs)):
            self.expect_announcements(node_txs + shared_txs)
            mininode_unique_txs, node_unique_txs, shared_txs = self.generate_txs(
                n_mininode, n_node, n_shared)
            mininode_txs = mininode_unique_txs + shared_txs
            node_txs = node_unique_txs + shared_txs
            assert(self.receive_reqreconcil(
                expected_set_size=len(node_txs)))

        more_node_txs = []
        if terminate_after_initial:
            self.transmit_sketch(txs_to_sketch=mininode_txs,
                                 extension=False, capacity=capacity)

            self.expect_announcements(node_unique_txs)

            more_node_txs.extend(self.generate_txs(0, 10, 0)[1])
            if expected_success:
                self.handle_reconciliation_finalization(expected_success=True,
                                                        expected_requested_txs=mininode_unique_txs,
                                                        might_be_requested_txs=shared_txs)
            else:
                # This happens only if one of the sets (or both) was empty.
                self.handle_reconciliation_finalization(expected_success=False,
                                                        expected_requested_txs=[],
                                                        might_be_requested_txs=[])
        else:
            self.transmit_sketch(txs_to_sketch=mininode_txs,
                                 extension=False, capacity=capacity)
            more_node_txs.extend(self.generate_txs(0, 4, 0)[1])
            self.handle_extension_request()
            more_node_txs.extend(self.generate_txs(0, 4, 0)[1])
            if expected_success:
                self.transmit_sketch(txs_to_sketch=mininode_txs,
                                     extension=True, capacity=capacity)

                self.handle_reconciliation_finalization(expected_success=True,
                                                        expected_requested_txs=mininode_unique_txs,
                                                        might_be_requested_txs=shared_txs)
            else:
                self.transmit_sketch(txs_to_sketch=mininode_txs,
                                     extension=True, capacity=capacity)
                self.handle_reconciliation_finalization(expected_success=False,
                                                        expected_requested_txs=[],
                                                        might_be_requested_txs=[])

        self.request_transactions(node_unique_txs)

        # Check those additional transactions are not lost.
        if more_node_txs != []:
            if self.receive_reqreconcil(expected_set_size=len(more_node_txs)):
                # If we received a non empty-set reconciliation request, terminate the round.
                self.transmit_sketch(
                    txs_to_sketch=[], extension=False, capacity=0)
                self.handle_reconciliation_finalization(expected_success=False,
                                                        expected_requested_txs=[],
                                                        might_be_requested_txs=[])

            # Check that those extra transactions are announced (either via low-fanout,
            # or via either of the 2 possible reconciliation rounds).
            self.expect_announcements(more_node_txs)

    def test_recon_initiator(self):
        # These node will consume some of the low-fanout announcements, and add to the
        # reconciliation peers queue.
        for _ in range(EXTRA_FANOUT_DESTINATIONS):
            fanout_destination = self.nodes[0].add_p2p_connection(TestP2PConn(1,
                                                                              random.randrange(
                                                                                  0xffffff),
                                                                              False,
                                                                              True),
                                                                  node_outgoing=True)
            fanout_destination.wait_for_verack()

        self.make_or_reset_reconciliation_conn(True)
        # 20 at node, 0 at mininode, 0 shared, early exit.
        self.reconciliation_initiator_flow(20, 0, 0, 0, True, False)
        # 0 at node, 20 at mininode, 0 shared, early exit.
        self.reconciliation_initiator_flow(0, 20, 0, 0, True, False)
        # 20 at node, 20 at mininode, 10 shared, initial reconciliation succeeds
        self.reconciliation_initiator_flow(20, 20, 10, 54, True, True)
        # 20 at node, 20 at mininode, 10 shared, initial reconciliation fails,
        # extension succeeds
        self.reconciliation_initiator_flow(20, 20, 10, 27, False, True)
        # 20 at node, 20 at mininode, 10 shared, initial reconciliation fails,
        # extension fails
        self.reconciliation_initiator_flow(20, 20, 10, 10, False, False)

        # Test disconnect on SKETCH violation by exceeding max sketch capacity
        MAX_SKETCH_CAPACITY = 2 << 12
        self.make_or_reset_reconciliation_conn(True)
        serialized_size = (MAX_SKETCH_CAPACITY) * 4 + 1
        self.test_node.send_sketch([1 * serialized_size])
        self.test_node.wait_for_disconnect()

    def run_test(self):
        self.mocktime = int(time.time())
        self.nodes[0].setmocktime(self.mocktime)
        self.blocks = self.nodes[0].generate(nblocks=512)
        self.sync_blocks()

        self.test_recon_initiator()
        self.restart_node(0)
        self.test_recon_responder()


if __name__ == '__main__':
    ReconciliationTest().main()
