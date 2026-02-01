# Block connect performance and disk I/O

When the node is slow to update the tip (e.g. more than a minute per block) and CPU usage is low, the bottleneck is often **disk I/O**. When CPU is busy during block connect, the bottleneck is **script verification**. This document describes how to interpret bench output, when disk matters, and design notes for tiered chainstate (SSD + HDD).

## Interpreting `-debug=bench` output

### Do the numbers add up?

Yes. The top-level timings are:

- **Load block from disk** (T1) – read block from `blocks/blk*.dat`
- **Connect total** (T2) – entire `ConnectBlock()` (see below)
- **Flush** (T3) – UTXO cache flush to LevelDB
- **Writing chainstate** (T4) – LevelDB sync to disk
- **Connect postprocess** (T5) – UpdateTip, mempool, etc.

**Total connect block** ≈ T1 + T2 + T3 + T4 + T5.

Inside **Connect total** (T2), `ConnectBlock()` breaks down as:

- **Sanity checks** – BIP30, etc.
- **Fork checks** – BIP34, script flags
- **Connect N transactions** – loop that builds the script-check queue (`control.Add`) and runs `control.Wait()` (script verification). Most of the time is in `control.Wait()`.
- **Verify M txins** – same window as “Connect transactions” (from start of tx loop to after `control.Wait()`), so it overlaps; the dominant cost is script verification plus the chainstate reads that feed it.
- **Index writing** – undo data, best block, etc.

So the breakdown is adequate for understanding where time goes. Note: **script verification involves reading chainstate** — for each input, the node fetches the previous output (UTXO) from the chainstate to run the script. So the 136s “Connect transactions” / “Verify txins” time is a **mix of CPU (script execution) and chainstate read I/O**. The “Writing chainstate” line only measures the **write** flush at the end of ConnectTip, not the reads during ConnectBlock. If chainstate is on a slow HDD, those reads can dominate; if it’s on SSD (or cached), CPU dominates. So we cannot conclude “purely CPU” from the current bench output alone — moving chainstate to SSD can still help if the 136s is partly chainstate read latency.

### What is the bottleneck in your log?

In the sample log you provided:

- **Connect total: 136539 ms** – almost all of the block connect time
- **Connect 2474 transactions / Verify 7588 txins** – script verification plus chainstate reads (we don’t currently separate the two in the log)
- **Writing chainstate: 3.42 ms** – write flush is very fast

So either (a) chainstate is already on a fast device / well cached, and the 136s is mostly CPU, or (b) chainstate is on a slow device and the 136s is partly chainstate read I/O. To tell which, try moving chainstate to SSD and re-measure; if Connect total drops a lot, chainstate read I/O was significant. Increasing script-check threads (`-par`) or moving chainstate to SSD are both valid things to try.

## When disk is the bottleneck

If **Writing chainstate** (or **Load block from disk**) is a large fraction of the total, then disk I/O is the bottleneck. In that case:

1. Put **chainstate** on SSD (e.g. symlink `datadir/chainstate` → directory on SSD).
2. Optionally put **blocks** (or just `blocks/index`) on SSD if “Load block from disk” is high.

See “Putting chainstate on SSD” below.

## Chainstate (LevelDB) file behavior

Chainstate is a single **LevelDB** database under `datadir/chainstate/`. LevelDB uses:

- **Write-ahead log (WAL)** and **memtable** – new writes go here first.
- **SST files** – in the chainstate directory these appear as **`*.ldb`** files. They are created by **compaction**.

By **immutable** we mean: once an SST (`.ldb`) file is written, LevelDB **never amends it in place**. It can **delete** the file later when compaction has merged its contents into newer files and it is no longer needed. So: never amended; can be deleted when superseded by compaction.

When UTXOs are spent:

- The “spend” is written to the WAL/memtable (and later to new SSTs by compaction).
- Old SST files are **not** updated; they are only **read**. Eventually they may be **deleted** when compaction obsoletes them. So a chainstate file is read-only after creation until (if ever) it is deleted.

## Tiered chainstate (SSD + HDD) – design notes

A possible future feature is **tiered chainstate**: keep “hot” chainstate files on the faster drive and “cold” ones on the slower drive, with a rolling access counter and dynamic threshold based on free space on the faster drive.

### Single path for LevelDB: symlinks, no Env map

