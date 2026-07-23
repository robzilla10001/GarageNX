#pragma once
// source/services/title_surface.hpp
//
// The "Installed Titles" surface, presented to a PC client as a flat directory of
// virtual NSP files:
//
//     /Installed Titles/Zelda [0100000000010000][BASE][v0].nsp
//
// Nothing here exists on disk. The listing is synthesized from ncm, and (from 3c)
// reading one of these files streams a PFS0 built on the fly from the title's NCAs.
// That is what makes "dump a title over FTP" work the way DBI does it.
//
// Enumeration is CACHED. Listing costs ncm queries, and a client may re-list a
// directory repeatedly while browsing; the cache is invalidated whenever the title
// database is marked dirty (install/delete), so it cannot go stale silently.

#include "core/ncm.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace Services {

// One synthesized entry in a virtual directory.
struct VirtualEntry {
    std::string name;            // wire filename, from title_naming
    uint64_t    size   = 0;      // bytes a client would download
    bool        is_dir = false;
};

// A snapshot of the current Installed Titles listing.
//
// Returned BY VALUE deliberately. The main loop rebuilds the underlying vector as
// names resolve; handing a transport a reference to it meant the transport could
// iterate the vector while the main thread cleared and refilled it — a use-after-
// free across threads. A snapshot costs one copy per listing and removes the race
// entirely. An empty result means "nothing enumerated yet", not an error, so a
// client sees an empty folder rather than a broken one.
std::vector<VirtualEntry> installed_titles_list();

// Resolve a wire filename back to the title it names. False if no such title —
// which is the correct answer for a stale filename a client cached before an
// uninstall.
bool installed_titles_find(const std::string& filename, Core::Ncm::Title& out);

// Resolve ONE pending display name. Call from the MAIN LOOP only, once per frame.
// Name resolution decrypts a Control NCA and opens ncm sessions; the on-device
// title screen has always done one per frame, and doing them all at once from a
// transport worker thread crashed the console. Transports therefore never resolve
// names themselves — they read what this has filled in, falling back to id-based
// names for anything not yet done, so listing is always immediate and safe.
void installed_titles_tick();

// Teardown: release any transport worker blocked waiting for the main loop.
void installed_titles_shutdown();

// Drop the cache. Called when titles are installed or removed.
void installed_titles_invalidate();

} // namespace Services
