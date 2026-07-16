#pragma once
// source/install/xci_reader.hpp
// Parse an XCI (game card image) to extract the installable NSPs inside its
// Secure HFS0 partition.
//
// XCI layout:
//   0x000  XCI header (0x200 bytes): magic "HEAD", partition table at 0x128
//   0x200  HFS0 root partition table (lists: "update", "normal", "secure", "logo")
//   Secure partition contains the installable content as one NSP per meta
//   (base, update, DLC — same as a standalone NSP from a shop download).
//
// The strategy: open the XCI, locate the Secure HFS0 partition, enumerate its
// files (which are themselves HFS0-packaged NCAs — effectively inline NSPs).
// We expose them as a flat list of PfsEntry-compatible records so the installer
// can treat XCI exactly like NSP.
//
// HFS0 on-disk format:
//   0x00  magic "HFS0"
//   0x04  u32 file_count
//   0x08  u32 string_table_size
//   0x0C  u32 reserved
//   0x10  entries[file_count] x 0x40 each:
//           u64 data_offset  (relative to data region)
//           u64 data_size
//           u32 name_offset
//           u32 hash_region_size
//           u64 reserved
//           u8  sha256[0x20]
//   then string table | then data

#include "install/nsp_reader.hpp"   // reuses PfsEntry
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace Install {

class XciReader {
public:
    explicit XciReader(const std::string& path);
    ~XciReader();

    XciReader(const XciReader&)            = delete;
    XciReader& operator=(const XciReader&) = delete;

    bool valid() const { return m_valid; }
    const std::string& error() const { return m_error; }

    // Entries from the Secure partition (NCAs / cnmt.nca files).
    const std::vector<PfsEntry>& entries() const { return m_entries; }

    // Read bytes from entry `idx` at `offset_in_entry` into `buf`.
    size_t read(size_t idx, uint64_t offset_in_entry, void* buf, size_t len);

    // Read full content of small entry (tik/cert/cnmt — must be ≤ 1 MB).
    bool read_all(size_t idx, std::vector<uint8_t>& out);

private:
    FILE*                 m_fp    = nullptr;
    bool                  m_valid = false;
    std::string           m_error;
    std::vector<PfsEntry> m_entries;

    void parse();
    // Returns true and fills `out_entries` with the HFS0 contents at
    // `hfs0_abs_offset` within the file.
    bool parse_hfs0(uint64_t hfs0_abs_offset, std::vector<PfsEntry>& out_entries,
                    const char* label);
};

} // namespace Install