LevelDB always opens files by a single path (e.g. `chainstate/000123.ldb`). We do **not** need the Env to track “where each file lives” or to check two locations. We only need the filesystem to resolve one path:

- **State of a file from LevelDB’s point of view:** either the file does not exist, or it exists at that path. If the path is a **symbolic link** to the file on another drive, the kernel resolves it; LevelDB just sees “file exists” and reads/writes it normally.

So for each logical file (e.g. `000123.ldb`), we have three possible states:

1. **Does not exist** (e.g. not created yet, or deleted after compaction).
2. **Exists in the “regular” place** – the path LevelDB uses is a real file on one drive.
3. **Exists on the other drive** – the path LevelDB uses is a **symlink** pointing to the file on the other drive.

Migration is then: move the file from drive A to drive B, then create a symlink at the original path → the new path (or the reverse). LevelDB keeps using the same path; it does not need to “check both locations” or maintain a map. We do not need to change LevelDB’s Env at all for *where* to look — we only need something (a helper process or background thread) that **moves** files and **creates/removes** symlinks based on popularity and free space.

### Which drive is “regular” and which is “other”

We can decide which drive is the “regular” (primary) chainstate location and which is the “other” by a convention using **one** symlink inside the chainstate directory:

- **`chainstate/ssd`** (or **`chainstate/faster`**) – symlink to a directory on the **faster** drive. Then the “regular” place (the chainstate dir itself) is the **slower** drive. Hot files can be moved to the SSD and replaced with symlinks `chainstate/000123.ldb` → `chainstate/ssd/000123.ldb`.
- **`chainstate/hdd`** (or **`chainstate/slower`**) – symlink to a directory on the **slower** drive. Then the “regular” place is the **faster** drive. Cold files can be moved to the HDD and replaced with symlinks.

**Rule:** exactly one of `chainstate/ssd` or `chainstate/hdd` (or `faster` / `slower`) should exist. If **both** exist, the situation is ambiguous (which is “other”?); the node should **abort** at startup and ask the user to remove one.

### What would still be required

1. **Access tracking** – a rolling-window read (or open) count per chainstate file. This can be done **outside** Bitcoin Core using **fatrace** or **inotifywatch** (or similar): a separate service watches the chainstate directory and counts open/read events per file. No custom LevelDB Env needed for popularity.

2. **Dynamic threshold** – from free space on the faster drive, compute how many (or how many bytes of) files can stay on the faster drive. Rank files by recent access; keep the most popular on the fast drive, the rest on the slow drive.

3. **Migration logic** – when a file should move: **pause** Bitcoin with SIGSTOP, do the move + symlink, then **resume** with SIGCONT. See “External service: pause with SIGSTOP for migration” below.

So: **no Env map of locations** and **no “check both places”** — just one path per file, with the path either a real file or a symlink. Access tracking can be an external service; migration is done while Bitcoin is paused (SIGSTOP), not fully stopped.

### External service: pause with SIGSTOP for migration

A **separate service** (not part of Bitcoin Core) can implement tiered chainstate without touching LevelDB. Instead of fully stopping and restarting Bitcoin (SIGTERM + start again), the service can **pause** the process with **SIGSTOP** and **resume** it with **SIGCONT**. The process stays alive but does no I/O while stopped, so no reads or writes occur during the move.

1. **Popularity** – use **fatrace** (file access trace) or **inotify** (e.g. **inotifywatch**) to watch the chainstate directory and count open/read events per file over a rolling window (e.g. last N hours). The service maintains a “hot” list of files.

   - **fatrace** uses the Linux fanotify API; it sees open/read/write/close system-wide and reports process + path. More reliable under heavy load and gives process info, but typically needs root or `CAP_SYS_ADMIN`. Prefer fatrace if the daemon can run with sufficient privileges.
   - **inotify** watches a directory for events (e.g. IN_ACCESS, IN_OPEN). No root needed for watching your own datadir; run the watcher as the same user as Bitcoin. You may need to raise `fs.inotify.max_user_watches` if chainstate has many files, and the event queue can overflow under very heavy I/O. Prefer inotify if you want to avoid root.

   **Do we need Bitcoin-internal info about file contents?** No. To decide which disk to store each file we only need: (a) **access count** per file (from fatrace or inotify), (b) **file size** (from `stat()`) to fit within SSD free space, and (c) **which files are immutable** — avoid moving LevelDB’s current log, manifest, and the most recently written SST; we can infer that from mtime or “don’t move the newest N files” without any knowledge of LevelDB’s internal format. No data that only Bitcoin Core could provide is required.

