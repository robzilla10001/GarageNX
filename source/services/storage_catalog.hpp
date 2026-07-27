// source/services/storage_catalog.hpp
//
// THE shared definition of every storage surface GarageNX exposes — the single
// source of truth that MTP, FTP, and HTTP all consume, so the three transports
// can never drift into different storage sets, names, gates, or safety rules.
//
// This is to storage what StreamDriver is to install: the anti-drift keystone.
// Before this, each transport hardcoded its own storages (MTP had three; FTP had
// its own chooser). They now all ask the catalog "what's enabled?" and "how do I
// treat this surface?" instead of deciding for themselves.
//
// This header is PURE: no libnx, no sockets, no mounting. It only DESCRIBES the
// surfaces (id, name, config gate, VFS root, access policy, confirm policy, kind).
// Actually mounting a partition and enforcing the policies is the transport's job,
// done in risk-ordered waves. Keeping the description pure makes it host-testable.

#pragma once

#include "config/config.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace Services {

// What kind of surface this is — determines how a transport treats it.
enum class StorageKind {
    Filesystem,   // a real mounted filesystem (SD, NAND, saves, album, gamecard)
    Install,      // a write-only drop target: writing a file installs it
    TitleQuery,   // NOT a filesystem — a synthesized view over ncm/ns metadata
                  // (the "Installed Titles" surface; read-only, listing derived)
};

// Whether a transport may mutate the surface, and how confirmation is required.
enum class Access {
    ReadWrite,    // normal read + write (SD, saves, album)
    ReadOnly,     // listing/reading only; writes rejected outright (gamecard,
                  //   Installed Titles, and NAND until a confirm is satisfied)
};

enum class Confirm {
    None,         // no extra confirmation — ordinary operations
    OnDevice,     // any mutating op must be confirmed by a modal ON THE CONSOLE
                  //   (the cross-transport NAND safety model). Enforced via the
                  //   ConfirmationBroker; see storage catalog notes.
};

// One storage surface. `id` is stable and transport-agnostic; transports map it
// to their own wire representation (MTP storage id, FTP root folder name, HTTP
// path prefix). `vfs_root` is the libnx mount prefix the surface lives under once
// mounted (e.g. "sdmc:", "bis_user:", "save:"), or "" for non-filesystem kinds.
struct StorageSurface {
    enum class Id : uint32_t {
        SdCard = 1,
        SdInstall,
        NandInstall,
        NandUser,
        NandSystem,
        Saves,
        Album,
        Gamecard,
        InstalledTitles,
    };

    Id           id;
    const char*  key;         // stable string key ("sd_card", "nand_user", ...)
    const char*  display;     // human name shown to a client ("SD Card", ...)
    const char*  vfs_root;    // libnx mount prefix, or "" for TitleQuery/Install
    StorageKind  kind;
    Access       access;
    Confirm      confirm;
};

// The catalog. Static description of every surface; per-surface enabled state is
// read from Config at query time so a settings change takes effect immediately.
class StorageCatalog {
public:
    // All surfaces, in a stable display order, regardless of enabled state.
    static const std::vector<StorageSurface>& all();

    // Is this surface currently enabled in config?  (The gate that decides whether
    // a transport should enumerate it at all.)
    static bool enabled(StorageSurface::Id id, const Config::Surfaces& cfg);

    // Enabled surfaces only, in display order — what a transport should expose.
    static std::vector<StorageSurface> enabled_surfaces(const Config::Surfaces& cfg);

    // Look up a surface by id. Returns nullptr if unknown.
    static const StorageSurface* find(StorageSurface::Id id);

    /// Which Filesystem surface owns this concrete VFS path? Matches the path's
    /// mount prefix ("sdmc:/x" -> SD Card, "bis_user:/y" -> NAND User). Returns
    /// nullptr if no surface claims it — callers should treat that as "unknown,
    /// deny" for any mutating operation. Enabled-ness is NOT considered here; ask
    /// enabled() separately if it matters.
    static const StorageSurface* surface_for_vfs(const std::string& vfs_path);

    /// Can a client's write ever reach this surface — freely, or after an
    /// on-device confirmation?
    ///
    /// `Access::ReadOnly` alone does NOT mean unwritable: NAND and Save Data are
    /// ReadOnly by POLICY and become writable per operation once the user
    /// confirms on the console. Only ReadOnly + Confirm::None is unwritable.
    ///
    /// This exists because that distinction has to be consulted from outside the
    /// guard. MTP's StorageInfo must report REAL capacity for anything writable:
    /// a host checks FreeSpaceInBytes before opening a transfer and refuses
    /// client-side if the file does not fit, so advertising zero on a writable
    /// surface produces "there is not enough space on the destination" for a
    /// 553-byte file, with no request reaching the device and nothing in a wire
    /// log. Save Data and NAND System both shipped with that bug.
    static bool is_writable(const StorageSurface& s);

    /// Is this surface's backing mount actually present RIGHT NOW?
    ///
    /// A surface can be enabled in config and still have no device behind it —
    /// a mount that failed, or one that has not happened yet. Listing such a
    /// surface produces a folder that errors on entry over FTP, and a storage
    /// whose enumeration fails with "could not get object handles" over MTP.
    ///
    /// Shared because the two transports disagreed: FTP probed and hid the
    /// surface, MTP did not probe and advertised it. Same config, same catalog,
    /// two behaviours — the drift this class exists to prevent.
    ///
    /// Non-Filesystem surfaces (Install targets, the synthesized Installed
    /// Titles) have no mount and are always available. Save Data is
    /// Filesystem-kind so the write guard recognises "save:" paths, but its top
    /// two levels are synthesized and nothing is mounted until a title folder is
    /// entered — probing "save:/" would hide it entirely, so it is exempt.
    static bool mount_available(const StorageSurface& s);

    // Does a mutating operation on this surface require on-device confirmation?
    static bool needs_confirmation(const StorageSurface& s) {
        return s.confirm == Confirm::OnDevice;
    }

    // May a transport write to this surface at all (before any confirmation)?
    // ReadOnly surfaces reject writes outright; NAND is ReadOnly here and only a
    // satisfied on-device confirmation elevates a single operation.
    static bool writable(const StorageSurface& s) {
        return s.access == Access::ReadWrite;
    }
};

} // namespace Services
