from bitcoinrpc.authproxy import AuthServiceProxy, JSONRPCException
from decimal import Decimal
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

send_amount = Decimal('0.000003')

# Create a new address for each transaction
while True:
    for i in range(50000):
        new_address = rpc_connection.getnewaddress()
        try:
            change_address = rpc_connection.getrawchangeaddress()
            # Create a raw transaction with no inputs and one output
            raw_tx = rpc_connection.createrawtransaction([], {new_address: send_amount})

            # Fund the raw transaction without adding the fee
            funded_tx = rpc_connection.fundrawtransaction(raw_tx, {"feeRate": 0, "changeAddress": change_address})

            # Decode the funded transaction
            decoded_tx = rpc_connection.decoderawtransaction(funded_tx["hex"])

            # Calculate the total input amount
            input_amount = Decimal(0)
            for input in decoded_tx["vin"]:
                tx_out = rpc_connection.gettxout(input["txid"], input["vout"])
                input_amount += Decimal(tx_out["value"])

            # Calculate the output amount
            output_amount = Decimal(0)
            for output in decoded_tx["vout"]:
                output_amount += Decimal(output["value"])

            # Calculate the fee (1 satoshi) and set the change output value
            fee = Decimal('0.00000001')
            change_output_value = input_amount - output_amount - fee

            # Update the change output value
            updated_outputs = {}
            for output in decoded_tx["vout"]:
                if "addresses" in output["scriptPubKey"]:
                    if output["scriptPubKey"]["addresses"][0] == change_address:
                        updated_outputs[change_address] = float(change_output_value)
                    else:
                        updated_outputs[new_address] = float(send_amount)

            # Create a new raw transaction with the modified output values
            updated_outputs_list = [{"address": k, "amount": v} for k, v in updated_outputs.items()]
            updated_raw_tx = rpc_connection.createrawtransaction(decoded_tx["vin"], updated_outputs_list)

            # Sign the updated raw transaction
            signed_tx = rpc_connection.signrawtransactionwithwallet(updated_raw_tx)

            # Send the signed transaction
            txid = rpc_connection.sendrawtransaction(signed_tx["hex"])
            print(f"Sent {send_amount} BTC to address {new_address} (TXID: {txid})")
        except JSONRPCException as e:
            print(f"Failed to send transaction: {e}")

    # Wait for a few seconds to allow the transactions to propagate
    time.sleep(5)

