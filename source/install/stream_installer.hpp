#pragma once
// source/install/stream_installer.hpp
//
// Push-based NSP installer for transports that deliver bytes sequentially and
// cannot seek: MTP/USB today, FTP/HTTP later.
//
// Why this exists: Install::install() is pull-based random-access — it reads the
// CNMT first (which can sit anywhere in the PFS0) and then reads each NCA by
// offset. A USB stream can only go forwards, so the existing installer cannot
// drive it directly. Buffering the whole NSP to disk first would defeat the
// point: the 4 GiB FAT32 file ceiling is precisely why stream install exists.
//
// How it works, and why install() is reused rather than reimplemented:
//   1. Parse the PFS0 header/table as it arrives, so every entry's byte range
//      is known before its data shows up.
//   2. Stream each NCA straight into an NCM placeholder and register it the
//      moment its last byte lands. Nothing is staged on the filesystem.
//   3. Tee the small entries (.cnmt.nca, .tik, .cert) into RAM as they pass.
//   4. finish() hands the entry list to the *existing, hardware-validated*
//      install(), with the small entries served from RAM. Every NCA hits
//      install()'s "already registered -> skip" path and is never read, so
//      meta registration and ticket import run exactly as they do for a local
//      NSP. install() itself is untouched.
//
// Slice 4a: plain NSP only. A compressed entry (.ncz) is rejected up front
// rather than half-installed — NCZ needs a pull-style reader, which is 4b.

#include "core/keys.hpp"
#include "core/ncm.hpp"
#include "install/installer.hpp"

#include <cstdint>
#include <string>
#include <vector>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#endif

namespace Install {

#ifdef PLATFORM_SWITCH
// Defined in installer.cpp. Declared here rather than in installer.hpp because
// that header does not pull in <switch.h>, so NcmContentId is not visible there.
// Shared so the streaming and file paths use one content-id hex decode.
bool content_id_from_name(const std::string& name, NcmContentId& out);
#endif

class StreamInstaller {
public:
    StreamInstaller(Core::Ncm::Storage storage,
                    const Core::Keys::Keyset& keys,
                    Progress& progress);
    ~StreamInstaller();

    StreamInstaller(const StreamInstaller&) = delete;
    StreamInstaller& operator=(const StreamInstaller&) = delete;

    /// Begin a transfer. `total_size` is the declared NSP size.
    bool begin(const std::string& filename, uint64_t total_size);

    /// Push the next chunk of the container. Must be called with bytes in
    /// order and with no gaps. Returns false once the install has failed.
    bool feed(const uint8_t* data, size_t len);

    /// All bytes delivered: register meta and import tickets via install().
    bool finish();

    /// Abandon a partial transfer, deleting any open placeholder.
    void abort();

    /// True once every entry has been consumed.
    bool complete() const { return m_phase == Phase::Done; }

    /// The container's true size in bytes, derived from the PFS0 header — 0
    /// until the header and table have arrived. This is the authority for how
    /// much to read: MTP's ObjectCompressedSize and data-container length are
    /// both 32-bit and carry 0xFFFFFFFF for anything over 4 GiB, whereas the
    /// PFS0 entry table is 64-bit and always exact.
    uint64_t container_size() const { return m_container_size; }

    bool ok() const { return m_phase != Phase::Failed; }
    const std::string& error() const { return m_error; }

private:
    enum class Phase { Header, Table, Data, Done, Failed };

    struct StreamEntry {
        std::string name;
        uint64_t    abs_off = 0;      // absolute offset within the container
        uint64_t    size    = 0;
        bool is_nca = false, is_cnmt_nca = false, is_tik = false;
        bool is_cert = false, is_ncz = false;
        uint64_t abs_end() const { return abs_off + size; }
    };

    bool   parse_header();
    bool   parse_table();
    size_t feed_data(const uint8_t* data, size_t len);
    bool   begin_entry();
    bool   write_entry(const uint8_t* data, size_t len);
    bool   end_entry();
    bool   fail(const std::string& why);

    Core::Ncm::Storage        m_storage;
    const Core::Keys::Keyset& m_keys;
    Progress&                 m_progress;

    Phase       m_phase = Phase::Header;
    std::string m_error;
    std::string m_filename;
    uint64_t    m_total = 0;
    uint64_t    m_pos   = 0;          // absolute read position in the container
    uint64_t    m_container_size = 0; // known once the table is parsed

    std::vector<uint8_t> m_hdr;       // first 0x10 bytes
    std::vector<uint8_t> m_table;     // entry table + string table
    size_t               m_table_size = 0;

    std::vector<StreamEntry> m_entries;
    size_t                   m_cur = 0;
    bool                     m_entry_open = false;
    bool                     m_entry_skip = false;   // NCA already registered

    std::vector<uint8_t> m_tee;       // RAM copy of the current small entry
    std::vector<std::pair<std::string, std::vector<uint8_t>>> m_small;  // name -> bytes

#ifdef PLATFORM_SWITCH
    NcmContentStorage m_cs{};
    bool              m_have_cs = false;
    NcmContentId      m_cid{};
    NcmPlaceHolderId  m_ph{};
    uint64_t          m_ph_off = 0;
#endif
};

} // namespace Install
