#include "MuxTunnelCore.h"

#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

// <sys/kern_control.h> is not part of the public iOS SDK: the few definitions that are needed.
#define muxAddressFamilySystem 32
#define muxControlName "com.apple.net.utun_control"

struct MuxControlInfo {
    uint32_t id;
    char name[96];
};

struct MuxSocketAddressControl {
    uint8_t length;
    uint8_t family;
    uint16_t systemAddress;
    uint32_t id;
    uint32_t unit;
    uint32_t reserved[5];
};

#define muxControlInfoRequest _IOWR('N', 3, struct MuxControlInfo)

int32_t muxTunnelCoreFindUtunDescriptor(void)
{
    struct MuxControlInfo info;
    memset(&info, 0, sizeof(info));
    strncpy(info.name, muxControlName, sizeof(info.name) - 1);

    for (int descriptor = 0; descriptor <= 1024; descriptor++) {
        struct MuxSocketAddressControl address;
        socklen_t length = sizeof(address);
        memset(&address, 0, sizeof(address));

        if (getpeername(descriptor, (struct sockaddr*)&address, &length) != 0)
            continue;
        if (address.family != muxAddressFamilySystem)
            continue;

        if (info.id == 0 && ioctl(descriptor, muxControlInfoRequest, &info) != 0)
            continue;

        if (address.id == info.id)
            return descriptor;
    }
    return -1;
}
