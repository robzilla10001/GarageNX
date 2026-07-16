#pragma once
// source/services/mtp_server.hpp
// Clean-room MTP (PTP-over-USB) responder built directly on libnx usb:ds.
//
// libhaze (Atmosphère's MTP responder) is GPLv2-only and therefore legally
// incompatible with this project's AGPLv3 licence, so the responder is ours.
// Protocol marshalling lives in mtp_data.{hpp,cpp} (unit-tested off-device);
// this file owns the USB transport and operation dispatch.
//
// Slice 1: enumerate and expose storages. Slice 2: browse (object handles,
// info, file reads). Slice 3: write — create/upload/delete. Install storages
// land in slice 4.
//
// Reuses NetworkService purely for its thread lifecycle — the base carries no
// network-specific API, only start/stop/status/last_error.

#include "services/service_manager.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#endif

namespace Services {

class MtpServer : public NetworkService {
public:
    MtpServer() = default;

    const char* name() const override { return "MTP"; }

    /// True once the host has configured the device (UsbState_Configured).
    bool     host_connected() const { return m_configured.load(); }
    /// True between OpenSession and CloseSession.
    bool     session_open()   const { return m_session.load(); }
    int      request_count()  const { return (int)m_requests.load(); }
    uint64_t bytes_sent()     const { return m_bytes_sent.load(); }
    uint64_t bytes_recv()     const { return m_bytes_recv.load(); }

protected:
    void run() override;

private:
#ifdef PLATFORM_SWITCH
    bool usb_init();
    void usb_exit();
    bool add_endpoints();

    // Bounded-wait transfers. A timeout leaves the URB cancelled and the
    // endpoint drained, so stop() stays responsive rather than blocking on a
    // host that has gone away.
    bool ep_read(void* buf, size_t size, size_t* transferred, uint64_t timeout_ns);
    bool ep_write(const void* buf, size_t size, uint64_t timeout_ns);

    UsbDsInterface* m_iface = nullptr;
    UsbDsEndpoint*  m_ep_in = nullptr;    // bulk IN  (device -> host)
    UsbDsEndpoint*  m_ep_out = nullptr;   // bulk OUT (host -> device)
    UsbDsEndpoint*  m_ep_intr = nullptr;  // interrupt IN (events; unused in slice 1)
#endif

    void handle_command(const std::vector<uint8_t>& packet);
    bool send_response(uint16_t code, uint32_t tid,
                       const uint32_t* params = nullptr, int nparams = 0);
    bool send_data(uint16_t code, uint32_t tid, const std::vector<uint8_t>& payload);

    std::vector<uint8_t> build_device_info() const;
    std::vector<uint8_t> build_storage_info(uint32_t storage_id) const;
    std::vector<uint8_t> build_object_info(uint32_t handle) const;

    // ── Object handle database ───────────────────────────────────────────────
    // MTP addresses everything by opaque u32 handle, and a handle must stay
    // valid for the life of the session. Handles are interned on first sight
    // (during a directory listing) and never reused within a session; handle N
    // is m_paths[N-1], since 0 is reserved as "invalid".
    uint32_t           intern(const std::string& vfs_path);
    const std::string* path_for(uint32_t handle) const;
    void               reset_objects();

    std::vector<std::string>              m_paths;     // handle-1 -> vfs path
    std::map<std::string, uint32_t>       m_by_path;   // vfs path -> handle

    // Streams a data container whose payload is a file, so a multi-GB read
    // never needs a multi-GB buffer.
    bool send_file_data(uint16_t code, uint32_t tid, const std::string& vfs_path, uint64_t size);

    // Host -> device data phase. read_data_container is for small datasets that
    // fit one transfer (ObjectInfo); recv_file_data streams a file to disk.
    bool read_data_container(std::vector<uint8_t>& out);
    bool recv_file_data(const std::string& vfs_path, uint64_t expected);

    // SendObjectInfo names the file and SendObject then supplies its bytes, so
    // the destination has to survive between the two commands.
    std::string m_pending_path;
    uint64_t    m_pending_size  = 0;
    bool        m_pending_valid = false;

    // usb:ds requires page-aligned DMA buffers; these are allocated aligned.
    uint8_t* m_buf = nullptr;
    size_t   m_buf_size = 0;

    std::atomic<bool>     m_configured{false};
    std::atomic<bool>     m_session{false};
    std::atomic<uint32_t> m_session_id{0};
    std::atomic<uint64_t> m_requests{0};
    std::atomic<uint64_t> m_bytes_sent{0};
    std::atomic<uint64_t> m_bytes_recv{0};
};

} // namespace Services
