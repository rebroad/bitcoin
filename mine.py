import time
from bitcoinrpc.authproxy import AuthServiceProxy

rpc_user = 'testuser'
rpc_password = 'mysecretpassword123'
rpc_port = '18332'

url = f"http://{rpc_user}:{rpc_password}@127.0.0.1:{rpc_port}/"
rpc_connection = AuthServiceProxy(url)

def get_unconfirmed_txids(rpc):
    unconfirmed_txids = []
    transactions = rpc.listtransactions("*", 1000)  # Adjust the count as needed
    for tx in transactions:
        if tx["category"] == "send" and tx["confirmations"] == 0:
            unconfirmed_txids.append(tx["txid"])
    return unconfirmed_txids

def mine_block_with_priority_txs(rpc):
    priority_txids = get_unconfirmed_txids(rpc)
    block_template = rpc.getblocktemplate({"rules": ["segwit"]})
    new_transactions = [tx for tx in block_template["transactions"] if tx["txid"] not in priority_txids]
    priority_transactions = [{"txid": txid, "weight": 0} for txid in priority_txids]
    block_template["transactions"] = priority_transactions + new_transactions

    response = rpc.generatetodescriptor(1, f"addr({rpc.getnewaddress()})")
    block_hash = response[0] if response else None
    return block_hash

while True:
    try:
        block_hash = mine_block_with_priority_txs(rpc_connection)
        print(f"Mined block: {block_hash}")
        time.sleep(10)  # Adjust the sleep interval as needed
    except Exception as e:
        print(f"Error: {e}")
        time.sleep(10)

