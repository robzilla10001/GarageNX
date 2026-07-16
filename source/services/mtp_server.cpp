// source/services/mtp_server.cpp

#include "services/mtp_server.hpp"
#include "services/mtp_data.hpp"
#include "core/storage.hpp"
#include "core/fs.hpp"
#include "core/keys.hpp"
#include "config/config.hpp"
#include "core/datetime.hpp"

#include <SDL2/SDL.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef PLATFORM_SWITCH
#include <malloc.h>
#endif

namespace Services {

using namespace Services::Mtp;

namespace {

// StorageIDs are (physical << 16) | logical. Beyond the browsable SD card we
// expose two write-only "drop zones": an NSP written to one is streamed straight
// into NCM instead of onto the filesystem, which is what lets a >4 GiB title
// install to a FAT32 card. This mirrors how DBI presents install targets.
constexpr uint32_t kStorageSd          = 0x00010001;
constexpr uint32_t kStorageSdInstall   = 0x00020001;
constexpr uint32_t kStorageNandInstall = 0x00030001;

bool is_install_storage(uint32_t id) {
    return id == kStorageSdInstall || id == kStorageNandInstall;
}

// usb:ds transfers must come from page-aligned memory. 1 MiB keeps future file
// transfers from thrashing; slice 1 barely uses it.
constexpr size_t kBufSize = 1024 * 1024;

} // namespace

bool MtpServer::storage_enabled(uint32_t id) const {
    const auto& m = Config::get().mtp;
    if (id == kStorageSd)          return m.sd_card;
    if (id == kStorageSdInstall)   return m.sd_install;
    if (id == kStorageNandInstall) return m.nand_install;
    return false;
}

// ─── Datasets ────────────────────────────────────────────────────────────────

std::vector<uint8_t> MtpServer::build_device_info() const {
    Writer w;
    w.u16(100);                       // Standard version 1.00
    w.u32(6);                         // VendorExtensionID: MTP
    w.u16(100);                       // VendorExtensionVersion
    w.str("microsoft.com: 1.0;");     // VendorExtensionDesc — hosts key off this
    w.u16(0);                         // FunctionalMode: standard

    // Advertise only what we actually answer. Claiming an operation we then
    // reject with OperationNotSupported makes hosts retry and stall.
    w.au16({
        Op::GetDeviceInfo, Op::OpenSession, Op::CloseSession,
        Op::GetStorageIDs, Op::GetStorageInfo,
        Op::GetNumObjects, Op::GetObjectHandles, Op::GetObjectInfo, Op::GetObject,
        Op::SendObjectInfo, Op::SendObject, Op::DeleteObject,
        Op::GetDevicePropDesc, Op::GetDevicePropValue,
    });
    w.au16({});                       // EventsSupported — none yet
    w.au16({Mtp::Prop::DeviceFriendlyName});   // DevicePropertiesSupported
    w.au16({});                       // CaptureFormats
    w.au16({});                       // PlaybackFormats

    w.str("GarageNX");                // Manufacturer
    w.str("GarageNX MTP Responder");  // Model
    w.str(APP_VERSION);               // DeviceVersion
    w.str("0000000000000001");        // SerialNumber
    return w.data();
}

std::vector<uint8_t> MtpServer::build_storage_info(uint32_t storage_id) const {
    Writer w;
    if (!storage_enabled(storage_id)) return w.data();

    if (is_install_storage(storage_id)) {
        const bool nand = (storage_id == kStorageNandInstall);
        const Core::Storage::SpaceInfo sp = nand ? Core::Storage::nand_user()
                                                 : Core::Storage::sd_card();
        w.u16(0x0003);                     // StorageType: FixedRAM — not removable media
        w.u16(0x0002);                     // FilesystemType: GenericHierarchical
        w.u16(0x0000);                     // AccessCapability: read-write (hosts refuse to write otherwise)
        w.u64(sp.total_bytes);
        w.u64(sp.free_bytes);
        w.u32(0xFFFFFFFF);
        w.str(nand ? "Install to NAND" : "Install to SD");   // StorageDescription
        w.str(nand ? "NANDINSTALL" : "SDINSTALL");            // VolumeIdentifier
        return w.data();
    }

    const Core::Storage::SpaceInfo sd = Core::Storage::sd_card();

    w.u16(0x0004);                    // StorageType: RemovableRAM
    w.u16(0x0002);                    // FilesystemType: GenericHierarchical
    w.u16(0x0000);                    // AccessCapability: read-write
    w.u64(sd.total_bytes);            // MaxCapacity
    w.u64(sd.free_bytes);             // FreeSpaceInBytes
    w.u32(0xFFFFFFFF);                // FreeSpaceInObjects: not tracked
    w.str("SD Card");                 // StorageDescription
    w.str("SDCARD");                  // VolumeIdentifier
    return w.data();
}


// ─── Object handle database ──────────────────────────────────────────────────

uint32_t MtpServer::intern(const std::string& vfs_path) {
    auto it = m_by_path.find(vfs_path);
    if (it != m_by_path.end()) return it->second;
    m_paths.push_back(vfs_path);
    const uint32_t h = (uint32_t)m_paths.size();   // handle 0 is reserved
    m_by_path.emplace(vfs_path, h);
    return h;
}

const std::string* MtpServer::path_for(uint32_t handle) const {
    if (handle == Mtp::kInvalidHandle || handle > m_paths.size()) return nullptr;
    return &m_paths[handle - 1];
}

void MtpServer::reset_objects() {
    m_paths.clear();
    m_by_path.clear();
}

// ─── ObjectInfo ──────────────────────────────────────────────────────────────

std::vector<uint8_t> MtpServer::build_object_info(uint32_t handle) const {
    Writer w;
    const std::string* p = path_for(handle);
    if (!p) return w.data();

    const bool is_dir = Fs::is_directory(*p);
    const uint64_t size = is_dir ? 0 : Fs::file_size(*p);

    // Parent: the interned handle of the containing directory, or the root
    // sentinel when this object sits directly in the storage root.
    const std::string parent_path = Fs::parent(*p);
    uint32_t parent_handle = Mtp::kRootParent;
    if (!parent_path.empty() && parent_path != "sdmc:" && parent_path != "sdmc:/") {
        auto it = m_by_path.find(parent_path);
        if (it != m_by_path.end()) parent_handle = it->second;
    }

    w.u32(kStorageSd);
    w.u16(is_dir ? Mtp::Fmt::Association : Mtp::Fmt::Undefined);
    w.u16(0);                                  // ProtectionStatus: none
    // ObjectCompressedSize is 32-bit in PTP. FAT32 caps files at 4 GiB so this
    // is exact there; on exFAT a >4 GiB file would be misreported, which is why
    // 64-bit sizes need the MTP object-property path (a later slice).
    w.u32(size > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)size);
    w.u16(0);                                  // ThumbFormat
    w.u32(0);                                  // ThumbCompressedSize
    w.u32(0);                                  // ThumbPixWidth
    w.u32(0);                                  // ThumbPixHeight
    w.u32(0);                                  // ImagePixWidth
    w.u32(0);                                  // ImagePixHeight
    w.u32(0);                                  // ImageBitDepth
    w.u32(parent_handle);
    w.u16(is_dir ? Mtp::kAssocGenericFolder : 0);
    w.u32(0);                                  // AssociationDesc
    w.u32(0);                                  // SequenceNumber
    w.str(Fs::basename(*p));                   // Filename
    w.str("");                                 // CaptureDate
    w.str("");                                 // ModificationDate
    w.str("");                                 // Keywords
    return w.data();
}


