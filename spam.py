from bitcoinrpc.authproxy import AuthServiceProxy, JSONRPCException
from decimal import Decimal
import time

rpc_user = 'testuser'
rpc_password = 'mysecretpassword123'
rpc_port = '18332'

url = f"http://{rpc_user}:{rpc_password}@127.0.0.1:{rpc_port}/"
rpc = AuthServiceProxy(url)

send_amount = Decimal('0.000003')

def wait_for_new_block(rpc, timeout=600):
    start_height = rpc.getblockcount()
    start_time = time.time()

    while True:
        current_height = rpc.getblockcount()
        if current_height > start_height:
            break
        if time.time() - start_time > timeout:
            break
        time.sleep(5)

def create_send_transaction(rpc, destination, amount):
    change_address = rpc.getrawchangeaddress()
    inputs = []
    outputs = {destination: float(amount)}

    raw_tx = rpc.createrawtransaction(inputs, outputs)
    options = {"changeAddress": change_address, "includeWatching": True}

    try:
        funded_tx = rpc.fundrawtransaction(raw_tx, options)
        signed_tx = rpc.signrawtransactionwithwallet(funded_tx["hex"])
        txid = rpc.sendrawtransaction(signed_tx["hex"])
        print(f"Sent {send_amount} BTC to address {destination} (TXID: {txid})")
        return True
    except JSONRPCException as e:
        if "too-long-mempool-chain" in str(e):
            print("Mempool chain limit reached. Waiting for the next block...")
            wait_for_new_block(rpc)
            return False
        else:
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

