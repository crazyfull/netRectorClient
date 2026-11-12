#include "TerminationSignal.h"

#include <atomic>
#include <csignal>

static std::atomic<int> g_signalNumber { 0 };

static void onTerminationSignal(int signalNumber)
{
    g_signalNumber.store(signalNumber);
}

void installTerminationHandler()
{
    std::signal(SIGINT, onTerminationSignal);
    std::signal(SIGTERM, onTerminationSignal);
}

bool isTerminationRequested()
{
    return g_signalNumber.load() != 0;
}

int receivedTerminationSignal()
{
    return g_signalNumber.load();
}
