# User Preferences

## Compilation Guidelines

1. **Never** perform compilations in `~/src/bitcoin.git`
2. **Prefer** using `~/src/bitcoin.make` for compilations
3. When compiling, use the command: `make -j8 src/qt/bitcoin-qt`

---

# Active Jobs

## Current Task
- **Main Job**: Verifying compilation of moorlane-core-master.wip's base commit
  - ✅ Identified the base commit: `f1ce67f09fbaba4013443bce416e46e5b2d37c19`
  - ❌ Attempted to compile the base commit but encountered compilation errors
    - This is unexpected since this should be a stable commit from the official Bitcoin repository
  - ✅ Successfully ran `./autogen.sh` and `./configure` to set up the proper build environment
  - ❌ Second compilation attempt made progress but still encountered errors
    - Now seeing missing header includes in the code (specifically '<stdexcept>' in support/lockedpool.cpp)
    - This suggests the base commit may have some issues or may require patches
  - ✅ **GCC-13 Compatibility Investigation**:
    - Confirmed that our compile issues are related to GCC-13 compatibility (our system is using GCC 13.3.0)
    - Found upstream commit `fadeb6b103` in core/master that specifically addresses GCC-13 compilation errors
    - The upstream fix adds missing includes like `<stdexcept>`, `<limits>`, and `<utility>` to various files
    - Similar fixes were applied in the moorlane-core-master.wip branch through commits like:
      - 4aebf2ebff (added stdexcept)
      - daf310a6bc (fixed header file)
      - 8bd14c8a5e (fixed try_lock error)
    - This confirms the working hypothesis that the base code needed modifications to work with newer GCC versions
  - This verification is needed because moorlane-core-master.wip branch had compilation errors that required patching

## Network Connections Reset Implementation
- **Task**: Modify codebase to reset Tor and I2P connections when network is re-enabled
  - ✅ Investigated connection management for Tor and I2P services
  - ✅ Modified `CConnman::SetNetworkActive` to reset connections when network is activated:
    - For Tor: Added call to `ResetTorBackoff()` to reset backoff timer and reconnect
    - For I2P: Added code to disconnect existing session so it will be freshly recreated
  - This implementation ensures when a user re-enables the network, Bitcoin immediately attempts to reconnect to privacy-enhancing network services rather than waiting for backoff timers

## Traffic Graph Data Validation and Timestamp Synthesis
- **Task**: Enhance TrafficGraphWidget's loadDataFromCSV() function to properly handle invalid timestamp data
  - ✅ Implemented robust timestamp validation with multiple checks:
    - Detection of future timestamps (beyond current time)
    - Identification of outdated timestamps (significantly in the past)
    - Verification of timestamp linearity and sequence integrity
  - ✅ Created intelligent timestamp synthesis for invalid data scenarios:
    - Analyzes 28-day range sample count to determine appropriate timespan
    - Scans debug.log to identify periods when bitcoind was actually running
    - Avoids generating timestamps during inactive periods (gaps of 30+ minutes)
  - This enhancement ensures the traffic graph displays accurate time-series data even when CSV files contain corrupted timestamps, improving visualization reliability without manual intervention

## Traffic Graph Data Persistence
- **Task**: Update TrafficGraphWidget to properly persist network traffic data across application restarts
  - ✅ Modified data storage location from temporary directory to application data directory:
    - Changed hard-coded `/tmp/trafficgraphdata` paths to use `clientModel->dataDir()`
    - Updated both binary and CSV data file paths for consistency
  - ✅ Added robust null pointer protection:
    - Added checks in TrafficGraphWidget destructor to verify clientModel exists before calling saveData()
    - Added similar null checks in loadData() and loadDataFromCSV() methods
    - Updated log messages to accurately reflect new file locations
  - These changes ensure network traffic history persists across application restarts and system reboots, preventing data loss and providing users with continuous historical network usage visualization

---
*This file will be updated with additional preferences as they are identified.*