// Stream a data container whose payload is a file. The container header
// declares the total length up front, then the payload follows across as many
// bulk transfers as it takes — so reading a 4 GiB file never needs a 4 GiB
// buffer.
bool MtpServer::send_file_data(uint16_t code, uint32_t tid,
                               const std::string& vfs_path, uint64_t size) {
#ifdef PLATFORM_SWITCH
    FILE* f = std::fopen(vfs_path.c_str(), "rb");
    if (!f) return false;

    const uint64_t total = Mtp::kHeaderSize + size;
    Writer hdr;
    hdr.u32(total > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)total);
    hdr.u16(Mtp::Type::Data);
    hdr.u16(code);
    hdr.u32(tid);
    std::memcpy(m_buf, hdr.data().data(), Mtp::kHeaderSize);

    // First transfer carries the header plus as much payload as fits.
    size_t want = m_buf_size - Mtp::kHeaderSize;
    if (want > size) want = (size_t)size;
    size_t got = std::fread(m_buf + Mtp::kHeaderSize, 1, want, f);
    if (!ep_write(m_buf, Mtp::kHeaderSize + got, 10000000000ULL)) { std::fclose(f); return false; }
    m_bytes_sent.fetch_add(Mtp::kHeaderSize + got);

    uint64_t remaining = size - got;
    while (remaining > 0 && !should_stop()) {
        size_t chunk = (m_buf_size < remaining) ? m_buf_size : (size_t)remaining;
        got = std::fread(m_buf, 1, chunk, f);
        if (got == 0) break;                       // short read: file changed under us
        if (!ep_write(m_buf, got, 10000000000ULL)) { std::fclose(f); return false; }
        m_bytes_sent.fetch_add(got);
        remaining -= got;
    }
    std::fclose(f);
    return remaining == 0;
#else
    (void)code; (void)tid; (void)vfs_path; (void)size;
    return false;
#endif
}


// Read one host->device data container that fits a single transfer. Used for
// ObjectInfo, which is small by construction.
bool MtpServer::read_data_container(std::vector<uint8_t>& out) {
#ifdef PLATFORM_SWITCH
    size_t got = 0;
    if (!ep_read(m_buf, m_buf_size, &got, 10000000000ULL)) return false;
    if (got < Mtp::kHeaderSize) return false;

    Mtp::Header h;
    if (!parse_header(m_buf, got, h) || h.type != Mtp::Type::Data) return false;

    m_bytes_recv.fetch_add(got);
    out.assign(m_buf + Mtp::kHeaderSize, m_buf + got);
    return true;
#else
    (void)out; return false;
#endif
}

