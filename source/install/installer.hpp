#pragma once
// source/install/installer.hpp
// NSP/XCI installation pipeline (Milestone 5).
//
// Installation path (mirrors the Move NCM pipeline, validated in M4):
//
//   For each meta (cnmt.nca) found in the container:
//     1. Parse the CNMT to get the content meta key and content list.
//     2. For each NCA:
//        a. GeneratePlaceHolderId → CreatePlaceHolder(content_id, ph, size)
//        b. Stream NCA bytes from the container → WritePlaceHolder (4 MB chunks)
//        c. Register(content_id, ph) — moves placeholder to registered path
//     3. Write the content meta record into the destination meta database.
//     4. Commit the meta database.
//
// After content + meta-DB land, HOS auto-creates the application record. No
// PushApplicationRecord call needed — proven by the Move operation in M4.
// (The ns.c study confirmed: ns:am records are auto-populated when NCM has both
// content and a committed meta-DB entry for a program-type title.)
//
// Ticket install:
//   If the container includes a .tik file, it is installed via ES cmd 1
//   (ImportTicket). This is required for titlekey-crypto titles to launch.
//   Cert is also imported if present (ES cmd 2 ImportCertificate is not
//   used by libnx; we import the ticket which carries its own cert chain ref).
//
// Storage target: caller chooses SD or NAND (BuiltInUser). The user picks via
// the action menu in FileBrowser — same affordance as DBI.

#include "core/keys.hpp"
#include "core/ncm.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace Install {

// Live progress for the UI (same shape as Dump::Progress / MoveProgress).
struct Progress {
    std::atomic<bool>     running{false};
    std::atomic<bool>     done{false};
    std::atomic<bool>     success{false};
    std::atomic<bool>     cancel{false};
    std::atomic<uint64_t> bytes_total{0};
    std::atomic<uint64_t> bytes_done{0};
    std::atomic<int>      ncas_total{0};
    std::atomic<int>      ncas_done{0};
    std::string           current_file;
    std::string           stage;    // "reading cnmt" / "installing" / "registering meta"
    std::string           message;  // final status / error

    // Verbose, thread-safe install log. The worker thread appends one line per
    // checkpoint via push_log(); the UI snapshots it for the on-screen box and
    // for the persisted per-install log file. Kept separate from `message`
    // (which is only the final one-line status/error).
    mutable std::mutex        log_mutex;
    std::vector<std::string>  log_lines;

    void push_log(const std::string& line) {
        std::lock_guard<std::mutex> lk(log_mutex);
        log_lines.push_back(line);
        // Bound memory on pathological containers; keep the most recent lines.
        static constexpr size_t kMaxLines = 2000;
        if (log_lines.size() > kMaxLines)
            log_lines.erase(log_lines.begin(),
                            log_lines.begin() + (log_lines.size() - kMaxLines));
    }
    std::vector<std::string> log_snapshot() const {
        std::lock_guard<std::mutex> lk(log_mutex);
        return log_lines;
    }
    size_t log_count() const {
        std::lock_guard<std::mutex> lk(log_mutex);
        return log_lines.size();
    }

    float fraction() const {
        uint64_t t = bytes_total.load();
        return t ? (float)bytes_done.load() / (float)t : 0.f;
    }
    void reset() {
        running=false; done=false; success=false; cancel=false;
        bytes_total=0; bytes_done=0; ncas_total=0; ncas_done=0;
        current_file.clear(); stage.clear(); message.clear();
        std::lock_guard<std::mutex> lk(log_mutex);
        log_lines.clear();
    }
};

// ReadFn: called by the installer to stream NCA data from the container.
// Returns number of bytes read; 0 signals error or EOF.
using ReadFn = std::function<size_t(uint64_t offset_in_entry, void* buf, size_t len)>;

// A single installable content record (NCA or ticket/cert).
struct ContentEntry {
    std::string name;
    uint64_t    size = 0;
    bool        is_nca      = false;
    bool        is_cnmt_nca = false;
    bool        is_tik      = false;
    bool        is_cert     = false;
    bool        is_ncz      = false;
    ReadFn      read;         // bound to the container reader for this entry
};

// Install a collection of content entries (as produced from NspReader or
// XciReader) into the given storage. Runs synchronously on the calling thread;
// callers are expected to run this on a background thread and poll `progress`.
//
// `keys` must contain at least header_key (for CNMT NCA decryption).
// `storage` is the destination (SD or NAND).
// Returns true on success.
bool install(std::vector<ContentEntry> contents,
             Core::Ncm::Storage storage,
             const Core::Keys::Keyset& keys,
             Progress& progress);

// Convenience: build a ContentEntry list from an already-opened NspReader or
// XciReader (both expose the same PfsEntry vector + read() method). This is
// a template so we don't need to know the concrete reader type here.
template<typename Reader>
std::vector<ContentEntry> entries_from_reader(Reader& r) {
    std::vector<ContentEntry> out;
    const auto& pfs = r.entries();
    out.reserve(pfs.size());
    for (size_t i = 0; i < pfs.size(); ++i) {
        ContentEntry e;
        e.name        = pfs[i].name;
        e.size        = pfs[i].size;
        e.is_nca      = pfs[i].is_nca;
        e.is_cnmt_nca = pfs[i].is_cnmt_nca;
        e.is_tik      = pfs[i].is_tik;
        e.is_cert     = pfs[i].is_cert;
        e.is_ncz      = pfs[i].is_ncz;
        e.read        = [&r, i](uint64_t off, void* buf, size_t len) -> size_t {
            return r.read(i, off, buf, len);
        };
        out.push_back(std::move(e));
    }
    return out;
}

} // namespace Install
