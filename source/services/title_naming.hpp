#pragma once
// source/services/title_naming.hpp
//
// ONE definition of how an installed title is named on the wire, shared by every
// transport and by the on-device UI. A title appears to a PC client as a single
// virtual NSP file:
//
//     Zelda Breath of the Wild [0100000000010000][BASE][v0].nsp
//
// Both directions matter. Listing needs title -> filename; a client then asks for
// that exact filename back (RETR / GetObject / GET), so we must be able to map
// filename -> title again. Doing this in one tested place is what stops the two
// halves drifting: a name we can generate but not parse is a file the user can see
// and not download.
//
// Pure and host-testable: no libnx, no ncm calls — it works on an already
// enumerated Core::Ncm::Title.

#include "core/ncm.hpp"

#include <cstdint>
#include <string>

namespace Services {

// The wire filename for a title, including the ".nsp" extension.
// Characters that are illegal in FAT filenames are replaced, and the name is
// length-capped, because this string becomes a real filename on the client's disk.
std::string title_to_filename(const Core::Ncm::Title& t);

// What was parsed back out of a filename. `ok` is false if the name doesn't look
// like one of ours (wrong extension, missing/!valid id).
struct ParsedTitleName {
    bool     ok         = false;
    uint64_t meta_id    = 0;   // the id we round-trip on: identifies THIS meta
    uint32_t version    = 0;
    Core::Ncm::TitleType type = Core::Ncm::TitleType::Other;
};

// Inverse of title_to_filename. Only the bracketed fields are parsed — the display
// name is decorative and may contain anything, so it is never used for matching.
ParsedTitleName parse_title_filename(const std::string& filename);

// Sanitise an arbitrary display name into something safe to put in a filename.
// Exposed because Save Data uses the same rule for its per-title folders.
std::string sanitize_for_filename(const std::string& raw);

// The folder name for a title's save data (no extension — it's a directory).
std::string title_to_save_dirname(const Core::Ncm::Title& t);

} // namespace Services
