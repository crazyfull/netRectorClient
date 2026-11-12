#ifndef TERMINATIONSIGNAL_H
#define TERMINATIONSIGNAL_H

// Portable (POSIX and Windows) handling of Ctrl+C / SIGTERM: the handler only sets a flag that
// the main loop polls.

// Installs the SIGINT and SIGTERM handlers.
void installTerminationHandler();

// True once a termination signal was received.
bool isTerminationRequested();

// The number of the received signal (0 = none).
int receivedTerminationSignal();

#endif // TERMINATIONSIGNAL_H
