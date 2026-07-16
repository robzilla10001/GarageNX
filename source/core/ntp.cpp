// source/core/ntp.cpp

#include "core/ntp.hpp"
#include <SDL2/SDL.h>
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
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <poll.h>
#endif

namespace Core::Ntp {

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
    uint32_t tx_ts_sec;       // transmit timestamp — the one we want
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

    // Local time for offset
    int64_t local = (int64_t)time(nullptr);
    r.offset_seconds = r.server_time - local;
    r.success = true;
    return r;
}

Result query(const std::string& server, int timeout_ms) {
    return do_query(server, timeout_ms);
}

Result sync(const std::string& server, int timeout_ms) {
    Result r = do_query(server, timeout_ms);
    if (!r.success) return r;

#ifdef PLATFORM_SWITCH
    // Set the system's user clock to the NTP time.
    // timeInitialize is normally already up (started in main); guard anyway.
    Result set_result = r;
    if (R_FAILED(timeSetCurrentTime(TimeType_NetworkSystemClock,
                                    (uint64_t)r.server_time))) {
        // Some setups only allow setting the user clock; try that too.
        if (R_FAILED(timeSetCurrentTime(TimeType_UserSystemClock,
                                        (uint64_t)r.server_time))) {
            set_result.success = false;
            set_result.error = "could not set system clock";
        }
    }
    return set_result;
#else
    // PC stub: don't touch the host clock; just report what we found.
    SDL_Log("Ntp::sync — (PC stub) would set clock to %lld (offset %lld s)",
            (long long)r.server_time, (long long)r.offset_seconds);
    return r;
#endif
}

} // namespace Core::Ntp
