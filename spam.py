from bitcoinrpc.authproxy import AuthServiceProxy, JSONRPCException
import time
import ssl
import os
import http.client

class CustomAuthServiceProxy(AuthServiceProxy):
    def __init__(self, service_url, context=None, timeout=None):
        if context is not None:
            if not isinstance(context, ssl.SSLContext):
                raise TypeError("context must be an instance of ssl.SSLContext")
            self.context = context
        self.timeout = timeout
        AuthServiceProxy.__init__(self, service_url, timeout=timeout)

    def _get_connection(self):
        if self.context is not None:
            return http.client.HTTPSConnection(
                self.service_url.hostname, self.service_url.port, context=self.context, timeout=self.timeout
            )
        else:
            return http.client.HTTPConnection(
                self.service_url.hostname, self.service_url.port, timeout=self.timeout
            )

rpc_user = 'testuser'
rpc_password = 'mysecretpassword123'
rpc_port = '18332'

os.environ['SSL_CERT_FILE'] = '/etc/ssl/certs/ca-certificates.crt'
url = f"https://{rpc_user}:{rpc_password}@127.0.0.1:{rpc_port}/"
ssl_context = ssl.create_default_context()
ssl_context.options |= ssl.OP_NO_TLSv1 | ssl.OP_NO_TLSv1_1
rpc_connection = CustomAuthServiceProxy(url, context=ssl_context)

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

