#pragma once
// source/install/nsp_reader.hpp
// Parse a PFS0 (NSP) container from a file path.
//
// PFS0 is the outer container format for NSP files. Layout:
//   0x00  magic "PFS0"
//   0x04  u32 file_count
//   0x08  u32 string_table_size
//   0x0C  u32 reserved
//   0x10  entries[file_count] x 0x18 each:
//           u64 data_offset    (relative to start of file data region)
//           u64 data_size
//           u32 name_offset    (into string table)
//           u32 reserved
//   then string table (NUL-terminated names)
//   then file data (at offset 0x10 + file_count*0x18 + string_table_size,
//                   padded to a 0x10 boundary)
//
// The reader opens the file once and holds the fd open for streaming. Callers
// read individual files by entry index; reading is done via the ReadFn callback
// so the installer can pipe data directly into NCM without an intermediate buffer.

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace Install {

struct PfsEntry {
    std::string name;
    uint64_t    offset = 0;   // absolute offset in the .nsp file
    uint64_t    size   = 0;
    bool        is_cnmt_nca = false;   // name ends in .cnmt.nca
    bool        is_nca      = false;   // name ends in .nca (covers .cnmt.nca too)
    bool        is_tik      = false;   // name ends in .tik
    bool        is_cert     = false;   // name ends in .cert
    bool        is_ncz      = false;   // name ends in .ncz (compressed NCA)
};

class NspReader {
public:
    explicit NspReader(const std::string& path);
    ~NspReader();

    // Non-copyable.
    NspReader(const NspReader&)            = delete;
    NspReader& operator=(const NspReader&) = delete;

    // true if the file was opened and PFS0 header parsed successfully.
    bool valid() const { return m_valid; }
    const std::string& error() const { return m_error; }

    const std::vector<PfsEntry>& entries() const { return m_entries; }

    // Read bytes from entry at index `idx`, starting at `offset_in_entry` bytes
    // into that entry's data, into `buf` (at most `len` bytes). Returns the
    // number of bytes actually read, or 0 on error / EOF.
    size_t read(size_t idx, uint64_t offset_in_entry, void* buf, size_t len);

    // Convenience: fill `out` with the full content of entry `idx`. Only safe
    // for small files (tik, cert, cnmt). Returns false if the entry exceeds 1 MB.
    bool read_all(size_t idx, std::vector<uint8_t>& out);

private:
    FILE*                m_fp    = nullptr;
    bool                 m_valid = false;
    std::string          m_error;
    std::vector<PfsEntry> m_entries;

    void parse();
};

} // namespace Install
