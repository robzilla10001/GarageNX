// source/services/mtp_server.cpp

#include "services/mtp_server.hpp"
#include "services/storage_catalog.hpp"
#include "services/write_guard.hpp"
#include "services/title_surface.hpp"
#include "services/save_surface.hpp"
#include "services/save_write.hpp"
#include "install/stream_driver.hpp"
#include "services/mtp_data.hpp"
#include "services/overlap_buffer.hpp"
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
// expose two write-only "drop zones": an NSP or NSZ written to one is streamed straight
// into NCM instead of onto the filesystem, which is what lets a >4 GiB title
// install to a FAT32 card. This mirrors how DBI presents install targets.
constexpr uint32_t kStorageSd          = 0x00010001;
constexpr uint32_t kStorageSdInstall   = 0x00020001;
constexpr uint32_t kStorageNandInstall = 0x00030001;
constexpr uint32_t kStorageAlbum       = 0x00040001;
constexpr uint32_t kStorageNandUser    = 0x00050001;
constexpr uint32_t kStorageNandSystem  = 0x00060001;
constexpr uint32_t kStorageTitles      = 0x00070001;
constexpr uint32_t kStorageSaves       = 0x00080001;
// Gamecard was missing from this list for the whole project — harmless while
// nothing mounted the surface, and immediately visible as "FTP/HTTP show the card,
// MTP does not" the moment it did. FTP and HTTP iterate the catalog generically and
// so gained the surface for free; MTP maps wire ids by hand and had to be told.
constexpr uint32_t kStorageGamecard    = 0x00090001;

// Virtual NSPs have no file on disk, so they are interned under a synthetic path
// prefix. That lets them flow through the SAME handle machinery as real files
// (intern/path_for) instead of needing a parallel handle table.
constexpr const char* kTitlesPrefix = "titles:/";

bool is_install_storage(uint32_t id) {
    return id == kStorageSdInstall || id == kStorageNandInstall;
}

// Map an MTP wire storage id (stable, cached by PC clients — must NOT change) to
// the shared StorageCatalog surface it represents.
bool mtp_id_to_surface(uint32_t wire_id, StorageSurface::Id& out) {
    switch (wire_id) {
        case kStorageGamecard:    out = StorageSurface::Id::Gamecard;    return true;
        case kStorageSd:          out = StorageSurface::Id::SdCard;      return true;
        case kStorageSdInstall:   out = StorageSurface::Id::SdInstall;   return true;
        case kStorageNandInstall: out = StorageSurface::Id::NandInstall; return true;
        case kStorageAlbum:       out = StorageSurface::Id::Album;       return true;
        case kStorageNandUser:    out = StorageSurface::Id::NandUser;    return true;
        case kStorageNandSystem:  out = StorageSurface::Id::NandSystem;  return true;
        case kStorageTitles:      out = StorageSurface::Id::InstalledTitles; return true;
        case kStorageSaves:       out = StorageSurface::Id::Saves;       return true;
        default: return false;
    }
}

// Reverse: a catalog surface id -> its MTP wire id (0 if the surface has no MTP
// storage id).
uint32_t surface_to_mtp_id(StorageSurface::Id sid) {
    switch (sid) {
        case StorageSurface::Id::SdCard:      return kStorageSd;
        case StorageSurface::Id::SdInstall:   return kStorageSdInstall;
        case StorageSurface::Id::NandInstall: return kStorageNandInstall;
        case StorageSurface::Id::Album:       return kStorageAlbum;
        case StorageSurface::Id::NandUser:    return kStorageNandUser;
        case StorageSurface::Id::NandSystem:  return kStorageNandSystem;
        case StorageSurface::Id::InstalledTitles: return kStorageTitles;
        case StorageSurface::Id::Saves:       return kStorageSaves;
        case StorageSurface::Id::Gamecard:    return kStorageGamecard;
        default: return 0;
    }
}

// Derive the MTP wire storage id that owns a concrete VFS path, by matching the
// path's mount prefix ("sdmc:/...", "album:/...") against the catalog's Filesystem
// surfaces. This is what makes browse storage-aware without a parallel handle->
// storage table: an object's storage is a function of its path. Falls back to the
// SD storage id if no prefix matches (shouldn't happen for interned paths).
uint32_t mtp_storage_for_path(const std::string& vfs_path) {
    if (vfs_path.compare(0, 8, kTitlesPrefix) == 0) return kStorageTitles;
    // Save objects are interned under their own synthetic prefix, and must be
    // matched BEFORE the catalog loop: they belong to the Saves surface even
    // though they are not "save:/..." paths. (The prefix cannot collide with the
    // "save:" mount root — see save_surface.hpp — but checking first keeps the
    // intent explicit rather than relying on that.)
    if (Services::save_is_synthetic(vfs_path)) return kStorageSaves;
    for (const auto& s : Services::StorageCatalog::all()) {
        if (s.kind != Services::StorageKind::Filesystem) continue;
        const std::string root = s.vfs_root;             // e.g. "sdmc:", "album:"
        if (vfs_path.compare(0, root.size(), root) == 0) {
            uint32_t id = surface_to_mtp_id(s.id);
            if (id) return id;
        }
    }
    return kStorageSd;
}

// Resolve an interned path to the concrete path a READ should use. Real paths
// pass through unchanged; a synthetic save path mounts its (user, title) through
// the shared choke point first. Returns "" when the object cannot be read as a
// file — an unresolvable save, or one of the synthesized folder levels.
std::string readable_path(const std::string& interned) {
    if (!Services::save_is_synthetic(interned)) return interned;
    if (Services::save_synth_is_synthesized_dir(interned)) return std::string();
    const std::string real = Services::save_resolve_synth(interned);
    if (real.empty() || Fs::is_directory(real)) return std::string();
    return real;
}

// usb:ds transfers must come from page-aligned memory. 1 MiB keeps future file
// transfers from thrashing; slice 1 barely uses it.
constexpr size_t kBufSize = 1024 * 1024;

} // namespace

bool MtpServer::storage_enabled(uint32_t id) const {
    // Defer to the shared catalog so MTP, FTP, and HTTP agree on what's enabled.
    // The wire id -> surface mapping preserves MTP's stable storage ids.
    StorageSurface::Id sid;
    if (!mtp_id_to_surface(id, sid)) return false;
    if (!StorageCatalog::enabled(sid, Config::get().mtp.surfaces)) return false;

    // Enabled is not the same as MOUNTED. MTP used to stop at the config check,
    // so a surface whose device was missing got advertised in GetStorageIDs and
    // then failed enumeration — the host reports "could not get object handles"
    // and the user has a storage they cannot open. FTP always probed and simply
    // hid it. Same catalog, same config, two behaviours; now one.
    const StorageSurface* s = StorageCatalog::find(sid);
    return s && StorageCatalog::mount_available(*s);
}

// ─── Datasets ────────────────────────────────────────────────────────────────

