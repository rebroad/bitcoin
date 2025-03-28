# User Preferences

## Compilation Guidelines

1. **Never** perform compilations in `~/src/bitcoin.git`
2. **Prefer** using `~/src/bitcoin.make` for compilations
3. When compiling, use the command: `make -j8 src/qt/bitcoin-qt`

---

# Active Jobs

## IBD Time Remaining Implementation
- **Task**: Modify Initial Block Download (IBD) criteria from 24-hour age threshold to estimated time remaining threshold
  - ✅ Investigated current IBD determination logic in `IsInitialBlockDownload()` function in `src/validation.cpp`
  - ✅ Explored how estimated time is calculated in `ModalOverlay` class in the GUI code
  - ✅ Decided on implementation approach:
    - Add global variable `nIBDTimeRemaining` to track estimated time remaining
    - Add command-line option `-ibdtimethreshold` (default: 30 minutes)
    - Update `IsInitialBlockDownload()` to use time remaining instead of tip age
    - Modify GUI code to update the global time remaining variable
  - 🔄 Details documented in `.LLM/IBD-ETA.md`
  - This enhancement provides a more intuitive measure of synchronization status and allows nodes to exit IBD state sooner when they're nearly synchronized

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

## TrafficGraphWidget Bug Fix
- **Task**: Fix out-of-bounds array access in TrafficGraphWidget causing application crash
  - ✅ Identified critical bug in TrafficGraphWidget::paintPath function:
    - Discovered loop condition `for (int i = 0; i < sampleCount++; ++i)` was incrementing sampleCount on each iteration
    - This caused the loop to attempt accessing elements beyond the array bounds, triggering a crash
  - ✅ Implemented fix by removing the post-increment operator:
    - Changed to `for (int i = 0; i < sampleCount; ++i)` to maintain proper bounds checking
    - Verified the fix prevents out-of-bounds access while preserving desired functionality
  - This fix resolves a critical stability issue that was causing the Bitcoin-Qt application to crash when viewing the network traffic graph
- **Task**: Fix visual artifact in TrafficGraphWidget showing diagonal lines
  - ✅ Identified visual issue where diagonal lines appeared on the graph from the last data point to the bottom left corner
  - ✅ Fixed by properly closing the path shape in the paintPath function:
    - Added horizontal line back to the starting point after drawing the vertical line to the bottom
    - Ensured the graph area is properly filled without unintended diagonal artifacts
  - This improvement enhances the visual appearance of the network traffic graph, providing a cleaner and more accurate representation of network activity

## Traffic Graph Slider Update Issue
- **Task**: Fix slider position not updating correctly when traffic data is loaded
  - ✅ Identified issue in `RPCConsole::setTrafficGraphRange` where the slider position was not correctly updated after data loading
  - ✅ Added `getCurrentRangeIndex()` method to `TrafficGraphWidget` to expose the current graph range index
  - ✅ Modified `RPCConsole::setTrafficGraphRange` to calculate the proper slider position based on the actual graph range:
    - Replaced the static increment of slider position (`set_slider_value += 200`) with a dynamic calculation
    - Used `getCurrentRangeIndex()` to determine the correct position when "bumping" occurs
  - This fix ensures the slider properly reflects the current time range after loading traffic data, providing accurate visual feedback to the user
---

## Multiple Onion Addresses Implementation
- **Task**: Modify Bitcoin Core to support multiple Tor onion addresses and vanity address prefixes
  - ✅ Add new configuration options to control onion address generation:
    - `-numonion=<n>`: Specify the number of onion addresses to create (default: 1)
    - `-onionmatch=<prefix>`: Optional prefix for generating vanity onion addresses
  - ✅ Modify the TorController class to handle multiple services:
    - Update member variables to use vectors for multiple private keys, service IDs, and services
    - Extend the ADD_ONION functionality to create multiple onion services
    - Implement proper error handling for multiple service creation attempts
  - ✅ Update private key file handling:
    - Modify GetPrivateKeyFile() to read/write multiple keys (one per line)
    - Implement proper backup and recovery mechanisms for multiple keys
  - ✅ Implement vanity onion address generation:
    - Add functionality to generate onion addresses with specified prefixes
    - Add timeout/attempt limits to prevent infinite loops during generation
  - ✅ Update connection handling:
    - Ensure all onion addresses are properly added to local address list
    - Update warning messages to reflect the new multi-address capability
  - ✅ Compiled successfully, with all features working as expected
  - This enhancement improves privacy by allowing Bitcoin Core nodes to operate on multiple Tor onion addresses simultaneously, while also adding customization options for users who want recognizable address prefixes. Implementation is complete and fully functional.

---
*This file will be updated with additional preferences as they are identified.*

