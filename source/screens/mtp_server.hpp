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
#include "install/stream_installer.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#endif

namespace Services {

class MtpServer : public NetworkService {
public:
    MtpServer() = default;

    // CRITICAL: stop() (which joins the worker thread) MUST run before any member
    // is destroyed. Without this destructor, C++ destroys members first and the
    // base ~NetworkService (which calls stop()) LAST — so m_install would be torn
    // down while the worker thread is still inside recv_install using it. That is
    // a cross-thread use-after-free: the worker runs abort() on a half-destroyed
    // installer while the UI thread's ~unique_ptr runs abort() too. It presented
    // as the MTP cancel crash (Data Abort at 0x0; the 2168-0002 in the report is a
    // stale result register, the real Exception Type is Data Abort). Joining here,
    // before members die, is the actual fix — the abort() guard/reorder/atomics
    // were treating symptoms of this ordering bug.
    ~MtpServer() override { stop(); }

    const char* name() const override { return "MTP"; }

    /// True once the host has configured the device (UsbState_Configured).
    bool     host_connected() const { return m_configured.load(); }
    /// True between OpenSession and CloseSession.
    bool     session_open()   const { return m_session.load(); }
    int      request_count()  const { return (int)m_requests.load(); }
    /// Live install state, so the screen can show what the installer is doing
    /// instead of leaving a failed transfer as an opaque host-side error.
    const Install::Progress& install_progress() const { return m_install_progress; }
    bool     installing()     const { return m_installing.load(); }
    uint64_t bytes_sent()     const { return m_bytes_sent.load(); }
    uint64_t bytes_recv()     const { return m_bytes_recv.load(); }

    // Declared wire size of the current incoming transfer, from SendObjectInfo —
    // i.e. the compressed (.nsz/.xcz) size actually crossing USB, which is the
    // correct ETA denominator. NSZ/XCZ decompress on-device, so installed bytes
    // exceed wire bytes; an ETA against installed size would run fast then stall.
    // Reads 0 when no size has been declared yet (ETA should show "—" until then).
    uint64_t current_wire_size() const { return m_wire_size.load(); }
    // Wire bytes received for the current transfer so far. Resets to 0 at the
    // start of each object. ETA = (current_wire_size - current_wire_recv) / rate.
    uint64_t current_wire_recv() const { return m_wire_recv.load(); }

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
    /// False when m_pending_size came from SendObjectInfo and was saturated at
    /// 0xFFFFFFFF. See recv_install(): it decides whether the host's declared
    /// length or the container's own table is the authority for the data phase.
    bool        m_pending_size_exact = false;

    // ── Install storages ─────────────────────────────────────────────────────
    // Writing an NSP to one of these streams it straight into NCM rather than
    // onto the filesystem — no staging file, so the FAT32 4 GiB ceiling never
    // applies. m_install is live only between SendObjectInfo and SendObject.
    bool  storage_enabled(uint32_t storage_id) const;
    /// `size_exact` distinguishes a 64-bit size the host actually declared
    /// (SendObjectPropList) from SendObjectInfo's u32, which saturates at
    /// 0xFFFFFFFF and is therefore meaningless at or above 4 GiB.
    bool  recv_install(uint32_t storage_id, const std::string& filename,
                       uint64_t size, bool size_exact);

    /// Arm m_pending_* for an incoming object and answer the host. Shared by
    /// SendObjectInfo and SendObjectPropList: those operations differ only in
    /// HOW the host declares an object — a fixed dataset vs a property list —
    /// not in what we do about it. Keeping one body means the install gate
    /// cannot drift between the two routes and leave one of them open.
    void  arm_incoming_object(uint32_t storage, uint32_t parent, uint16_t fmt,
                              const std::string& filename, uint64_t size,
                              bool size_exact, uint32_t tid);

    // Read and discard the rest of a data phase. A host that is mid-send blocks
    // forever if the device simply stops reading, so every early exit has to
    // consume what is still coming before answering.
    void  drain_data(uint64_t remaining);

    void save_install_log(const std::string& filename, bool ok);
    /// Refuse an install before the data phase and leave a trace of WHY.
    /// save_install_log() is otherwise only reached from recv_install(), so a
    /// SendObjectInfo rejection used to push its reason into m_install_progress
    /// and then discard it unread — the host sees a bare PTP response code and
    /// the user sees nothing at all. Always use this instead of a raw push_log
    /// when refusing in the SendObjectInfo handler.
    void reject_install(const std::string& filename, const std::string& reason);

    std::atomic<bool>                        m_installing{false};
    std::unique_ptr<Install::StreamInstaller> m_install;
    Install::Progress                         m_install_progress;
    uint32_t                                  m_pending_storage = 0;

    // usb:ds requires page-aligned DMA buffers; these are allocated aligned.
    uint8_t* m_buf = nullptr;
    size_t   m_buf_size = 0;

    std::atomic<bool>     m_configured{false};
    std::atomic<bool>     m_session{false};
    std::atomic<uint32_t> m_session_id{0};
    std::atomic<uint64_t> m_requests{0};
    std::atomic<uint64_t> m_bytes_sent{0};
    std::atomic<uint64_t> m_bytes_recv{0};
    // Current-transfer wire accounting for the ETA (see accessors above).
    std::atomic<uint64_t> m_wire_size{0};
    std::atomic<uint64_t> m_wire_recv{0};
};

} // namespace Services