// Stream a host->device data container straight to disk. `expected` is the size
// SendObjectInfo declared, used when the container header carries the
// "undefined length" sentinel instead of a real total.
bool MtpServer::recv_file_data(const std::string& vfs_path, uint64_t expected) {
#ifdef PLATFORM_SWITCH
    size_t got = 0;
    if (!ep_read(m_buf, m_buf_size, &got, 10000000000ULL)) return false;
    if (got < Mtp::kHeaderSize) return false;

    Mtp::Header h;
    if (!parse_header(m_buf, got, h) || h.type != Mtp::Type::Data) return false;

    uint64_t payload;
    if (h.length == 0xFFFFFFFFu) payload = expected;            // undefined length
    else                         payload = h.length - Mtp::kHeaderSize;

    // Only create the file once the host has actually started sending, so a
    // rejected transfer does not leave a 0-byte turd on the card.
    FILE* f = std::fopen(vfs_path.c_str(), "wb");
    if (!f) return false;

    uint64_t written = 0;
    size_t first = got - Mtp::kHeaderSize;
    if (first > payload) first = (size_t)payload;
    if (first > 0) {
        if (std::fwrite(m_buf + Mtp::kHeaderSize, 1, first, f) != first) { std::fclose(f); return false; }
        written += first;
    }
    m_bytes_recv.fetch_add(got);

    while (written < payload && !should_stop()) {
        if (!ep_read(m_buf, m_buf_size, &got, 10000000000ULL)) break;
        if (got == 0) break;
        size_t take = got;
        if (written + take > payload) take = (size_t)(payload - written);   // ignore any ZLP/padding
        if (std::fwrite(m_buf, 1, take, f) != take) { std::fclose(f); return false; }
        m_bytes_recv.fetch_add(got);
        written += take;
    }
    std::fclose(f);

    if (written != payload) { std::remove(vfs_path.c_str()); return false; }
    return true;
#else
    (void)vfs_path; (void)expected; return false;
#endif
}


// Persist the install log. A stream install has no on-screen overlay to scroll
// back through, so the file is the only forensic record of why it failed.
void MtpServer::save_install_log(const std::string& filename, bool ok) {
    const std::string dir = Config::get().paths.log_folder;
    if (!dir.empty() && !Fs::exists(dir)) Fs::make_directory(dir);
    const std::string path = (dir.empty() ? std::string("sdmc:/switch/GarageNX/logs") : dir)
                           + "/" + Core::DateTime::log_stamp_now() + "_mtp.log";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "GarageNX MTP stream install — %s\n",
                 Core::DateTime::clock_string_now().c_str());
    std::fprintf(f, "Source: %s\nResult: %s\n\n", filename.c_str(), ok ? "SUCCESS" : "FAILED");
    for (const auto& line : m_install_progress.log_snapshot())
        std::fprintf(f, "%s\n", line.c_str());
    if (!m_install_progress.message.empty())
        std::fprintf(f, "\n> %s\n", m_install_progress.message.c_str());
    std::fclose(f);
    SDL_Log("MTP: install log -> %s", path.c_str());
}

void MtpServer::drain_data(uint64_t remaining) {
#ifdef PLATFORM_SWITCH
    size_t got = 0;
    while (remaining > 0 && !should_stop()) {
        if (!ep_read(m_buf, m_buf_size, &got, 10000000000ULL)) break;
        if (got == 0) break;
        remaining -= (got < remaining) ? got : remaining;
    }
#else
    (void)remaining;
#endif
}

// Stream an NSP arriving over USB straight into NCM. Nothing touches the
// filesystem, so a title larger than 4 GiB installs fine to a FAT32 card — the
// bytes go from the bulk endpoint into a placeholder and never exist as a file.
bool MtpServer::recv_install(uint32_t storage_id, const std::string& filename, uint64_t size) {
#ifdef PLATFORM_SWITCH
    const auto dest = (storage_id == kStorageNandInstall) ? Core::Ncm::Storage::BuiltIn
                                                          : Core::Ncm::Storage::SdCard;
    m_install = std::make_unique<Install::StreamInstaller>(
        dest, Core::Keys::get(), m_install_progress);

    m_installing.store(true);
    if (!m_install->begin(filename, size)) {
        save_install_log(filename, false);
        m_install.reset(); m_installing.store(false); return false;
    }

    // First transfer carries the data-container header plus the first payload.
    size_t got = 0;
    if (!ep_read(m_buf, m_buf_size, &got, 10000000000ULL)) { m_install->abort(); m_install.reset(); return false; }
    if (got < Mtp::kHeaderSize) { m_install->abort(); m_install.reset(); return false; }

    Mtp::Header h;
    if (!parse_header(m_buf, got, h) || h.type != Mtp::Type::Data) {
        m_install->abort(); m_install.reset(); return false;
    }
    const uint64_t payload = (h.length == 0xFFFFFFFFu) ? size
                                                       : (h.length - Mtp::kHeaderSize);

    uint64_t fed = 0;
    size_t first = got - Mtp::kHeaderSize;
    if (first > payload) first = (size_t)payload;
    if (first > 0 && !m_install->feed(m_buf + Mtp::kHeaderSize, first)) {
        m_install->abort();
        save_install_log(filename, false);
        m_install.reset(); m_installing.store(false);
        drain_data(payload - first);   // let the host finish; do not wedge it
        return false;
    }
    fed += first;
    m_bytes_recv.fetch_add(got);

    while (fed < payload && !should_stop()) {
        if (!ep_read(m_buf, m_buf_size, &got, 10000000000ULL)) break;
        if (got == 0) break;
        size_t take = got;
        if (fed + take > payload) take = (size_t)(payload - fed);   // ignore ZLP/padding
        if (!m_install->feed(m_buf, take)) {
            m_install->abort();
            save_install_log(filename, false);
            m_install.reset(); m_installing.store(false);
            drain_data(payload - fed - take);
            return false;
        }
        m_bytes_recv.fetch_add(got);
        fed += take;
    }

    if (fed != payload) {
        // Truncated transfer: drop the open placeholder rather than registering
        // a half-written NCA.
        m_install_progress.push_log("ERROR: transfer ended early (" +
            std::to_string(fed) + " of " + std::to_string(payload) + " bytes)");
        m_install->abort();
        save_install_log(filename, false);
        m_install.reset();
        m_installing.store(false);
        return false;
    }

    const bool ok = m_install->finish();
    save_install_log(filename, ok);
    m_install.reset();
    m_installing.store(false);
    return ok;
#else
    (void)storage_id; (void)filename; (void)size;
    return false;
#endif
}

