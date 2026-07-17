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
// Slice 4b adds NSZ: an .ncz entry cannot be written through as it arrives,
// because NczDecompressor pulls by offset (and re-reads the header region) while
// USB pushes. Such an entry is fed to an NczWindow while a worker thread pulls
// through it and decompresses into the placeholder. See ncz_window.hpp for why a
// pipe cannot serve that, and end_entry()/abort() below for the join ordering.

#include "core/keys.hpp"
#include "core/ncm.hpp"
#include "install/installer.hpp"
#include "install/ncz_window.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#else
#include <thread>
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

    /// The container's size as derived from its own entry table — 0 until the
    /// table has arrived, and 0 for formats that cannot express it.
    ///
    /// This is NOT the authority for how much to read, and has not been since
    /// Option C: a host-declared 64-bit size (SendObjectPropList) outranks it,
    /// and this demotes to a cross-check that logs disagreements. The host's
    /// number describes the TRANSFER; this one describes its CONTENTS. They
    /// coincide for PFS0, whose last entry ends at the file's end, which is why
    /// the old inverted rule worked.
    ///
    /// They do not coincide for XCI, whose trailing padding — an untrimmed image
    /// is padded to the gamecard capacity — belongs to the transfer but to no
    /// entry. An XCI front-end must therefore report 0 here rather than infer a
    /// length from `secure`: coming up short does not fail an install, it leaves
    /// unread bytes in the endpoint and desyncs the session.
    uint64_t container_size() const { return m_container_size; }

    bool ok() const { return m_phase != Phase::Failed; }
    const std::string& error() const { return m_error; }

private:
    // The collector runs one rule: COLLECT `m_want_len` bytes at absolute offset
    // `m_want_off`, then run `m_step`. Everything between the current position
    // and `m_want_off` is discarded as it passes — a stream cannot rewind, so
    // bytes we do not want are simply dropped.
    //
    // PFS0 is two steps (header, then table) that happen to be contiguous. XCI
    // (slice 4c) is four steps at scattered offsets — magic at 0x100, the root
    // HFS0 wherever 0x130 points, then the `secure` HFS0 potentially gigabytes
    // downstream — and needs no new machinery, only new steps.
    enum class Phase { Collect, Data, Done, Failed };

    enum class Step {
        Pfs0Header,   // 0x10 bytes at 0: magic, file count, string-table size
        Pfs0Table,    // count*0x18 + sts bytes at 0x10: entry table + names
    };

    struct StreamEntry {
        std::string name;
        uint64_t    abs_off = 0;      // absolute offset within the container
        uint64_t    size    = 0;
        bool is_nca = false, is_cnmt_nca = false, is_tik = false;
        bool is_cert = false, is_ncz = false;
        uint64_t abs_end() const { return abs_off + size; }
    };

    /// Ask for `len` bytes at absolute offset `off`, to be handed to `step`.
    /// Refuses a backwards `off` outright: a stream cannot rewind, so asking is
    /// a caller bug, not a runtime condition. `off == m_pos` (contiguous) is
    /// normal; `off > m_pos` skips the bytes between.
    bool   want(uint64_t off, size_t len, Step step);
    /// Dispatch the completed collection to whichever step asked for it.
    bool   run_step();

    bool   parse_pfs0_header();
    bool   parse_pfs0_table();
    /// The format-independent tail: sort into stream order, NSZ key
    /// precondition, container size, NCA count, enter Phase::Data. Nothing below
    /// this line knows whether the bytes came from a PFS0 or an XCI's secure
    /// partition — which is what makes 4c a front-end rather than a rewrite.
    /// `container_size` is the front-end's business: exact for PFS0, 0 for XCI.
    bool   finalize_entries(uint64_t container_size);

    size_t feed_data(const uint8_t* data, size_t len);
    bool   begin_entry();
    bool   write_entry(const uint8_t* data, size_t len);
    bool   end_entry();
    bool   fail(const std::string& why);

    // ── NSZ (slice 4b) ───────────────────────────────────────────────────────
    /// Spin up the window and decompression worker for the current .ncz entry.
    bool ncz_begin();
    /// Runs on the worker: size the placeholder from the NCZ header, create it,
    /// then decompress into it. Pulls through m_ncz_win, which blocks until the
    /// MTP thread has pushed far enough.
    void ncz_worker();
    /// Close the window and join the worker. Safe to call when not running, and
    /// idempotent. `graceful` false means abandon (abort) rather than finish.
    /// ALWAYS joins before the caller touches the placeholder — see abort().
    void ncz_join(bool graceful);
#ifdef PLATFORM_SWITCH
    static void ncz_thread_entry(void* self);
#endif

    Core::Ncm::Storage        m_storage;
    const Core::Keys::Keyset& m_keys;
    Progress&                 m_progress;

    Phase       m_phase = Phase::Collect;
    std::string m_error;
    std::string m_filename;
    uint64_t    m_total = 0;
    uint64_t    m_pos   = 0;          // absolute read position in the container
    uint64_t    m_container_size = 0; // known once the table is parsed

    // ── The collector (see Phase/Step above) ─────────────────────────────────
    std::vector<uint8_t> m_blob;      // the bytes of the collection in flight
    uint64_t             m_want_off = 0;
    size_t               m_want_len = 0;
    Step                 m_step = Step::Pfs0Header;

    // Stashed by parse_pfs0_header(). parse_pfs0_table() cannot re-read these
    // from the header: the old design kept a header buffer and a table buffer
    // alive at once, whereas one general blob holds only the collection in
    // flight — by table-parse time it holds the TABLE, and reading counts out of
    // it would yield plausible garbage rather than an error.
    uint32_t m_pfs0_count = 0;
    uint32_t m_pfs0_sts   = 0;

    std::vector<StreamEntry> m_entries;
    size_t                   m_cur = 0;
    bool                     m_entry_open = false;
    bool                     m_entry_skip = false;   // NCA already registered

    std::vector<uint8_t> m_tee;       // RAM copy of the current small entry
    std::vector<std::pair<std::string, std::vector<uint8_t>>> m_small;  // name -> bytes

    // ── NSZ worker state (slice 4b) ──────────────────────────────────────────
    // m_ncz_error is written by the worker and read by the MTP thread only after
    // ncz_join(), which is a happens-before edge — no lock needed. Everything
    // else here is touched by one thread at a time by the same argument.
    std::unique_ptr<NczWindow> m_ncz_win;
    std::string                m_ncz_error;
    bool                       m_ncz_running = false;
#ifndef PLATFORM_SWITCH
    std::thread m_ncz_thread;
#endif

#ifdef PLATFORM_SWITCH
    Thread            m_ncz_thread{};
    NcmContentStorage m_cs{};
    bool              m_have_cs = false;
    NcmContentId      m_cid{};
    NcmPlaceHolderId  m_ph{};
    uint64_t          m_ph_off = 0;
    // The worker creates the placeholder (only it knows the decompressed size),
    // so ownership cannot be inferred from m_entry_open as it can on the plain
    // path. abort() consults this to decide whether there is anything to delete.
    bool              m_ph_open = false;
#endif
};

} // namespace Install
