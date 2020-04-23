#include "chainparams.h"
#include "utilioprio.h"
#include "validation.h"
#include "validationinterface.h"
#include "net.h"

void CConnman::ThreadValidation()
{
    {
    IOPRIO_IDLER(true);

    LogPrintf("%s: Starting\n", __func__);
    int nSleep = 0;
    while (!flagInterruptMsgProc) {
        if (fActivateChain) {
            if (!fActivatingChain) {
	        if (nSleep != 100)
                    LogPrint("tip", "%s: Slept %dms. Calling FormBestChain()\n", __func__, nSleep);
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

    } // end IOPRIO_IDLER scope
}

