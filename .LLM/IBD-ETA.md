# IBD Criteria Using Estimated Time Remaining

## Overview
This modification changes the criteria for determining Initial Block Download (IBD) state from a fixed 24-hour age threshold to an estimated time remaining threshold. By default, the node will consider itself in IBD if the estimated time remaining is more than 30 minutes.

## Files Modified

1. **src/validation.h**
   - Added declaration for `nIBDTimeRemaining` variable
   - Added constant `DEFAULT_IBD_TIME_THRESHOLD` set to 30 minutes (in seconds)

2. **src/validation.cpp**
   - Added global variable `nIBDTimeRemaining` to track estimated time remaining
   - Modified `IsInitialBlockDownload()` to use the estimated time remaining instead of tip age
   - Added command-line option `-ibdtimethreshold` (default: 30 minutes)

3. **src/qt/modaloverlay.cpp**
   - Updated `tipUpdate()` to set the global `nIBDTimeRemaining` value based on sync progress

## How It Works

1. The `ModalOverlay` class in the GUI calculates the estimated time remaining for synchronization 
2. This estimated time is stored in the global variable `nIBDTimeRemaining`
3. The `IsInitialBlockDownload()` function checks if this time remaining is greater than the threshold
4. When the estimated time remaining falls below the threshold (default 30 minutes), the node exits IBD mode

## Command Line Options

- `-ibdtimethreshold=<minutes>`: Set the time threshold in minutes (default: 30)

## Benefits

- More accurate representation of actual synchronization status
- Allows the node to start accepting transactions sooner when it's nearly synchronized
- More intuitive than the previous 24-hour tip age check 