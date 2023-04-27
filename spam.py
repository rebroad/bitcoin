from bitcoinrpc.authproxy import AuthServiceProxy, JSONRPCException
import time

class CustomAuthServiceProxy(AuthServiceProxy):
    def __init__(self, service_url, timeout=None):
        self.timeout = timeout
        AuthServiceProxy.__init__(self, service_url, timeout=timeout)

rpc_user = 'testuser'
rpc_password = 'mysecretpassword123'
rpc_port = '18332'

url = f"http://{rpc_user}:{rpc_password}@127.0.0.1:{rpc_port}/"
rpc_connection = CustomAuthServiceProxy(url)

# Create a new address for each transaction
while True:
    for i in range(50000):
        new_address = rpc_connection.getnewaddress()
        try:
            # Create a raw transaction with no inputs and one output
            raw_tx = rpc_connection.createrawtransaction([], {new_address: 0.000003})

            # Fund the raw transaction, setting the fee to 1 satoshi
            funded_tx = rpc_connection.fundrawtransaction(raw_tx, {"feeRate": 0, "subtractFeeFromOutputs": [0]})
            tx_fee = int(funded_tx["fee"] * 100000000)  # Convert to satoshis
            if tx_fee > 1:
                tx_fee = 1

            # Set the fee to 1 satoshi
            funded_tx = rpc_connection.fundrawtransaction(raw_tx, {"feeRate": 0, "replaceable": True, "subtractFeeFromOutputs": [0], "fee_amount": tx_fee / 100000000})

            # Sign the funded transaction
            signed_tx = rpc_connection.signrawtransactionwithwallet(funded_tx["hex"])

            # Send the signed transaction
            txid = rpc_connection.sendrawtransaction(signed_tx["hex"])
            print(f"Sent 0.000003 BTC to address {new_address} (TXID: {txid})")
        except JSONRPCException as e:
            print(f"Failed to send transaction: {e}")

    # Wait for a few seconds to allow the transactions to propagate
    time.sleep(5)