// ─── Dispatch ────────────────────────────────────────────────────────────────

bool MtpServer::send_response(uint16_t code, uint32_t tid,
                              const uint32_t* params, int nparams) {
    const auto pkt = make_response(code, tid, params, nparams);
#ifdef PLATFORM_SWITCH
    if (pkt.size() > m_buf_size) return false;
    std::memcpy(m_buf, pkt.data(), pkt.size());
    if (!ep_write(m_buf, pkt.size(), 5000000000ULL)) return false;
    m_bytes_sent.fetch_add(pkt.size());
    return true;
#else
    (void)pkt; return true;
#endif
}

bool MtpServer::send_data(uint16_t code, uint32_t tid,
                          const std::vector<uint8_t>& payload) {
    const auto pkt = make_data(code, tid, payload);
#ifdef PLATFORM_SWITCH
    if (pkt.size() > m_buf_size) return false;
    std::memcpy(m_buf, pkt.data(), pkt.size());
    if (!ep_write(m_buf, pkt.size(), 5000000000ULL)) return false;
    m_bytes_sent.fetch_add(pkt.size());
    return true;
#else
    (void)pkt; return true;
#endif
}

void MtpServer::handle_command(const std::vector<uint8_t>& packet) {
    Command c;
    if (!parse_command(packet.data(), packet.size(), c)) return;
    m_requests.fetch_add(1);

    const uint32_t tid = c.hdr.transaction_id;

    // Every operation except GetDeviceInfo and OpenSession requires a session.
    const bool needs_session =
        (c.hdr.code != Op::GetDeviceInfo && c.hdr.code != Op::OpenSession);
    if (needs_session && !m_session.load()) {
        send_response(Rc::SessionNotOpen, tid);
        return;
    }

    switch (c.hdr.code) {
        case Op::GetDeviceInfo:
            if (send_data(Op::GetDeviceInfo, tid, build_device_info()))
                send_response(Rc::Ok, tid);
            break;

        case Op::OpenSession: {
            if (c.nparams < 1 || c.param[0] == 0) { send_response(Rc::InvalidParameter, tid); break; }
            if (m_session.load()) {
                // Report the existing session id, as the spec requires.
                const uint32_t cur = m_session_id.load();
                send_response(Rc::SessionAlreadyOpen, tid, &cur, 1);
                break;
            }
            reset_objects();   // handles are only meaningful within a session
            m_session_id.store(c.param[0]);
            m_session.store(true);
            send_response(Rc::Ok, tid);
            break;
        }

        case Op::CloseSession:
            m_session.store(false);
            m_session_id.store(0);
            reset_objects();
            send_response(Rc::Ok, tid);
            break;

        case Op::GetStorageIDs: {
            std::vector<uint32_t> ids;
            if (storage_enabled(kStorageSd))          ids.push_back(kStorageSd);
            if (storage_enabled(kStorageSdInstall))   ids.push_back(kStorageSdInstall);
            if (storage_enabled(kStorageNandInstall)) ids.push_back(kStorageNandInstall);
            Writer w;
            w.au32(ids);
            if (send_data(Op::GetStorageIDs, tid, w.data()))
                send_response(Rc::Ok, tid);
            break;
        }

        case Op::GetStorageInfo: {
            if (c.nparams < 1) { send_response(Rc::InvalidParameter, tid); break; }
            auto info = build_storage_info(c.param[0]);
            if (info.empty()) { send_response(Rc::InvalidStorageId, tid); break; }
            if (send_data(Op::GetStorageInfo, tid, info))
                send_response(Rc::Ok, tid);
            break;
        }


        case Op::GetNumObjects:
        case Op::GetObjectHandles: {
            if (c.nparams < 1) { send_response(Rc::InvalidParameter, tid); break; }
            // param[0] storage (0xFFFFFFFF = any), param[1] format filter,
            // param[2] parent handle.
            if (is_install_storage(c.param[0])) {
                // Write-only drop zone: nothing to enumerate.
                if (c.hdr.code == Op::GetNumObjects) {
                    const uint32_t zero = 0;
                    send_response(Rc::Ok, tid, &zero, 1);
                } else {
                    Writer w; w.au32({});
                    if (send_data(Op::GetObjectHandles, tid, w.data())) send_response(Rc::Ok, tid);
                }
                break;
            }
            if (c.param[0] != kStorageSd && c.param[0] != 0xFFFFFFFF) {
                send_response(Rc::InvalidStorageId, tid); break;
            }
            const uint32_t parent = (c.nparams >= 3) ? c.param[2] : Mtp::kRootParent;

            std::string dir;
            if (parent == Mtp::kRootParent || parent == 0) {
                dir = "sdmc:/";
            } else {
                const std::string* p = path_for(parent);
                if (!p) { send_response(Rc::InvalidObjectHandle, tid); break; }
                dir = *p;
            }

            bool ok = false;
            const auto entries = Fs::list(dir, &ok);
            if (!ok) { send_response(Rc::InvalidParentObject, tid); break; }

            if (c.hdr.code == Op::GetNumObjects) {
                const uint32_t n = (uint32_t)entries.size();
                send_response(Rc::Ok, tid, &n, 1);
                break;
            }

            std::vector<uint32_t> handles;
            handles.reserve(entries.size());
            for (const auto& e : entries) handles.push_back(intern(Fs::join(dir, e.name)));

            Writer w;
            w.au32(handles);
            if (send_data(Op::GetObjectHandles, tid, w.data()))
                send_response(Rc::Ok, tid);
            break;
        }

        case Op::GetObjectInfo: {
            if (c.nparams < 1) { send_response(Rc::InvalidParameter, tid); break; }
            auto info = build_object_info(c.param[0]);
            if (info.empty()) { send_response(Rc::InvalidObjectHandle, tid); break; }
            if (send_data(Op::GetObjectInfo, tid, info))
                send_response(Rc::Ok, tid);
            break;
        }

        case Op::GetObject: {
            if (c.nparams < 1) { send_response(Rc::InvalidParameter, tid); break; }
            const std::string* p = path_for(c.param[0]);
            if (!p) { send_response(Rc::InvalidObjectHandle, tid); break; }
            if (Fs::is_directory(*p)) { send_response(Rc::InvalidObjectHandle, tid); break; }
            const std::string path = *p;   // path_for points into m_paths; copy before streaming
            if (send_file_data(Op::GetObject, tid, path, Fs::file_size(path)))
                send_response(Rc::Ok, tid);
            else
                send_response(Rc::IncompleteTransfer, tid);
            break;
        }


        case Op::SendObjectInfo: {
            // params: storage, parent. The host then sends an ObjectInfo
            // dataset naming the object; SendObject supplies the bytes after.
            const uint32_t storage = (c.nparams >= 1) ? c.param[0] : kStorageSd;
            uint32_t parent        = (c.nparams >= 2) ? c.param[1] : Mtp::kRootParent;
            if (storage != 0 && !storage_enabled(storage)) { send_response(Rc::InvalidStorageId, tid); break; }

            std::vector<uint8_t> data;
            if (!read_data_container(data)) { send_response(Rc::IncompleteTransfer, tid); break; }

            // Walk the ObjectInfo dataset. Only format, size and filename
            // matter to us; the rest is thumbnail/image metadata we skip.
            Reader r(data.data(), data.size());
            uint32_t d32; uint16_t d16;
            uint16_t fmt = 0; uint32_t comp_size = 0; std::string filename;
            r.u32(d32);                    // StorageID (host echo)
            r.u16(fmt);                    // ObjectFormat
            r.u16(d16);                    // ProtectionStatus
            r.u32(comp_size);              // ObjectCompressedSize
            r.u16(d16);                    // ThumbFormat
            // ThumbCompressedSize, ThumbPixWidth, ThumbPixHeight,
            // ImagePixWidth, ImagePixHeight, ImageBitDepth — exactly six.
            for (int i = 0; i < 6; i++) r.u32(d32);
            r.u32(d32);                    // ParentObject (host echo; we trust the param)
            r.u16(d16);                    // AssociationType
            r.u32(d32);                    // AssociationDesc
            r.u32(d32);                    // SequenceNumber
            r.str(filename);
            if (!r.ok() || filename.empty()) { send_response(Rc::InvalidParameter, tid); break; }

            // Refuse anything that could escape the storage root.
            if (filename.find('/') != std::string::npos ||
                filename.find('\\') != std::string::npos ||
                filename == "." || filename == "..") {
                send_response(Rc::InvalidParameter, tid); break;
            }

            // An install storage takes the file into NCM, not onto disk.
            if (is_install_storage(storage)) {
                if (fmt == Mtp::Fmt::Association) { send_response(Rc::AccessDenied, tid); break; }

                // Reject what we cannot install *now*, before the data phase.
                // Failing after the host has pushed several GB is both rude and
                // (since the endpoint stops being drained) a hang.
                std::string low = filename;
                for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
                const bool is_nsp = low.size() > 4 && low.compare(low.size()-4, 4, ".nsp") == 0;
                if (!is_nsp) {
                    m_install_progress.push_log("Rejected " + filename +
                        ": stream install currently accepts .nsp only");
                    send_response(Rc::InvalidParameter, tid);
                    break;
                }
                m_pending_storage = storage;
                m_pending_path    = filename;   // name only; nothing is written to a filesystem
                m_pending_size    = comp_size;
                m_pending_valid   = true;
                const uint32_t h = intern("install:" + filename);
                const uint32_t rp[3] = {storage, Mtp::kRootParent, h};
                send_response(Rc::Ok, tid, rp, 3);
                break;
            }

            std::string dir;
            if (parent == Mtp::kRootParent || parent == 0) {
                dir = "sdmc:/";
            } else {
                const std::string* pp = path_for(parent);
                if (!pp) { send_response(Rc::InvalidParentObject, tid); break; }
                dir = *pp;
            }
            const std::string dest = Fs::join(dir, filename);
            m_pending_storage = storage;

            if (fmt == Mtp::Fmt::Association) {
                // A folder is complete at SendObjectInfo time; no SendObject follows.
                if (!Fs::make_directory(dest)) { send_response(Rc::GeneralError, tid); break; }
                m_pending_valid = false;
                const uint32_t h = intern(dest);
                const uint32_t rp[3] = {kStorageSd, parent, h};
                send_response(Rc::Ok, tid, rp, 3);
                break;
            }

            m_pending_path  = dest;
            m_pending_size  = comp_size;
            m_pending_valid = true;

            const uint32_t h = intern(dest);
            const uint32_t rp[3] = {kStorageSd, parent, h};
            send_response(Rc::Ok, tid, rp, 3);
            break;
        }

        case Op::SendObject: {
            // Must be preceded by SendObjectInfo — the spec has no other way to
            // know where the bytes belong.
            if (!m_pending_valid) { send_response(Rc::GeneralError, tid); break; }
            const std::string dest    = m_pending_path;
            const uint64_t    size    = m_pending_size;
            const uint32_t    storage = m_pending_storage;
            m_pending_valid = false;   // one SendObject per SendObjectInfo

            if (is_install_storage(storage)) {
                send_response(recv_install(storage, dest, size) ? Rc::Ok : Rc::GeneralError, tid);
            } else {
                send_response(recv_file_data(dest, size) ? Rc::Ok : Rc::IncompleteTransfer, tid);
            }
            break;
        }

        case Op::DeleteObject: {
            if (c.nparams < 1) { send_response(Rc::InvalidParameter, tid); break; }
            const std::string* p = path_for(c.param[0]);
            if (!p) { send_response(Rc::InvalidObjectHandle, tid); break; }
            const std::string path = *p;   // copy: path_for points into m_paths
            const bool ok = Fs::is_directory(path) ? Fs::remove_directory_recursive(path)
                                                   : Fs::remove_file(path);
            send_response(ok ? Rc::Ok : Rc::AccessDenied, tid);
            break;
        }


        case Op::GetDevicePropDesc: {
            if (c.nparams < 1 || c.param[0] != Mtp::Prop::DeviceFriendlyName) {
                send_response(Rc::ParameterNotSupported, tid); break;
            }
            Writer w;
            w.u16(Mtp::Prop::DeviceFriendlyName);
            w.u16(Mtp::kTypeStr);      // DataType: string
            w.u8(0);                   // GetSet: read-only
            w.str("GarageNX");         // DefaultValue
            w.str("GarageNX");         // CurrentValue
            w.u8(0);                   // FormFlag: none
            if (send_data(Op::GetDevicePropDesc, tid, w.data()))
                send_response(Rc::Ok, tid);
            break;
        }

        case Op::GetDevicePropValue: {
            if (c.nparams < 1 || c.param[0] != Mtp::Prop::DeviceFriendlyName) {
                send_response(Rc::ParameterNotSupported, tid); break;
            }
            Writer w;
            w.str("GarageNX");
            if (send_data(Op::GetDevicePropValue, tid, w.data()))
                send_response(Rc::Ok, tid);
            break;
        }

        default:
            send_response(Rc::OperationNotSupported, tid);
            break;
    }
}

