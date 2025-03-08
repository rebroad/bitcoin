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

---
*This file will be updated with additional preferences as they are identified.*

