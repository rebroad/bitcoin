// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/guistate.h>
#include <logging.h>

#include <atomic>

// Global GUI state tracking
static std::atomic<bool> g_gui_in_use{false};

bool IsGuiInUse()
{
    return g_gui_in_use.load();
}

void SetGuiInUse(bool in_use)
{
    g_gui_in_use.store(in_use);
    LogPrint(BCLog::QT, "GUI state changed: %s\n", in_use ? "in use" : "not in use");
} 