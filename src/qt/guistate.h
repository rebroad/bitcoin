// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_GUISTATE_H
#define BITCOIN_QT_GUISTATE_H

#include <atomic>

// GUI state management functions
bool IsGuiInUse();
void SetGuiInUse(bool in_use);

#endif // BITCOIN_QT_GUISTATE_H 