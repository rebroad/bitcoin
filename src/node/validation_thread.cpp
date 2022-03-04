#include <node/validation_thread.h>
#include <validation.h>

namespace node {

class ChainstateManager;

void ThreadValidation()
{
    LogPrintf("%s: Starting\n", __func__);
    int nSleep = 0;
    while (true) {
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
        std::this_thread::sleep_for(std::chrono::milliseconds(nSleep));
    }
    LogPrintf("%s: Exiting\n", __func__);
}
} // namespace node
