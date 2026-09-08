// source/core/ntp.cpp

#include "core/ntp.hpp"
#include "core/datetime.hpp"
#include <SDL2/SDL.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <poll.h>

// timeInitialize() opens time:u by default, and a time:u session cannot set the
// network clock - timeSetCurrentTime(TimeType_NetworkSystemClock) fails, so the
// NTP query succeeded but the clock never moved. Requesting the System service
// type (time:s) makes the session able to set it, which is exactly what the
// reference tool switch-time does. The user-clock fallback below still covers
// consoles where time:s is refused.
TimeServiceType __nx_time_service_type = TimeServiceType_System;

// Appends one line to sdmc:/switch/GarageNX/logs/ntp.log (the dir is created
// by boot's ensure_directories). Query and clock-set Results land here so
// hardware triage starts from recorded evidence - same discipline as
// wifi.log / pctl.log. SDL_Log output is invisible without nxlink.
static void ntp_log(const char* fmt, ...) {
    FILE* f = ::fopen("sdmc:/switch/GarageNX/logs/ntp.log", "a");
    if (!f) return;
    char line[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    ::fprintf(f, "%s\n", line);
    ::fclose(f);
}
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <poll.h>
#endif

namespace Core::Ntp {

#ifdef PLATFORM_SWITCH
// libnx's Result (u32) collides with this namespace's Result struct inside the
// namespace, so alias the struct for the one function that needs both.
using NtpResult = Core::Ntp::Result;
#endif

// NTP epoch (1900) to Unix epoch (1970) offset in seconds.
static constexpr uint64_t NTP_UNIX_DELTA = 2208988800ULL;

// A minimal 48-byte SNTP packet.
#pragma pack(push, 1)
struct NtpPacket {
    uint8_t  li_vn_mode;      // leap indicator, version, mode
    uint8_t  stratum;
    uint8_t  poll;
    uint8_t  precision;
    uint32_t root_delay;
    uint32_t root_dispersion;
    uint32_t ref_id;
    uint32_t ref_ts_sec;
    uint32_t ref_ts_frac;
    uint32_t orig_ts_sec;
    uint32_t orig_ts_frac;
    uint32_t recv_ts_sec;
    uint32_t recv_ts_frac;
    uint32_t tx_ts_sec;       // transmit timestamp - the one we want
    uint32_t tx_ts_frac;
};
#pragma pack(pop)

static Result do_query(const std::string& server, int timeout_ms) {
    Result r;

    // Resolve the server
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo* res = nullptr;
    if (getaddrinfo(server.c_str(), "123", &hints, &res) != 0 || !res) {
        r.error = "DNS resolution failed";
        return r;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        r.error = "socket() failed";
        freeaddrinfo(res);
        return r;
    }

    // Build request packet: LI=0, VN=4, Mode=3 (client) → 0b00100011 = 0x23
    NtpPacket pkt;
    std::memset(&pkt, 0, sizeof(pkt));
    pkt.li_vn_mode = 0x23;

    if (sendto(sock, &pkt, sizeof(pkt), 0,
               res->ai_addr, res->ai_addrlen) < 0) {
        r.error = "sendto() failed";
        close(sock);
        freeaddrinfo(res);
        return r;
    }

    // Wait for the reply with a timeout
    struct pollfd pfd;
    pfd.fd = sock;
    pfd.events = POLLIN;
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0) {
        r.error = (pr == 0) ? "timed out" : "poll() failed";
        close(sock);
        freeaddrinfo(res);
        return r;
    }

    NtpPacket resp;
    ssize_t n = recv(sock, &resp, sizeof(resp), 0);
    close(sock);
    freeaddrinfo(res);

    if (n < (ssize_t)sizeof(resp)) {
        r.error = "short reply";
        return r;
    }

    // Transmit timestamp is network byte order; convert.
    uint32_t tx_sec = ntohl(resp.tx_ts_sec);
    if (tx_sec == 0) {
        r.error = "invalid server time";
        return r;
    }

    r.server_time = (int64_t)((uint64_t)tx_sec - NTP_UNIX_DELTA);

    // Local time for offset (live service read, not the newlib boot snapshot)
    int64_t local = (int64_t)Core::DateTime::now_unix();
    r.offset_seconds = r.server_time - local;
    r.success = true;
    return r;
}

Result query(const std::string& server, int timeout_ms) {
    return do_query(server, timeout_ms);
}

Result sync(const std::string& server, int timeout_ms) {
    Result r = do_query(server, timeout_ms);
    if (!r.success) {
#ifdef PLATFORM_SWITCH
        ntp_log("query failed: %s", r.error.c_str());
#endif
        return r;
    }

#ifdef PLATFORM_SWITCH
    NtpResult set_result = r;

    u64 pre_user = 0, pre_net = 0;
    timeGetCurrentTime(TimeType_UserSystemClock, &pre_user);
    timeGetCurrentTime(TimeType_NetworkSystemClock, &pre_net);
    ntp_log("=== sync: server=%s timeout=%dms", server.c_str(), timeout_ms);
    ntp_log("pre : user=%lld net=%lld app_time=%lld",
            (long long)pre_user, (long long)pre_net, (long long)time(nullptr));
    ntp_log("server_time=%lld offset=%llds",
            (long long)r.server_time, (long long)r.offset_seconds);

    u32 rc_net = timeSetCurrentTime(TimeType_NetworkSystemClock,
                                    (uint64_t)r.server_time);
    if (R_SUCCEEDED(rc_net)) {
        u64 post_net = 0;
        timeGetCurrentTime(TimeType_NetworkSystemClock, &post_net);
        ntp_log("set NetworkSystemClock: OK (post read=%lld)", (long long)post_net);
        ntp_log("post: app_time=%lld (newlib snapshot) live_user=%lld",
                (long long)time(nullptr), (long long)Core::DateTime::now_unix());
        set_result.detail = "network clock set";
        return set_result;
    }
    ntp_log("set NetworkSystemClock FAILED: 0x%08X", rc_net);
    // Some setups only allow setting the user clock; try that too.
    u32 rc_user = timeSetCurrentTime(TimeType_UserSystemClock,
                                     (uint64_t)r.server_time);
    if (R_SUCCEEDED(rc_user)) {
        ntp_log("set UserSystemClock: OK (fallback)");
        ntp_log("post: app_time=%lld (newlib snapshot) live_user=%lld",
                (long long)time(nullptr), (long long)Core::DateTime::now_unix());
        set_result.detail = "user clock set (network clock refused)";
        return set_result;
    }
    ntp_log("set UserSystemClock FAILED: 0x%08X", rc_user);
    ntp_log("post: app_time=%lld (newlib snapshot) live_user=%lld",
            (long long)time(nullptr), (long long)Core::DateTime::now_unix());
    char b_net[9], b_user[9];
    snprintf(b_net, sizeof(b_net), "%08X", rc_net);
    snprintf(b_user, sizeof(b_user), "%08X", rc_user);
    set_result.success = false;
    set_result.error = std::string("could not set clock (network 0x") +
                       b_net + ", user 0x" + b_user + ")";
    ntp_log("sync failed: %s", set_result.error.c_str());
    return set_result;
#else
    // PC stub: don't touch the host clock; just report what we found.
    SDL_Log("Ntp::sync - (PC stub) would set clock to %lld (offset %lld s)",
            (long long)r.server_time, (long long)r.offset_seconds);
    return r;
#endif
}

} // namespace Core::Ntp