2. **Schedule** – every T hours (e.g. 2), the service runs a migration pass:
   - **Pause** the Bitcoin process: send **SIGSTOP** to the bitcoind/bitcoin-qt process (e.g. `kill -STOP <pid>`). The process is suspended; it no longer runs and does no I/O, but it remains in memory and keeps its file descriptors open.
   - For each file that should be moved (based on popularity and free space on the faster drive):
     - **Check** whether any *other* process (not Bitcoin) has the file open (e.g. `lsof path` or `fuser path`). If something else (backup, virus scanner, etc.) has it open, **skip** that file and try again next run.
     - Only move **immutable** chainstate files (e.g. SST `*.ldb` files that are no longer being written). Do **not** move the LevelDB log, manifest, or other files that may be open for write — moving those while the process has them open can cause corruption when it resumes.
   - For each file that is safe: perform the **copy + symlink** (see below).
   - **Resume** the Bitcoin process: send **SIGCONT** (e.g. `kill -CONT <pid>`).

3. **Copy + symlink (atomic replace)** – while Bitcoin is paused (SIGSTOP):
   - **Copy** the file from the current location (A) to the other drive (B). The process may still have A open (fd to the old inode); we can still read A as another process.
   - **Create** a symlink at a temporary name (e.g. `A.symlink`) pointing to B.
   - **Rename** `A.symlink` to A (atomically replacing the file at A with the symlink). Now A is a symlink to B. The old inode (original file content) is no longer reachable by path A; the process still holds an open fd to that inode until it closes and reopens the file. When it later reopens A, it will follow the symlink to B. For immutable SST files the content is unchanged, so this is safe.
   - If the service crashes after copy but before rename, A is still the real file; delete the copy at B and retry later.

4. **Race conditions** – while the process is stopped (SIGSTOP), it cannot perform I/O, so there is no in-process race during the move. The process may still have the old file (inode) open; when it resumes and eventually reopens that file (e.g. when LevelDB closes and reopens it), it will see the symlink and use B. Only move read-only (immutable) chainstate files; do not move active log/manifest files.

Using SIGSTOP/SIGCONT avoids the cost and delay of a full process stop/restart and keeps the node’s in-memory state (e.g. mempool, connections) intact.

### Practical approach today

- **Symlink the whole chainstate** to the faster drive (see “Putting chainstate on SSD” below). That gives most of the benefit when chainstate I/O is the bottleneck.
- Per-file tiering with popularity and symlinks is a smaller change than previously described (no Env map, no dual-path lookup), but still needs access tracking and safe migration.

## Putting chainstate on SSD (blocks on HDD)

Bitcoin Core does not support splitting the datadir across two devices via config. You can achieve “chainstate on SSD, blocks on HDD” with **symlinks**.

1. **Stop** Bitcoin Core.
2. **Move** chainstate to the SSD and symlink it back:

   ```bash
   BITCOIN_DATADIR="$HOME/.bitcoin"   # or your -datadir
   SSD_CHAINSTATE="/path/on/ssd/chainstate"

   mv "$BITCOIN_DATADIR/chainstate" "$SSD_CHAINSTATE"
   ln -s "$SSD_CHAINSTATE" "$BITCOIN_DATADIR/chainstate"
   ```

3. **Start** Bitcoin Core. It will use chainstate on SSD and blocks on the original datadir (HDD).

Optionally, put `blocks/index` on SSD the same way if block index I/O is slow.

## Enabling bench logging and paths

- **`-debug=bench`** – enables the timings above and the path lines below.
- With bench enabled, the code logs:
  - **ReadBlockFromDisk file: \<path\>** – block file used (under `blocks/`).
  - **Writing chainstate (\<path\>): Xms** – chainstate directory and time.

That confirms which device is used for blocks and chainstate.

## Optional: see all file access during block connect

To see every file open under `blocks/` and `chainstate/` during block connect:

- **fatrace:** `sudo fatrace -f -t 2>&1 | grep -E 'bitcoin|chainstate|blk[0-9]'`
- **strace:** `sudo strace -f -e openat,open -p $(pgrep -x bitcoind) 2>&1 | grep -E 'blocks/|chainstate'`

This is for verification only; no code changes required.