std::vector<uint8_t> MtpServer::build_device_info() const {
    Writer w;
    w.u16(100);                       // Standard version 1.00
    w.u32(6);                         // VendorExtensionID: MTP
    w.u16(100);                       // VendorExtensionVersion
    // VendorExtensionDesc — hosts key off this. Advertising "android.com" tells
    // gvfs/libmtp we support the Android MTP i/o extensions (partial reads), which
    // is what makes it OPEN files in place instead of refusing and forcing a copy.
    // Without this string, gvfs marks the mount copy-only and never even issues a
    // read (confirmed by wire log: GetObjectInfo for every file, zero GetObject).
    w.str("microsoft.com: 1.0; android.com: 1.0;");
    w.u16(0);                         // FunctionalMode: standard

    // Advertise only what we actually answer. Claiming an operation we then
    // reject with OperationNotSupported makes hosts retry and stall.
    w.au16({
        Op::GetDeviceInfo, Op::OpenSession, Op::CloseSession,
        Op::GetStorageIDs, Op::GetStorageInfo,
        Op::GetNumObjects, Op::GetObjectHandles, Op::GetObjectInfo, Op::GetObject,
        Op::GetPartialObject, Op::GetPartialObject64,
        Op::SendObjectInfo, Op::SendObject, Op::DeleteObject,
        Op::GetDevicePropDesc, Op::GetDevicePropValue,
        // MTP 1.1. A host uses SendObjectPropList only if we claim it here, and
        // libmtp reaches it via GetObjectPropsSupported — advertising 0x9808
        // alone leaves it unreachable. Together they are the only way an object
        // of 4 GiB or more gets a truthful size: SendObjectInfo cannot express
        // one. Both routes converge on arm_incoming_object().
        // GetObjectPropDesc is not optional alongside these two: a host asks
        // for a description of every property GetObjectPropsSupported names.
        Op::GetObjectPropsSupported, Op::GetObjectPropDesc, Op::GetObjectPropValue,
        Op::SetObjectPropValue,
        Op::SendObjectPropList,
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

    if (storage_id == kStorageAlbum) {
        // Album lives on the SD card, so its space figures are the SD card's.
        // Name comes from the catalog so it matches every other transport.
        const Core::Storage::SpaceInfo sp = Core::Storage::sd_card();
        const StorageSurface* s = StorageCatalog::find(StorageSurface::Id::Album);
        w.u16(0x0004);                    // StorageType: RemovableRAM (on SD)
        w.u16(0x0002);                    // FilesystemType: GenericHierarchical
        w.u16(0x0000);                    // AccessCapability: read-write
        w.u64(sp.total_bytes);
        w.u64(sp.free_bytes);
        w.u32(0xFFFFFFFF);
        w.str(s ? s->display : "Album");  // StorageDescription
        w.str("ALBUM");                   // VolumeIdentifier
        return w.data();
    }

    if (storage_id == kStorageTitles) {
        // Synthesized read-only surface. Capacity figures are meaningless here, so
        // report the SD card's (the titles are read FROM the console, not stored
        // in a filesystem the host can measure).
        const Core::Storage::SpaceInfo sp = Core::Storage::sd_card();
        const StorageSurface* s = StorageCatalog::find(StorageSurface::Id::InstalledTitles);
        w.u16(0x0003);                    // StorageType: FixedRAM
        w.u16(0x0002);                    // FilesystemType: GenericHierarchical
        w.u16(0x0001);                    // AccessCapability: READ-ONLY
        w.u64(sp.total_bytes);
        w.u64(0);                         // no free space: nothing can be written here
        w.u32(0xFFFFFFFF);
        w.str(s ? s->display : "Installed Titles");
        w.str("TITLES");
        return w.data();
    }

    if (storage_id == kStorageSaves) {
        // Three-level synthesized surface: the top two levels are accounts and
        // titles, and only a title folder has a real filesystem behind it — and
        // then only while it is the one mounted slot.
        //
        // FREE SPACE MUST BE REAL, and this is the one field that cannot be
        // copied from Installed Titles. A host checks FreeSpaceInBytes BEFORE it
        // opens a transfer, and refuses client-side if the file does not fit —
        // "there is not enough space on the destination" for a 553-byte save,
        // with no request ever reaching us and nothing to see in a wire log.
        // Advertising 0 is only truthful for a surface that can NEVER be written;
        // Save Data can be, once the on-device confirmation is satisfied.
        //
        // NAND (User) is the pattern to copy here, not Installed Titles: it is
        // the other surface that is read-only by POLICY yet writable per
        // operation after a confirmation, and its writes are hardware-verified.
        // So: same AccessCapability (read-only is a hint to the host; the actual
        // gate is the write guard plus the modal) and the same real capacity
        // figures. Saves live in the NAND user partition, so those figures are
        // the honest ones.
        //
        // CAVEAT worth knowing: a Switch save has its own per-title quota, far
        // smaller than the partition. One number in StorageInfo cannot express
        // that, so an oversized write still fails mid-transfer rather than being
        // refused up front. Better than refusing everything.
        const Core::Storage::SpaceInfo sp = Core::Storage::nand_user();
        const StorageSurface* s = StorageCatalog::find(StorageSurface::Id::Saves);
        w.u16(0x0003);                    // StorageType: FixedRAM
        w.u16(0x0002);                    // FilesystemType: GenericHierarchical
        w.u16(0x0001);                    // AccessCapability: read-only BY POLICY,
                                          //   exactly as NAND (User) reports it
        w.u64(sp.total_bytes);
        w.u64(sp.free_bytes);             // real, or a host refuses every write
        w.u32(0xFFFFFFFF);
        w.str(s ? s->display : "Save Data");
        w.str("SAVEDATA");
        return w.data();
    }

    if (storage_id == kStorageNandUser || storage_id == kStorageNandSystem) {
        const bool is_user = (storage_id == kStorageNandUser);
        const StorageSurface::Id sid = is_user ? StorageSurface::Id::NandUser
                                               : StorageSurface::Id::NandSystem;
        const StorageSurface* s = StorageCatalog::find(sid);
        // BOTH partitions report real capacity. System used to report a default
        // SpaceInfo{} — zero total, zero free — described as "unknown capacity",
        // but zero free is not read by a host as "unknown": it is read as FULL.
        // A host checks FreeSpaceInBytes BEFORE opening a transfer and refuses
        // client-side, so every confirmed write to NAND System would have failed
        // with a misleading "not enough space" and nothing reaching the device.
        // That is the same bug that broke Save Data writes; it was latent here
        // only because NAND System is config-OFF by default.
        const Core::Storage::SpaceInfo sp = is_user ? Core::Storage::nand_user()
                                                    : Core::Storage::nand_system();
        w.u16(0x0003);                    // StorageType: FixedRAM (internal NAND)
        w.u16(0x0002);                    // FilesystemType: GenericHierarchical
        w.u16(0x0001);                    // AccessCapability: READ-ONLY (policy)
        w.u64(sp.total_bytes);
        w.u64(sp.free_bytes);
        w.u32(0xFFFFFFFF);
        w.str(s ? s->display : (is_user ? "NAND (User)" : "NAND (System)"));
        w.str(is_user ? "NANDUSER" : "NANDSYS");
        return w.data();
    }

    if (storage_id == kStorageGamecard) {
        // A game card is REMOVABLE and physically READ-ONLY.
        //
        // Capacity is reported as 0/0 deliberately. There is no statvfs for the
        // gamecard mount, and the alternative — borrowing the SD card's figures,
        // which is what the fallback below used to do — would be a lie a host
        // acts on. Zero FREE space is exactly right here: it is what stops a host
        // from opening a write it cannot complete. (Contrast NAND System, where
        // zero free was a BUG precisely because that surface is writable through
        // the confirmation broker.)
        const StorageSurface* s = StorageCatalog::find(StorageSurface::Id::Gamecard);
        w.u16(0x0004);                    // StorageType: RemovableRAM
        w.u16(0x0002);                    // FilesystemType: GenericHierarchical
        w.u16(0x0001);                    // AccessCapability: READ-ONLY
        w.u64(0);                         // MaxCapacity: unknown
        w.u64(0);                         // FreeSpaceInBytes: none — it is read-only
        w.u32(0xFFFFFFFF);
        w.str(s ? s->display : "Game Card");
        w.str("GAMECARD");
        return w.data();
    }

    // Anything not handled above. This USED to be an unconditional "SD Card"
    // block, which meant a storage with no case of its own silently described
    // itself as the SD card: the game card appeared in hosts as a second
    // "SD Card", disambiguated only by its numeric id. A fallback that
    // impersonates a real storage is worse than one that refuses.
    //
    // So: only the actual SD id gets the SD description, and every other id is
    // rejected as invalid rather than described as something it is not. Adding a
    // surface therefore FAILS LOUDLY here instead of masquerading.
    if (storage_id != kStorageSd)
        return {};                    // empty -> caller answers InvalidStorageId

    const Core::Storage::SpaceInfo sd = Core::Storage::sd_card();
    const StorageSurface* sds = StorageCatalog::find(StorageSurface::Id::SdCard);
    w.u16(0x0004);                    // StorageType: RemovableRAM
    w.u16(0x0002);                    // FilesystemType: GenericHierarchical
    w.u16(0x0000);                    // AccessCapability: read-write
    w.u64(sd.total_bytes);            // MaxCapacity
    w.u64(sd.free_bytes);             // FreeSpaceInBytes
    w.u32(0xFFFFFFFF);                // FreeSpaceInObjects: not tracked
    w.str(sds ? sds->display : "SD Card");   // from the catalog, like every other
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
    // Handles are the only thing that referred to a mounted save, so dropping
    // them means nothing should stay mounted. Leaving a save mounted past the
    // end of a session would hold a limited system resource for no reader.
    Services::save_surface_release();
}

// ─── ObjectInfo ──────────────────────────────────────────────────────────────

std::vector<uint8_t> MtpServer::build_object_info(uint32_t handle) const {
    Writer w;
    const std::string* p = path_for(handle);
    if (!p) return w.data();

    // Virtual NSPs have no file on disk: their size comes from the cached exact
    // NSP computation, not from stat().
    const bool is_virtual = (p->compare(0, 8, kTitlesPrefix) == 0);
    const bool is_save    = Services::save_is_synthetic(*p);

    bool     is_dir = false;
    uint64_t size   = 0;
    if (is_virtual) {
        size = Services::installed_titles_exact_size(p->substr(8));
    } else if (is_save) {
        // A user folder or a title folder is a directory BY CONSTRUCTION, so it
        // is answered without mounting anything. That matters: a host asks for an
        // ObjectInfo for every object it lists, and mounting to answer would
        // mount and unmount every save on the console just to browse one folder —
        // the bulk-churn pattern the single-slot design exists to avoid.
        if (Services::save_synth_is_synthesized_dir(*p)) {
            is_dir = true;
        } else {
            const std::string real = Services::save_resolve_synth(*p);
            if (real.empty()) return w.data();   // caller answers InvalidObjectHandle
            is_dir = Fs::is_directory(real);
            size   = is_dir ? 0 : Fs::file_size(real);
        }
    } else {
        is_dir = Fs::is_directory(*p);
        size   = is_dir ? 0 : Fs::file_size(*p);
    }

    // Parent: the interned handle of the containing directory, or the root
    // sentinel when this object sits directly in a storage root. A storage root is
    // any surface mount prefix ("sdmc:/", "album:/", ...), matched via the catalog
    // so browse works across all Filesystem surfaces, not just SD.
    const std::string parent_path = Fs::parent(*p);
    uint32_t parent_handle = Mtp::kRootParent;
    // The synthetic save root ("savedata:/") is a storage root too: a user folder
    // sits directly in the Save Data storage and must report kRootParent, not a
    // handle we never interned.
    bool parent_is_storage_root = parent_path.empty() ||
                                  parent_path == Services::save_synth_prefix();
    for (const auto& s : Services::StorageCatalog::all()) {
        if (s.kind != Services::StorageKind::Filesystem) continue;
        const std::string root = s.vfs_root;                  // "sdmc:"
        if (parent_path == root || parent_path == root + "/") {
            parent_is_storage_root = true;
            break;
        }
    }
    if (!parent_is_storage_root) {
        auto it = m_by_path.find(parent_path);
        if (it != m_by_path.end()) parent_handle = it->second;
    }

    w.u32(mtp_storage_for_path(*p));
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
bool MtpServer::send_stream_data(uint16_t code, uint32_t tid,
                                 Core::NspStream::Source& src) {
#ifdef PLATFORM_SWITCH
    const uint64_t size  = src.total_size();
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
    size_t filled = 0;
    while (filled < want) {
        const int64_t got = src.read(m_buf + Mtp::kHeaderSize + filled, want - filled);
        if (got < 0) return false;
        if (got == 0) break;
        filled += (size_t)got;
    }
    if (!ep_write(m_buf, Mtp::kHeaderSize + filled, 10000000000ULL)) return false;
    m_bytes_sent.fetch_add(Mtp::kHeaderSize + filled);

    uint64_t remaining = size - filled;
    while (remaining > 0 && !should_stop()) {
        const size_t chunk = (m_buf_size < remaining) ? m_buf_size : (size_t)remaining;
        size_t n = 0;
        while (n < chunk) {
            const int64_t got = src.read(m_buf + n, chunk - n);
            if (got < 0) return false;
            if (got == 0) break;
            n += (size_t)got;
        }
        if (n == 0) break;                    // stream ended early
        if (!ep_write(m_buf, n, 10000000000ULL)) return false;
        m_bytes_sent.fetch_add(n);
        remaining -= n;
    }
    // Only a COMPLETE NSP is usable, so a short stream is a failure, not a
    // truncated success.
    return remaining == 0;
#else
    (void)code; (void)tid; (void)src;
    return false;
#endif
}

bool MtpServer::send_file_data(uint16_t code, uint32_t tid,
                               const std::string& vfs_path, uint64_t size,
                               uint64_t offset) {
#ifdef PLATFORM_SWITCH
    FILE* f = std::fopen(vfs_path.c_str(), "rb");
    if (!f) return false;

    // For a partial read (GetPartialObject), seek to the requested offset first;
    // `size` is then the number of bytes to send from there. offset==0 is a normal
    // full read.
    if (offset != 0 && std::fseek(f, (long)offset, SEEK_SET) != 0) {
        std::fclose(f);
        return false;
    }

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
    (void)code; (void)tid; (void)vfs_path; (void)size; (void)offset;
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
    // Publish the ETA denominator: `expected` is the wire (compressed) size the
    // host declared in SendObjectInfo. Reset the per-object received counter.
    m_wire_size.store(expected);
    m_wire_recv.store(0);

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
    m_wire_recv.fetch_add(first);   // wire payload only, for the ETA

    // Overlap the remaining USB reads with the SD writes; strictly alternating
    // leaves each idle while the other works.
    OverlapBuffer ov(m_buf_size, [&](const uint8_t* p, size_t n) {
        return std::fwrite(p, 1, n, f) == n;
    });

    while (written < payload && !should_stop()) {
        uint8_t* dst = ov.valid() ? ov.acquire() : m_buf;
        if (!dst) break;                                  // sink failed
        if (!ep_read(dst, m_buf_size, &got, 10000000000ULL)) break;
        if (got == 0) break;
        size_t take = got;
        if (written + take > payload) take = (size_t)(payload - written);   // ignore any ZLP/padding
        if (ov.valid()) {
            if (!ov.submit(take)) break;
        } else if (std::fwrite(dst, 1, take, f) != take) {
            break;
        }
        m_bytes_recv.fetch_add(got);
        m_wire_recv.fetch_add(take);   // wire payload only, for the ETA
        written += take;
    }
    const bool sink_ok = ov.valid() ? ov.flush() : true;   // drain before closing
    // Join the worker BEFORE fclose(). The sink closes over `f`; flush() waits
    // for in-flight work but does not stop the worker, so without this the
    // worker's thread object and fclose() are only ordered by the destructor,
    // which runs at scope exit — after fclose(). quiesce() makes the ordering
    // explicit rather than incidental. Same lesson as recv_install's cancel
    // path (2347-0018), lower stakes here because this path has no early cancel
    // branch, but the same class of bug if one is ever added.
    ov.quiesce();
    std::fclose(f);
    // NO-COMMIT: deliberate. On a save, the partial write and this removal are
    // BOTH uncommitted, so the journal discards them together at unmount and the
    // save returns to its last committed state. Not committing IS the rollback;
    // adding a commit here would make a failed transfer durable.
    if (!sink_ok) { std::remove(vfs_path.c_str()); return false; }

    if (written != payload) { std::remove(vfs_path.c_str()); return false; }  // NO-COMMIT: as above
    return true;
#else
    (void)vfs_path; (void)expected; return false;
#endif
}


// Persist the install log. A stream install has no on-screen overlay to scroll
// back through, so the file is the only forensic record of why it failed.
void MtpServer::save_install_log(const std::string& filename, bool ok) {
    const std::string dir = Config::get().paths.log_folder;
    if (!dir.empty() && !Fs::exists(dir)) Fs::make_directory(dir);  // NO-COMMIT: log folder on SD, never a save
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

// Refuse an install and leave a trace. MTP can only answer the host with a bare
// response code — libmtp turns that into "Could not send object info", which
// says nothing about why — so the reason has to be recorded on this side or it
// is lost. reset() first: m_install_progress may still hold the previous
// install's lines, and a rejection log stapled to an unrelated transfer's
// history is worse than none.
void MtpServer::reject_install(const std::string& filename, const std::string& reason) {
    m_install_progress.reset();
    m_install_progress.push_log("Rejected: " + filename);
    m_install_progress.push_log(reason);
    m_install_progress.message = reason;
    SDL_Log("MTP: rejected %s — %s", filename.c_str(), reason.c_str());
    save_install_log(filename, false);
}

void MtpServer::arm_incoming_object(uint32_t storage, uint32_t parent, uint16_t fmt,
                                    const std::string& filename, uint64_t size,
                                    bool size_exact, uint32_t tid) {
    m_pending_save_synth.clear();   // set below only for a Save Data destination

    // Refuse anything that could escape the storage root.
    if (filename.find('/') != std::string::npos ||
        filename.find('\\') != std::string::npos ||
        filename == "." || filename == "..") {
        send_response(Rc::InvalidParameter, tid); return;
    }

    // An install storage takes the file into NCM, not onto disk.
    if (is_install_storage(storage)) {
        if (fmt == Mtp::Fmt::Association) {
            reject_install(filename, "an install target takes files, not folders");
            send_response(Rc::AccessDenied, tid);
            return;
        }

        // Reject what we cannot install *now*, before the data phase.
        // Failing after the host has pushed several GB is both rude and
        // (since the endpoint stops being drained) a hang.
        //
        // .nsz joined .nsp in slice 4b: an NSZ is a PFS0 exactly like an
        // NSP, just with .ncz entries in place of .nca, so StreamInstaller
        // parses the container identically and NczWindow handles the
        // compressed entries. .xci/.xcz joined in 4c, which gave
        // StreamInstaller an HFS0 front-end; an XCZ is likewise just an XCI
        // whose secure partition holds .ncz files.
        std::string low = filename;
        for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
        auto has_ext = [&low](const char* ext) {
            const size_t n = std::strlen(ext);
            return low.size() > n && low.compare(low.size() - n, n, ext) == 0;
        };
        const bool is_xci = has_ext(".xci") || has_ext(".xcz");
        if (!has_ext(".nsp") && !has_ext(".nsz") && !is_xci) {
            reject_install(filename,
                "stream install accepts .nsp, .nsz, .xci and .xcz only");
            send_response(Rc::InvalidParameter, tid);
            return;
        }

        // An XCI is the one container whose length we cannot recover from its
        // own contents. A PFS0's last entry ends at the file's end, so
        // container_size() corrects a saturated 32-bit size as soon as the
        // header lands; a gamecard image continues past `secure` into padding
        // that belongs to the transfer but to no entry, so StreamInstaller
        // reports 0 and there is nothing to correct with. Without an exact
        // size the data phase has no bound: recv_install would read until the
        // host happened to send a ZLP, and a session that desyncs is worse
        // than a transfer that is refused.
        //
        // size_exact is false only when the host saturated at 0xFFFFFFFF, so
        // this refuses exactly the >= 4 GiB XCIs from hosts that never sent
        // SendObjectPropList. A smaller XCI over plain SendObjectInfo still
        // carries an exact size and is fine. This is what Option C bought.
        if (is_xci && !size_exact) {
            reject_install(filename,
                "an XCI of 4 GiB or more needs an exact 64-bit size, which this "
                "host did not send (MTP 1.1 SendObjectPropList). Try another "
                "client, or install this image from the SD card.");
            send_response(Rc::InvalidParameter, tid);
            return;
        }

        // Keys live in a singleton that must be load()ed before get()
        // is valid — a service thread gets nothing for free. Check here
        // rather than after the data phase, so a keyless console fails
        // instantly instead of eating a multi-GB transfer first.
        if (!Core::Keys::available()) {
            Core::Keys::load();
            if (!Core::Keys::available()) {
                reject_install(filename, Core::Keys::requirement_message());
                send_response(Rc::AccessDenied, tid);
                return;
            }
        }
        m_pending_storage    = storage;
        m_pending_path       = filename;   // name only; nothing is written to a filesystem
        m_pending_size       = size;
        m_pending_size_exact = size_exact;
        m_pending_valid      = true;
        const uint32_t h = intern("install:" + filename);
        const uint32_t rp[3] = {storage, Mtp::kRootParent, h};
        send_response(Rc::Ok, tid, rp, 3);
        return;
    }

    // ── Save Data ────────────────────────────────────────────────────────────
    // Mirrors FTP exactly. A write INSIDE a title's save resolves to the mounted
    // "save:/..." path and then goes through the guard, which raises the on-device
    // confirmation (Saves is ReadOnly + Confirm::OnDevice) and performs the write
    // if the user allows it. A write at the storage root, or inside one of the
    // synthesized levels, has no title behind it and is refused outright with NO
    // prompt — which is also what FTP does: to_vfs() returns "" for those levels
    // and the command answers 550 without asking anyone anything.
    //
    // RESOLVING BEFORE GUARDING IS THE WHOLE POINT. save_resolve() mounts the
    // title named in the synthetic path, so the guard classifies — and the user
    // confirms — the save the write will actually land in. Guarding the synthetic
    // path instead would default-deny (no surface claims that prefix), and
    // guarding a bare "save:/" derived from the storage root would confirm against
    // whichever save happened to be mounted from the last browse.
    std::string save_parent_synth;   // non-empty => this is a Save Data write
    {
        const std::string* sparent = (parent != Mtp::kRootParent && parent != 0)
                                     ? path_for(parent) : nullptr;
        if (sparent && Services::save_is_synthetic(*sparent))
            save_parent_synth = *sparent;      // copy: path_for points into m_paths

        // Storage root of Save Data: the level above any user, so there is no
        // (user, title) to resolve and nothing a prompt could truthfully name.
        if (storage == kStorageSaves && save_parent_synth.empty()) {
            send_response(Rc::AccessDenied, tid);
            return;
        }
    }

    std::string dir;
    if (!save_parent_synth.empty()) {
        // Mounts the named title, then hands back its concrete path. Empty means
        // the parent was a user folder or an unknown title — refuse, no prompt.
        dir = Services::save_resolve_synth(save_parent_synth);
        if (dir.empty()) { send_response(Rc::AccessDenied, tid); return; }
    } else if (parent == Mtp::kRootParent || parent == 0) {
        // Root of the TARGET storage — not always the SD card. Hardcoding "sdmc:/"
        // meant a write aimed at another storage's root silently landed on SD.
        dir = "sdmc:/";
        StorageSurface::Id sid;
        if (mtp_id_to_surface(storage, sid)) {
            if (const StorageSurface* surf = StorageCatalog::find(sid))
                if (surf->kind == StorageKind::Filesystem)
                    dir = std::string(surf->vfs_root) + "/";
        }
    } else {
        const std::string* pp = path_for(parent);
        if (!pp) { send_response(Rc::InvalidParentObject, tid); return; }
        dir = *pp;
    }
    const std::string dest = Fs::join(dir, filename);

    // The HANDLE must stay synthetic for a save object, even though the
    // filesystem work uses the concrete path: a handle keyed on "save:/slot1.dat"
    // would mean a different file as soon as another title is mounted, which is
    // the whole reason save objects are interned under "savedata:/" in the first
    // place. m_pending_path, by contrast, is what SendObject writes to, so it
    // must be the real path.
    const std::string handle_key = save_parent_synth.empty()
                                 ? dest
                                 : Fs::join(save_parent_synth, filename);

    // Write guard: protected storages (NAND, Save Data) need on-device
    // confirmation, and this
    // runs on the MTP worker thread so blocking here is correct. One check covers
    // both the folder branch below and the file transfer it arms. (The install
    // storages returned earlier and are not affected.)
    if (Services::guard_write("USB-MTP", "write", dest, Config::get().mtp.surfaces)
            != Services::WriteDecision::Allow) {
        send_response(Rc::AccessDenied, tid);
        return;
    }

    m_pending_storage = storage;

    if (fmt == Mtp::Fmt::Association) {
        // A folder is complete at SendObjectInfo time; no SendObject follows.
        if (!Services::SaveWrite::make_directory(dest)) { send_response(Rc::GeneralError, tid); return; }
        if (!Services::save_commit_if_save_path(dest)) {
            send_response(Rc::GeneralError, tid); return;
        }
        m_pending_valid = false;
        const uint32_t h = intern(handle_key);
        const uint32_t rp[3] = {storage, parent, h};
        send_response(Rc::Ok, tid, rp, 3);
        return;
    }

    m_pending_path       = dest;      // real path: SendObject writes here
    m_pending_save_synth = save_parent_synth.empty() ? std::string() : handle_key;
    m_pending_size       = size;
    m_pending_size_exact = size_exact;
    m_pending_valid      = true;

    const uint32_t h = intern(handle_key);
    const uint32_t rp[3] = {storage, parent, h};
    send_response(Rc::Ok, tid, rp, 3);
    return;
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
bool MtpServer::recv_install(uint32_t storage_id, const std::string& filename,
                             uint64_t size, bool size_exact) {
#ifdef PLATFORM_SWITCH
    const auto dest = (storage_id == kStorageNandInstall) ? Core::Ncm::Storage::BuiltIn
                                                          : Core::Ncm::Storage::SdCard;

    // get() is only valid after load(); without this the installer receives an
    // empty keyset and the CNMT fails to decrypt.
    if (!Core::Keys::available()) Core::Keys::load();
    if (!Core::Keys::available()) {
        m_install_progress.reset();
        m_install_progress.message = Core::Keys::requirement_message();
        m_install_progress.push_log("ERROR: " + Core::Keys::requirement_message());
        save_install_log(filename, false);
        return false;
    }

    m_install = std::make_unique<Install::StreamInstaller>(
        dest, Core::Keys::get(), m_install_progress);

    m_installing.store(true);
    if (!m_install->begin(filename, size)) {
        save_install_log(filename, false);
        m_install.reset(); m_installing.store(false); return false;
    }

    // Wire accounting for the screen's average/ETA. This is the install path —
    // the case that actually wants an ETA. (recv_file_data(), the plain file-copy
    // path, has its own copy of this; an install never goes through it.) Reset
    // the per-object received counter; the size is published below once payload
    // has been resolved to the best-known value.
    m_wire_size.store(0);
    m_wire_recv.store(0);

    // ── Drive the install via the transport-agnostic StreamDriver ────────────
    // MTP-specific work stays here: read the first transfer and strip the 12-byte
    // data-container header. Everything after — size correction, overlap buffer,
    // feed loop, and the load-bearing teardown order — lives in Install::drive(),
    // shared with the (upcoming) FTP and HTTP install paths.
    size_t got = 0;
    if (!ep_read(m_buf, m_buf_size, &got, 10000000000ULL)) { m_install->abort(); m_install.reset(); m_installing.store(false); return false; }
    if (got < Mtp::kHeaderSize) { m_install->abort(); m_install.reset(); m_installing.store(false); return false; }

    Mtp::Header h;
    if (!parse_header(m_buf, got, h) || h.type != Mtp::Type::Data) {
        m_install->abort(); m_install.reset(); m_installing.store(false); return false;
    }

    // MTP's data-container length is 32-bit (0xFFFFFFFF above 4 GiB). It seeds the
    // driver's size guess; the driver refines it from the PFS0/HFS0 table, and a
    // host-declared exact 64-bit size (size_exact) still wins.
    const bool undefined_len = (h.length == 0xFFFFFFFFu);
    const uint64_t declared = size_exact ? size
                             : (undefined_len ? 0 : (h.length - Mtp::kHeaderSize));

    Install::StreamSource src;
    src.buffer      = m_buf;
    src.buffer_size = m_buf_size;
    src.read = [this](uint8_t* buf, size_t n) -> ssize_t {
        size_t rd = 0;
        if (!ep_read(buf, n, &rd, 10000000000ULL)) return -1;
        m_bytes_recv.fetch_add(rd);   // raw bytes over the wire (matches ↓recv total)
        return (ssize_t)rd;   // 0 = ZLP/end
    };
    src.stop  = [this] { return should_stop(); };
    src.drain = [this](uint64_t remaining) { drain_data(remaining); };

    Install::WireSink wire;
    wire.set_size = [this](uint64_t sz) { m_wire_size.store(sz); };
    wire.add_recv = [this](uint64_t n)  { m_wire_recv.fetch_add(n); };

    Install::FirstChunk first{ m_buf + Mtp::kHeaderSize, got - Mtp::kHeaderSize };

    const Install::DriveResult dr =
        Install::drive(*m_install, src, first, declared, size_exact, wire);

    switch (dr) {
        case Install::DriveResult::Ok:
            save_install_log(filename, true);
            m_install.reset(); m_installing.store(false);
            return true;
        case Install::DriveResult::Cancelled:
            m_install_progress.push_log("Install cancelled by user");
            save_install_log(filename, false);
            m_install.reset(); m_installing.store(false);
            return false;
        case Install::DriveResult::ShortRead:
            m_install_progress.push_log("ERROR: transfer ended early");
            save_install_log(filename, false);
            m_install.reset(); m_installing.store(false);
            return false;
        case Install::DriveResult::FeedError:
        case Install::DriveResult::FinishError:
        default:
            save_install_log(filename, false);
            m_install.reset(); m_installing.store(false);
            return false;
    }

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
            // Iterate the CATALOG, not a hand-written list. This was eight
            // hardcoded lines, and adding Gamecard meant touching FOUR separate
            // places: the id constant, mtp_id_to_surface(), surface_to_mtp_id()
            // and this. I updated three and missed this one, which is why the card
            // showed on FTP and HTTP and not here — the same class of miss as the
            // round before, in a list I had just written a lesson about.
            //
            // Deriving the list removes the fourth place permanently: a surface
            // that has a wire id and is enabled now appears here automatically,
            // exactly as it does on the transports that never had this problem.
            // Order follows catalog order, which is not semantically meaningful to
            // MTP (hosts key off the ids) and is the canonical order elsewhere.
            std::vector<uint32_t> ids;
            for (const auto& s : Services::StorageCatalog::all()) {
                const uint32_t id = surface_to_mtp_id(s.id);
                if (id && storage_enabled(id)) ids.push_back(id);
            }
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
            // Accept any enabled Filesystem storage (SD, Album, ...) or the
            // "all storages" sentinel. The storage's vfs_root is its browse root.
            std::string storage_root;
            if (c.param[0] == 0xFFFFFFFF) {
                storage_root = "sdmc:/";   // "any" defaults to SD's tree
            } else if (c.param[0] == kStorageTitles) {
                // Synthesized: one virtual NSP per installed title, interned under
                // the titles: prefix so each gets a normal object handle.
                if (!storage_enabled(kStorageTitles)) {
                    send_response(Rc::InvalidStorageId, tid); break;
                }
                const uint32_t parent_t = (c.nparams >= 3) ? c.param[2] : Mtp::kRootParent;
                std::vector<uint32_t> handles;
                if (parent_t == Mtp::kRootParent || parent_t == 0) {
                    for (const auto& e : Services::installed_titles_list())
                        handles.push_back(intern(std::string(kTitlesPrefix) + e.name));
                }
                if (c.hdr.code == Op::GetNumObjects) {
                    const uint32_t n = (uint32_t)handles.size();
                    send_response(Rc::Ok, tid, &n, 1);
                } else {
                    Writer w; w.au32(handles);
                    if (send_data(Op::GetObjectHandles, tid, w.data()))
                        send_response(Rc::Ok, tid);
                }
                break;
            } else if (c.param[0] == kStorageSaves) {
                // Three levels: users, that user's titles, then the real save.
                // Only the third has a filesystem behind it, and reaching it
                // mounts that (user, title) — see save_surface.hpp. Every entry
                // is interned under the "savedata:/" prefix so a handle names a
                // specific title's file and cannot be confused with the same
                // leaf name in another title's save.
                if (!storage_enabled(kStorageSaves)) {
                    send_response(Rc::InvalidStorageId, tid); break;
                }
                const uint32_t parent_s = (c.nparams >= 3) ? c.param[2] : Mtp::kRootParent;
                std::vector<uint32_t> handles;

                if (parent_s == Mtp::kRootParent || parent_s == 0) {
                    for (const auto& name : Services::save_user_names())
                        handles.push_back(intern(Services::save_synth_path(name)));
                } else {
                    const std::string* ps = path_for(parent_s);
                    if (!ps || !Services::save_is_synthetic(*ps)) {
                        send_response(Rc::InvalidParentObject, tid); break;
                    }
                    const std::string rel = Services::save_synth_rel(*ps);
                    if (rel.empty()) {   // no handle is ever interned for the root
                        send_response(Rc::InvalidParentObject, tid); break;
                    }
                    const auto sp = Services::sp_split_save(rel);
                    if (sp.level == Services::SavePath::Level::Titles) {
                        for (const auto& label : Services::save_title_labels(sp.user))
                            handles.push_back(intern(
                                Services::save_synth_path(rel + "/" + label)));
                    } else {
                        // Inside a title: mount it and list the real filesystem.
                        // Every child belongs to the SAME title, so the single
                        // mount slot stays put for the whole listing.
                        const std::string dir = Services::save_resolve(sp);
                        if (dir.empty()) {
                            send_response(Rc::InvalidParentObject, tid); break;
                        }
                        bool sok = false;
                        for (const auto& e : Fs::list(dir, &sok))
                            handles.push_back(intern(
                                Services::save_synth_path(rel + "/" + e.name)));
                        if (!sok) { send_response(Rc::InvalidParentObject, tid); break; }
                    }
                }

                if (c.hdr.code == Op::GetNumObjects) {
                    const uint32_t n = (uint32_t)handles.size();
                    send_response(Rc::Ok, tid, &n, 1);
                } else {
                    Writer w; w.au32(handles);
                    if (send_data(Op::GetObjectHandles, tid, w.data()))
                        send_response(Rc::Ok, tid);
                }
                break;
            } else {
                StorageSurface::Id sid;
                const StorageSurface* surf = nullptr;
                if (mtp_id_to_surface(c.param[0], sid))
                    surf = StorageCatalog::find(sid);
                if (!surf || surf->kind != StorageKind::Filesystem ||
                    !StorageCatalog::enabled(sid, Config::get().mtp.surfaces)) {
                    send_response(Rc::InvalidStorageId, tid); break;
                }
                storage_root = std::string(surf->vfs_root) + "/";
            }
            const uint32_t parent = (c.nparams >= 3) ? c.param[2] : Mtp::kRootParent;

            std::string dir;
            if (parent == Mtp::kRootParent || parent == 0) {
                dir = storage_root;
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

            // Virtual NSP: build and stream it rather than reading a file.
            if (path.compare(0, 8, kTitlesPrefix) == 0) {
                const std::string leaf = path.substr(8);
                Core::Ncm::Title t;
                if (!Services::installed_titles_find(leaf, t)) {
                    send_response(Rc::InvalidObjectHandle, tid); break;
                }
                auto src = Core::NspStream::open(t, Core::Keys::get(), nullptr);
                if (!src) { send_response(Rc::GeneralError, tid); break; }
                if (send_stream_data(Op::GetObject, tid, *src))
                    send_response(Rc::Ok, tid);
                else
                    send_response(Rc::IncompleteTransfer, tid);
                break;
            }

            // Save objects name a file inside a specific title's save; resolving
            // mounts that title. A synthesized folder level is not readable.
            const std::string rpath = readable_path(path);
            if (rpath.empty()) { send_response(Rc::InvalidObjectHandle, tid); break; }

            if (send_file_data(Op::GetObject, tid, rpath, Fs::file_size(rpath)))
                send_response(Rc::Ok, tid);
            else
                send_response(Rc::IncompleteTransfer, tid);
            break;
        }

        case Op::GetPartialObject: {
            // params: handle, offset, maxbytes. Reads a byte range — this is what
            // hosts (e.g. Linux gvfs/MTP) use to OPEN a file in place rather than
            // downloading the whole thing, so without it "open" fails and the user
            // must copy the file out first. The response's final parameter is the
            // actual number of bytes sent.
            if (c.nparams < 3) { send_response(Rc::InvalidParameter, tid); break; }
            const std::string* p = path_for(c.param[0]);
            if (!p) { send_response(Rc::InvalidObjectHandle, tid); break; }
            if (Fs::is_directory(*p)) { send_response(Rc::InvalidObjectHandle, tid); break; }
            const std::string path = readable_path(*p);
            if (path.empty()) { send_response(Rc::InvalidObjectHandle, tid); break; }
            const uint64_t fsize  = Fs::file_size(path);
            const uint64_t offset = c.param[1];
            uint64_t count        = c.param[2];
            if (offset >= fsize) count = 0;                       // nothing past EOF
            else if (offset + count > fsize) count = fsize - offset;  // clamp to EOF
            if (send_file_data(Op::GetPartialObject, tid, path, count, offset)) {
                const uint32_t sent = (uint32_t)count;
                send_response(Rc::Ok, tid, &sent, 1);
            } else {
                send_response(Rc::IncompleteTransfer, tid);
            }
            break;
        }

        case Op::GetPartialObject64: {
            // Android i/o extension. This is what gvfs/libmtp actually use to open
            // a file in place. Params: handle, offset_lo, offset_hi, count. The
            // 64-bit offset is split across two u32 params. Response's final
            // parameter is the number of bytes sent.
            if (c.nparams < 4) { send_response(Rc::InvalidParameter, tid); break; }
            const std::string* p = path_for(c.param[0]);
            if (!p) { send_response(Rc::InvalidObjectHandle, tid); break; }
            if (Fs::is_directory(*p)) { send_response(Rc::InvalidObjectHandle, tid); break; }
            const std::string path = readable_path(*p);
            if (path.empty()) { send_response(Rc::InvalidObjectHandle, tid); break; }
            const uint64_t fsize  = Fs::file_size(path);
            const uint64_t offset = (uint64_t)c.param[1] | ((uint64_t)c.param[2] << 32);
            uint64_t count        = c.param[3];
            if (offset >= fsize) count = 0;
            else if (offset + count > fsize) count = fsize - offset;
            if (send_file_data(Op::GetPartialObject64, tid, path, count, offset)) {
                const uint32_t sent = (uint32_t)count;
                send_response(Rc::Ok, tid, &sent, 1);
            } else {
                send_response(Rc::IncompleteTransfer, tid);
            }
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

            // ObjectCompressedSize is u32. A host reports 0xFFFFFFFF for
            // anything >= 4 GiB, so the value is only meaningful below that —
            // which is exactly why SendObjectPropList exists.
            arm_incoming_object(storage, parent, fmt, filename, comp_size,
                                comp_size != 0xFFFFFFFFu, tid);
            break;
        }

        case Op::SendObjectPropList: {
            // MTP 1.1. Parameters: storage, parent, format, then
            // ObjectCompressedSize split HIGH/LOW across params 4 and 5. That
            // split is the whole point: it is the only route by which a host can
            // tell us an object is larger than 4 GiB.
            if (c.nparams < 5) { send_response(Rc::InvalidParameter, tid); break; }
            const uint32_t storage = c.param[0];
            const uint32_t parent  = c.param[1];
            const uint16_t fmt     = (uint16_t)c.param[2];
            const uint64_t size    = ((uint64_t)c.param[3] << 32) | (uint64_t)c.param[4];
            if (storage != 0 && !storage_enabled(storage)) {
                send_response(Rc::InvalidStorageId, tid); break;
            }

            std::vector<uint8_t> data;
            if (!read_data_container(data)) { send_response(Rc::IncompleteTransfer, tid); break; }

            std::vector<Mtp::ObjectProp> props;
            uint32_t bad = 0;
            if (!Mtp::parse_object_prop_list(data.data(), data.size(), props, &bad)) {
                // The spec expects the index of the offending element back, so
                // the host can tell which property we choked on.
                const uint32_t rp[1] = {bad};
                send_response(Rc::InvalidObjectPropFormat, tid, rp, 1);
                break;
            }

            // The filename lives in the dataset, not the parameters.
            const Mtp::ObjectProp* fn = Mtp::find_prop(props, Mtp::ObjProp::ObjectFileName);
            if (!fn || fn->text.empty()) { send_response(Rc::InvalidDataset, tid); break; }

            arm_incoming_object(storage, parent, fmt, fn->text, size, true, tid);
            break;
        }

        case Op::GetObjectPropDesc: {
            // Params: property code, object format. A host asks this about EVERY
            // property GetObjectPropsSupported named, to learn its datatype
            // before it can encode a value. libmtp abandons the whole send if
            // one fails — "could not get property description" — so this is a
            // hard obligation created by the answer above, not an extra.
            if (c.nparams < 1) { send_response(Rc::InvalidParameter, tid); break; }
            Writer w;
            if (!Mtp::build_object_prop_desc((uint16_t)c.param[0], w)) {
                send_response(Rc::ObjectPropNotSupported, tid);
                break;
            }
            if (send_data(Op::GetObjectPropDesc, tid, w.data()))
                send_response(Rc::Ok, tid);
            break;
        }

        case Op::GetObjectPropValue: {
            // Params: object handle, property code. Return that ONE property's
            // value in its declared datatype. This is what gvfs/libmtp call for
            // every object to learn its size and name — 261 times in the wire log.
            // Without it we returned OperationNotSupported for each, so the host
            // never learned file sizes and rendered every file BLANK, never even
            // issuing a read. The datatypes MUST match build_object_prop_desc()
            // exactly or the host desyncs.
            if (c.nparams < 2) { send_response(Rc::InvalidParameter, tid); break; }
            const std::string* pv = path_for(c.param[0]);
            if (!pv) { send_response(Rc::InvalidObjectHandle, tid); break; }

            // Resolve save objects the same way build_object_info does, and for
            // the same reason: the synthesized levels answer without mounting,
            // the real ones resolve through the shared choke point. Getting this
            // wrong here (not in GetObjectInfo) is what made every file render
            // blank the last time — gvfs reads size through THIS path.
            const bool pv_is_save = Services::save_is_synthetic(*pv);
            const bool pv_save_synth = pv_is_save &&
                                       Services::save_synth_is_synthesized_dir(*pv);
            std::string pv_real;
            if (pv_is_save && !pv_save_synth) {
                pv_real = Services::save_resolve_synth(*pv);
                if (pv_real.empty()) { send_response(Rc::InvalidObjectHandle, tid); break; }
            }
            const bool pv_is_dir = pv_save_synth ? true
                                 : pv_is_save    ? Fs::is_directory(pv_real)
                                                 : Fs::is_directory(*pv);
            const uint16_t pv_prop = (uint16_t)c.param[1];
            Writer pw;
            bool pv_ok = true;

            if (pv_prop == Mtp::ObjProp::StorageId) {
                pw.u32(mtp_storage_for_path(*pv));
            } else if (pv_prop == Mtp::ObjProp::ObjectFormat) {
                pw.u16(pv_is_dir ? Mtp::Fmt::Association : Mtp::Fmt::Undefined);
            } else if (pv_prop == Mtp::ObjProp::ProtectionStatus) {
                pw.u16(0);
            } else if (pv_prop == Mtp::ObjProp::ObjectSize) {
                if (pv->compare(0, 8, kTitlesPrefix) == 0)
                    pw.u64(Services::installed_titles_exact_size(pv->substr(8)));
                else if (pv_is_dir)
                    pw.u64(0);
                else if (pv_is_save)
                    pw.u64(Fs::file_size(pv_real));
                else
                    pw.u64(Fs::file_size(*pv));   // the crucial one
            } else if (pv_prop == Mtp::ObjProp::ObjectFileName) {
                pw.str(Fs::basename(*pv));
            } else if (pv_prop == Mtp::ObjProp::ParentObject) {
                const std::string parent_path = Fs::parent(*pv);
                uint32_t parent_handle = Mtp::kRootParent;
                bool is_root = parent_path.empty() ||
                               parent_path == Services::save_synth_prefix();
                for (const auto& s : Services::StorageCatalog::all()) {
                    if (s.kind != Services::StorageKind::Filesystem) continue;
                    const std::string root = s.vfs_root;
                    if (parent_path == root || parent_path == root + "/") { is_root = true; break; }
                }
                if (!is_root) {
                    auto it = m_by_path.find(parent_path);
                    if (it != m_by_path.end()) parent_handle = it->second;
                }
                pw.u32(parent_handle);
            } else {
                pv_ok = false;
            }

            if (!pv_ok) {
                send_response(Rc::ObjectPropNotSupported, tid);
            } else if (send_data(Op::GetObjectPropValue, tid, pw.data())) {
                send_response(Rc::Ok, tid);
            }
            break;
        }

        case Op::SetObjectPropValue: {
            // Params: object handle, property code; the new value arrives in a data
            // phase. This is how hosts RENAME over MTP (libmtp sets ObjectFileName)
            // — without it clients report "no known way of setting metadata" and
            // rename fails even though the descriptor advertises the property as
            // settable.
            if (c.nparams < 2) { send_response(Rc::InvalidParameter, tid); break; }
            const uint32_t sp_handle = c.param[0];
            const uint16_t sp_prop   = (uint16_t)c.param[1];

            const std::string* sp_cur = path_for(sp_handle);
            if (!sp_cur) { send_response(Rc::InvalidObjectHandle, tid); break; }
            const std::string sp_old = *sp_cur;   // copy before any map mutation

            std::vector<uint8_t> sp_data;
            if (!read_data_container(sp_data)) {
                send_response(Rc::IncompleteTransfer, tid); break;
            }

            if (sp_prop == Mtp::ObjProp::ObjectFileName) {
                Reader sr(sp_data.data(), sp_data.size());
                std::string sp_name;
                if (!sr.str(sp_name) || sp_name.empty()) {
                    send_response(Rc::InvalidParameter, tid); break;
                }
                // Reject path separators: a rename may only change the leaf name,
                // never relocate an object into another directory or storage.
                if (sp_name.find('/') != std::string::npos ||
                    sp_name.find('\\') != std::string::npos) {
                    send_response(Rc::InvalidParameter, tid); break;
                }
                // For a save object the guard and the rename both work on the
                // MOUNTED path, while the HANDLE keeps its synthetic key — the
                // handle must keep naming (user, title, leaf), not a bare
                // "save:/..." that means something else once another title mounts.
                const bool sp_is_save = Services::save_is_synthetic(sp_old);
                std::string sp_real_old = sp_old;
                if (sp_is_save) {
                    sp_real_old = Services::save_resolve_synth(sp_old);
                    // A synthesized level (a user folder) has no file behind it.
                    if (sp_real_old.empty()) { send_response(Rc::AccessDenied, tid); break; }
                }
                const std::string sp_real_new = Fs::join(Fs::parent(sp_real_old), sp_name);

                // Rename mutates BOTH names, so it goes through the move guard:
                // renaming inside NAND or a save (or out of one) needs on-device
                // confirmation, asked as ONE prompt.
                if (Services::save_is_mount_root(sp_real_old)) {
                    send_response(Rc::AccessDenied, tid); break;
                }
                if (Services::guard_move("USB-MTP", "rename", sp_real_old, sp_real_new,
                                         Config::get().mtp.surfaces)
                        != Services::WriteDecision::Allow) {
                    send_response(Rc::AccessDenied, tid); break;
                }
                if (!Services::SaveWrite::rename(sp_real_old, sp_real_new)) {
                    send_response(Rc::GeneralError, tid); break;
                }
                if (!Services::save_commit_if_save_path(sp_real_new)) {
                    send_response(Rc::GeneralError, tid); break;
                }
                const std::string sp_new = Fs::join(Fs::parent(sp_old), sp_name);
                // Keep the handle valid and pointing at the new path, so the host
                // can keep using it without a full re-enumeration.
                if (sp_handle >= 1 && sp_handle <= m_paths.size()) {
                    m_by_path.erase(sp_old);
                    m_paths[sp_handle - 1] = sp_new;
                    m_by_path[sp_new] = sp_handle;
                }
                send_response(Rc::Ok, tid);
                break;
            }

            if (sp_prop == Mtp::ObjProp::ProtectionStatus) {
                // We don't track per-object protection; accept and ignore rather
                // than failing a client's housekeeping mid-rename.
                send_response(Rc::Ok, tid);
                break;
            }

            send_response(Rc::ObjectPropNotSupported, tid);
            break;
        }

        case Op::GetObjectPropsSupported: {
            if (c.nparams < 1) { send_response(Rc::InvalidParameter, tid); break; }
            // Not decoration: libmtp calls this BEFORE SendObjectPropList and
            // gives up if it fails, so 0x9808 is unreachable without it.
            //
            // Answering here creates an obligation. The host then calls
            // GetObjectPropDesc for EVERY code named below, and abandons the
            // send if any one is missing. So this list must satisfy both:
            //   * build_object_prop_desc() can describe it, and
            //   * parse_object_prop_list() can decode the value it invites.
            // tests/mtp_protocol_test.cpp asserts both directions over exactly
            // this list; keep them in step.
            Writer w;
            w.au16({Mtp::ObjProp::StorageId, Mtp::ObjProp::ObjectFormat,
                    Mtp::ObjProp::ProtectionStatus, Mtp::ObjProp::ObjectSize,
                    Mtp::ObjProp::ObjectFileName, Mtp::ObjProp::ParentObject});
            if (send_data(Op::GetObjectPropsSupported, tid, w.data()))
                send_response(Rc::Ok, tid);
            break;
        }

        case Op::SendObject: {
            // Must be preceded by SendObjectInfo or SendObjectPropList — the
            // spec has no other way to know where the bytes belong. Both arm the
            // same m_pending_* state; they differ only in whether the declared
            // size can be trusted at or above 4 GiB.
            if (!m_pending_valid) { send_response(Rc::GeneralError, tid); break; }
            std::string       dest    = m_pending_path;
            const uint64_t    size    = m_pending_size;
            const uint32_t    storage = m_pending_storage;
            const bool        exact   = m_pending_size_exact;
            const std::string save_synth = m_pending_save_synth;
            m_pending_valid = false;   // one SendObject per declaration
            m_pending_save_synth.clear();

            // A Save Data destination is re-resolved here, because this is a
            // SEPARATE command from the SendObjectInfo that armed it. A client is
            // free to browse another title in between, which remounts the single
            // save slot and would leave `dest` — a bare "save:/..." path — aimed
            // inside a different game's save. Re-resolving from the synthetic key
            // re-establishes the (user, title) this object names.
            //
            // No second confirmation: the synthetic key pins user, title and leaf,
            // so re-resolving cannot reach a destination the user did not already
            // approve. It can only fail, and failing is a refusal.
            if (!save_synth.empty()) {
                dest = Services::save_resolve_synth(save_synth);
                if (dest.empty()) { send_response(Rc::AccessDenied, tid); break; }
            }

            if (is_install_storage(storage)) {
                send_response(recv_install(storage, dest, size, exact) ? Rc::Ok : Rc::GeneralError, tid);
            } else {
                bool ok = recv_file_data(dest, size);
                // Commit BEFORE answering: the client treats our Ok as durable,
                // and an uncommitted save write is discarded at unmount. Reporting
                // success for bytes that will vanish is the worst available answer.
                if (ok && !Services::save_commit_if_save_path(dest)) ok = false;
                send_response(ok ? Rc::Ok : Rc::IncompleteTransfer, tid);
            }
            break;
        }

        case Op::DeleteObject: {
            if (c.nparams < 1) { send_response(Rc::InvalidParameter, tid); break; }
            const std::string* p = path_for(c.param[0]);
            if (!p) { send_response(Rc::InvalidObjectHandle, tid); break; }
            const std::string path = *p;   // copy: path_for points into m_paths

            // A save object is guarded by its MOUNTED path, not its handle key —
            // resolving first mounts the title the handle names, so the guard
            // classifies and the user confirms the file that will actually be
            // deleted. Same order as FTP's DELE (to_vfs, then guard_write). An
            // unresolvable path is a synthesized level with no file behind it:
            // refuse without asking, exactly as FTP answers 550 there.
            std::string target = path;
            if (Services::save_is_synthetic(path)) {
                target = Services::save_resolve_synth(path);
                if (target.empty()) {
                    Services::guard_log_note("USB-MTP", "delete", "REJECT",
                                             "save-unresolved", path);
                    send_response(Rc::AccessDenied, tid); break;
                }
            }

            // The save mount root is the filesystem, not an object in it. A host
            // deleting a title folder clears its contents and then removes the
            // folder; prompting for that and failing anyway teaches the user that
            // approving these dialogs does nothing.
            // Deleting the save "folder" over MTP means wiping the save: the
            // title folder is the synthesized mount point, not an rmdir target.
            // Guarded, because wiping a save is destructive and unrecoverable.
            if (Services::save_is_mount_root(target)) {
                if (Services::guard_write("USB-MTP", "wipe save", target,
                                          Config::get().mtp.surfaces)
                        != Services::WriteDecision::Allow) {
                    send_response(Rc::AccessDenied, tid); break;
                }
                const bool wiped = Services::save_wipe(target);
                if (!wiped) Services::guard_log_note("USB-MTP", "wipe save", "FAILED",
                                                     "wipe-failed", target);
                send_response(wiped ? Rc::Ok : Rc::GeneralError, tid);
                break;
            }
            if (Services::guard_write("USB-MTP", "delete", target, Config::get().mtp.surfaces)
                    != Services::WriteDecision::Allow) {
                send_response(Rc::AccessDenied, tid);
                break;
            }
            bool ok = Fs::is_directory(target)
                          ? Services::SaveWrite::remove_directory_recursive(target)
                          : Services::SaveWrite::remove_file(target);
            if (!ok) Services::guard_log_note("USB-MTP", "delete", "FAILED",
                                              "fs-remove-failed", target);
            // A journalled save discards uncommitted changes at unmount, so a
            // delete that is not committed comes back. No-op for other surfaces.
            if (ok && !Services::save_commit_if_save_path(target)) ok = false;
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
