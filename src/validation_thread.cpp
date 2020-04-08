#include "chainparams.h"
#include "validation.h"
#include "validationinterface.h"
#include "net.h"

std::atomic<bool> fActivateChain(true);

void CConnman::ThreadValidation()
{
    int nSleep;
    while (!flagInterruptMsgProc) {
        if (fActivateChain && !fActivatingChain) {
            fActivateChain = false;
            FormBestChain();
        }
        nSleep = 100;
        if (!fActivateChain)
            MilliSleep(nSleep);
        else
            nSleep = 0;
    }
}

