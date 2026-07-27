// source/services/storage_paths.hpp
//
// Catalog-driven path resolution shared by every transport. Given a transport-
// neutral absolute path of the form  /<Storage Display Name>/<rest...>  this maps
// it to the concrete libnx VFS path  <vfs_root>/<rest...>  for whichever enabled
// StorageCatalog surface matches the first path component — or reports that the
// path is the root chooser, an install target, a title-query surface, or invalid.
//
// This generalizes what ftp_paths.hpp did for FTP's three hardcoded folders: the
// storage set now comes from the catalog, so adding a surface (Album now; NAND,
// gamecard later) makes it browseable over ALL transports at once, with no
// per-transport path code. Pure (no libnx/sockets) → host-tested.

#pragma once

#include "services/storage_catalog.hpp"
#include "config/config.hpp"

#include <string>

namespace Services {

enum class PathKind {
    Root,          // "/" — the chooser; list the enabled storage folders
    StorageRoot,   // "/<Storage>" exactly — the top of one surface
    Filesystem,    // "/<Storage>/rest" under a Filesystem surface — real file I/O
    Install,       // under an Install surface — writing installs
    TitleQuery,    // under a TitleQuery surface (Installed Titles) — synthesized
    SaveData,      // under Save Data — THREE levels: <User>/<Title>/<file...>.
                   //   The first two are synthesized (accounts, then that user's
                   //   titles); only the third is a real mount. `rel` holds the
                   //   part after "/Save Data/" and the caller decomposes it,
                   //   because unlike every other Filesystem surface this one has
                   //   no single vfs_root that <rest> can simply be appended to.
    Invalid,       // not under any enabled storage root
};

struct ResolvedPath {
    PathKind           kind = PathKind::Invalid;
    StorageSurface::Id id{};         // valid unless Root/Invalid
    std::string        vfs;          // concrete "<vfs_root>/rest" for Filesystem;
                                     // "" otherwise
    std::string        rel;          // path relative to the surface root
    bool               has_surface = false;
};

// Split "/A/b/c" → first="A", rest="b/c". "/" → first="", rest="".
inline void sp_split_first(const std::string& posix,
                           std::string& first, std::string& rest) {
    size_t i = 0;
    while (i < posix.size() && posix[i] == '/') ++i;
    size_t start = i;
    while (i < posix.size() && posix[i] != '/') ++i;
    first = posix.substr(start, i - start);
    while (i < posix.size() && posix[i] == '/') ++i;
    rest = (i < posix.size()) ? posix.substr(i) : std::string();
}

inline bool sp_is_root(const std::string& posix) {
    for (char c : posix) if (c != '/') return false;
    return true;
}

// Find an ENABLED surface whose display name matches `name` exactly. Returns
// nullptr if no enabled surface matches (disabled surfaces are invisible, so a
// path into one resolves to Invalid — a client can't reach a disabled storage).
inline const StorageSurface* sp_find_enabled_by_name(const std::string& name,
                                                     const Config::Surfaces& cfg) {
    for (const auto& s : StorageCatalog::all()) {
        if (name == s.display && StorageCatalog::enabled(s.id, cfg))
            return &s;
    }
    return nullptr;
}

// Resolve a transport-neutral absolute path against the catalog.
inline ResolvedPath sp_resolve(const std::string& posix, const Config::Surfaces& cfg) {
    ResolvedPath out;
    if (sp_is_root(posix)) { out.kind = PathKind::Root; return out; }

    std::string first, rest;
    sp_split_first(posix, first, rest);

    const StorageSurface* s = sp_find_enabled_by_name(first, cfg);
    if (!s) { out.kind = PathKind::Invalid; return out; }

    out.id          = s->id;
    out.has_surface = true;
    out.rel         = rest;

    switch (s->kind) {
        case StorageKind::Install:
            out.kind = PathKind::Install;
            return out;
        case StorageKind::TitleQuery:
            out.kind = PathKind::TitleQuery;
            return out;
        case StorageKind::Filesystem:
            if (rest.empty()) {
                if (s->id == StorageSurface::Id::Saves) {
                    out.kind = PathKind::SaveData;   // rel empty => list users
                    out.vfs.clear();
                } else {
                    out.kind = PathKind::StorageRoot;
                    out.vfs  = std::string(s->vfs_root) + "/";   // "sdmc:/"
                }
            } else {
                if (s->id == StorageSurface::Id::Saves) {
                    // Do NOT map onto vfs_root here: "save:" only becomes valid
                    // once a specific (user, title) is mounted, and the mapping
                    // would produce a bogus "save:/<User>/<Title>/..." path.
                    out.kind = PathKind::SaveData;
                    out.vfs.clear();
                } else {
                    out.kind = PathKind::Filesystem;
                    out.vfs  = std::string(s->vfs_root) + "/" + rest;
                }
            }
            return out;
    }
    out.kind = PathKind::Invalid;
    return out;
}

// ─── Save Data decomposition ──────────────────────────────────────────────────
//
// Splits the `rel` of a PathKind::SaveData result into its three levels. Pure
// string work — no mounting, no libnx — so the level logic is host-testable and
// the transports only have to act on the answer.
struct SavePath {
    enum class Level {
        Users,    // "/Save Data"                      -> list users
        Titles,   // "/Save Data/<User>"               -> list that user's titles
        Files,    // "/Save Data/<User>/<Title>/..."   -> mount, then real file I/O
    };
    Level       level = Level::Users;
    std::string user;      // display name as it appeared in the path
    std::string title;     // title folder name (carries the id in brackets)
    std::string rest;      // remainder inside the save, "" at the save root
};

inline SavePath sp_split_save(const std::string& rel) {
    SavePath out;
    if (rel.empty()) { out.level = SavePath::Level::Users; return out; }

    const size_t s1 = rel.find('/');
    if (s1 == std::string::npos) {
        out.level = SavePath::Level::Titles;
        out.user  = rel;
        return out;
    }
    out.user = rel.substr(0, s1);

    const std::string after = rel.substr(s1 + 1);
    if (after.empty()) {          // trailing slash: still the user level
        out.level = SavePath::Level::Titles;
        return out;
    }

    const size_t s2 = after.find('/');
    out.level = SavePath::Level::Files;
    if (s2 == std::string::npos) {
        out.title = after;        // the save root itself
    } else {
        out.title = after.substr(0, s2);
        out.rest  = after.substr(s2 + 1);
    }
    return out;
}

} // namespace Services