// ─── USB transport ───────────────────────────────────────────────────────────
#ifdef PLATFORM_SWITCH

bool MtpServer::add_endpoints() {
    // MTP (PTP class) mandates three endpoints: bulk in, bulk out, interrupt in.
    struct usb_interface_descriptor idesc = {
        .bLength            = USB_DT_INTERFACE_SIZE,
        .bDescriptorType    = USB_DT_INTERFACE,
        .bInterfaceNumber   = 0,
        .bAlternateSetting  = 0,
        .bNumEndpoints      = 3,
        .bInterfaceClass    = USB_CLASS_IMAGE,   // 6 — what makes hosts load their MTP/PTP driver
        .bInterfaceSubClass = 0x01,              // Still Image Capture
        .bInterfaceProtocol = 0x01,              // PTP
        .iInterface         = 0,
    };

    struct usb_endpoint_descriptor ep_in = {
        .bLength          = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType  = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_IN | 1,
        .bmAttributes     = USB_TRANSFER_TYPE_BULK,
        .wMaxPacketSize   = 0x40,
        .bInterval        = 0,
    };
    struct usb_endpoint_descriptor ep_out = {
        .bLength          = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType  = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_OUT | 1,
        .bmAttributes     = USB_TRANSFER_TYPE_BULK,
        .wMaxPacketSize   = 0x40,
        .bInterval        = 0,
    };
    struct usb_endpoint_descriptor ep_intr = {
        .bLength          = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType  = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_IN | 2,
        .bmAttributes     = USB_TRANSFER_TYPE_INTERRUPT,
        .wMaxPacketSize   = 0x18,
        .bInterval        = 6,
    };
    struct usb_ss_endpoint_companion_descriptor companion = {
        .bLength            = sizeof(struct usb_ss_endpoint_companion_descriptor),
        .bDescriptorType    = USB_DT_SS_ENDPOINT_COMPANION,
        .bMaxBurst          = 0x0F,
        .bmAttributes       = 0x00,
        .wBytesPerInterval  = 0x00,
    };

    // Full speed (USB 1.1): 64-byte bulk packets.
    if (R_FAILED(usbDsInterface_AppendConfigurationData(m_iface, UsbDeviceSpeed_Full, &idesc, USB_DT_INTERFACE_SIZE))) return false;
    if (R_FAILED(usbDsInterface_AppendConfigurationData(m_iface, UsbDeviceSpeed_Full, &ep_in, USB_DT_ENDPOINT_SIZE))) return false;
    if (R_FAILED(usbDsInterface_AppendConfigurationData(m_iface, UsbDeviceSpeed_Full, &ep_out, USB_DT_ENDPOINT_SIZE))) return false;
    if (R_FAILED(usbDsInterface_AppendConfigurationData(m_iface, UsbDeviceSpeed_Full, &ep_intr, USB_DT_ENDPOINT_SIZE))) return false;

    // High speed (USB 2.0): bulk packets grow to 512.
    ep_in.wMaxPacketSize = 0x200;
    ep_out.wMaxPacketSize = 0x200;
    if (R_FAILED(usbDsInterface_AppendConfigurationData(m_iface, UsbDeviceSpeed_High, &idesc, USB_DT_INTERFACE_SIZE))) return false;
    if (R_FAILED(usbDsInterface_AppendConfigurationData(m_iface, UsbDeviceSpeed_High, &ep_in, USB_DT_ENDPOINT_SIZE))) return false;
    if (R_FAILED(usbDsInterface_AppendConfigurationData(m_iface, UsbDeviceSpeed_High, &ep_out, USB_DT_ENDPOINT_SIZE))) return false;
    if (R_FAILED(usbDsInterface_AppendConfigurationData(m_iface, UsbDeviceSpeed_High, &ep_intr, USB_DT_ENDPOINT_SIZE))) return false;

    // Super speed (USB 3.0): 1024-byte bulk, and every endpoint needs a
    // companion descriptor immediately after it.
    ep_in.wMaxPacketSize = 0x400;
    ep_out.wMaxPacketSize = 0x400;
    if (R_FAILED(usbDsInterface_AppendConfigurationData(m_iface, UsbDeviceSpeed_Super, &idesc, USB_DT_INTERFACE_SIZE))) return false;
    if (R_FAILED(usbDsInterface_AppendConfigurationData(m_iface, UsbDeviceSpeed_Super, &ep_in, USB_DT_ENDPOINT_SIZE))) return false;
    if (R_FAILED(usbDsInterface_AppendConfigurationData(m_iface, UsbDeviceSpeed_Super, &companion, sizeof(companion)))) return false;
    if (R_FAILED(usbDsInterface_AppendConfigurationData(m_iface, UsbDeviceSpeed_Super, &ep_out, USB_DT_ENDPOINT_SIZE))) return false;
    if (R_FAILED(usbDsInterface_AppendConfigurationData(m_iface, UsbDeviceSpeed_Super, &companion, sizeof(companion)))) return false;
    if (R_FAILED(usbDsInterface_AppendConfigurationData(m_iface, UsbDeviceSpeed_Super, &ep_intr, USB_DT_ENDPOINT_SIZE))) return false;
    if (R_FAILED(usbDsInterface_AppendConfigurationData(m_iface, UsbDeviceSpeed_Super, &companion, sizeof(companion)))) return false;

    if (R_FAILED(usbDsInterface_RegisterEndpoint(m_iface, &m_ep_in,   ep_in.bEndpointAddress)))   return false;
    if (R_FAILED(usbDsInterface_RegisterEndpoint(m_iface, &m_ep_out,  ep_out.bEndpointAddress)))  return false;
    if (R_FAILED(usbDsInterface_RegisterEndpoint(m_iface, &m_ep_intr, ep_intr.bEndpointAddress))) return false;

    return R_SUCCEEDED(usbDsInterface_EnableInterface(m_iface));
}

