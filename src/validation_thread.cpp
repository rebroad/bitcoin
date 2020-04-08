#include "chainparams.h"
#include "validation.h"
#include "validationinterface.h"
#include "net.h"

std::atomic<bool> fActivateChain(true);

void CConnman::ThreadValidation()
{
    int nSleep = 0;
    while (!flagInterruptMsgProc) {
        if (fActivateChain && !fActivatingChain) {
            fActivateChain = false;
	    if (nSleep != 100)
                LogPrint("block", "%s: Slept %dms. Calling FormBestChain()\n", __func__, nSleep);
            FormBestChain();
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

