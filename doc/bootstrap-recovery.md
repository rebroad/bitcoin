Bootstrap Recovery (`-bootstrap`)
=================================

Overview
--------
`-bootstrap=<ip[:port]>` enables a startup recovery mode for a specific failure case:

- chainstate has a non-null best-block hash
- block index does not contain that hash
- startup would normally fail with `Error initializing block database`

When this mismatch is detected, startup enters a temporary header-bootstrap mode.

How It Works
------------
1. Startup detects `LoadChainTip()` failure due to missing chainstate tip hash in block index.
2. If `-bootstrap` is set, startup does **not** abort.
3. The missing chainstate tip hash is recorded as a bootstrap target.
4. Block download/processing is temporarily paused.
5. The node continues normal P2P header synchronization.
6. Once headers include the recorded target hash, the node finalizes by re-running `LoadChainTip()`.
7. Block download resumes automatically.

Peer Selection
--------------
- `-bootstrap` is only used if bootstrap recovery is actually needed.
- If recovery is needed, the configured peer is added as a manual outbound peer (`addnode`-style) to bias header acquisition.
- If recovery is not needed, `-bootstrap` is effectively inert.

Protocol / Compatibility
------------------------
- This uses the standard Bitcoin P2P protocol (`getheaders`/`headers` flow).
- No custom wire messages are used.
- The peer providing headers does **not** need to run this patch; an unmodified node is sufficient.

Usage
-----
Example:

`bitcoind -bootstrap=192.168.192.1`

or in `bitcoin.conf`:

`bootstrap=192.168.192.1`

Logging
-------
Relevant log markers include:

- deferring `LoadChainTip()` for bootstrap
- adding bootstrap peer
- ignoring block/cmpctblock while bootstrap is pending
- finalizing bootstrap and loading the chain tip

Notes
-----
- This feature is intended for recovery of index/chainstate anchoring issues.
- It does not replace full repair paths such as `-reindex` or `-reindex-chainstate` when underlying data is actually corrupted.