bool MtpServer::usb_init() {
    m_buf = (uint8_t*)memalign(0x1000, kBufSize);
    if (!m_buf) { set_error("MTP: out of memory for USB buffer"); return false; }
    m_buf_size = kBufSize;

    if (R_FAILED(usbDsInitialize())) { set_error("usbDsInitialize failed"); return false; }

    // Identify as an MTP device. The strings are what the host shows the user.
    u8 iManufacturer = 0, iProduct = 0, iSerial = 0;
    const u16 langs[1] = {0x0409};   // en-US
    if (R_FAILED(usbDsAddUsbLanguageStringDescriptor(nullptr, langs, 1))) { set_error("lang descriptor failed"); return false; }
    if (R_FAILED(usbDsAddUsbStringDescriptor(&iManufacturer, "GarageNX"))) { set_error("string descriptor failed"); return false; }
    if (R_FAILED(usbDsAddUsbStringDescriptor(&iProduct, "GarageNX MTP Responder"))) { set_error("string descriptor failed"); return false; }
    if (R_FAILED(usbDsAddUsbStringDescriptor(&iSerial, "0000000000000001"))) { set_error("string descriptor failed"); return false; }

    struct usb_device_descriptor dev = {
        .bLength            = USB_DT_DEVICE_SIZE,
        .bDescriptorType    = USB_DT_DEVICE,
        .bcdUSB             = 0x0110,
        .bDeviceClass       = 0x00,   // class is declared per-interface
        .bDeviceSubClass    = 0x00,
        .bDeviceProtocol    = 0x00,
        .bMaxPacketSize0    = 0x40,
        .idVendor           = 0x057E,   // Nintendo
        // 057e:201d is the id libmtp's device database knows as "Nintendo
        // Switch / Switch Lite". Without it the host falls through to the
        // generic PTP/camera backend (libgphoto2): the device mounts as
        // gphoto2:// with a camera icon and storages named "store_XXXXXXXX",
        // and mtp-detect reports no raw devices at all. This id is what puts us
        // on the libmtp path.
        .idProduct          = 0x201D,
        .bcdDevice          = 0x0100,
        .iManufacturer      = iManufacturer,
        .iProduct           = iProduct,
        .iSerialNumber      = iSerial,
        .bNumConfigurations = 0x01,
    };
    if (R_FAILED(usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Full, &dev))) { set_error("device descriptor (FS) failed"); return false; }
    dev.bcdUSB = 0x0200;
    if (R_FAILED(usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &dev))) { set_error("device descriptor (HS) failed"); return false; }
    dev.bcdUSB = 0x0300;
    dev.bMaxPacketSize0 = 0x09;   // SuperSpeed encodes EP0 size as a power of two
    if (R_FAILED(usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &dev))) { set_error("device descriptor (SS) failed"); return false; }

    // Binary Object Store — required for the host to negotiate SuperSpeed.
    const u8 bos[0x16] = {
        0x05, USB_DT_BOS, 0x16, 0x00, 0x02,
        0x07, USB_DT_DEVICE_CAPABILITY, 0x02, 0x02, 0x00, 0x00, 0x00,
        0x0A, USB_DT_DEVICE_CAPABILITY, 0x03, 0x00, 0x0E, 0x00, 0x03, 0x00, 0x00, 0x00,
    };
    if (R_FAILED(usbDsSetBinaryObjectStore(bos, sizeof(bos)))) { set_error("BOS failed"); return false; }

    if (R_FAILED(usbDsRegisterInterface(&m_iface))) { set_error("RegisterInterface failed"); return false; }
    if (!add_endpoints()) { set_error("endpoint setup failed"); return false; }
    if (R_FAILED(usbDsEnable())) { set_error("usbDsEnable failed"); return false; }
    return true;
}

