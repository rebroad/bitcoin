#include "validation_thread.h"
#include "validation.h"
#include "net.h"

class ChainstateManager;

void CConnman::ThreadValidation()
{
    LogPrintf("%s: Starting\n", __func__);
    int nSleep = 0;
    while (!interruptNet) {
        if (fActivateChain) {
            if (!fActivatingChain) {
	        if (nSleep != 100)
                    LogPrintf("%s: Slept %dms. Calling FormBestChain()\n", __func__, nSleep);
                fActivateChain = false;
                FormBestChain();
            }
        }
        if (fActivateChain)
            nSleep = 0;
        else
            nSleep = 100;
        if (!interruptNet.sleep_for(std::chrono::milliseconds(nSleep)))
            return;
    }
    LogPrintf("%s: Exiting\n", __func__);
}
