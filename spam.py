from bitcoinrpc.authproxy import AuthServiceProxy, JSONRPCException
from decimal import Decimal
import time

rpc_user = 'testuser'
rpc_password = 'mysecretpassword123'
rpc_port = '18332'

url = f"http://{rpc_user}:{rpc_password}@127.0.0.1:{rpc_port}/"
rpc = AuthServiceProxy(url)

send_amount = Decimal('0.000003')

def get_min_relay_fee(rpc):
    network_info = rpc.getnetworkinfo()
    return Decimal(network_info["relayfee"])

fee_rate = get_min_relay_fee(rpc) * 100_000_000  # satoshis per byte

def select_utxo(rpc, min_value):
    while True:
        unspent_outputs = rpc.listunspent(0)
        suitable_utxos = sorted([utxo for utxo in unspent_outputs if Decimal(utxo["amount"]) >= min_value], key=lambda x: x["amount"])
        if suitable_utxos: return suitable_utxos
        print("No suitable UTXOs. Waiting for 30 seconds before retrying.")
        time.sleep(30)

def create_send_transaction(rpc, destination, amount):
    change_address = rpc.getrawchangeaddress()

    while True:
        suitable_utxos = select_utxo(rpc, amount)
        for utxo in suitable_utxos:
            input_value = Decimal(utxo["amount"])
            inputs = [{"txid": utxo["txid"], "vout": utxo["vout"]}]
            outputs = {destination: float(amount)}
            raw_tx = rpc.createrawtransaction(inputs, outputs)
            signed_tx = rpc.signrawtransactionwithwallet(raw_tx)
            transaction_size = len(signed_tx["hex"]) // 2  # Calculate the transaction size in bytes
            fee = fee_rate * transaction_size / 100_000_000  # Calculate the fee in BTC
            change_value = input_value - amount - fee
            if change_value > 0:
                outputs[change_address] = float(change_value)
            raw_tx = rpc.createrawtransaction(inputs, outputs)
            signed_tx = rpc.signrawtransactionwithwallet(raw_tx)
            try:
                txid = rpc.sendrawtransaction(signed_tx["hex"])
                print(f"Sent {send_amount} BTC to address {destination} (TXID: {txid})")
                return True
            except JSONRPCException as e:
                if "too-long-mempool-chain" in str(e):
                    print(f"Failed to send transaction with UTXO {utxo['txid']}: {e}")
                    continue
                print(f"Failed to send transaction: {e}")
                return False

new_address = rpc.getnewaddress()
while True:
    for _ in range(50000):
        try:
            if create_send_transaction(rpc, new_address, send_amount):
                new_address = rpc.getnewaddress()
        except JSONRPCException as e:
            print(f"Failed to send transaction: {e}")
    time.sleep(5)