void MtpServer::usb_exit() {
    if (m_iface) { usbDsInterface_DisableInterface(m_iface); usbDsInterface_Close(m_iface); m_iface = nullptr; }
    usbDsExit();
    m_ep_in = m_ep_out = m_ep_intr = nullptr;
    if (m_buf) { free(m_buf); m_buf = nullptr; m_buf_size = 0; }
}

bool MtpServer::ep_read(void* buf, size_t size, size_t* transferred, uint64_t timeout_ns) {
    if (!m_ep_out) return false;
    u32 urb = 0;
    if (R_FAILED(usbDsEndpoint_PostBufferAsync(m_ep_out, buf, size, &urb))) return false;

    if (R_FAILED(eventWait(&m_ep_out->CompletionEvent, timeout_ns))) {
        // Nothing arrived in time. Cancel and drain, or the next post would
        // complete against this stale URB.
        usbDsEndpoint_Cancel(m_ep_out);
        eventWait(&m_ep_out->CompletionEvent, UINT64_MAX);
        eventClear(&m_ep_out->CompletionEvent);
        return false;
    }
    eventClear(&m_ep_out->CompletionEvent);

    UsbDsReportData report;
    if (R_FAILED(usbDsEndpoint_GetReportData(m_ep_out, &report))) return false;
    u32 n = 0;
    if (R_FAILED(usbDsParseReportData(&report, urb, nullptr, &n))) return false;
    if (transferred) *transferred = n;
    return true;
}

