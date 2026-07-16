// source/core/net.cpp

#include "core/net.hpp"

#ifdef PLATFORM_SWITCH
#include <switch.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cstdio>
#include <cstring>

namespace Core::Net {

std::string current_ip() {
#ifdef PLATFORM_SWITCH
    // libnx nifm gives the console's current IPv4 directly (nifm is already
    // initialized in main). The address comes back with the first octet in the
    // low byte. NOTE: if it ever displays reversed on hardware, flip the shifts.
    u32 ip = 0;
    Result rc = nifmGetCurrentIpAddress(&ip);
    if (R_FAILED(rc) || ip == 0) return "0.0.0.0";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                  (unsigned)(ip & 0xFF), (unsigned)((ip >> 8) & 0xFF),
                  (unsigned)((ip >> 16) & 0xFF), (unsigned)((ip >> 24) & 0xFF));
    return buf;
#else
    // PC: discover the outbound interface IP by "connecting" a UDP socket
    // (no packets are sent) and reading back the local address.
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return "127.0.0.1";
    struct sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port   = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);
    std::string out = "127.0.0.1";
    if (::connect(s, (struct sockaddr*)&remote, sizeof(remote)) == 0) {
        struct sockaddr_in local{};
        socklen_t len = sizeof(local);
        if (getsockname(s, (struct sockaddr*)&local, &len) == 0)
            out = inet_ntoa(local.sin_addr);
    }
    ::close(s);
    return out;
#endif
}

std::string hostname() {
#ifdef PLATFORM_SWITCH
    // gethostname() is not available on the Switch newlib toolchain; use the
    // system device nickname when present, otherwise a fixed name.
    SetSysDeviceNickName nick{};
    if (R_SUCCEEDED(setsysGetDeviceNickname(&nick)) && nick.nickname[0])
        return nick.nickname;
    return "GarageNX";
#else
    char buf[128] = {0};
    if (::gethostname(buf, sizeof(buf) - 1) == 0 && buf[0]) return buf;
    return "GarageNX";
#endif
}

std::string link_url(const std::string& scheme, const std::string& ip, uint16_t port) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s://%s:%u/", scheme.c_str(), ip.c_str(), (unsigned)port);
    return buf;
}

} // namespace Core::Net
