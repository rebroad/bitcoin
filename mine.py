def mine_blocks(rpc, num_blocks, address=None):
    try:
        if address is None:
            block_hashes = rpc.generate(num_blocks)
        else:
            block_hashes = rpc.generatetoaddress(num_blocks, address)
        return block_hashes
    except JSONRPCException as e:
        print(f"Failed to mine blocks: {e}")
        return []

# Replace the values below with your own configuration
rpc_user = 'testuser'
rpc_password = 'mysecretpassword123'
rpc_port = '18332'

url = f"http://{rpc_user}:{rpc_password}@127.0.0.1:{rpc_port}/"
rpc_connection = AuthServiceProxy(url)

num_blocks = 1  # Number of blocks to mine
mining_address = rpc_connection.getnewaddress()  # Get a new address for mining rewards

block_hashes = mine_blocks(rpc_connection, num_blocks, mining_address)
print(f"Mined {len(block_hashes)} block(s) with hashes: {block_hashes}")

