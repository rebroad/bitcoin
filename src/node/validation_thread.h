#ifndef BITCOIN_NODE_VALIDAITONTHREAD_H
#define BITCOIN_NODE_VALIDATIONTHREAD_H

#include <atomic>

namespace node {
void ThreadValidation();
extern std::atomic<bool> fActivateChain; // Set to true to trigger validation thread
} // namespace node

#endif
