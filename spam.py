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
            # Send 0.0000000246 BTC from the wallet to the new address
            txid = rpc_connection.sendtoaddress(new_address, 0.0000000246, "", "", True, "", 246)
            print(f"Sent 0.0000000246 BTC to address {new_address} (TXID: {txid})")
        except JSONRPCException as e:
            print(f"Failed to send transaction: {e}")

    # Wait for a few seconds to allow the transactions to propagate
    time.sleep(5)

