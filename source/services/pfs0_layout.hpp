#pragma once
// source/services/pfs0_layout.hpp
//
// The PFS0 (NSP) container layout, as pure arithmetic. Extracted so that the size
// we ADVERTISE in a listing and the bytes we later STREAM come from one place —
// if they disagree by even one byte, a client either truncates the download or
// errors out at the end, and that is a miserable bug to chase.
//
// Layout (little-endian):
//   0x00  "PFS0"
//   0x04  u32 file_count
//   0x08  u32 string_table_size   (includes padding up to the 16-byte alignment)
//   0x0C  u32 reserved (0)
//   0x10  entry[file_count], each 0x18 bytes:
//           0x00 u64 data offset, relative to the END of the header
//           0x08 u64 file size
//           0x10 u32 name offset into the string table
//           0x14 u32 reserved (0)
//   ...   string table (NUL-terminated names), padded so the header is 16-aligned
//   ...   file data, concatenated in entry order, no padding between files
//
// Pure: no libnx, no I/O. Host-tested.

#include <cstdint>
#include <string>
#include <vector>

namespace Services {

struct Pfs0File {
    std::string name;
    uint64_t    size = 0;
};

struct Pfs0Layout {
    std::vector<uint8_t>  header;         // complete header, ready to write
    uint64_t              header_size = 0;
    uint64_t              total_size  = 0;   // header + every file's data
    std::vector<uint64_t> data_offsets;      // ABSOLUTE offset of each file in the NSP
};

/// Build the full header and offsets for a set of files.
Pfs0Layout pfs0_build(const std::vector<Pfs0File>& files);

/// Total NSP size only, without materialising the header. This is what a directory
/// listing advertises, so it must agree with pfs0_build().total_size exactly.
uint64_t pfs0_total_size(const std::vector<Pfs0File>& files);

} // namespace Services