bool MtpServer::ep_write(const void* buf, size_t size, uint64_t timeout_ns) {
    if (!m_ep_in) return false;
    u32 urb = 0;
    if (R_FAILED(usbDsEndpoint_PostBufferAsync(m_ep_in, (void*)buf, size, &urb))) return false;

    if (R_FAILED(eventWait(&m_ep_in->CompletionEvent, timeout_ns))) {
        usbDsEndpoint_Cancel(m_ep_in);
        eventWait(&m_ep_in->CompletionEvent, UINT64_MAX);
        eventClear(&m_ep_in->CompletionEvent);
        return false;
    }
    eventClear(&m_ep_in->CompletionEvent);

    UsbDsReportData report;
    if (R_FAILED(usbDsEndpoint_GetReportData(m_ep_in, &report))) return false;
    u32 n = 0;
    return R_SUCCEEDED(usbDsParseReportData(&report, urb, nullptr, &n));
}

void MtpServer::run() {
    if (!usb_init()) { usb_exit(); return; }   // set_error already called

    while (!should_stop()) {
        UsbState st = UsbState_Detached;
        usbDsGetState(&st);
        const bool configured = (st == UsbState_Configured);
        m_configured.store(configured);

        if (!configured) {
            // No host: drop any session and idle politely.
            if (m_session.load()) { m_session.store(false); m_session_id.store(0); }
            m_pending_valid = false;
            svcSleepThread(100000000ULL);   // 100ms
            continue;
        }

        // 250ms read timeout keeps should_stop() responsive while idle.
        size_t got = 0;
        if (!ep_read(m_buf, m_buf_size, &got, 250000000ULL)) continue;
        if (got < Mtp::kHeaderSize) continue;

        m_bytes_recv.fetch_add(got);
        handle_command(std::vector<uint8_t>(m_buf, m_buf + got));
    }

    m_configured.store(false);
    m_session.store(false);
    usb_exit();
}

#else   // ── PC stub ──────────────────────────────────────────────────────────

void MtpServer::run() {
    // No USB device stack on the PC build; the screen still renders and reports
    // a clean error rather than pretending to be connected.
    set_error("MTP requires Switch hardware");
}

#endif

} // namespace Services
