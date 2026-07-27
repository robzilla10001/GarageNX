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

// Request enumeration WITHOUT blocking, for callers that are themselves on the
// main thread. Returns whatever is ready right now (possibly empty on the first
// call), and marks the work wanted so installed_titles_tick() advances it over
// the following frames.
//
// This exists because installed_titles_list() BLOCKS the caller until the main
// loop has ticked the work forward — which is correct for a transport worker (the
// loop keeps running) but a deadlock-shaped stall for a MAIN-THREAD caller: the
// loop cannot tick while the main thread is parked inside this call, so it waits
// out the full timeout and returns unresolved names. Symptom: the on-device Save
// Manager took ~10s on the first browse after boot and showed id-only names, then
// was instant on re-entry because the cache had warmed meanwhile.
//
// A main-thread caller uses this and its OWN per-frame redraw to fill names in,
// exactly as the screen already does for its own list — never installed_titles_list().
std::vector<VirtualEntry> installed_titles_request_nonblocking();

// Display names resolved so far, for the id-labelling path. True once enumeration
// has completed (names may still be filling in). Lets a main-thread screen show
// the list immediately and re-label as resolution progresses.
bool installed_titles_enumerated();

// True once there is NO name-resolution work left — enumeration finished AND every
// title has been through the resolver. This is the signal a main-thread caller
// needs when it must have final names before doing something irreversible with
// them (naming a backup directory, say), because "enumerated" alone is true while
// names are still filling in one per tick.
//
// Becomes true even when keys are unavailable: in that case names can never
// resolve, the resolver marks itself finished immediately, and a caller waiting on
// this is released rather than sitting out a timeout for names that will never
// arrive.
bool installed_titles_names_resolved();

// Resolve ONE pending display name. Call from the MAIN LOOP only, once per frame.
// Name resolution decrypts a Control NCA and opens ncm sessions; the on-device
// title screen has always done one per frame, and doing them all at once from a
// transport worker thread crashed the console. Transports therefore never resolve
// names themselves — they read what this has filled in, falling back to id-based
// names for anything not yet done, so listing is always immediate and safe.
void installed_titles_tick();

// Exact NSP byte count for a virtual title file, as the stream will produce it.
// Computed once per title (it must read each NCA header to detect titlekey crypto)
// and CACHED, because MTP asks for an object's size every time a client browses.
// Returns 0 if the title is unknown or its stream cannot be built.
// Safe to call from a transport worker: it is one title's work, client-paced.
uint64_t installed_titles_exact_size(const std::string& filename);

// Display name for an APPLICATION id, from names already resolved by the main
// loop. Returns "" if unknown (not yet resolved, or no such title) — callers fall
// back to showing the raw id. Cache-only: never triggers ncm work, so it is safe to
// call from a transport thread while listing.
std::string installed_titles_name_for_app(uint64_t application_id);

// Teardown: release any transport worker blocked waiting for the main loop.
void installed_titles_shutdown();

// Drop the cache. Called when titles are installed or removed.
void installed_titles_invalidate();

} // namespace Services
