#include "raiseFileDescriptorLimit.h"

#if !defined(_WIN32)
#include <sys/resource.h>
#endif

int raiseFileDescriptorLimit()
{
#if defined(_WIN32)
    return 2048;    // no per-process descriptor limit to raise; select() sets its own bound
#else
    constexpr rlim_t MaxTableSize = 1048576;

    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0)
        return 1024;

    if (limit.rlim_cur < limit.rlim_max) {
        limit.rlim_cur = limit.rlim_max;
        setrlimit(RLIMIT_NOFILE, &limit);
        getrlimit(RLIMIT_NOFILE, &limit);
    }

    return static_cast<int>(limit.rlim_cur > MaxTableSize ? MaxTableSize : limit.rlim_cur);
#endif
}
