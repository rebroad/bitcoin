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
        nSleep = 100;
        if (!fActivateChain)
            MilliSleep(nSleep);
        else
            nSleep = 0;
    }

    } // end IOPRIO_IDLER scope
}

