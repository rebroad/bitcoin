from bitcoinrpc.authproxy import AuthServiceProxy, JSONRPCException
from decimal import Decimal
import time

rpc_user = 'testuser'
rpc_password = 'mysecretpassword123'
rpc_port = '18332'

url = f"http://{rpc_user}:{rpc_password}@127.0.0.1:{rpc_port}/"
rpc_connection = AuthServiceProxy(url)

send_amount = Decimal('0.000003')

def create_send_transaction(rpc, destination, amount):
    change_address = rpc.getrawchangeaddress()
    raw_tx = rpc.createrawtransaction([], {destination: float(amount)})
    funded_tx = rpc.fundrawtransaction(raw_tx, {"feeRate": 0, "changeAddress": change_address})
    decoded_tx = rpc.decoderawtransaction(funded_tx["hex"])
    input_amount = sum(Decimal(tx_out["value"]) for vin in decoded_tx["vin"] for tx_out in [rpc.gettxout(vin["txid"], vin["vout"])])
    fee = Decimal('0.00000001')
    change_value = input_amount - amount - fee
    updated_outputs = {destination: float(amount), change_address: float(change_value)}
    updated_raw_tx = rpc.createrawtransaction(decoded_tx["vin"], updated_outputs)
    signed_tx = rpc.signrawtransactionwithwallet(updated_raw_tx)
    return rpc.sendrawtransaction(signed_tx["hex"])

while True:
    for _ in range(50000):
        new_address = rpc_connection.getnewaddress()
        try:
            txid = create_send_transaction(rpc_connection, new_address, send_amount)
            print(f"Sent {send_amount} BTC to address {new_address} (TXID: {txid})")
        except JSONRPCException as e:
            print(f"Failed to send transaction: {e}")
    time.sleep(5)

