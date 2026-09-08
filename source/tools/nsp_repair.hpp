#pragma once
// source/tools/nsp_repair.hpp
//
// Repair for dumped NSPs that are missing their ticket/cert pair.
//
// WHY THIS EXISTS: a titlekey-protected title whose common ticket cannot be
// fetched at dump time is still dumped - nsp_stream writes a COMPLETE, valid
// PFS0 container and records the failure in its note ("TITLEKEY TITLE WITH NO
// TICKET - may not install"). The user ends up with an NSP on the SD card
// that looks finished but will not install. The Tools op "Repair tickets with
// dump errors" finds such dumps, confirms a common ticket NOW exists on the
// console, and re-dumps the title through the proven pipeline.
//
// WHAT LIVES HERE: the pure, hardware-independent half - reading a PFS0
// container, locating the .cnmt.nca, and reading the CNMT record's title id
// to identify which title a dump belongs to. No libnx, no SD-card writes.
// The Switch-side half (ES ticket lookup, the re-dump, the Tools op wiring)
// lives in tools_screen.cpp, on top of these primitives.
//
// WHY THE RIGHTS ID IS NOT DERIVED HERE: the rights ID lives in the NCA
// header (offset 0x230), INSIDE the encrypted NCA entry - the outer PFS0
// container cannot see it. What it CAN see is the CNMT record's title id,
// which is sufficient: Core::Es::list_common_tickets() reports each common
// ticket's title id, so the caller matches dump to ticket by title id and lets
// the dump pipeline handle rights-id details.
//
// The PFS0 layout matches services/pfs0_layout.hpp: "PFS0" magic, u32 file
// count, u32 string-table size, u32 reserved, entries[0x18], string table,
// then file data.

#include <cstdint>
#include <string>
#include <vector>

namespace Tools::NspRepair {

// Repair state of a dumped NSP, decided from its contents alone.
enum class State {
    Complete,      // has a .tik entry - nothing to do
    MissingTik,    // no .tik entry - candidate for repair (titlekey crypto is
                   // confirmed by the caller, via the ES ticket list)
    Malformed,     // not a readable PFS0 / truncated header / no .cnmt.nca
};

struct Inspection {
    State       state = State::Malformed;
    std::string detail;            // human-readable reason for the state
    uint64_t    title_id = 0;      // from the CNMT record (0 when unreadable)
    bool        has_cert = false;  // .cert present alongside (informational)
    uint64_t    data_end = 0;      // end offset of the last file's data
};

// Read a PFS0 container and classify it. `data` holds the container (or its
// prefix - everything up to and including the .cnmt.nca entry's data is
// enough; entries beyond the buffer are ignored for classification).
Inspection inspect(const uint8_t* data, size_t size);

// Convenience overload for host tests and in-memory buffers.
Inspection inspect(const std::vector<uint8_t>& data);

// ── File-oriented primitives ─────────────────────────────────────────────────
// Scanning real dumps cannot read whole multi-GB files into memory, so the
// same parsing is exposed in seek-sized pieces: read the first 0x10 bytes →
// read_header; then the entries+string-table region → read_entries; then seek
// to header_size + entry.data_off and read the .cnmt.nca entry →
// title_id_from_cnmt_nca. inspect() above is implemented on top of these.

struct Header {
    bool     ok           = false;
    uint32_t count        = 0;
    uint32_t strtab_size  = 0;
    uint64_t header_size  = 0;   // 0x10 + 0x18*count + strtab_size (data base)
};

// `first` holds at least the first 0x10 bytes of the container.
Header read_header(const uint8_t* first, size_t size);

struct EntryInfo {
    std::string name;
    uint64_t    data_off = 0;   // relative to the data base (header_size)
    uint64_t    size     = 0;
};

// `entries_and_strtab` starts at container offset 0x10 and covers the entry
// array plus the whole string table (header_size - 0x10 bytes).
std::vector<EntryInfo> read_entries(const uint8_t* entries_and_strtab,
                                    size_t size, uint32_t count,
                                    uint32_t strtab_size);

// Extract the cnmt record's title id from a .cnmt.nca entry's data (the
// nested PFS0 walk). Returns 0 when the record cannot be read.
uint64_t title_id_from_cnmt_nca(const uint8_t* nca, size_t size);

// True when this filename is a ticket/cert entry ("<32 hex>.tik" / ".cert").
bool is_tik_name(const std::string& name);
bool is_cert_name(const std::string& name);

} // namespace Tools::NspRepair
