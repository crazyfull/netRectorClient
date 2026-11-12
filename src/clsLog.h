#ifndef CLSLOG_H
#define CLSLOG_H
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <string>

// ============================== Log ==========================================
// All library messages go through here. The app can forward them to its own logger:
//   netrector::setLogHandler([](int level, const char* msg) { ... });
// Default sink: stderr (Android: logcat).
namespace netrector {

enum LogLevel { LOG_ERROR = 0, LOG_WARN = 1, LOG_INFO = 2, LOG_DEBUG = 3 };

typedef void (*LogHandler)(int level, const char* message);

void setLogHandler(LogHandler handler);
void setLogLevel(int maxLevel);     // messages with a higher level are dropped (default: LOG_INFO)
#if defined(__GNUC__) || defined(__clang__)
void logMessage(int level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#else
void logMessage(int level, const char* fmt, ...);
#endif
bool logEnabled(int level);

} // namespace netrector

#define NR_LOG(level, ...) do { if (netrector::logEnabled(level)) netrector::logMessage(level, __VA_ARGS__); } while (0)

#endif // CLSLOG_H
